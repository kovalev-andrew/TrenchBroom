/*
 Copyright (C) 2026 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/BezierBendTool.h"

#include "Logger.h"
#include "NotifierConnection.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Camera.h"
#include "mdl/BrushNode.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/EditorContext.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include <QComboBox>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include "kd/result.h"
#include "kd/map_utils.h"

#include "vm/mat_ext.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace tb::ui
{
namespace
{
constexpr auto BendEpsilon = 0.001;
constexpr size_t MaxBendUndoSteps = 128u;

size_t axisIndex(const BezierBendTool::Axis axis)
{
  switch (axis)
  {
  case BezierBendTool::Axis::X:
    return 0u;
  case BezierBendTool::Axis::Y:
    return 1u;
  case BezierBendTool::Axis::Z:
    return 2u;
    switchDefault();
  }
}

vm::vec3d axisVector(const BezierBendTool::Axis axis)
{
  switch (axis)
  {
  case BezierBendTool::Axis::X:
    return vm::vec3d{1, 0, 0};
  case BezierBendTool::Axis::Y:
    return vm::vec3d{0, 1, 0};
  case BezierBendTool::Axis::Z:
    return vm::vec3d{0, 0, 1};
    switchDefault();
  }
}

vm::vec3d handleComponentAxis(const BezierBendTool::HandleComponent component)
{
  switch (component)
  {
  case BezierBendTool::HandleComponent::XAxis:
    return vm::vec3d{1, 0, 0};
  case BezierBendTool::HandleComponent::YAxis:
    return vm::vec3d{0, 1, 0};
  case BezierBendTool::HandleComponent::ZAxis:
    return vm::vec3d{0, 0, 1};
  case BezierBendTool::HandleComponent::Center:
    return vm::vec3d{0, 0, 0};
    switchDefault();
  }
}

RgbaF handleComponentColor(const BezierBendTool::HandleComponent component)
{
  switch (component)
  {
  case BezierBendTool::HandleComponent::XAxis:
    return RgbaF{1.0f, 0.18f, 0.18f, 1.0f};
  case BezierBendTool::HandleComponent::YAxis:
    return RgbaF{0.25f, 1.0f, 0.35f, 1.0f};
  case BezierBendTool::HandleComponent::ZAxis:
    return RgbaF{0.22f, 0.55f, 1.0f, 1.0f};
  case BezierBendTool::HandleComponent::Center:
    return RgbaF{1.0f, 0.1f, 0.75f, 1.0f};
    switchDefault();
  }
}

size_t handleComponentIndex(const BezierBendTool::HandleComponent component)
{
  switch (component)
  {
  case BezierBendTool::HandleComponent::XAxis:
    return 0u;
  case BezierBendTool::HandleComponent::YAxis:
    return 1u;
  case BezierBendTool::HandleComponent::ZAxis:
    return 2u;
  case BezierBendTool::HandleComponent::Center:
    return 0u;
    switchDefault();
  }
}

std::tuple<vm::vec3d, vm::vec3d> rotationArcBasis(
  const BezierBendTool::HandleComponent component)
{
  switch (component)
  {
  case BezierBendTool::HandleComponent::XAxis:
    return {vm::vec3d{0, 1, 0}, vm::vec3d{0, 0, 1}};
  case BezierBendTool::HandleComponent::YAxis:
    return {vm::vec3d{0, 0, 1}, vm::vec3d{1, 0, 0}};
  case BezierBendTool::HandleComponent::ZAxis:
    return {vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};
  case BezierBendTool::HandleComponent::Center:
    return {vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};
    switchDefault();
  }
}

vm::vec3d rotationArcPoint(
  const vm::vec3d& position,
  const BezierBendTool::HandleComponent component,
  const double radius,
  const double angle)
{
  const auto [u, v] = rotationArcBasis(component);
  return position + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius);
}

std::tuple<size_t, double> curveSegment(const size_t pointCount, const double t)
{
  if (pointCount < 2u)
  {
    return {0u, 0.0};
  }

  const auto scaled = std::clamp(t, 0.0, 1.0) * static_cast<double>(pointCount - 1u);
  auto index = static_cast<size_t>(std::floor(scaled));
  if (index >= pointCount - 1u)
  {
    index = pointCount - 2u;
    return {index, 1.0};
  }

  return {index, scaled - static_cast<double>(index)};
}

std::tuple<size_t, double> curveSegment(
  const std::vector<vm::vec3d>& points, const double t)
{
  return curveSegment(points.size(), t);
}

vm::vec3d curvePoint(const std::vector<vm::vec3d>& points, const double t)
{
  if (points.empty())
  {
    return vm::vec3d{0, 0, 0};
  }
  if (points.size() == 1u)
  {
    return points.front();
  }

  const auto [index, u] = curveSegment(points, t);
  const auto& p0 = points[index == 0u ? index : index - 1u];
  const auto& p1 = points[index];
  const auto& p2 = points[index + 1u];
  const auto& p3 = points[std::min(index + 2u, points.size() - 1u)];

  const auto u2 = u * u;
  const auto u3 = u2 * u;
  return (p1 * 2.0 + (p2 - p0) * u + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * u2
          + (-p0 + p1 * 3.0 - p2 * 3.0 + p3) * u3)
         * 0.5;
}

vm::vec3d curveTangent(
  const std::vector<vm::vec3d>& points, const double t, const vm::vec3d& fallback)
{
  if (points.size() < 2u)
  {
    return fallback;
  }

  const auto [index, u] = curveSegment(points, t);
  const auto& p0 = points[index == 0u ? index : index - 1u];
  const auto& p1 = points[index];
  const auto& p2 = points[index + 1u];
  const auto& p3 = points[std::min(index + 2u, points.size() - 1u)];

  const auto u2 = u * u;
  const auto tangent = ((p2 - p0) + (p0 * 4.0 - p1 * 10.0 + p2 * 8.0 - p3 * 2.0) * u
                        + (-p0 * 3.0 + p1 * 9.0 - p2 * 9.0 + p3 * 3.0) * u2)
                       * 0.5;
  if (vm::is_zero(tangent, vm::Cd::almost_zero()))
  {
    return fallback;
  }
  return vm::normalize(tangent);
}

double curveValue(
  const std::vector<double>& values, const double t, const double defaultValue)
{
  if (values.empty())
  {
    return defaultValue;
  }
  if (values.size() == 1u)
  {
    return values.front();
  }

  const auto [index, u] = curveSegment(values.size(), t);
  return values[index] + (values[index + 1u] - values[index]) * u;
}

vm::vec3d curveVec3(
  const std::vector<vm::vec3d>& values, const double t, const vm::vec3d& defaultValue)
{
  if (values.empty())
  {
    return defaultValue;
  }
  if (values.size() == 1u)
  {
    return values.front();
  }

  const auto [index, u] = curveSegment(values.size(), t);
  return values[index] + (values[index + 1u] - values[index]) * u;
}

vm::mat4x4d bendBrushTransform(
  const BezierBendTool::Axis axis,
  const double axisMin,
  const double axisExtent,
  const vm::vec3d& axisStart,
  const std::vector<vm::vec3d>& controlPoints,
  const std::vector<vm::vec3d>& controlScales,
  const std::vector<vm::vec3d>& controlRotations,
  const vm::vec3d& brushCenter)
{
  const auto axisDir = axisVector(axis);
  const auto axisCoord = brushCenter[axisIndex(axis)];
  const auto t = std::clamp((axisCoord - axisMin) / axisExtent, 0.0, 1.0);

  const auto basePoint = axisStart + axisDir * (axisCoord - axisMin);
  const auto lateralOffset = brushCenter - basePoint;
  const auto tangent = curveTangent(controlPoints, t, axisDir);
  const auto rotation = vm::rotation_matrix(axisDir, tangent);
  const auto handleRotation = curveVec3(controlRotations, t, vm::vec3d{0, 0, 0});
  const auto localRotation =
    vm::rotation_matrix(handleRotation.x(), handleRotation.y(), handleRotation.z());
  const auto scale = curveVec3(controlScales, t, vm::vec3d::fill(1.0));
  const auto targetCenter =
    curvePoint(controlPoints, t)
    + vm::vec3d{rotation * localRotation * (lateralOffset * scale)};

  return vm::translation_matrix(targetCenter) * rotation * localRotation
         * vm::scaling_matrix(scale) * vm::translation_matrix(-brushCenter);
}

vm::vec3d continuousBendPoint(
  const BezierBendTool::Axis axis,
  const double axisMin,
  const double axisExtent,
  const vm::vec3d& axisStart,
  const std::vector<vm::vec3d>& controlPoints,
  const std::vector<vm::vec3d>& controlScales,
  const std::vector<vm::vec3d>& controlRotations,
  const vm::vec3d& point)
{
  const auto axisDir = axisVector(axis);
  const auto axisCoord = point[axisIndex(axis)];
  const auto t = std::clamp((axisCoord - axisMin) / axisExtent, 0.0, 1.0);
  const auto basePoint = axisStart + axisDir * (axisCoord - axisMin);
  const auto lateralOffset = point - basePoint;
  const auto tangent = curveTangent(controlPoints, t, axisDir);
  const auto rotation = vm::rotation_matrix(axisDir, tangent);
  const auto handleRotation = curveVec3(controlRotations, t, vm::vec3d{0, 0, 0});
  const auto localRotation =
    vm::rotation_matrix(handleRotation.x(), handleRotation.y(), handleRotation.z());
  const auto scale = curveVec3(controlScales, t, vm::vec3d::fill(1.0));
  return curvePoint(controlPoints, t)
         + vm::vec3d{rotation * localRotation * (lateralOffset * scale)};
}

std::optional<mdl::Brush> createContinuouslyBentBrush(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const BezierBendTool::Axis axis,
  const double axisMin,
  const double axisExtent,
  const vm::vec3d& axisStart,
  const std::vector<vm::vec3d>& controlPoints,
  const std::vector<vm::vec3d>& controlScales,
  const std::vector<vm::vec3d>& controlRotations)
{
  auto points = std::vector<vm::vec3d>{};
  const auto sourcePoints = sourceBrush.vertexPositions();
  points.reserve(sourcePoints.size());
  for (const auto& point : sourcePoints)
  {
    points.push_back(continuousBendPoint(
      axis,
      axisMin,
      axisExtent,
      axisStart,
      controlPoints,
      controlScales,
      controlRotations,
      point));
  }

  const auto materialName = sourceBrush.faceCount() > 0u
                              ? sourceBrush.face(0).attributes().materialName()
                              : map.currentMaterialName();
  const auto builder = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto result = builder.createBrush(points, materialName);
  if (!result)
  {
    return std::nullopt;
  }

  auto brush = std::move(result).value();
  brush.cloneFaceAttributesFrom(sourceBrush);
  return brush;
}

std::optional<mdl::Brush> createBentBrush(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const BezierBendTool::Axis axis,
  const double axisMin,
  const double axisExtent,
  const vm::vec3d& axisStart,
  const std::vector<vm::vec3d>& controlPoints,
  const std::vector<vm::vec3d>& controlScales,
  const std::vector<vm::vec3d>& controlRotations,
  const bool keepJoints)
{
  if (keepJoints)
  {
    if (
      auto brush = createContinuouslyBentBrush(
        map,
        sourceBrush,
        axis,
        axisMin,
        axisExtent,
        axisStart,
        controlPoints,
        controlScales,
        controlRotations))
    {
      return brush;
    }
  }

  auto brush = sourceBrush;
  const auto transform = bendBrushTransform(
    axis,
    axisMin,
    axisExtent,
    axisStart,
    controlPoints,
    controlScales,
    controlRotations,
    sourceBrush.bounds().center());
  const auto result =
    brush.transform(map.worldBounds(), transform, map.editorContext().uvLock());
  if (!result)
  {
    return std::nullopt;
  }
  return brush;
}

std::optional<mdl::BrushFace> createAxisClipFace(
  const mdl::Map& map,
  const BezierBendTool::Axis axis,
  const double position,
  const bool positiveNormal)
{
  auto p0 = vm::vec3d{0, 0, 0};
  auto p1 = vm::vec3d{0, 0, 0};
  auto p2 = vm::vec3d{0, 0, 0};

  switch (axis)
  {
  case BezierBendTool::Axis::X:
    p0 = vm::vec3d{position, 0, 0};
    p1 = vm::vec3d{position, 1, 0};
    p2 = vm::vec3d{position, 0, 1};
    break;
  case BezierBendTool::Axis::Y:
    p0 = vm::vec3d{0, position, 0};
    p1 = vm::vec3d{0, position, 1};
    p2 = vm::vec3d{1, position, 0};
    break;
  case BezierBendTool::Axis::Z:
    p0 = vm::vec3d{0, 0, position};
    p1 = vm::vec3d{1, 0, position};
    p2 = vm::vec3d{0, 1, position};
    break;
    switchDefault();
  }

  if (!positiveNormal)
  {
    std::swap(p1, p2);
  }

  auto face = mdl::BrushFace::create(
    p0,
    p1,
    p2,
    mdl::BrushFaceAttributes(map.currentMaterialName()),
    map.worldNode().mapFormat());
  if (!face)
  {
    return std::nullopt;
  }
  return std::move(face).value();
}

std::optional<mdl::Brush> clipBrushToAxisLimit(
  const mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const BezierBendTool::Axis axis,
  const double position,
  const bool keepGreater)
{
  const auto component = axisIndex(axis);
  for (const auto positiveNormal : {true, false})
  {
    auto brush = sourceBrush;
    auto face = createAxisClipFace(map, axis, position, positiveNormal);
    if (!face)
    {
      continue;
    }
    if (!brush.clip(map.worldBounds(), std::move(*face)))
    {
      continue;
    }

    if (
      keepGreater ? brush.bounds().min[component] >= position - BendEpsilon
                  : brush.bounds().max[component] <= position + BendEpsilon)
    {
      return brush;
    }
  }

  return std::nullopt;
}

std::vector<mdl::Brush> subdivideBrushForBend(
  const mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const BezierBendTool::Axis axis,
  const int subdivisions)
{
  const auto sliceCount = size_t{1u}
                          << static_cast<size_t>(std::clamp(subdivisions, 0, 6));
  if (sliceCount <= 1u)
  {
    return {sourceBrush};
  }

  const auto component = axisIndex(axis);
  const auto bounds = sourceBrush.bounds();
  const auto min = bounds.min[component];
  const auto extent = bounds.size()[component];
  if (extent <= BendEpsilon)
  {
    return {sourceBrush};
  }

  auto result = std::vector<mdl::Brush>{};
  result.reserve(sliceCount);

  for (size_t i = 0u; i < sliceCount; ++i)
  {
    auto slice = sourceBrush;
    const auto start =
      min + extent * static_cast<double>(i) / static_cast<double>(sliceCount);
    const auto end =
      min + extent * static_cast<double>(i + 1u) / static_cast<double>(sliceCount);

    if (i > 0u)
    {
      if (auto clipped = clipBrushToAxisLimit(map, slice, axis, start, true))
      {
        slice = std::move(*clipped);
      }
      else
      {
        continue;
      }
    }

    if (i + 1u < sliceCount)
    {
      if (auto clipped = clipBrushToAxisLimit(map, slice, axis, end, false))
      {
        slice = std::move(*clipped);
      }
      else
      {
        continue;
      }
    }

    result.push_back(std::move(slice));
  }

  return result.empty() ? std::vector<mdl::Brush>{sourceBrush} : result;
}

class BezierBendToolPage : public QWidget
{
private:
  BezierBendTool& m_tool;
  NotifierConnection m_notifierConnection;
  QSpinBox* m_segments = nullptr;
  QSpinBox* m_subdivisions = nullptr;
  QComboBox* m_axis = nullptr;
  QComboBox* m_mode = nullptr;
  QCheckBox* m_keepJoints = nullptr;
  QPushButton* m_default = nullptr;
  QPushButton* m_apply = nullptr;

public:
  explicit BezierBendToolPage(BezierBendTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  void createGui()
  {
    auto* label = new QLabel{tr("Bezier Bend")};

    m_segments = new QSpinBox{};
    m_segments->setRange(3, 64);
    m_segments->setSingleStep(1);
    m_segments->setToolTip(tr("Number of editable curve segments"));

    m_subdivisions = new QSpinBox{};
    m_subdivisions->setRange(0, 6);
    m_subdivisions->setSingleStep(1);
    m_subdivisions->setToolTip(tr("Extra brush subdivision iterations before bending"));

    m_axis = new QComboBox{};
    m_axis->addItem(tr("X"));
    m_axis->addItem(tr("Y"));
    m_axis->addItem(tr("Z"));
    m_axis->setToolTip(tr("Axis used as the bend length"));

    m_mode = new QComboBox{};
    m_mode->addItem(tr("Move"));
    m_mode->addItem(tr("Scale"));
    m_mode->addItem(tr("Rotate"));
    m_mode->setToolTip(tr("How curve handles are manipulated"));

    m_keepJoints = new QCheckBox{tr("Keep joints")};
    m_keepJoints->setToolTip(
      tr("Bend brush vertices continuously so adjacent pieces stay connected"));

    m_default = new QPushButton{tr("Default")};
    m_apply = new QPushButton{tr("Apply")};

    connect(
      m_segments,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setSegments(value); });
    connect(
      m_subdivisions,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setSubdivisions(value); });
    connect(
      m_axis,
      QOverload<int>::of(&QComboBox::currentIndexChanged),
      this,
      [&](const int value) { m_tool.setAxis(static_cast<BezierBendTool::Axis>(value)); });
    connect(
      m_mode,
      QOverload<int>::of(&QComboBox::currentIndexChanged),
      this,
      [&](const int value) {
        m_tool.setManipulationMode(static_cast<BezierBendTool::ManipulationMode>(value));
      });
    connect(m_keepJoints, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setKeepJoints(checked);
    });
    connect(m_default, &QPushButton::clicked, this, [&] { m_tool.resetSettings(); });
    connect(m_apply, &QPushButton::clicked, this, [&] { m_tool.applyBendTool(); });

    m_notifierConnection +=
      m_tool.settingsDidChangeNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolActivatedNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolDeactivatedNotifier.connect([&](auto&) { updateControls(); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Segments")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_segments, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Subdiv")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_subdivisions, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Axis")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_axis, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Mode")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_mode, 0, Qt::AlignVCenter);
    layout->addWidget(m_keepJoints, 0, Qt::AlignVCenter);
    layout->addWidget(m_default, 0, Qt::AlignVCenter);
    layout->addWidget(m_apply, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    setLayout(layout);
    updateControls();
  }

  void updateControls()
  {
    const auto active = m_tool.active();
    setVisible(active);

    {
      const auto blocker = QSignalBlocker{m_segments};
      m_segments->setValue(m_tool.segments());
    }
    {
      const auto blocker = QSignalBlocker{m_subdivisions};
      m_subdivisions->setValue(m_tool.subdivisions());
    }
    {
      const auto blocker = QSignalBlocker{m_axis};
      m_axis->setCurrentIndex(static_cast<int>(m_tool.axis()));
    }
    {
      const auto blocker = QSignalBlocker{m_mode};
      m_mode->setCurrentIndex(static_cast<int>(m_tool.manipulationMode()));
    }
    {
      const auto blocker = QSignalBlocker{m_keepJoints};
      m_keepJoints->setChecked(m_tool.keepJoints());
    }

    m_segments->setEnabled(active);
    m_subdivisions->setEnabled(active);
    m_axis->setEnabled(active);
    m_mode->setEnabled(active);
    m_keepJoints->setEnabled(active);
    m_default->setEnabled(active);
    m_apply->setEnabled(active && m_tool.bendActive());
  }
};
} // namespace

const mdl::HitType::Type BezierBendTool::HandleHitType = mdl::HitType::freeType();

BezierBendTool::BezierBendTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

bool BezierBendTool::canAttachToSelection() const
{
  const auto& selection = m_document.map().selection();
  return selection.hasOnlyGroups() && selection.groups.size() == 1u
         && !selection.allBrushes().empty();
}

bool BezierBendTool::bendActive() const
{
  return m_bend.has_value();
}

int BezierBendTool::segments() const
{
  return m_segments;
}

int BezierBendTool::subdivisions() const
{
  return m_subdivisions;
}

BezierBendTool::Axis BezierBendTool::axis() const
{
  return m_axis;
}

BezierBendTool::ManipulationMode BezierBendTool::manipulationMode() const
{
  return m_manipulationMode;
}

bool BezierBendTool::keepJoints() const
{
  return m_keepJoints;
}

double BezierBendTool::handleAxisLength() const
{
  return std::max(64.0, grid().actualSize() * 8.0);
}

const mdl::Grid& BezierBendTool::grid() const
{
  return m_document.map().grid();
}

void BezierBendTool::resetSettings()
{
  if (!active())
  {
    return;
  }

  pushUndoState();
  m_segments = 8;
  m_subdivisions = 0;
  m_manipulationMode = ManipulationMode::Move;
  m_keepJoints = true;
  if (m_bend)
  {
    resetControlPoints(m_bend->bounds);
    applyPreview();
  }
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::setSegments(const int segments)
{
  const auto clampedSegments = std::clamp(segments, 3, 64);
  if (m_segments == clampedSegments)
  {
    return;
  }

  pushUndoState();
  const auto oldControlPoints = m_controlPoints;
  const auto oldControlScales = m_controlScales;
  const auto oldControlRotations = m_controlRotations;
  m_segments = clampedSegments;
  if (!oldControlPoints.empty())
  {
    m_controlPoints.clear();
    m_controlScales.clear();
    m_controlRotations.clear();
    m_controlPoints.reserve(static_cast<size_t>(m_segments) + 1u);
    m_controlScales.reserve(static_cast<size_t>(m_segments) + 1u);
    m_controlRotations.reserve(static_cast<size_t>(m_segments) + 1u);
    for (int i = 0; i <= m_segments; ++i)
    {
      const auto t = static_cast<double>(i) / static_cast<double>(m_segments);
      m_controlPoints.push_back(curvePoint(oldControlPoints, t));
      m_controlScales.push_back(curveVec3(oldControlScales, t, vm::vec3d::fill(1.0)));
      m_controlRotations.push_back(curveVec3(oldControlRotations, t, vm::vec3d{0, 0, 0}));
    }
  }
  else if (m_bend)
  {
    resetControlPoints(m_bend->bounds);
  }

  if (m_bend)
  {
    applyPreview();
  }
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::setSubdivisions(const int subdivisions)
{
  const auto clampedSubdivisions = std::clamp(subdivisions, 0, 6);
  if (m_subdivisions == clampedSubdivisions)
  {
    return;
  }

  pushUndoState();
  m_subdivisions = clampedSubdivisions;
  if (m_bend)
  {
    applyPreview();
  }
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::setAxis(const Axis axis)
{
  if (m_axis == axis)
  {
    return;
  }

  pushUndoState();
  m_axis = axis;
  if (m_bend)
  {
    closeBendTransaction(false);
    m_bend = std::nullopt;
    beginBend();
  }
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::setManipulationMode(const ManipulationMode manipulationMode)
{
  if (m_manipulationMode == manipulationMode)
  {
    return;
  }

  pushUndoState();
  m_manipulationMode = manipulationMode;
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::setKeepJoints(const bool keepJoints)
{
  if (m_keepJoints == keepJoints)
  {
    return;
  }

  pushUndoState();
  m_keepJoints = keepJoints;
  if (m_bend)
  {
    applyPreview();
  }
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::applyBendTool()
{
  if (m_closingBend)
  {
    return;
  }

  if (!m_bend)
  {
    beginBend();
  }

  if (!m_bend)
  {
    return;
  }

  m_closingBend = true;
  closeBendTransaction(true);
  m_bend = std::nullopt;
  m_controlPoints.clear();
  m_controlScales.clear();
  m_controlRotations.clear();
  m_undoStack.clear();
  bendDidRequestDeactivateNotifier(*this);
  m_closingBend = false;
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::cancelBendTool()
{
  if (m_closingBend)
  {
    return;
  }

  m_closingBend = true;
  closeBendTransaction(false);
  m_bend = std::nullopt;
  m_controlPoints.clear();
  m_controlScales.clear();
  m_controlRotations.clear();
  m_undoStack.clear();
  bendDidRequestDeactivateNotifier(*this);
  m_closingBend = false;
  refreshViews();
  notifySettingsChanged();
}

void BezierBendTool::pushUndoState()
{
  if (!active() || !m_bend || m_controlPoints.empty())
  {
    return;
  }

  auto state = BendUndoState{};
  state.controlPoints = m_controlPoints;
  state.controlScales = m_controlScales;
  state.controlRotations = m_controlRotations;
  state.axis = m_axis;
  state.manipulationMode = m_manipulationMode;
  state.segments = m_segments;
  state.subdivisions = m_subdivisions;
  state.keepJoints = m_keepJoints;

  if (!m_undoStack.empty())
  {
    const auto& last = m_undoStack.back();
    if (
      last.controlPoints == state.controlPoints && last.axis == state.axis
      && last.controlScales == state.controlScales
      && last.controlRotations == state.controlRotations
      && last.manipulationMode == state.manipulationMode
      && last.segments == state.segments && last.subdivisions == state.subdivisions
      && last.keepJoints == state.keepJoints)
    {
      return;
    }
  }

  if (m_undoStack.size() >= MaxBendUndoSteps)
  {
    m_undoStack.erase(m_undoStack.begin());
  }
  m_undoStack.push_back(std::move(state));
}

bool BezierBendTool::undoBendStep()
{
  if (!active() || m_undoStack.empty())
  {
    return false;
  }

  auto state = std::move(m_undoStack.back());
  m_undoStack.pop_back();

  const auto axisChanged = m_axis != state.axis;
  if (axisChanged && m_bend)
  {
    closeBendTransaction(false);
    m_bend = std::nullopt;
  }

  m_axis = state.axis;
  m_manipulationMode = state.manipulationMode;
  m_segments = state.segments;
  m_subdivisions = state.subdivisions;
  m_keepJoints = state.keepJoints;
  m_controlPoints = std::move(state.controlPoints);
  m_controlScales = std::move(state.controlScales);
  m_controlRotations = std::move(state.controlRotations);
  const auto restoredControlPoints = m_controlPoints;
  const auto restoredControlScales = m_controlScales;
  const auto restoredControlRotations = m_controlRotations;

  if (!m_bend)
  {
    if (!beginBend())
    {
      refreshViews();
      notifySettingsChanged();
      return true;
    }
    m_controlPoints = restoredControlPoints;
    m_controlScales = restoredControlScales;
    m_controlRotations = restoredControlRotations;
  }

  applyPreview();
  refreshViews();
  notifySettingsChanged();
  return true;
}

vm::vec3d BezierBendTool::handlePosition(const size_t index) const
{
  return index < m_controlPoints.size() ? m_controlPoints[index] : vm::vec3d{0, 0, 0};
}

vm::vec3d BezierBendTool::handleScale(const size_t index) const
{
  return index < m_controlScales.size() ? m_controlScales[index] : vm::vec3d::fill(1.0);
}

vm::vec3d BezierBendTool::handleRotation(const size_t index) const
{
  return index < m_controlRotations.size() ? m_controlRotations[index]
                                           : vm::vec3d{0, 0, 0};
}

bool BezierBendTool::moveHandleTo(const size_t index, const vm::vec3d& position)
{
  if (!m_bend || index >= m_controlPoints.size())
  {
    return false;
  }

  if (vm::is_zero(m_controlPoints[index] - position, BendEpsilon))
  {
    return true;
  }

  m_controlPoints[index] = position;
  return applyPreview();
}

bool BezierBendTool::scaleHandleTo(
  const size_t index, const HandleComponent component, const double scale)
{
  if (!m_bend || index >= m_controlScales.size())
  {
    return false;
  }

  const auto clampedScale = std::clamp(scale, 0.05, 10.0);
  auto newScale = m_controlScales[index];
  if (component == HandleComponent::Center)
  {
    newScale = vm::vec3d::fill(clampedScale);
  }
  else
  {
    newScale[handleComponentIndex(component)] = clampedScale;
  }

  if (vm::is_zero(m_controlScales[index] - newScale, BendEpsilon))
  {
    return true;
  }

  m_controlScales[index] = newScale;
  return applyPreview();
}

bool BezierBendTool::rotateHandleTo(
  const size_t index, const HandleComponent component, const double rotation)
{
  if (!m_bend || index >= m_controlRotations.size())
  {
    return false;
  }

  auto newRotation = m_controlRotations[index];
  if (component == HandleComponent::Center)
  {
    const auto axis = axisIndex(m_axis);
    newRotation[axis] = rotation;
  }
  else
  {
    newRotation[handleComponentIndex(component)] = rotation;
  }

  if (vm::is_zero(m_controlRotations[index] - newRotation, BendEpsilon))
  {
    return true;
  }

  m_controlRotations[index] = newRotation;
  return applyPreview();
}

void BezierBendTool::pickHandle(
  const vm::ray3d& pickRay,
  const gl::Camera& camera,
  const double handleRadius,
  mdl::PickResult& pickResult) const
{
  if (!active() || !m_bend)
  {
    return;
  }

  for (size_t i = 0u; i < m_controlPoints.size(); ++i)
  {
    const auto position = m_controlPoints[i];
    if (
      const auto distance = camera.pickPointHandle(pickRay, position, handleRadius * 3.2))
    {
      pickResult.addHit(
        mdl::Hit{
          HandleHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          HandleHit{i, HandleComponent::Center, position}});
    }

    const auto axisLength = handleAxisLength();
    const auto centerClearance = std::max(4.0, grid().actualSize() * 0.5) * 1.6;
    for (const auto component :
         {HandleComponent::XAxis, HandleComponent::YAxis, HandleComponent::ZAxis})
    {
      if (m_manipulationMode == ManipulationMode::Rotate)
      {
        const auto radius = axisLength * 0.75;
        constexpr auto ArcSegments = 16;
        auto previous = rotationArcPoint(position, component, radius, 0.0);
        for (int j = 1; j <= ArcSegments; ++j)
        {
          const auto angle =
            vm::Cd::half_pi() * static_cast<double>(j) / static_cast<double>(ArcSegments);
          const auto current = rotationArcPoint(position, component, radius, angle);
          const auto segment = vm::segment3d{previous, current};
          if (
            const auto axisDistance =
              camera.pickLineSegmentHandle(pickRay, segment, handleRadius * 1.7))
          {
            pickResult.addHit(
              mdl::Hit{
                HandleHitType,
                *axisDistance,
                vm::point_at_distance(pickRay, *axisDistance),
                HandleHit{i, component, position}});
          }
          previous = current;
        }
      }
      else
      {
        const auto axis = handleComponentAxis(component);
        const auto segment =
          vm::segment3d{position + axis * centerClearance, position + axis * axisLength};
        if (
          const auto axisDistance =
            camera.pickLineSegmentHandle(pickRay, segment, handleRadius * 1.5))
        {
          pickResult.addHit(
            mdl::Hit{
              HandleHitType,
              *axisDistance,
              vm::point_at_distance(pickRay, *axisDistance),
              HandleHit{i, component, position}});
        }
      }
    }
  }
}

void BezierBendTool::render(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult) const
{
  if (!active() || !m_bend)
  {
    return;
  }

  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setShowOccludedObjects();
  renderService.setLineWidth(2.0f);

  using namespace mdl::HitFilters;
  auto highlighted = std::optional<HandleHit>{};
  if (const auto& hit = pickResult.first(type(HandleHitType)); hit.isMatch())
  {
    highlighted = hit.target<HandleHit>();
  }

  renderService.setForegroundColor(RgbaF{1.0f, 0.1f, 0.75f, 1.0f});
  for (size_t i = 1u; i < m_controlPoints.size(); ++i)
  {
    renderService.renderLine(
      vm::vec3f{m_controlPoints[i - 1u]}, vm::vec3f{m_controlPoints[i]});
  }

  renderService.setForegroundColor(RgbaF{0.35f, 1.0f, 0.65f, 1.0f});
  const auto curveSegments = std::max(3, m_segments);
  auto previous = curvePoint(m_controlPoints, 0.0);
  for (int i = 1; i <= curveSegments * 4; ++i)
  {
    const auto t = static_cast<double>(i) / static_cast<double>(curveSegments * 4);
    const auto current = curvePoint(m_controlPoints, t);
    renderService.renderLine(vm::vec3f{previous}, vm::vec3f{current});
    previous = current;
  }

  renderService.setForegroundColor(RgbaF{1.0f, 0.1f, 0.75f, 1.0f});
  for (size_t i = 0u; i < m_controlPoints.size(); ++i)
  {
    const auto size = std::max(8.0, grid().actualSize() * 0.75);
    const auto half = vm::vec3d::fill(size * 0.5);
    renderService.renderBounds(
      vm::bbox3f{vm::bbox3d{m_controlPoints[i] - half, m_controlPoints[i] + half}});
    renderService.renderHandle(vm::vec3f{m_controlPoints[i]});
    if (
      highlighted && highlighted->index == i
      && highlighted->component == HandleComponent::Center)
    {
      renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
      renderService.renderHandleHighlight(vm::vec3f{m_controlPoints[i]});
      renderService.setForegroundColor(RgbaF{1.0f, 0.1f, 0.75f, 1.0f});
    }

    const auto axisLength = handleAxisLength();
    const auto centerClearance = std::max(4.0, grid().actualSize() * 0.5) * 1.6;
    for (const auto component :
         {HandleComponent::XAxis, HandleComponent::YAxis, HandleComponent::ZAxis})
    {
      renderService.setLineWidth(
        highlighted && highlighted->index == i && highlighted->component == component
          ? 6.0f
          : 4.0f);
      renderService.setForegroundColor(handleComponentColor(component));
      if (m_manipulationMode == ManipulationMode::Rotate)
      {
        const auto radius = axisLength * 0.75;
        constexpr auto ArcSegments = 16;
        auto arcPrevious = rotationArcPoint(m_controlPoints[i], component, radius, 0.0);
        for (int j = 1; j <= ArcSegments; ++j)
        {
          const auto angle =
            vm::Cd::half_pi() * static_cast<double>(j) / static_cast<double>(ArcSegments);
          const auto current =
            rotationArcPoint(m_controlPoints[i], component, radius, angle);
          renderService.renderLine(vm::vec3f{arcPrevious}, vm::vec3f{current});
          arcPrevious = current;
        }
        renderService.renderHandle(
          vm::vec3f{
            rotationArcPoint(m_controlPoints[i], component, radius, vm::Cd::half_pi())});
      }
      else
      {
        const auto axis = handleComponentAxis(component);
        renderService.renderLine(
          vm::vec3f{m_controlPoints[i] + axis * centerClearance},
          vm::vec3f{m_controlPoints[i] + axis * axisLength});
        renderService.renderHandle(vm::vec3f{m_controlPoints[i] + axis * axisLength});
      }
    }
  }
}

bool BezierBendTool::beginBend()
{
  if (m_bend)
  {
    return true;
  }

  if (!canAttachToSelection())
  {
    return false;
  }

  auto& map = m_document.map();
  auto state = BendState{};
  state.group = map.selection().groups.front();
  state.bounds = state.group->logicalBounds();

  const auto axis = axisIndex(m_axis);
  state.axisMin = state.bounds.min[axis];
  state.axisExtent = state.bounds.size()[axis];
  state.axisStart = state.bounds.center();
  state.axisStart[axis] = state.axisMin;
  if (state.axisExtent <= BendEpsilon)
  {
    return false;
  }

  const auto& brushes = map.selection().allBrushes();
  state.brushes.reserve(brushes.size());
  for (auto* brushNode : brushes)
  {
    state.brushes.push_back(
      BendBrush{brushNode, brushNode->brush(), brushNode->logicalBounds().center()});
  }

  if (state.brushes.empty())
  {
    return false;
  }

  resetControlPoints(state.bounds);
  map.startTransaction("Bezier Bend Group", mdl::TransactionScope::LongRunning);
  m_bendTransactionOpen = true;
  m_bend = std::move(state);
  applyPreview();
  notifySettingsChanged();
  return true;
}

void BezierBendTool::closeBendTransaction(const bool commit)
{
  if (!m_bendTransactionOpen)
  {
    return;
  }

  if (commit)
  {
    m_document.map().commitTransaction();
  }
  else
  {
    m_document.map().cancelTransaction();
  }
  m_bendTransactionOpen = false;
}

bool BezierBendTool::applyPreview()
{
  if (!m_bend || !m_bendTransactionOpen)
  {
    return false;
  }

  auto& map = m_document.map();
  map.rollbackTransaction();

  if (m_controlPoints.size() < 2u)
  {
    return false;
  }

  if (m_subdivisions > 0)
  {
    auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
    auto originalNodes = std::vector<mdl::Node*>{};
    originalNodes.reserve(m_bend->brushes.size());

    for (const auto& bendBrush : m_bend->brushes)
    {
      auto* parent = bendBrush.node->parent();
      if (!parent)
      {
        kdl::map_clear_and_delete(nodesToAdd);
        return false;
      }

      const auto sourceBrushes =
        subdivideBrushForBend(map, bendBrush.originalBrush, m_axis, m_subdivisions);
      if (sourceBrushes.empty())
      {
        kdl::map_clear_and_delete(nodesToAdd);
        return false;
      }

      auto& parentNodes = nodesToAdd[parent];
      for (const auto& sourceBrush : sourceBrushes)
      {
        auto brush = createBentBrush(
          map,
          sourceBrush,
          m_axis,
          m_bend->axisMin,
          m_bend->axisExtent,
          m_bend->axisStart,
          m_controlPoints,
          m_controlScales,
          m_controlRotations,
          m_keepJoints);
        if (!brush)
        {
          kdl::map_clear_and_delete(nodesToAdd);
          return false;
        }
        parentNodes.push_back(new mdl::BrushNode{std::move(*brush)});
      }
      originalNodes.push_back(bendBrush.node);
    }

    const auto addedNodes = mdl::addNodes(map, nodesToAdd);
    if (addedNodes.empty())
    {
      return false;
    }
    mdl::removeNodes(map, originalNodes);

    refreshViews();
    notifySettingsChanged();
    return true;
  }

  auto nodesToUpdate = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  nodesToUpdate.reserve(m_bend->brushes.size());

  for (const auto& bendBrush : m_bend->brushes)
  {
    auto brush = createBentBrush(
      map,
      bendBrush.originalBrush,
      m_axis,
      m_bend->axisMin,
      m_bend->axisExtent,
      m_bend->axisStart,
      m_controlPoints,
      m_controlScales,
      m_controlRotations,
      m_keepJoints);
    if (!brush)
    {
      m_document.logger().error() << "Could not bend brush";
      return false;
    }

    nodesToUpdate.emplace_back(bendBrush.node, mdl::NodeContents{std::move(*brush)});
  }

  const auto didUpdate =
    mdl::updateNodeContents(map, "Bezier Bend Group", std::move(nodesToUpdate));
  if (didUpdate)
  {
    refreshViews();
    notifySettingsChanged();
  }
  return didUpdate;
}

void BezierBendTool::resetControlPoints(const vm::bbox3d& bounds)
{
  const auto axis = axisIndex(m_axis);
  const auto axisDir = axisVector(m_axis);
  const auto min = bounds.min[axis];
  const auto extent = std::max(BendEpsilon, bounds.size()[axis]);
  auto start = bounds.center();
  start[axis] = min;

  m_controlPoints.clear();
  m_controlScales.clear();
  m_controlRotations.clear();
  m_controlPoints.reserve(static_cast<size_t>(m_segments) + 1u);
  m_controlScales.reserve(static_cast<size_t>(m_segments) + 1u);
  m_controlRotations.reserve(static_cast<size_t>(m_segments) + 1u);
  for (int i = 0; i <= m_segments; ++i)
  {
    const auto t = static_cast<double>(i) / static_cast<double>(m_segments);
    m_controlPoints.push_back(start + axisDir * (extent * t));
    m_controlScales.push_back(vm::vec3d::fill(1.0));
    m_controlRotations.push_back(vm::vec3d{0, 0, 0});
  }
}

void BezierBendTool::notifySettingsChanged()
{
  settingsDidChangeNotifier(*this);
  notifyToolHandleSelectionChanged();
}

bool BezierBendTool::doActivate()
{
  m_segments = 8;
  m_subdivisions = 0;
  m_manipulationMode = ManipulationMode::Move;
  m_keepJoints = true;
  m_bendTransactionOpen = false;
  m_closingBend = false;
  m_controlPoints.clear();
  m_controlScales.clear();
  m_controlRotations.clear();
  m_undoStack.clear();
  return beginBend();
}

bool BezierBendTool::doDeactivate()
{
  if (!m_closingBend)
  {
    closeBendTransaction(false);
    m_bend = std::nullopt;
    m_controlPoints.clear();
    m_controlScales.clear();
    m_controlRotations.clear();
    m_undoStack.clear();
  }
  notifySettingsChanged();
  return true;
}

QWidget* BezierBendTool::doCreatePage(QWidget* parent)
{
  return new BezierBendToolPage{*this, parent};
}

} // namespace tb::ui
