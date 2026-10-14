/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/SettleDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "mdl/BrushFace.h"

#include <algorithm>
#include <limits>

namespace tb::ui
{
class SettlePreview : public QWidget
{
  const std::vector<mdl::Brush>& m_source;
  const std::vector<mdl::Brush>& m_obstacles;
  const std::vector<vm::mat4x4d>& m_transforms;
  const std::vector<mdl::SettleTriangle>& m_triangles;

public:
  SettlePreview(
    const std::vector<mdl::Brush>& source,
    const std::vector<mdl::Brush>& obstacles,
    const std::vector<vm::mat4x4d>& transforms,
    QWidget* parent,
    const std::vector<mdl::SettleTriangle>& triangles)
    : QWidget{parent}
    , m_source{source}
    , m_obstacles{obstacles}
    , m_transforms{transforms}
    , m_triangles{triangles}
  {
    setMinimumSize(440, 320);
    setAccessibleName(tr("Settle animated wireframe preview"));
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    auto painter = QPainter{this};
    painter.fillRect(rect(), QColor{30, 33, 39});
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_source.empty())
    {
      return;
    }
    const auto project = [](const vm::vec3d& p) {
      return QPointF{
        (p.x() - p.y()) * 0.7071067812,
        (p.x() + p.y()) * 0.4082482905 - p.z() * 0.8164965809};
    };
    // Fit original and current pieces together, so the fall remains visible.
    auto minX = std::numeric_limits<double>::max();
    auto minY = minX;
    auto maxX = -minX;
    auto maxY = -minX;
    for (size_t i = 0; i < m_source.size(); ++i)
    {
      const auto transform = i < m_transforms.size() ? m_transforms[i] : vm::mat4x4d{};
      for (const auto& p : m_source[i].vertexPositions())
      {
        for (const auto& q : {project(p), project(transform * p)})
        {
          minX = std::min(minX, q.x());
          maxX = std::max(maxX, q.x());
          minY = std::min(minY, q.y());
          maxY = std::max(maxY, q.y());
        }
      }
    }
    const auto center = QPointF{(minX + maxX) / 2, (minY + maxY) / 2};
    const auto scale = std::min(
      (width() - 40.0) / std::max(64.0, maxX - minX),
      (height() - 40.0) / std::max(64.0, maxY - minY));
    const auto screen = [&](const vm::vec3d& p) {
      return QPointF{width() / 2.0, height() / 2.0} + (project(p) - center) * scale;
    };
    // Fill static faces faintly: even an enormous floor whose edges are offscreen
    // remains visible behind the fragments.
    painter.setPen(QPen{QColor{70, 78, 90}, 1});
    painter.setBrush(QColor{65, 72, 85, 45});
    for (const auto& brush : m_obstacles)
    {
      for (const auto& face : brush.faces())
      {
        auto polygon = QPolygonF{};
        for (const auto& p : face.vertexPositions())
        {
          polygon << screen(p);
        }
        painter.drawPolygon(polygon);
      }
    }
    for (const auto& triangle : m_triangles)
    {
      auto polygon = QPolygonF{};
      for (const auto& p : triangle)
        polygon << screen(p);
      painter.drawPolygon(polygon);
    }
    painter.setBrush(Qt::NoBrush);
    for (size_t i = 0; i < m_source.size(); ++i)
    {
      const auto transform = i < m_transforms.size() ? m_transforms[i] : vm::mat4x4d{};
      painter.setPen(QPen{QColor::fromHsv(int((i * 137) % 360), 160, 245), 1.5});
      for (const auto* edge : m_source[i].edges())
      {
        painter.drawLine(
          screen(transform * edge->firstVertex()->position()),
          screen(transform * edge->secondVertex()->position()));
      }
    }
  }
};

SettleDialog::SettleDialog(
  std::vector<mdl::Brush> source,
  std::vector<mdl::Brush> obstacles,
  const vm::bbox3d& bounds,
  QWidget* parent,
  std::vector<mdl::SettleTriangle> patchTriangles)
  : QDialog{parent}
  , m_source{std::move(source)}
  , m_obstacles{std::move(obstacles)}
  , m_bounds{bounds}
  , m_patchTriangles{std::move(patchTriangles)}
  , m_friction{new QDoubleSpinBox{this}}
  , m_bounce{new QDoubleSpinBox{this}}
  , m_kick{new QDoubleSpinBox{this}}
  , m_duration{new QDoubleSpinBox{this}}
  , m_seed{new QSpinBox{this}}
  , m_status{new QLabel{this}}
  , m_pause{new QPushButton{tr("Pause"), this}}
  , m_buttons{new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this}}
  , m_timer{new QTimer{this}}
  , m_preview{
      new SettlePreview{m_source, m_obstacles, m_transforms, this, m_patchTriangles}}
{
  setWindowTitle(tr("Settle Brushes"));
  resize(620, 700);
  auto* layout = new QVBoxLayout{this};
  auto* explanation = new QLabel{
    tr(
      "Selected brushes collide with visible brushes and Quake 3 patches. Hidden "
      "objects and point entities are ignored. No automatic floor is added."),
    this};
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto* form = new QFormLayout{};
  m_friction->setObjectName("settleFriction");
  m_friction->setRange(0, 1);
  m_friction->setSingleStep(0.1);
  m_friction->setValue(0.8);
  m_bounce->setObjectName("settleBounce");
  m_bounce->setRange(0, 1);
  m_bounce->setSingleStep(0.05);
  m_bounce->setValue(0.05);
  m_kick->setObjectName("settleKick");
  m_kick->setRange(0, 256);
  m_kick->setValue(32);
  m_kick->setSuffix(tr(" units/s"));
  m_duration->setObjectName("settleDuration");
  m_duration->setRange(0.1, 20);
  m_duration->setValue(8);
  m_duration->setSuffix(tr(" s"));
  m_seed->setObjectName("settleSeed");
  m_seed->setRange(0, std::numeric_limits<int>::max());
  m_seed->setValue(1);
  form->addRow(tr("Friction"), m_friction);
  form->addRow(tr("Bounce"), m_bounce);
  form->addRow(tr("Initial kick (0 = drop)"), m_kick);
  form->addRow(tr("Duration"), m_duration);
  form->addRow(tr("Seed"), m_seed);
  layout->addLayout(form);
  auto* controls = new QHBoxLayout{};
  auto* simulate = new QPushButton{tr("Simulate / Restart"), this};
  simulate->setObjectName("settleStart");
  m_pause->setObjectName("settlePause");
  controls->addWidget(simulate);
  controls->addWidget(m_pause);
  layout->addLayout(controls);
  layout->addWidget(m_preview, 1);
  m_status->setObjectName("settleStatus");
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
  layout->addWidget(m_buttons);
  m_timer->setObjectName("settleTimer");
  m_timer->setInterval(16);
  for (auto* parameter : {m_friction, m_bounce, m_kick, m_duration})
  {
    connect(parameter, &QDoubleSpinBox::valueChanged, this, [this] { reset(); });
  }
  connect(m_seed, &QSpinBox::valueChanged, this, [this] { reset(); });
  connect(simulate, &QPushButton::clicked, this, [this] { start(); });
  connect(m_pause, &QPushButton::clicked, this, [this] { pause(); });
  connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
  connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(this, &QDialog::finished, this, [this] { m_timer->stop(); });
  reset();
}
SettleDialog::~SettleDialog() = default;

void SettleDialog::reset()
{
  m_timer->stop();
  m_simulation.reset();
  m_result.clear();
  m_transforms.clear();
  m_pause->setEnabled(false);
  m_pause->setText(tr("Pause"));
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  m_status->setText(tr("Press Simulate to preview. The map changes only after Apply."));
  m_preview->update();
}
void SettleDialog::start()
{
  reset();
  const auto options = mdl::BrushSettleOptions{
    m_friction->value(),
    m_bounce->value(),
    m_kick->value(),
    m_duration->value(),
    uint32_t(m_seed->value())};
  auto result = mdl::BrushSettleSimulation::create(
    m_source, m_obstacles, m_bounds, options, m_patchTriangles);
  if (!result)
  {
    (void)(result | kdl::if_error([this](const auto& e) {
             m_status->setText(QString::fromStdString(e.msg));
           }));
    return;
  }
  m_simulation = std::move(result).value();
  m_pause->setEnabled(true);
  m_timer->start();
}
void SettleDialog::tick()
{
  if (!m_simulation || !m_timer->isActive())
  {
    return;
  }
  m_simulation->step();
  m_transforms = m_simulation->transforms();
  m_preview->update();
  m_status->setText(tr("Simulating: %1 s. Pause to apply this moment.")
                      .arg(m_simulation->elapsed(), 0, 'f', 2));
  if (m_simulation->finished())
  {
    m_timer->stop();
    m_pause->setEnabled(false);
    validate();
  }
}
void SettleDialog::pause()
{
  if (!m_simulation)
  {
    return;
  }
  if (m_timer->isActive())
  {
    m_timer->stop();
    m_pause->setText(tr("Resume"));
    validate();
  }
  else
  {
    m_result.clear();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_pause->setText(tr("Pause"));
    m_timer->start();
  }
}
void SettleDialog::validate()
{
  m_result.clear();
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  auto result = m_simulation->result();
  if (!result)
  {
    (void)(result | kdl::if_error([this](const auto& e) {
             m_status->setText(QString::fromStdString(e.msg));
           }));
    return;
  }
  m_result = std::move(result).value();
  m_status->setText(
    m_simulation->sleeping()
      ? tr(
          "All pieces are at rest. Apply keeps their positions; Undo restores the "
          "originals.")
      : tr("Paused at %1 s. Pieces may still be moving; Apply freezes this moment.")
          .arg(m_simulation->elapsed(), 0, 'f', 2));
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
}
std::vector<mdl::Brush> SettleDialog::takeBrushes()
{
  return std::move(m_result);
}
} // namespace tb::ui
