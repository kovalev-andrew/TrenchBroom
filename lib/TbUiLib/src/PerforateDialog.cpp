/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/PerforateDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mdl/BrushFace.h"

#include <algorithm>
#include <limits>
namespace tb::ui
{
namespace
{
class PerforatePreview : public QWidget
{
  const mdl::BrushFace& m_face;
  const mdl::Perforation& m_result;

public:
  PerforatePreview(
    const mdl::BrushFace& face, const mdl::Perforation& result, QWidget* parent)
    : QWidget{parent}
    , m_face{face}
    , m_result{result}
  {
    setMinimumSize(400, 280);
    setAccessibleName(tr("Through-hole preview viewed along the selected face normal"));
  }
  void paintEvent(QPaintEvent*) override
  {
    auto painter = QPainter{this};
    painter.fillRect(rect(), QColor{30, 33, 39});
    painter.setRenderHint(QPainter::Antialiasing);
    const auto n = m_face.boundary().normal;
    const auto u = vm::normalize(
      vm::cross(n, std::abs(n.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{0, 1, 0}));
    const auto v = vm::cross(n, u);
    const auto project = [&](const vm::vec3d& p) {
      return QPointF{vm::dot(p, u), -vm::dot(p, v)};
    };
    auto outline = QPolygonF{};
    for (const auto& p : m_face.vertexPositions())
      outline << project(p);
    const auto bounds = outline.boundingRect();
    const auto scale = std::min(
      (width() - 32.0) / std::max(1.0, bounds.width()),
      (height() - 32.0) / std::max(1.0, bounds.height()));
    const auto screen = [&](const QPointF& p) {
      return QPointF{width() / 2.0, height() / 2.0} + (p - bounds.center()) * scale;
    };
    auto polygon = QPolygonF{};
    for (const auto& p : outline)
      polygon << screen(p);
    painter.setPen(QPen{QColor{170, 180, 195}, 1.5});
    painter.setBrush(QColor{70, 80, 95});
    painter.drawPolygon(polygon);
    painter.setPen(QPen{QColor{255, 180, 80}, 2});
    painter.setBrush(QColor{30, 33, 39});
    for (const auto& plug : m_result.plugs)
    {
      const auto index = plug.findFace(n);
      if (!index)
        continue;
      polygon.clear();
      for (const auto& p : plug.face(*index).vertexPositions())
        polygon << screen(project(p));
      painter.drawPolygon(polygon);
    }
  }
};
} // namespace
PerforateDialog::PerforateDialog(
  const mdl::Brush& source,
  const size_t faceIndex,
  const vm::bbox3d& bounds,
  const mdl::MapFormat format,
  QWidget* parent)
  : QDialog{parent}
  , m_source{source}
  , m_faceIndex{faceIndex}
  , m_bounds{bounds}
  , m_format{format}
  , m_count{new QSpinBox{this}}
  , m_sides{new QSpinBox{this}}
  , m_seed{new QSpinBox{this}}
  , m_min{new QDoubleSpinBox{this}}
  , m_max{new QDoubleSpinBox{this}}
  , m_spacing{new QDoubleSpinBox{this}}
  , m_keep{new QCheckBox{tr("Keep plugs as separate func_wall entities"), this}}
  , m_status{new QLabel{this}}
  , m_buttons{new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this}}
  , m_preview{new PerforatePreview{m_source.face(faceIndex), m_result, this}}
{
  setWindowTitle(tr("Perforate Brush"));
  resize(580, 720);
  auto* layout = new QVBoxLayout{this};
  auto* explanation = new QLabel{
    tr(
      "Create through-holes perpendicular to the selected face. Size is the largest "
      "distance across a hole. All holes must fit; no partial changes are applied."),
    this};
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto* form = new QFormLayout{};
  m_count->setObjectName("holeCount");
  m_count->setRange(1, 64);
  m_count->setValue(4);
  m_sides->setObjectName("holeSides");
  m_sides->setRange(3, 32);
  m_sides->setValue(6);
  m_seed->setObjectName("holeSeed");
  m_seed->setRange(0, std::numeric_limits<int>::max());
  m_seed->setValue(1);
  m_min->setObjectName("holeMinSize");
  m_max->setObjectName("holeMaxSize");
  m_spacing->setObjectName("holeSpacing");
  for (auto* spin : {m_min, m_max, m_spacing})
  {
    spin->setRange(spin == m_spacing ? 1 : 4, 131072);
    spin->setSuffix(tr(" units"));
    spin->setDecimals(2);
  }
  m_min->setValue(16);
  m_max->setValue(32);
  m_spacing->setValue(4);
  m_keep->setObjectName("keepPlugs");
  m_keep->setChecked(true);
  form->addRow(tr("Holes"), m_count);
  form->addRow(tr("Contour sides"), m_sides);
  form->addRow(tr("Minimum diameter"), m_min);
  form->addRow(tr("Maximum diameter"), m_max);
  form->addRow(tr("Minimum spacing / edge margin"), m_spacing);
  form->addRow(tr("Seed"), m_seed);
  layout->addLayout(form);
  layout->addWidget(m_keep);
  auto* generateButton = new QPushButton{tr("Generate preview"), this};
  generateButton->setObjectName("generatePerforation");
  layout->addWidget(generateButton);
  layout->addWidget(m_preview, 1);
  m_status->setWordWrap(true);
  m_status->setObjectName("perforateStatus");
  layout->addWidget(m_status);
  m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
  layout->addWidget(m_buttons);
  for (auto* spin : {m_count, m_sides, m_seed})
    connect(spin, &QSpinBox::valueChanged, this, [this] { invalidate(); });
  for (auto* spin : {m_min, m_max, m_spacing})
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { invalidate(); });
  connect(generateButton, &QPushButton::clicked, this, [this] { generate(); });
  connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  generate();
}
void PerforateDialog::invalidate()
{
  m_result = {};
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  m_preview->update();
  m_status->setText(tr("Generate a preview before applying."));
}
void PerforateDialog::generate()
{
  invalidate();
  const auto options = mdl::PerforateOptions{
    size_t(m_count->value()),
    size_t(m_sides->value()),
    m_min->value(),
    m_max->value(),
    m_spacing->value(),
    uint32_t(m_seed->value())};
  auto result = mdl::perforateBrush(m_source, m_faceIndex, m_bounds, m_format, options);
  if (!result)
  {
    (void)(result | kdl::if_error([this](const auto& error) {
             m_status->setText(QString::fromStdString(error.msg));
           }));
    return;
  }
  m_result = std::move(result).value();
  m_preview->update();
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
  m_status->setText(
    tr(
      "%1 holes ready; %2 remaining brushes. Kept plugs will be selected. If removed, "
      "the remaining brushes will be selected. Undo restores the original.")
      .arg(m_result.plugs.size())
      .arg(m_result.remainder.size()));
}
mdl::Perforation PerforateDialog::takePerforation()
{
  return std::move(m_result);
}
bool PerforateDialog::keepPlugs() const
{
  return m_keep->isChecked();
}
} // namespace tb::ui
