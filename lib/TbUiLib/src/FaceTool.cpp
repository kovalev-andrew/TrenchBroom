/*
 Copyright (C) 2010 Kristian Duske

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

#include "ui/FaceTool.h"
#include "gl/Camera.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Logger.h"
#include "NotifierConnection.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/NodeHandles.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PickResult.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"
#include "kd/map_utils.h"
#include "kd/result.h"
#include "kd/set_temp.h"
#include "kd/string_format.h"

#include "vm/mat_ext.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace tb::ui
{
namespace
{
constexpr auto InsetEpsilon = 0.001;
constexpr auto MinInsetScale = 0.05;
constexpr auto SpinEpsilon = 0.001;
constexpr auto MinSpinFlexibility = -1.0;
constexpr auto MaxSpinFlexibility = 1.0;

class InsetToolPage : public QWidget
{
private:
  FaceTool& m_tool;
  NotifierConnection m_notifierConnection;
  QDoubleSpinBox* m_amount = nullptr;
  QCheckBox* m_snapToGrid = nullptr;
  QPushButton* m_default = nullptr;
  QPushButton* m_apply = nullptr;

public:
  explicit InsetToolPage(FaceTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  void createGui()
  {
    auto* label = new QLabel{tr("Inset")};

    m_amount = new QDoubleSpinBox{};
    m_amount->setKeyboardTracking(false);
    m_amount->setRange(0.0, 131072.0);
    m_amount->setDecimals(3);
    m_amount->setSingleStep(1.0);
    m_amount->setToolTip(tr("Inset amount"));

    m_snapToGrid = new QCheckBox{tr("Snap to grid")};
    m_snapToGrid->setToolTip(tr("Snap the inset contour to the current grid"));

    m_default = new QPushButton{tr("Default")};
    m_apply = new QPushButton{tr("Apply")};

    connect(
      m_amount,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setInsetAmount(value); });
    connect(m_snapToGrid, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setInsetSnapToGrid(checked);
    });
    connect(m_default, &QPushButton::clicked, this, [&] { m_tool.resetInsetSettings(); });
    connect(m_apply, &QPushButton::clicked, this, [&] { m_tool.applyInsetTool(); });

    m_notifierConnection +=
      m_tool.insetSettingsDidChangeNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolHandleSelectionChangedNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolActivatedNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolDeactivatedNotifier.connect([&](auto&) { updateControls(); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Amount")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_amount, 0, Qt::AlignVCenter);
    layout->addWidget(m_snapToGrid, 0, Qt::AlignVCenter);
    layout->addWidget(m_default, 0, Qt::AlignVCenter);
    layout->addWidget(m_apply, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    setLayout(layout);
    updateControls();
  }

  void updateControls()
  {
    const auto active = m_tool.insetToolActive();
    const auto faceSelected = m_tool.insetFaceSelected() || m_tool.insetActive();
    setVisible(active);

    {
      const auto blocker = QSignalBlocker{m_amount};
      m_amount->setValue(m_tool.insetAmount());
    }
    m_amount->setSingleStep(m_tool.insetAmountStep());
    {
      const auto blocker = QSignalBlocker{m_snapToGrid};
      m_snapToGrid->setChecked(m_tool.insetSnapToGrid());
    }

    m_amount->setEnabled(active && faceSelected);
    m_snapToGrid->setEnabled(active);
    m_default->setEnabled(active);
    m_apply->setEnabled(active && (faceSelected || m_tool.insetAmount() <= InsetEpsilon));
  }
};

class SpinToolPage : public QWidget
{
private:
  FaceTool& m_tool;
  NotifierConnection m_notifierConnection;
  QSpinBox* m_segments = nullptr;
  QDoubleSpinBox* m_flexibility = nullptr;
  QDoubleSpinBox* m_endScale = nullptr;
  QPushButton* m_default = nullptr;
  QPushButton* m_apply = nullptr;

public:
  explicit SpinToolPage(FaceTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  void createGui()
  {
    auto* label = new QLabel{tr("Spin")};

    m_segments = new QSpinBox{};
    m_segments->setRange(1, 64);
    m_segments->setSingleStep(1);
    m_segments->setToolTip(tr("Number of extruded segments"));

    m_flexibility = new QDoubleSpinBox{};
    m_flexibility->setRange(MinSpinFlexibility, MaxSpinFlexibility);
    m_flexibility->setDecimals(2);
    m_flexibility->setSingleStep(0.1);
    m_flexibility->setToolTip(tr("Curve flexibility"));

    m_endScale = new QDoubleSpinBox{};
    m_endScale->setRange(0.05, 8.0);
    m_endScale->setDecimals(3);
    m_endScale->setSingleStep(0.05);
    m_endScale->setToolTip(tr("Scale at the last segment"));

    m_default = new QPushButton{tr("Default")};
    m_apply = new QPushButton{tr("Apply")};

    connect(
      m_segments,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setSpinSegments(value); });
    connect(
      m_flexibility,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setSpinFlexibility(value); });
    connect(
      m_endScale,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setSpinEndScale(value); });
    connect(m_default, &QPushButton::clicked, this, [&] { m_tool.resetSpinSettings(); });
    connect(m_apply, &QPushButton::clicked, this, [&] {
      const auto snapVerticesToInteger =
        QSettings{}.value("MapWindow/AutoSnapVerticesToInteger", false).toBool();
      m_tool.applySpinTool(snapVerticesToInteger);
    });

    m_notifierConnection +=
      m_tool.spinSettingsDidChangeNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolHandleSelectionChangedNotifier.connect([&](auto&) { updateControls(); });
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
    layout->addWidget(new QLabel{tr("Flex")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_flexibility, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("End scale")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_endScale, 0, Qt::AlignVCenter);
    layout->addWidget(m_default, 0, Qt::AlignVCenter);
    layout->addWidget(m_apply, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    setLayout(layout);
    updateControls();
  }

  void updateControls()
  {
    const auto active = m_tool.spinToolActive();
    const auto faceSelected = m_tool.spinFacesSelected() || m_tool.spinActive();
    setVisible(active);

    {
      const auto blocker = QSignalBlocker{m_segments};
      m_segments->setValue(m_tool.spinSegments());
    }
    {
      const auto blocker = QSignalBlocker{m_flexibility};
      m_flexibility->setValue(m_tool.spinFlexibility());
    }
    {
      const auto blocker = QSignalBlocker{m_endScale};
      m_endScale->setValue(m_tool.spinEndScale());
    }

    m_segments->setEnabled(active);
    m_flexibility->setEnabled(active);
    m_endScale->setEnabled(active);
    m_default->setEnabled(active);
    m_apply->setEnabled(active && faceSelected);
  }
};

class FaceToolPage : public QWidget
{
public:
  explicit FaceToolPage(FaceTool& tool, QWidget* parent)
    : QWidget{parent}
  {
    auto* layout = new QVBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(new InsetToolPage{tool, this});
    layout->addWidget(new SpinToolPage{tool, this});
    setLayout(layout);
  }
};

std::optional<double> maxInsetAmount(const vm::polygon3d& face)
{
  const auto center = face.center();
  auto minDistance = std::numeric_limits<double>::max();
  for (const auto& vertex : face.vertices())
  {
    minDistance = std::min(minDistance, vm::length(vertex - center));
  }

  const auto maxAmount = minDistance * (1.0 - MinInsetScale);
  if (maxAmount <= InsetEpsilon)
  {
    return std::nullopt;
  }
  return maxAmount;
}

double insetScale(const double amount, const double maxAmount)
{
  return std::clamp(1.0 - amount / maxAmount * (1.0 - MinInsetScale), MinInsetScale, 1.0);
}

std::vector<vm::vec3d> scaledFaceVertices(
  const vm::polygon3d& face, const double amount, const double maxAmount)
{
  const auto center = face.center();
  const auto scale = insetScale(amount, maxAmount);

  auto result = std::vector<vm::vec3d>{};
  result.reserve(face.vertices().size());
  for (const auto& vertex : face.vertices())
  {
    result.push_back(center + (vertex - center) * scale);
  }
  return result;
}

std::optional<mdl::Brush> createPrismCutter(
  const mdl::BrushBuilder& builder,
  const std::string& materialName,
  const std::vector<vm::vec3d>& footprint,
  const vm::vec3d& normal,
  const double extent)
{
  if (footprint.size() < 3u)
  {
    return std::nullopt;
  }

  auto points = std::vector<vm::vec3d>{};
  points.reserve(footprint.size() * 2u);
  for (const auto& vertex : footprint)
  {
    points.push_back(vertex + normal * extent);
  }
  for (const auto& vertex : footprint)
  {
    points.push_back(vertex - normal * extent);
  }

  auto brush = builder.createBrush(points, materialName);
  if (!brush)
  {
    return std::nullopt;
  }
  return std::move(brush).value();
}

std::vector<mdl::Brush> createInsetBrushes(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const size_t faceIndex,
  const double amount,
  const double maxAmount,
  const bool snapToGrid)
{
  if (faceIndex >= sourceBrush.faceCount())
  {
    return {};
  }

  const auto& sourceFace = sourceBrush.face(faceIndex);
  const auto outer = sourceFace.polygon();
  const auto& outerVertices = outer.vertices();
  if (outerVertices.size() < 3u)
  {
    return {};
  }

  auto innerVertices = scaledFaceVertices(outer, amount, maxAmount);
  const auto normal = sourceFace.normal();
  const auto extent = std::max(64.0, vm::length(sourceBrush.bounds().size()) + 64.0);
  if (snapToGrid)
  {
    const auto& sourceGrid = map.grid();
    auto snapGrid = mdl::Grid{sourceGrid.size()};
    snapGrid.setRotation(sourceGrid.rotation());
    for (auto& vertex : innerVertices)
    {
      vertex = snapGrid.snap(vertex, sourceFace.boundary());
    }
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};
  const auto materialName = map.currentMaterialName();

  auto cutters = std::vector<mdl::Brush>{};
  cutters.reserve(outerVertices.size() + 1u);

  if (
    auto cutter = createPrismCutter(builder, materialName, innerVertices, normal, extent))
  {
    cutters.push_back(std::move(*cutter));
  }
  else
  {
    return {};
  }

  for (size_t i = 0u; i < outerVertices.size(); ++i)
  {
    const auto j = (i + 1u) % outerVertices.size();
    auto footprint = std::vector<vm::vec3d>{
      outerVertices[i], outerVertices[j], innerVertices[j], innerVertices[i]};
    if (auto cutter = createPrismCutter(builder, materialName, footprint, normal, extent))
    {
      cutters.push_back(std::move(*cutter));
    }
    else
    {
      return {};
    }
  }

  auto result = std::vector<mdl::Brush>{};
  result.reserve(cutters.size());
  for (const auto& cutter : cutters)
  {
    auto fragment = sourceBrush;
    if (fragment.intersect(map.worldBounds(), cutter))
    {
      fragment.cloneFaceAttributesFrom(sourceBrush);
      result.push_back(std::move(fragment));
    }
  }

  return result;
}

vm::vec3d normalizedOr(const vm::vec3d& value, const vm::vec3d& fallback)
{
  return vm::is_zero(value, vm::Cd::almost_zero()) ? fallback : vm::normalize(value);
}

double lerp(const double lhs, const double rhs, const double t)
{
  return lhs + (rhs - lhs) * t;
}

vm::vec3d lerp(const vm::vec3d& lhs, const vm::vec3d& rhs, const double t)
{
  return lhs + (rhs - lhs) * t;
}

bool sameFaceHandles(
  const std::vector<vm::polygon3d>& lhs, const std::vector<vm::polygon3d>& rhs)
{
  if (lhs.size() != rhs.size())
  {
    return false;
  }

  auto used = std::vector<bool>(rhs.size(), false);
  for (const auto& left : lhs)
  {
    auto found = false;
    for (size_t i = 0u; i < rhs.size(); ++i)
    {
      if (!used[i] && compareUnoriented(left, rhs[i], SpinEpsilon) == 0)
      {
        used[i] = true;
        found = true;
        break;
      }
    }

    if (!found)
    {
      return false;
    }
  }
  return true;
}

bool sameSpinUndoState(
  const FaceTool::SpinUndoState& lhs, const FaceTool::SpinUndoState& rhs)
{
  const auto sameEndPosition =
    (!lhs.endPosition && !rhs.endPosition)
    || (lhs.endPosition && rhs.endPosition && vm::squared_length(*lhs.endPosition - *rhs.endPosition) <= SpinEpsilon * SpinEpsilon);

  return sameFaceHandles(lhs.handles, rhs.handles) && lhs.segments == rhs.segments
         && std::abs(lhs.flexibility - rhs.flexibility) <= SpinEpsilon
         && std::abs(lhs.endScale - rhs.endScale) <= SpinEpsilon && sameEndPosition
         && vm::squared_length(lhs.endRotation - rhs.endRotation)
              <= SpinEpsilon * SpinEpsilon;
}

vm::vec3d spinPathDelta(
  const vm::vec3d& endDelta,
  const vm::vec3d& normal,
  const double flexibility,
  const double t)
{
  const auto straight = endDelta * t;
  const auto length = vm::length(endDelta);
  const auto control = normal * length * 0.5;
  const auto curved = control * (2.0 * (1.0 - t) * t) + endDelta * (t * t);

  return lerp(
    straight, curved, std::clamp(flexibility, MinSpinFlexibility, MaxSpinFlexibility));
}

vm::vec3d spinPathTangent(
  const vm::vec3d& endDelta,
  const vm::vec3d& normal,
  const double flexibility,
  const double t)
{
  const auto straight = endDelta;
  const auto length = vm::length(endDelta);
  const auto control = normal * length * 0.5;
  const auto curved = control * (2.0 - 4.0 * t) + endDelta * (2.0 * t);

  return normalizedOr(
    lerp(
      straight, curved, std::clamp(flexibility, MinSpinFlexibility, MaxSpinFlexibility)),
    normal);
}

double smoothStep(const double t)
{
  const auto clampedT = std::clamp(t, 0.0, 1.0);
  return clampedT * clampedT * (3.0 - 2.0 * clampedT);
}

vm::mat4x4d spinPartialRotation(
  const vm::vec3d& from, const vm::vec3d& to, const double amount)
{
  const auto clampedAmount = std::clamp(amount, 0.0, 1.0);
  if (clampedAmount <= 0.0)
  {
    return vm::mat4x4d::identity();
  }

  const auto cosAngle = std::clamp(vm::dot(from, to), -1.0, 1.0);
  if (std::abs(cosAngle - 1.0) <= vm::Cd::almost_zero())
  {
    return vm::mat4x4d::identity();
  }

  auto axis = vm::cross(from, to);
  if (vm::is_zero(axis, vm::Cd::almost_zero()))
  {
    axis = vm::cross(from, vm::vec3d{0, 0, 1});
    if (vm::is_zero(axis, vm::Cd::almost_zero()))
    {
      axis = vm::cross(from, vm::vec3d{1, 0, 0});
    }
  }

  return vm::rotation_matrix(vm::normalize(axis), std::acos(cosAngle) * clampedAmount);
}

std::vector<vm::vec3d> spinSectionVertices(
  const FaceTool::SpinFace& face,
  const vm::vec3d& baseCenter,
  const vm::vec3d& baseNormal,
  const vm::vec3d& endDelta,
  const double flexibility,
  const double endScale,
  const vm::vec3d& endRotation,
  const double t)
{
  const auto center = baseCenter + spinPathDelta(endDelta, baseNormal, flexibility, t);
  const auto scale = lerp(1.0, endScale, t);
  const auto smoothT = smoothStep(t);
  const auto tangent = spinPathTangent(endDelta, baseNormal, flexibility, t);
  const auto pathRotation = spinPartialRotation(baseNormal, tangent, smoothT);
  const auto userRotation =
    vm::rotation_matrix(vm::vec3d{1, 0, 0}, endRotation.x() * smoothT)
    * vm::rotation_matrix(vm::vec3d{0, 1, 0}, endRotation.y() * smoothT)
    * vm::rotation_matrix(vm::vec3d{0, 0, 1}, endRotation.z() * smoothT);
  const auto rotation = userRotation * pathRotation;

  auto result = std::vector<vm::vec3d>{};
  result.reserve(face.polygon.vertices().size());
  for (const auto& vertex : face.polygon.vertices())
  {
    result.push_back(center + rotation * ((vertex - baseCenter) * scale));
  }
  return result;
}

vm::vec3d spinComponentAxis(const FaceTool::SpinHandleComponent component)
{
  switch (component)
  {
  case FaceTool::SpinHandleComponent::XAxis:
  case FaceTool::SpinHandleComponent::XRotation:
    return vm::vec3d{1, 0, 0};
  case FaceTool::SpinHandleComponent::YAxis:
  case FaceTool::SpinHandleComponent::YRotation:
    return vm::vec3d{0, 1, 0};
  case FaceTool::SpinHandleComponent::ZAxis:
  case FaceTool::SpinHandleComponent::ZRotation:
    return vm::vec3d{0, 0, 1};
  case FaceTool::SpinHandleComponent::Center:
    return vm::vec3d{0, 0, 0};
    switchDefault();
  }
}

std::pair<vm::vec3d, vm::vec3d> spinRotationCircleAxes(
  const FaceTool::SpinHandleComponent component)
{
  switch (component)
  {
  case FaceTool::SpinHandleComponent::XRotation:
    return {vm::vec3d{0, 1, 0}, vm::vec3d{0, 0, 1}};
  case FaceTool::SpinHandleComponent::YRotation:
    return {vm::vec3d{1, 0, 0}, vm::vec3d{0, 0, 1}};
  case FaceTool::SpinHandleComponent::ZRotation:
    return {vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};
  case FaceTool::SpinHandleComponent::Center:
  case FaceTool::SpinHandleComponent::XAxis:
  case FaceTool::SpinHandleComponent::YAxis:
  case FaceTool::SpinHandleComponent::ZAxis:
    return {vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};
    switchDefault();
  }
}

Color spinComponentColor(const FaceTool::SpinHandleComponent component)
{
  switch (component)
  {
  case FaceTool::SpinHandleComponent::XAxis:
  case FaceTool::SpinHandleComponent::XRotation:
    return pref(Preferences::XAxisColor);
  case FaceTool::SpinHandleComponent::YAxis:
  case FaceTool::SpinHandleComponent::YRotation:
    return pref(Preferences::YAxisColor);
  case FaceTool::SpinHandleComponent::ZAxis:
  case FaceTool::SpinHandleComponent::ZRotation:
    return pref(Preferences::ZAxisColor);
  case FaceTool::SpinHandleComponent::Center:
    return RgbaF{1.0f, 0.1f, 0.75f, 1.0f};
    switchDefault();
  }
}

std::optional<mdl::Brush> createSpinPrismBrush(
  mdl::Map& map,
  const std::vector<vm::vec3d>& section0,
  const std::vector<vm::vec3d>& section1,
  const std::string& materialName)
{
  if (section0.size() != section1.size() || section0.size() < 3u)
  {
    return std::nullopt;
  }

  const auto attributes = mdl::BrushFaceAttributes{
    materialName, map.gameInfo().gameConfig.faceAttribsConfig.defaults};
  auto faces = std::vector<mdl::BrushFace>{};
  faces.reserve(section0.size() + 2u);

  const auto addFace = [&](
                         const vm::vec3d& p0, const vm::vec3d& p1, const vm::vec3d& p2) {
    auto face =
      mdl::BrushFace::create(p0, p1, p2, attributes, map.worldNode().mapFormat());
    if (!face)
    {
      return false;
    }

    faces.push_back(std::move(face).value());
    return true;
  };

  if (!addFace(section0[0], section0[2], section0[1]))
  {
    return std::nullopt;
  }
  if (!addFace(section1[0], section1[1], section1[2]))
  {
    return std::nullopt;
  }

  for (size_t j = 0u; j < section0.size(); ++j)
  {
    const auto next = (j + 1u) % section0.size();
    if (!addFace(section0[j], section0[next], section1[next]))
    {
      return std::nullopt;
    }
  }

  auto brush = mdl::Brush::create(map.worldBounds(), std::move(faces));
  if (!brush)
  {
    return std::nullopt;
  }

  return std::move(brush).value();
}

std::vector<mdl::Brush> createSpinBrushes(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const FaceTool::SpinFace& face,
  const vm::vec3d& baseCenter,
  const vm::vec3d& baseNormal,
  const vm::vec3d& endPosition,
  const int segments,
  const double flexibility,
  const double endScale,
  const vm::vec3d& endRotation)
{
  if (segments <= 0 || face.polygon.vertices().size() < 3u)
  {
    return {};
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};
  const auto materialName = map.currentMaterialName();
  const auto endDelta = endPosition - baseCenter;

  auto result = std::vector<mdl::Brush>{};
  result.reserve(static_cast<size_t>(segments));

  for (int i = 0; i < segments; ++i)
  {
    const auto t0 = static_cast<double>(i) / static_cast<double>(segments);
    const auto t1 = static_cast<double>(i + 1) / static_cast<double>(segments);
    const auto section0 = spinSectionVertices(
      face, baseCenter, baseNormal, endDelta, flexibility, endScale, endRotation, t0);
    const auto section1 = spinSectionVertices(
      face, baseCenter, baseNormal, endDelta, flexibility, endScale, endRotation, t1);

    auto brush = createSpinPrismBrush(map, section0, section1, materialName);
    if (!brush)
    {
      auto points = std::vector<vm::vec3d>{};
      points.reserve(section0.size() + section1.size());
      points.insert(std::end(points), std::begin(section0), std::end(section0));
      points.insert(std::end(points), std::begin(section1), std::end(section1));

      auto hullBrush = builder.createBrush(points, materialName);
      if (!hullBrush)
      {
        return {};
      }

      brush = std::move(hullBrush).value();
    }

    brush->cloneFaceAttributesFrom(sourceBrush);
    result.push_back(std::move(*brush));
  }

  return result;
}

void snapBrushNodesToInteger(
  mdl::Map& map, const std::vector<mdl::BrushNode*>& brushNodes)
{
  for (auto* brushNode : brushNodes)
  {
    if (!brushNode)
    {
      continue;
    }

    auto brush = brushNode->brush();
    if (!brush.canSnapVertices(map.worldBounds(), 1.0))
    {
      continue;
    }

    brush.snapVertices(map.worldBounds(), 1.0, map.editorContext().uvLock())
      | kdl::transform([&]() { brushNode->setBrush(std::move(brush)); })
      | kdl::transform_error([&](auto e) {
          map.logger().error() << "Could not snap spin brush vertices: " << e.msg;
        });
  }
}

} // namespace

FaceTool::FaceTool(MapDocument& document)
  : BrushHandleToolBase{document}
{
}

const mdl::HitType::Type FaceTool::SpinHandleHitType = mdl::HitType::freeType();

void FaceTool::pick(
  const vm::ray3d& pickRay,
  const gl::Camera& camera,
  const double handleRadius,
  mdl::PickResult& pickResult) const
{
  m_document.map().nodeHandles().pick<mdl::FaceHandle>(
    pickResult, mdl::FaceHandle::HandleHitType, pickRay, camera, handleRadius);
}

std::tuple<vm::vec3d, vm::vec3d> FaceTool::handlePositionAndHitPoint(
  const std::vector<mdl::Hit>& hits) const
{
  contract_pre(!hits.empty());

  const auto& hit = hits.front();
  contract_assert(hit.hasType(mdl::FaceHandle::HandleHitType));

  return {hit.target<mdl::FaceHandle>().position.center(), hit.hitPoint()};
}

FaceTool::MoveResult FaceTool::move(const vm::vec3d& delta)
{
  auto& map = m_document.map();

  const auto facePositions =
    mdl::FaceHandle::getPositions(map.nodeHandles().selectedHandles<mdl::FaceHandle>());
  const auto transform = vm::translation_matrix(delta);
  if (transformFaces(map, facePositions, transform))
  {
    m_dragHandlePosition = m_dragHandlePosition.transform(transform);
    return MoveResult::Continue;
  }
  return MoveResult::Deny;
}

std::string FaceTool::actionName() const
{
  return kdl::str_plural(
    m_document.map().nodeHandles().selectedHandleCount<mdl::FaceHandle>(),
    "Move Face",
    "Move Faces");
}

void FaceTool::removeSelection()
{
  auto& map = m_document.map();

  const auto handles =
    map.nodeHandles().selectedHandles<mdl::FaceHandle>() | kdl::ranges::to<std::vector>();
  const auto vertexPositions = mdl::FaceHandle::getVertices(handles);

  const auto commandName =
    kdl::str_plural(handles.size(), "Remove Brush Face", "Remove Brush Faces");
  removeVertices(map, commandName, vertexPositions);
}

bool FaceTool::insetToolActive() const
{
  return m_insetToolActive;
}

bool FaceTool::insetFaceSelected() const
{
  return geometryHandles().selectedHandleCount() == 1u;
}

bool FaceTool::selectInsetFace(const mdl::PickResult& pickResult)
{
  if (!m_insetToolActive || m_inset)
  {
    return false;
  }

  for (const auto& hit : pickResult.all())
  {
    if (const auto face = mdl::hitToFaceHandle(hit); face && face->node()->selected())
    {
      geometryHandles().deselectAll();
      geometryHandles().select(face->face().polygon());
      refreshViews();
      notifyInsetSettingsChanged();
      return true;
    }
  }
  return false;
}

double FaceTool::insetAmount() const
{
  return m_insetAmount;
}

double FaceTool::insetAmountStep() const
{
  return m_insetSnapToGrid
           ? std::max(1.0, mdl::Grid::actualSize(m_document.map().grid().size()))
           : 1.0;
}

bool FaceTool::insetSnapToGrid() const
{
  return m_insetSnapToGrid;
}

void FaceTool::activateInsetTool()
{
  if (m_insetToolActive)
  {
    notifyInsetSettingsChanged();
    return;
  }

  if (m_inset)
  {
    cancelInset();
  }

  m_insetToolActive = true;
  m_insetAmount = 0.0;
  m_insetSnapToGrid = m_document.map().grid().snap();
  geometryHandles().deselectAll();

  refreshViews();
  notifyInsetSettingsChanged();
}

void FaceTool::cancelInsetTool()
{
  if (m_inset)
  {
    m_document.map().cancelTransaction();
    m_inset = std::nullopt;
  }

  m_insetToolActive = false;
  m_insetAmount = 0.0;
  m_insetSnapToGrid = false;
  geometryHandles().deselectAll();

  refreshViews();
  notifyInsetSettingsChanged();
}

void FaceTool::applyInsetTool()
{
  if (!m_insetToolActive)
  {
    return;
  }

  if (!m_inset && m_insetAmount > InsetEpsilon)
  {
    applyInsetSettingsPreview();
  }

  finishInset();

  auto& map = m_document.map();
  geometryHandles().deselectAll();
  mdl::deselectAll(map);

  m_insetToolActive = false;
  m_insetAmount = 0.0;
  m_insetSnapToGrid = false;

  refreshViews();
  notifyInsetSettingsChanged();
}

void FaceTool::resetInsetSettings()
{
  auto selectedFaces = geometryHandles().selectedHandles();
  if (m_inset)
  {
    selectedFaces = m_inset->originalHandles;
    m_document.map().cancelTransaction();
    m_inset = std::nullopt;
  }

  m_insetAmount = 0.0;
  m_insetSnapToGrid = m_document.map().grid().snap();
  geometryHandles().deselectAll();
  geometryHandles().select(selectedFaces);

  refreshViews();
  notifyInsetSettingsChanged();
}

void FaceTool::setInsetAmount(const double amount)
{
  m_insetAmount = std::max(0.0, amount);
  if (!applyInsetSettingsPreview())
  {
    notifyInsetSettingsChanged();
  }
}

void FaceTool::setInsetSnapToGrid(const bool snapToGrid)
{
  if (m_insetSnapToGrid == snapToGrid)
  {
    return;
  }

  m_insetSnapToGrid = snapToGrid;
  if (!applyInsetSettingsPreview())
  {
    notifyInsetSettingsChanged();
  }
}

bool FaceTool::spinToolActive() const
{
  return m_spinToolActive;
}

bool FaceTool::spinActive() const
{
  return m_spin.has_value();
}

bool FaceTool::spinFacesSelected() const
{
  return geometryHandles().selectedHandleCount() > 0u;
}

int FaceTool::spinSegments() const
{
  return m_spinSegments;
}

double FaceTool::spinFlexibility() const
{
  return m_spinFlexibility;
}

double FaceTool::spinEndScale() const
{
  return m_spinEndScale;
}

vm::vec3d FaceTool::spinEndPosition() const
{
  if (m_spin)
  {
    return m_spin->endPosition;
  }

  return m_customSpinEndPosition.value_or(vm::vec3d{0, 0, 0});
}

vm::vec3d FaceTool::spinEndRotation() const
{
  return m_spinEndRotation;
}

double FaceTool::spinHandleAxisLength() const
{
  return std::max(96.0, grid().actualSize() * 12.0);
}

double FaceTool::spinHandleRotationRadius() const
{
  return std::max(48.0, grid().actualSize() * 6.0) * 0.85;
}

double spinHandleCenterClearance(const mdl::Grid& grid)
{
  return std::max(4.0, grid.actualSize() * 0.5) * 0.75;
}

void FaceTool::activateSpinTool()
{
  if (m_spinToolActive)
  {
    notifySpinSettingsChanged();
    return;
  }

  if (m_insetToolActive)
  {
    cancelInsetTool();
  }
  else if (m_inset)
  {
    cancelInset();
  }

  m_spinToolActive = true;
  m_spinSegments = 6;
  m_spinFlexibility = 0.0;
  m_spinEndScale = 1.0;
  m_customSpinEndPosition = std::nullopt;
  m_spinEndRotation = vm::vec3d{0, 0, 0};
  m_spinUndoStack.clear();

  m_spinSelectionConnection.disconnect();
  m_spinSelectionConnection += toolHandleSelectionChangedNotifier.connect([&](auto&) {
    if (m_spinToolActive && !m_spinSelectionChangeLocked && !m_spinPreviewChangeLocked)
    {
      applySpinSettingsPreview();
    }
  });

  if (spinFacesSelected())
  {
    applySpinSettingsPreview();
  }

  refreshViews();
  notifySpinSettingsChanged();
}

void FaceTool::cancelSpinTool()
{
  m_spinSelectionConnection.disconnect();

  if (m_spin)
  {
    m_document.map().cancelTransaction();
    m_spin = std::nullopt;
  }

  m_spinToolActive = false;
  m_spinSegments = 6;
  m_spinFlexibility = 0.0;
  m_spinEndScale = 1.0;
  m_customSpinEndPosition = std::nullopt;
  m_spinEndRotation = vm::vec3d{0, 0, 0};
  m_spinUndoStack.clear();
  geometryHandles().deselectAll();

  refreshViews();
  notifySpinSettingsChanged();
  notifyToolHandleSelectionChanged();
}

void FaceTool::applySpinTool(const bool snapVerticesToInteger)
{
  if (!m_spinToolActive)
  {
    return;
  }

  if (!m_spin)
  {
    applySpinSettingsPreview();
  }

  finishSpin(snapVerticesToInteger);

  auto& map = m_document.map();
  geometryHandles().deselectAll();
  mdl::deselectAll(map);

  m_spinSelectionConnection.disconnect();
  m_spinToolActive = false;
  m_customSpinEndPosition = std::nullopt;
  m_spinEndRotation = vm::vec3d{0, 0, 0};
  m_spinUndoStack.clear();

  refreshViews();
  notifySpinSettingsChanged();
  notifyToolHandleSelectionChanged();
}

void FaceTool::resetSpinSettings()
{
  saveSpinUndoState();

  auto selectedFaces = geometryHandles().selectedHandles();
  if (m_spin)
  {
    selectedFaces = m_spin->originalHandles;
    m_document.map().cancelTransaction();
    m_spin = std::nullopt;
  }

  m_spinSegments = 6;
  m_spinFlexibility = 0.0;
  m_spinEndScale = 1.0;
  m_customSpinEndPosition = std::nullopt;
  m_spinEndRotation = vm::vec3d{0, 0, 0};

  {
    const auto selectionGuard = kdl::set_temp{m_spinSelectionChangeLocked};
    geometryHandles().deselectAll();
    geometryHandles().select(selectedFaces);
  }

  if (m_spinToolActive && !selectedFaces.empty())
  {
    applySpinSettingsPreview();
  }

  refreshViews();
  notifySpinSettingsChanged();
}

void FaceTool::saveSpinUndoState()
{
  if (!m_spinToolActive)
  {
    return;
  }

  auto state = SpinUndoState{};
  state.handles = m_spin ? m_spin->originalHandles : geometryHandles().selectedHandles();
  state.segments = m_spinSegments;
  state.flexibility = m_spinFlexibility;
  state.endScale = m_spinEndScale;
  if (m_spin || m_customSpinEndPosition)
  {
    state.endPosition = spinEndPosition();
  }
  state.endRotation = m_spinEndRotation;

  if (state.handles.empty() && !state.endPosition)
  {
    return;
  }

  if (!m_spinUndoStack.empty() && sameSpinUndoState(m_spinUndoStack.back(), state))
  {
    return;
  }

  m_spinUndoStack.push_back(std::move(state));
}

bool FaceTool::undoSpinStep()
{
  if (!m_spinToolActive || m_spinUndoStack.empty())
  {
    return false;
  }

  auto state = std::move(m_spinUndoStack.back());
  m_spinUndoStack.pop_back();

  auto& map = m_document.map();
  if (m_spin)
  {
    map.cancelTransaction();
    m_spin = std::nullopt;
  }

  m_spinSegments = state.segments;
  m_spinFlexibility = state.flexibility;
  m_spinEndScale = state.endScale;
  m_customSpinEndPosition = state.endPosition;
  m_spinEndRotation = state.endRotation;

  {
    const auto selectionGuard = kdl::set_temp{m_spinSelectionChangeLocked};
    geometryHandles().deselectAll();
    geometryHandles().select(state.handles);
  }

  if (!state.handles.empty())
  {
    applySpinSettingsPreview();
  }

  refreshViews();
  notifySpinSettingsChanged();
  notifyToolHandleSelectionChanged();
  return true;
}

void FaceTool::setSpinSegments(const int segments)
{
  const auto clampedSegments = std::clamp(segments, 1, 64);
  if (m_spinSegments == clampedSegments)
  {
    return;
  }

  saveSpinUndoState();
  m_spinSegments = clampedSegments;
  if (!applySpinSettingsPreview())
  {
    notifySpinSettingsChanged();
  }
}

void FaceTool::setSpinFlexibility(const double flexibility)
{
  const auto clampedFlexibility =
    std::clamp(flexibility, MinSpinFlexibility, MaxSpinFlexibility);
  if (std::abs(m_spinFlexibility - clampedFlexibility) <= SpinEpsilon)
  {
    return;
  }

  saveSpinUndoState();
  m_spinFlexibility = clampedFlexibility;
  if (!applySpinSettingsPreview())
  {
    notifySpinSettingsChanged();
  }
}

void FaceTool::setSpinEndScale(const double endScale)
{
  const auto clampedEndScale = std::clamp(endScale, 0.05, 8.0);
  if (std::abs(m_spinEndScale - clampedEndScale) <= SpinEpsilon)
  {
    return;
  }

  saveSpinUndoState();
  m_spinEndScale = clampedEndScale;
  if (!applySpinSettingsPreview())
  {
    notifySpinSettingsChanged();
  }
}

bool FaceTool::moveSpinEndHandleTo(const vm::vec3d& position)
{
  if (!m_spinToolActive)
  {
    return false;
  }

  if (vm::squared_length(position - spinEndPosition()) <= SpinEpsilon * SpinEpsilon)
  {
    return true;
  }

  m_customSpinEndPosition = position;
  if (m_spin)
  {
    m_spin->endPosition = position;
  }

  return applySpinSettingsPreview();
}

bool FaceTool::setSpinEndRotation(const vm::vec3d& rotation)
{
  if (!m_spinToolActive)
  {
    return false;
  }

  if (vm::squared_length(rotation - m_spinEndRotation) <= SpinEpsilon * SpinEpsilon)
  {
    return true;
  }

  m_spinEndRotation = rotation;
  return applySpinSettingsPreview();
}

void FaceTool::pickSpinEndHandle(
  const vm::ray3d& pickRay,
  const gl::Camera& camera,
  const double handleRadius,
  mdl::PickResult& pickResult) const
{
  if (!m_spinToolActive || !m_spin)
  {
    return;
  }

  const auto end = m_spin->endPosition;
  if (const auto distance = camera.pickPointHandle(pickRay, end, handleRadius * 2.4))
  {
    pickResult.addHit(
      mdl::Hit{
        SpinHandleHitType,
        *distance,
        vm::point_at_distance(pickRay, *distance),
        SpinHandleHit{SpinHandleComponent::Center, end}});
  }

  const auto axisLength = spinHandleAxisLength();
  const auto centerClearance = spinHandleCenterClearance(grid());
  for (const auto component :
       {SpinHandleComponent::XAxis,
        SpinHandleComponent::YAxis,
        SpinHandleComponent::ZAxis})
  {
    const auto axis = spinComponentAxis(component);
    const auto segment =
      vm::segment3d{end + axis * centerClearance, end + axis * axisLength};
    if (
      const auto distance =
        camera.pickLineSegmentHandle(pickRay, segment, handleRadius * 1.4))
    {
      pickResult.addHit(
        mdl::Hit{
          SpinHandleHitType,
          *distance,
          vm::point_at_distance(pickRay, *distance),
          SpinHandleHit{component, end}});
    }
  }

  const auto radius = spinHandleRotationRadius();
  constexpr auto CircleSegments = size_t{64u};
  for (const auto component :
       {SpinHandleComponent::XRotation,
        SpinHandleComponent::YRotation,
        SpinHandleComponent::ZRotation})
  {
    const auto [axisA, axisB] = spinRotationCircleAxes(component);
    for (size_t i = 0u; i < CircleSegments; ++i)
    {
      const auto a0 =
        vm::Cd::two_pi() * static_cast<double>(i) / static_cast<double>(CircleSegments);
      const auto a1 = vm::Cd::two_pi() * static_cast<double>(i + 1u)
                      / static_cast<double>(CircleSegments);
      const auto p0 = end + (axisA * std::cos(a0) + axisB * std::sin(a0)) * radius;
      const auto p1 = end + (axisA * std::cos(a1) + axisB * std::sin(a1)) * radius;

      if (
        const auto distance = camera.pickLineSegmentHandle(
          pickRay, vm::segment3d{p0, p1}, handleRadius * 1.4))
      {
        pickResult.addHit(
          mdl::Hit{
            SpinHandleHitType,
            *distance,
            vm::point_at_distance(pickRay, *distance),
            SpinHandleHit{component, end}});
      }
    }
  }
}

void FaceTool::renderSpinTool(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult) const
{
  if (!m_spinToolActive || !m_spin)
  {
    return;
  }

  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setShowOccludedObjects();
  renderService.setLineWidth(2.0f);

  const auto center = m_spin->baseCenter;
  const auto end = m_spin->endPosition;
  const auto size = std::max(4.0, grid().actualSize() * 0.5);
  const auto half = vm::vec3d::fill(size * 0.5);
  const auto bounds = vm::bbox3d{end - half, end + half};

  renderService.setForegroundColor(RgbaF{1.0f, 0.1f, 0.75f, 1.0f});
  renderService.renderLine(vm::vec3f{center}, vm::vec3f{end});
  renderService.renderBounds(vm::bbox3f{bounds});
  renderService.renderHandle(vm::vec3f{end});

  using namespace mdl::HitFilters;
  auto highlighted = std::optional<SpinHandleComponent>{};
  if (const auto& hit = pickResult.first(type(SpinHandleHitType)); hit.isMatch())
  {
    highlighted = hit.target<SpinHandleHit>().component;
  }

  if (highlighted && *highlighted == SpinHandleComponent::Center)
  {
    renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
    renderService.renderHandleHighlight(vm::vec3f{end});
  }

  const auto axisLength = spinHandleAxisLength();
  const auto centerClearance = spinHandleCenterClearance(grid());
  for (const auto component :
       {SpinHandleComponent::XAxis,
        SpinHandleComponent::YAxis,
        SpinHandleComponent::ZAxis})
  {
    const auto axis = spinComponentAxis(component);
    renderService.setLineWidth(highlighted && *highlighted == component ? 6.0f : 4.0f);
    renderService.setForegroundColor(spinComponentColor(component));
    renderService.renderLine(
      vm::vec3f{end + axis * centerClearance}, vm::vec3f{end + axis * axisLength});
    renderService.renderHandle(vm::vec3f{end + axis * axisLength});
  }

  const auto radius = static_cast<float>(spinHandleRotationRadius());
  for (const auto component :
       {SpinHandleComponent::XRotation,
        SpinHandleComponent::YRotation,
        SpinHandleComponent::ZRotation})
  {
    const auto axis = spinComponentAxis(component);
    const auto [axisA, axisB] = spinRotationCircleAxes(component);
    renderService.setLineWidth(highlighted && *highlighted == component ? 4.0f : 2.0f);
    renderService.setForegroundColor(spinComponentColor(component));
    renderService.renderCircle(
      vm::vec3f{end},
      vm::find_abs_max_component(axis),
      64,
      radius,
      vm::vec3f{axisA},
      vm::vec3f{axisB});
  }
}

bool FaceTool::beginSpin()
{
  return beginSpin(geometryHandles().selectedHandles());
}

bool FaceTool::beginSpin(const std::vector<vm::polygon3d>& handles)
{
  if (handles.empty())
  {
    return false;
  }

  auto& map = m_document.map();
  if (m_spin)
  {
    if (sameFaceHandles(handles, m_spin->originalHandles))
    {
      return true;
    }

    map.cancelTransaction();
    m_spin = std::nullopt;
  }

  auto state = SpinState{};
  state.originalHandles = handles;

  auto faceCount = size_t{0u};
  auto normalSum = vm::vec3d{0, 0, 0};

  for (const auto& handle : handles)
  {
    const auto incidentBrushes = findIncidentBrushes(handle);
    if (incidentBrushes.size() != 1u)
    {
      return false;
    }

    auto* brushNode = incidentBrushes.front();
    const auto faceIndex = brushNode->brush().findFace(handle, InsetEpsilon);
    if (!faceIndex)
    {
      return false;
    }

    auto brushIt = std::ranges::find_if(
      state.brushes, [&](const auto& brush) { return brush.node == brushNode; });
    if (brushIt == std::end(state.brushes))
    {
      state.brushes.push_back(SpinBrush{brushNode, brushNode->brush(), {}});
      brushIt = std::prev(std::end(state.brushes));
    }

    const auto alreadyAdded = std::ranges::any_of(
      brushIt->faces, [&](const auto& face) { return face.faceIndex == *faceIndex; });
    if (alreadyAdded)
    {
      continue;
    }

    const auto& sourceFace = brushNode->brush().face(*faceIndex);
    const auto polygon = sourceFace.polygon();
    if (polygon.vertices().size() < 3u)
    {
      return false;
    }

    const auto normal = normalizedOr(sourceFace.normal(), vm::vec3d{0, 0, 1});
    const auto center = polygon.center();
    brushIt->faces.push_back(SpinFace{*faceIndex, polygon, normal, center});
    state.baseCenter = state.baseCenter + center;
    normalSum = normalSum + normal;
    ++faceCount;
  }

  if (faceCount == 0u)
  {
    return false;
  }

  state.baseCenter = state.baseCenter / static_cast<double>(faceCount);
  state.defaultNormal = normalizedOr(normalSum, vm::vec3d{0, 0, 1});
  state.endPosition = m_customSpinEndPosition.value_or(defaultSpinEndPosition(state));

  map.startTransaction("Spin Faces", mdl::TransactionScope::LongRunning);
  m_spin = std::move(state);
  return true;
}

bool FaceTool::applySpinSettingsPreview()
{
  if (!m_spinToolActive)
  {
    return false;
  }

  if (m_spinPreviewChangeLocked)
  {
    return true;
  }

  const auto previewGuard = kdl::set_temp{m_spinPreviewChangeLocked};

  auto& map = m_document.map();
  const auto selectedHandles = geometryHandles().selectedHandles();
  if (
    m_spin && !selectedHandles.empty()
    && !sameFaceHandles(selectedHandles, m_spin->originalHandles))
  {
    saveSpinUndoState();
    map.cancelTransaction();
    m_spin = std::nullopt;
    m_spinSegments = 6;
    m_spinFlexibility = 0.0;
    m_spinEndScale = 1.0;
    m_customSpinEndPosition = std::nullopt;
    m_spinEndRotation = vm::vec3d{0, 0, 0};
  }

  if (!m_spin && !beginSpin(selectedHandles))
  {
    return false;
  }

  if (!m_spin)
  {
    return false;
  }

  updateSpinStateSettings();

  m_spin->previewBrushNodes.clear();
  map.rollbackTransaction();

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};

  for (const auto& spinBrush : m_spin->brushes)
  {
    auto* parent = spinBrush.node->parent();
    if (!parent)
    {
      kdl::map_clear_and_delete(nodesToAdd);
      return false;
    }

    auto& parentNodes = nodesToAdd[parent];
    for (const auto& face : spinBrush.faces)
    {
      auto segmentBrushes = createSpinBrushes(
        map,
        spinBrush.originalBrush,
        face,
        m_spin->baseCenter,
        m_spin->defaultNormal,
        m_spin->endPosition,
        m_spinSegments,
        m_spinFlexibility,
        m_spinEndScale,
        m_spinEndRotation);
      if (segmentBrushes.empty())
      {
        kdl::map_clear_and_delete(nodesToAdd);
        return false;
      }

      for (auto& brush : segmentBrushes)
      {
        parentNodes.push_back(new mdl::BrushNode{std::move(brush)});
      }
    }
  }

  const auto addedNodes = mdl::addNodes(map, nodesToAdd);
  if (addedNodes.empty())
  {
    return false;
  }

  for (auto* node : addedNodes)
  {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
    {
      m_spin->previewBrushNodes.push_back(brushNode);
    }
  }

  {
    const auto selectionGuard = kdl::set_temp{m_spinSelectionChangeLocked};
    geometryHandles().deselectAll();
    geometryHandles().select(m_spin->originalHandles);
  }

  refreshViews();
  notifySpinSettingsChanged();
  return true;
}

void FaceTool::finishSpin(const bool snapVerticesToInteger)
{
  if (!m_spin)
  {
    return;
  }

  if (snapVerticesToInteger)
  {
    snapBrushNodesToInteger(m_document.map(), m_spin->previewBrushNodes);
  }

  m_document.map().commitTransaction();
  m_spin = std::nullopt;
}

void FaceTool::cancelSpin()
{
  if (!m_spin)
  {
    return;
  }

  m_document.map().cancelTransaction();
  m_spin = std::nullopt;
  m_spinUndoStack.clear();
  refreshViews();
}

void FaceTool::updateSpinStateSettings()
{
  if (!m_spin)
  {
    return;
  }

  m_spin->endPosition = m_customSpinEndPosition.value_or(defaultSpinEndPosition(*m_spin));
}

void FaceTool::notifySpinSettingsChanged()
{
  spinSettingsDidChangeNotifier(*this);
}

vm::vec3d FaceTool::defaultSpinEndPosition(const SpinState& state) const
{
  const auto step = std::max(1.0, grid().actualSize());
  const auto distance = std::max(step, step * static_cast<double>(m_spinSegments));
  return state.baseCenter + state.defaultNormal * distance;
}

bool FaceTool::insetActive() const
{
  return m_inset.has_value();
}

bool FaceTool::beginInset()
{
  if (m_inset)
  {
    return true;
  }

  auto& map = m_document.map();
  const auto handles = geometryHandles().selectedHandles();
  if (handles.size() != 1u)
  {
    return false;
  }

  auto state = InsetState{};
  state.originalHandles = handles;
  state.maxAmount = std::numeric_limits<double>::max();
  state.step = std::max(1.0, grid().actualSize());

  for (const auto& handle : handles)
  {
    const auto incidentBrushes = findIncidentBrushes(handle);
    if (incidentBrushes.size() != 1u)
    {
      return false;
    }

    auto* brushNode = incidentBrushes.front();
    const auto faceIndex = brushNode->brush().findFace(handle, InsetEpsilon);
    if (!faceIndex)
    {
      return false;
    }

    const auto currentMaxAmount =
      maxInsetAmount(brushNode->brush().face(*faceIndex).polygon());
    if (!currentMaxAmount)
    {
      return false;
    }

    auto brushIt = std::ranges::find_if(
      state.brushes, [&](const auto& brush) { return brush.node == brushNode; });
    if (brushIt == std::end(state.brushes))
    {
      state.brushes.push_back(InsetBrush{brushNode, brushNode->brush(), {}});
      brushIt = std::prev(std::end(state.brushes));
    }

    const auto alreadyAdded = std::ranges::any_of(
      brushIt->faces, [&](const auto& face) { return face.faceIndex == *faceIndex; });
    if (!alreadyAdded && !brushIt->faces.empty())
    {
      return false;
    }

    if (!alreadyAdded)
    {
      brushIt->faces.push_back(InsetFace{*faceIndex, *currentMaxAmount});
      state.maxAmount = std::min(state.maxAmount, *currentMaxAmount);
    }
  }

  if (state.brushes.empty() || state.maxAmount <= InsetEpsilon)
  {
    return false;
  }

  state.step = std::min(state.step, state.maxAmount);
  map.startTransaction("Inset Face", mdl::TransactionScope::LongRunning);
  m_inset = std::move(state);
  return true;
}

bool FaceTool::applyInsetPreview(const double targetAmount)
{
  if (!m_inset)
  {
    return false;
  }

  auto& map = m_document.map();
  const auto amount = std::clamp(targetAmount, 0.0, m_inset->maxAmount);
  if (amount <= InsetEpsilon)
  {
    map.rollbackTransaction();
    geometryHandles().deselectAll();
    geometryHandles().select(m_inset->originalHandles);
    m_inset->amount = 0.0;
    refreshViews();
    return true;
  }

  map.rollbackTransaction();

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto originalNodes = std::vector<mdl::Node*>{};
  originalNodes.reserve(m_inset->brushes.size());

  for (const auto& insetBrush : m_inset->brushes)
  {
    auto insetBrushNodes = std::vector<mdl::BrushNode*>{};
    for (const auto& insetFace : insetBrush.faces)
    {
      auto insetBrushes = createInsetBrushes(
        map,
        insetBrush.originalBrush,
        insetFace.faceIndex,
        amount,
        insetFace.maxAmount,
        m_insetSnapToGrid);
      if (insetBrushes.empty())
      {
        kdl::map_clear_and_delete(nodesToAdd);
        return false;
      }

      for (auto& brush : insetBrushes)
      {
        insetBrushNodes.push_back(new mdl::BrushNode{std::move(brush)});
      }
    }

    if (insetBrushNodes.empty())
    {
      kdl::map_clear_and_delete(nodesToAdd);
      return false;
    }

    auto* parent = insetBrush.node->parent();
    if (!parent)
    {
      kdl::map_clear_and_delete(nodesToAdd);
      return false;
    }

    auto& parentNodes = nodesToAdd[parent];
    for (auto* node : insetBrushNodes)
    {
      parentNodes.push_back(node);
    }
    originalNodes.push_back(insetBrush.node);
  }

  mdl::deselectAll(map);
  const auto addedNodes = mdl::addNodes(map, nodesToAdd);
  if (addedNodes.empty())
  {
    return false;
  }
  mdl::removeNodes(map, originalNodes);
  mdl::selectNodes(map, addedNodes);

  m_inset->amount = amount;
  geometryHandles().deselectAll();
  refreshViews();
  return true;
}

bool FaceTool::adjustInsetAmount(const int delta)
{
  if (delta == 0 || (!m_inset && delta < 0) || !beginInset())
  {
    return false;
  }

  const auto targetAmount =
    std::clamp(m_inset->amount + double(delta) * m_inset->step, 0.0, m_inset->maxAmount);
  if (std::abs(targetAmount - m_inset->amount) <= InsetEpsilon)
  {
    return false;
  }

  const auto didApply = applyInsetPreview(targetAmount);
  if (didApply)
  {
    updateInsetSettingsFromState();
  }
  return didApply;
}

void FaceTool::finishInset()
{
  if (!m_inset)
  {
    return;
  }

  if (m_inset->amount <= InsetEpsilon)
  {
    m_document.map().cancelTransaction();
  }
  else
  {
    m_document.map().commitTransaction();
  }
  m_inset = std::nullopt;
}

void FaceTool::cancelInset()
{
  if (!m_inset)
  {
    return;
  }

  m_document.map().cancelTransaction();
  m_inset = std::nullopt;
  refreshViews();
}

bool FaceTool::applyInsetSettingsPreview()
{
  if (!m_insetToolActive)
  {
    return false;
  }

  if (m_insetAmount <= InsetEpsilon)
  {
    if (!m_inset)
    {
      return false;
    }

    const auto originalHandles = m_inset->originalHandles;
    m_document.map().cancelTransaction();
    m_inset = std::nullopt;
    geometryHandles().deselectAll();
    geometryHandles().select(originalHandles);
    refreshViews();
    notifyInsetSettingsChanged();
    return true;
  }

  if (!m_inset && !beginInset())
  {
    return false;
  }

  if (!m_inset)
  {
    return false;
  }

  const auto didApply = applyInsetPreview(m_insetAmount);
  if (didApply)
  {
    updateInsetSettingsFromState();
  }
  return didApply;
}

void FaceTool::updateInsetSettingsFromState()
{
  if (m_inset)
  {
    m_insetAmount = m_inset->amount;
  }

  notifyInsetSettingsChanged();
}

void FaceTool::notifyInsetSettingsChanged()
{
  insetSettingsDidChangeNotifier(*this);
  notifyToolHandleSelectionChanged();
}

bool FaceTool::doDeactivate()
{
  if (m_inset)
  {
    m_document.map().cancelTransaction();
    m_inset = std::nullopt;
  }

  m_insetToolActive = false;
  m_insetAmount = 0.0;
  m_insetSnapToGrid = false;

  if (m_spin)
  {
    m_document.map().cancelTransaction();
    m_spin = std::nullopt;
  }

  m_spinSelectionConnection.disconnect();
  m_spinToolActive = false;
  m_spinSegments = 6;
  m_spinFlexibility = 0.0;
  m_spinEndScale = 1.0;
  m_customSpinEndPosition = std::nullopt;
  m_spinEndRotation = vm::vec3d{0, 0, 0};
  m_spinUndoStack.clear();

  return BrushHandleToolBase<mdl::FaceHandle>::doDeactivate();
}

QWidget* FaceTool::doCreatePage(QWidget* parent)
{
  return new FaceToolPage{*this, parent};
}

} // namespace tb::ui
