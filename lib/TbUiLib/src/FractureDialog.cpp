/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/FractureDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mdl/BrushFaceAttributes.h"
#include "mdl/BrushFracture.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace tb::ui
{
class FracturePreview : public QWidget
{
private:
  const std::vector<mdl::Brush>& m_fragments;
  vm::vec3d m_center;
  double m_explode = 0.5;

public:
  FracturePreview(
    const std::vector<mdl::Brush>& fragments, const vm::vec3d& center, QWidget* parent)
    : QWidget{parent}
    , m_fragments{fragments}
    , m_center{center}
  {
    setMinimumSize(420, 300);
    setAccessibleName(tr("Fracture wireframe preview"));
  }

  void setExplode(const double value)
  {
    m_explode = value;
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    auto painter = QPainter{this};
    painter.fillRect(rect(), QColor{30, 33, 39});
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_fragments.empty())
    {
      painter.setPen(Qt::white);
      painter.drawText(rect(), Qt::AlignCenter, tr("Generate a preview"));
      return;
    }
    const auto project = [](const vm::vec3d& p) {
      return QPointF{
        (p.x() - p.y()) * 0.7071067812,
        (p.x() + p.y()) * 0.4082482905 - p.z() * 0.8164965809};
    };
    struct Line
    {
      QPointF a;
      QPointF b;
      QColor color;
    };
    auto lines = std::vector<Line>{};
    auto extent = 1.0;
    for (size_t i = 0; i < m_fragments.size(); ++i)
    {
      const auto& brush = m_fragments[i];
      const auto offset = (brush.bounds().center() - m_center) * m_explode;
      const auto color = QColor::fromHsv(int((i * 137) % 360), 160, 245);
      for (const auto* edge : brush.edges())
      {
        const auto a = project(edge->firstVertex()->position() + offset - m_center);
        const auto b = project(edge->secondVertex()->position() + offset - m_center);
        extent = std::max(
          {extent, std::abs(a.x()), std::abs(a.y()), std::abs(b.x()), std::abs(b.y())});
        lines.push_back({a, b, color});
      }
    }
    const auto scale = (double(std::min(width(), height())) - 32.0) / (2.0 * extent);
    const auto origin = QPointF{width() / 2.0, height() / 2.0};
    for (const auto& line : lines)
    {
      painter.setPen(QPen{line.color, 1.3});
      painter.drawLine(origin + line.a * scale, origin + line.b * scale);
    }
  }
};

FractureDialog::FractureDialog(
  const mdl::Brush& source,
  const vm::bbox3d& worldBounds,
  const mdl::MapFormat mapFormat,
  const std::string& material,
  QWidget* parent)
  : QDialog{parent}
  , m_source{source}
  , m_worldBounds{worldBounds}
  , m_mapFormat{mapFormat}
  , m_count{new QSpinBox{this}}
  , m_seed{new QSpinBox{this}}
  , m_material{new QLineEdit{QString::fromStdString(material), this}}
  , m_status{new QLabel{this}}
  , m_buttons{new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this}}
  , m_preview{new FracturePreview{m_fragments, source.bounds().center(), this}}
{
  setWindowTitle(tr("Fracture Brush (Voronoi)"));
  resize(560, 620);
  m_count->setRange(2, int(mdl::MaxFractureFragments));
  m_count->setValue(12);
  m_seed->setRange(0, std::numeric_limits<int>::max());
  m_seed->setValue(1);
  auto* form = new QFormLayout{};
  form->addRow(tr("Fragments"), m_count);
  form->addRow(tr("Seed"), m_seed);
  form->addRow(tr("Cut material"), m_material);
  auto* generate = new QPushButton{tr("Generate Preview"), this};
  auto* nextSeed = new QPushButton{tr("Another Variant"), this};
  auto* previewButtons = new QHBoxLayout{};
  previewButtons->addWidget(generate);
  previewButtons->addWidget(nextSeed);
  auto* explode = new QSlider{Qt::Horizontal, this};
  explode->setRange(0, 200);
  explode->setValue(50);
  explode->setAccessibleName(tr("Preview separation"));
  auto* layout = new QVBoxLayout{this};
  layout->addLayout(form);
  layout->addLayout(previewButtons);
  layout->addWidget(m_preview, 1);
  layout->addWidget(new QLabel{tr("Separation (preview only)"), this});
  layout->addWidget(explode);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
  layout->addWidget(m_buttons);

  connect(m_count, &QSpinBox::valueChanged, this, [this] { invalidatePreview(); });
  connect(m_seed, &QSpinBox::valueChanged, this, [this] { invalidatePreview(); });
  connect(m_material, &QLineEdit::textChanged, this, [this] { invalidatePreview(); });
  connect(generate, &QPushButton::clicked, this, [this] { generatePreview(); });
  connect(nextSeed, &QPushButton::clicked, this, [this] {
    m_seed->setValue(m_seed->value() == m_seed->maximum() ? 0 : m_seed->value() + 1);
    generatePreview();
  });
  connect(explode, &QSlider::valueChanged, this, [this](const int value) {
    m_preview->setExplode(double(value) / 100.0);
  });
  connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  generatePreview();
}

void FractureDialog::invalidatePreview()
{
  m_fragments.clear();
  m_preview->update();
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  m_status->setText(tr("Parameters changed. Generate a preview before applying."));
}

void FractureDialog::generatePreview()
{
  invalidatePreview();
  const auto material = m_material->text().trimmed();
  if (
    material.isEmpty() || material.contains('"') || material.contains('\n')
    || material.contains('\r'))
  {
    m_status->setText(tr("Enter a valid cut material name."));
    return;
  }
  auto result = mdl::fractureBrush(
    m_source,
    m_worldBounds,
    m_mapFormat,
    mdl::BrushFaceAttributes{material.toStdString()},
    size_t(m_count->value()),
    uint32_t(m_seed->value()));
  if (!result)
  {
    (void)(result | kdl::if_error([this](const auto& error) {
             m_status->setText(QString::fromStdString(error.msg));
           }));
    return;
  }
  m_fragments = std::move(result).value();
  m_preview->update();
  m_status->setText(tr("%1 fragments ready. Apply replaces the brush; Undo restores it.")
                      .arg(m_fragments.size()));
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
}

std::vector<mdl::Brush> FractureDialog::takeFragments()
{
  return std::move(m_fragments);
}
} // namespace tb::ui
