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

#include "ui/SculptTool.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>

#include "Logger.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/overload.h"
#include "kd/result.h"

#include "vm/scalar.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto MinBrushRadius = 1.0;
constexpr auto MaxBrushRadius = 4096.0;
constexpr auto MinBrushIntensity = 0.1;
constexpr auto MaxBrushIntensity = 1024.0;
constexpr auto VertexEpsilon = 0.001;
constexpr auto HullEpsilon = 0.01;
constexpr auto MinSurfaceDepth = 1.0;
constexpr auto SurfaceDepthGridFactor = 0.25;
constexpr auto MinSmoothSurfaceDepth = 8.0;
constexpr auto SmoothSurfaceDepthRadiusFactor = 0.35;
constexpr auto SmoothIntensityScale = 8.0;

struct SmoothSample
{
  vm::vec3d position;
  vm::vec3d planarPosition;
  double normalDistance = 0.0;
};

std::vector<mdl::BrushNode*> collectEditableBrushes(mdl::Map& map)
{
  auto result = std::vector<mdl::BrushNode*>{};
  const auto& editorContext = map.editorContext();

  map.worldNode().visitChildren(
    kdl::overload(
      [](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::GroupNode& groupNode) {
        groupNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, mdl::EntityNode& entityNode) {
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) {
        if (editorContext.visible(brushNode) && editorContext.editable(brushNode))
        {
          result.push_back(&brushNode);
        }
      },
      [](mdl::PatchNode&) {}));

  return result;
}

std::optional<std::tuple<vm::vec3d, vm::vec3d>> planeBasis(const vm::vec3d& normal)
{
  const auto helper =
    std::abs(normal.z()) < 0.75 ? vm::vec3d{0, 0, 1} : vm::vec3d{1, 0, 0};
  const auto u = vm::cross(helper, normal);
  if (vm::is_zero(vm::squared_length(u), vm::Cd::almost_zero()))
  {
    return std::nullopt;
  }

  const auto axisU = vm::normalize(u);
  const auto axisV = vm::normalize(vm::cross(normal, axisU));
  return std::tuple{axisU, axisV};
}

double smoothFalloff(const double distance, const double radius)
{
  const auto t = std::clamp(distance / radius, 0.0, 1.0);
  const auto inv = 1.0 - t;
  return inv * inv * (3.0 - 2.0 * inv);
}

std::vector<vm::vec3d> uniquePoints(const std::vector<vm::vec3d>& points)
{
  auto result = std::vector<vm::vec3d>{};
  result.reserve(points.size());

  for (const auto& point : points)
  {
    if (std::ranges::none_of(result, [&](const auto& other) {
          return vm::squared_length(point - other) < HullEpsilon * HullEpsilon;
        }))
    {
      result.push_back(point);
    }
  }

  return result;
}

std::optional<std::vector<vm::vec3d>> stableHullInput(
  const std::vector<vm::vec3d>& points)
{
  auto unique = uniquePoints(points);
  if (unique.size() < 4u)
  {
    return std::nullopt;
  }

  auto first = size_t{0u};
  auto second = size_t{0u};
  auto maxLineLength = 0.0;
  for (size_t i = 0u; i < unique.size(); ++i)
  {
    for (size_t j = i + 1u; j < unique.size(); ++j)
    {
      const auto length = vm::squared_length(unique[i] - unique[j]);
      if (length > maxLineLength)
      {
        first = i;
        second = j;
        maxLineLength = length;
      }
    }
  }
  if (maxLineLength < HullEpsilon * HullEpsilon)
  {
    return std::nullopt;
  }

  auto third = size_t{0u};
  auto maxLineDistance = 0.0;
  const auto line = unique[second] - unique[first];
  for (size_t i = 0u; i < unique.size(); ++i)
  {
    if (i == first || i == second)
    {
      continue;
    }

    const auto crossLength =
      vm::squared_length(vm::cross(unique[i] - unique[first], line));
    if (crossLength > maxLineDistance)
    {
      third = i;
      maxLineDistance = crossLength;
    }
  }
  if (maxLineDistance < HullEpsilon * HullEpsilon * maxLineLength)
  {
    return std::nullopt;
  }

  const auto normal = vm::normalize(
    vm::cross(unique[second] - unique[first], unique[third] - unique[first]));
  auto fourth = size_t{0u};
  auto maxPlaneDistance = 0.0;
  for (size_t i = 0u; i < unique.size(); ++i)
  {
    if (i == first || i == second || i == third)
    {
      continue;
    }

    const auto distance = std::abs(vm::dot(unique[i] - unique[first], normal));
    if (distance > maxPlaneDistance)
    {
      fourth = i;
      maxPlaneDistance = distance;
    }
  }
  if (maxPlaneDistance < HullEpsilon)
  {
    return std::nullopt;
  }

  auto stable = std::vector<vm::vec3d>{};
  stable.reserve(unique.size());
  stable.push_back(unique[first]);
  stable.push_back(unique[second]);
  stable.push_back(unique[third]);
  stable.push_back(unique[fourth]);

  for (size_t i = 0u; i < unique.size(); ++i)
  {
    if (i != first && i != second && i != third && i != fourth)
    {
      stable.push_back(unique[i]);
    }
  }

  return stable;
}

std::optional<vm::vec3d> sculptedPosition(
  const vm::vec3d& position,
  const vm::vec3d& center,
  const vm::vec3d& normal,
  const double radius,
  const double intensity,
  const double surfaceDepth,
  const double sign)
{
  const auto delta = position - center;
  const auto normalDistance = vm::dot(delta, normal);
  if (normalDistance < -VertexEpsilon || normalDistance > surfaceDepth)
  {
    return std::nullopt;
  }

  const auto planarDelta = delta - normal * normalDistance;
  const auto distance = vm::length(planarDelta);
  if (distance > radius)
  {
    return std::nullopt;
  }

  const auto depthFalloff =
    1.0 - std::clamp(std::max(0.0, normalDistance) / surfaceDepth, 0.0, 1.0);
  return position
         + normal * (sign * intensity * smoothFalloff(distance, radius) * depthFalloff);
}

std::vector<SmoothSample> collectSmoothSamples(
  const std::vector<mdl::BrushNode*>& brushNodes,
  const vm::vec3d& center,
  const vm::vec3d& normal,
  const double radius,
  const double surfaceDepth)
{
  auto samples = std::vector<SmoothSample>{};

  for (auto* brushNode : brushNodes)
  {
    for (const auto* vertex : brushNode->brush().vertices())
    {
      const auto& position = vertex->position();
      const auto delta = position - center;
      const auto normalDistance = vm::dot(delta, normal);
      if (std::abs(normalDistance) > surfaceDepth)
      {
        continue;
      }

      const auto planarPosition = position - normal * normalDistance;
      if (vm::length(planarPosition - center) <= radius)
      {
        samples.push_back({position, planarPosition, normalDistance});
      }
    }
  }

  return samples;
}

std::optional<vm::vec3d> smoothedPosition(
  const vm::vec3d& position,
  const vm::vec3d& center,
  const vm::vec3d& normal,
  const double radius,
  const double intensity,
  const double surfaceDepth,
  const std::vector<SmoothSample>& samples)
{
  const auto delta = position - center;
  const auto normalDistance = vm::dot(delta, normal);
  if (std::abs(normalDistance) > surfaceDepth)
  {
    return std::nullopt;
  }

  const auto planarPosition = position - normal * normalDistance;
  const auto centerDistance = vm::length(planarPosition - center);
  if (centerDistance > radius)
  {
    return std::nullopt;
  }

  auto weightSum = 0.0;
  auto averageDistance = 0.0;
  for (const auto& sample : samples)
  {
    const auto sampleDistance = vm::length(sample.planarPosition - planarPosition);
    if (sampleDistance > radius)
    {
      continue;
    }

    const auto weight = smoothFalloff(sampleDistance, radius);
    weightSum += weight;
    averageDistance += sample.normalDistance * weight;
  }

  if (weightSum <= 0.0)
  {
    return std::nullopt;
  }

  averageDistance /= weightSum;
  const auto strength = std::clamp(intensity / SmoothIntensityScale, 0.0, 1.0);
  const auto displacement =
    (averageDistance - normalDistance) * strength * smoothFalloff(centerDistance, radius);

  if (std::abs(displacement) < HullEpsilon)
  {
    return std::nullopt;
  }

  return position + normal * displacement;
}

std::vector<vm::vec3f> makeBrushCircle(
  const vm::vec3d& center, const vm::vec3d& normal, const double radius)
{
  auto basis = planeBasis(normal);
  if (!basis)
  {
    return {};
  }

  const auto& [u, v] = *basis;
  constexpr auto Segments = size_t{64};
  auto points = std::vector<vm::vec3f>{};
  points.reserve(Segments + 1u);
  for (size_t i = 0; i <= Segments; ++i)
  {
    const auto angle =
      (static_cast<double>(i) / static_cast<double>(Segments)) * vm::Cd::two_pi();
    points.push_back(
      vm::vec3f{center + (u * std::cos(angle) + v * std::sin(angle)) * radius});
  }
  return points;
}

class SculptToolPage : public QWidget
{
private:
  SculptTool& m_tool;
  QComboBox* m_mode = nullptr;
  QDoubleSpinBox* m_radius = nullptr;
  QDoubleSpinBox* m_intensity = nullptr;
  QCheckBox* m_preventCollapse = nullptr;

public:
  explicit SculptToolPage(SculptTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  void createGui()
  {
    m_mode = new QComboBox{};
    m_mode->addItem(tr("Raise"));
    m_mode->addItem(tr("Lower"));
    m_mode->addItem(tr("Smooth"));

    m_radius = new QDoubleSpinBox{};
    m_radius->setRange(MinBrushRadius, MaxBrushRadius);
    m_radius->setDecimals(1);
    m_radius->setSingleStep(8.0);

    m_intensity = new QDoubleSpinBox{};
    m_intensity->setRange(MinBrushIntensity, MaxBrushIntensity);
    m_intensity->setDecimals(1);
    m_intensity->setSingleStep(1.0);
    m_preventCollapse = new QCheckBox{tr("Prevent collapse")};
    m_preventCollapse->setChecked(m_tool.preventCollapse());
    m_preventCollapse->setToolTip(
      tr("Keep the back surface intact and preserve at least one unit of thickness"));
    connect(m_preventCollapse, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setPreventCollapse(checked);
    });

    connect(
      m_mode,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int index) {
        m_tool.setMode(
          index == 0   ? SculptTool::Mode::Raise
          : index == 1 ? SculptTool::Mode::Lower
                       : SculptTool::Mode::Smooth);
      });
    connect(
      m_radius,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setRadius(value); });
    connect(
      m_intensity,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setIntensity(value); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(new QLabel{tr("Mode")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_mode, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Width")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_radius, 0, Qt::AlignVCenter);
    layout->addWidget(new QLabel{tr("Intensity")}, 0, Qt::AlignVCenter);
    layout->addWidget(m_intensity, 0, Qt::AlignVCenter);
    layout->addWidget(m_preventCollapse, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    setLayout(layout);
    updateControls();
  }

  void updateControls()
  {
    {
      const auto blocker = QSignalBlocker{m_mode};
      m_mode->setCurrentIndex(
        m_tool.mode() == SculptTool::Mode::Raise   ? 0
        : m_tool.mode() == SculptTool::Mode::Lower ? 1
                                                   : 2);
    }
    {
      const auto blocker = QSignalBlocker{m_radius};
      m_radius->setValue(m_tool.radius());
    }
    {
      const auto blocker = QSignalBlocker{m_intensity};
      m_intensity->setValue(m_tool.intensity());
    }
  }
};

} // namespace

SculptTool::SculptTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

SculptTool::~SculptTool() = default;

SculptTool::Mode SculptTool::mode() const
{
  return m_mode;
}

void SculptTool::setMode(const Mode mode)
{
  m_mode = mode;
}

double SculptTool::radius() const
{
  return m_radius;
}

void SculptTool::setRadius(const double radius)
{
  m_radius = std::clamp(radius, MinBrushRadius, MaxBrushRadius);
}

double SculptTool::intensity() const
{
  return m_intensity;
}

void SculptTool::setIntensity(const double intensity)
{
  m_intensity = std::clamp(intensity, MinBrushIntensity, MaxBrushIntensity);
}

bool SculptTool::preventCollapse() const
{
  return m_preventCollapse;
}

void SculptTool::setPreventCollapse(const bool preventCollapse)
{
  m_preventCollapse = preventCollapse;
}

bool SculptTool::hasBrush() const
{
  return m_brush.has_value();
}

void SculptTool::updateBrush(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(mdl::BrushNode::BrushHitType));
  if (const auto faceHandle = mdl::hitToFaceHandle(hit))
  {
    m_brush = Brush{faceHandle->node(), hit.hitPoint(), faceHandle->face().boundary()};
  }
  else
  {
    m_brush.reset();
  }
}

void SculptTool::clearBrush()
{
  m_brush.reset();
}

void SculptTool::beginStroke()
{
  if (m_transactionOpen)
  {
    return;
  }

  m_document.map().startTransaction("Sculpt Brushes", mdl::TransactionScope::LongRunning);
  m_transactionOpen = true;
  m_transactionChanged = false;
}

bool SculptTool::applyBrush()
{
  if (!m_brush)
  {
    return false;
  }

  const auto changed = sculptAt(m_brush->position, m_brush->plane);
  m_transactionChanged = m_transactionChanged || changed;
  return changed;
}

void SculptTool::endStroke()
{
  if (!m_transactionOpen)
  {
    return;
  }

  if (m_transactionChanged)
  {
    m_document.map().commitTransaction();
  }
  else
  {
    m_document.map().cancelTransaction();
  }
  m_transactionOpen = false;
  m_transactionChanged = false;
}

void SculptTool::cancelStroke()
{
  if (m_transactionOpen)
  {
    m_document.map().cancelTransaction();
    m_transactionOpen = false;
    m_transactionChanged = false;
  }
}

void SculptTool::render(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch) const
{
  if (!m_brush)
  {
    return;
  }

  const auto normal = vm::normalize(m_brush->plane.normal);
  auto circle = makeBrushCircle(m_brush->position, normal, m_radius);
  if (circle.empty())
  {
    return;
  }

  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setShowOccludedObjects();
  const auto color = m_mode == Mode::Raise   ? RgbaF{0.35f, 1.0f, 0.55f, 1.0f}
                     : m_mode == Mode::Lower ? RgbaF{1.0f, 0.35f, 0.35f, 1.0f}
                                             : RgbaF{0.35f, 0.65f, 1.0f, 1.0f};
  renderService.setForegroundColor(color);
  renderService.renderLineStrip(circle);
  renderService.renderLine(
    vm::vec3f{m_brush->position},
    vm::vec3f{m_brush->position + normal * std::max(8.0, m_intensity)});
}

bool SculptTool::sculptAt(const vm::vec3d& center, const vm::plane3d& plane)
{
  auto& map = m_document.map();
  auto nodesToUpdate = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  const auto normal = vm::normalize(plane.normal);
  const auto sign = m_mode == Mode::Raise ? 1.0 : -1.0;
  const auto surfaceDepth =
    std::max(MinSurfaceDepth, map.grid().actualSize() * SurfaceDepthGridFactor);
  const auto smoothSurfaceDepth = std::max(
    {surfaceDepth,
     MinSmoothSurfaceDepth,
     map.grid().actualSize(),
     m_radius * SmoothSurfaceDepthRadiusFactor});
  const auto brushNodes = collectEditableBrushes(map);
  const auto smoothSamples =
    m_mode == Mode::Smooth
      ? collectSmoothSamples(brushNodes, center, normal, m_radius, smoothSurfaceDepth)
      : std::vector<SmoothSample>{};
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  // Resolve shared vertices once, using the tightest thickness limit of all neighbours.
  using VertexKey = std::array<long long, 3>;
  const auto vertexKey = [](const vm::vec3d& p) {
    return VertexKey{
      std::llround(p.x() / VertexEpsilon),
      std::llround(p.y() / VertexEpsilon),
      std::llround(p.z() / VertexEpsilon)};
  };
  auto displacements = std::map<VertexKey, double>{};
  auto thicknessLimits = std::map<VertexKey, double>{};
  if (m_preventCollapse)
  {
    for (const auto* brushNode : brushNodes)
    {
      const auto& brush = brushNode->brush();
      for (const auto* vertex : brush.vertices())
      {
        const auto& position = vertex->position();
        const auto delta = position - center;
        const auto normalDistance = vm::dot(delta, normal);
        if (
          std::abs(normalDistance) > smoothSurfaceDepth
          || vm::length(delta - normal * normalDistance) > m_radius)
        {
          continue;
        }
        const auto key = vertexKey(position);
        const auto onFrontSurface =
          std::ranges::any_of(brush.faces(), [&](const auto& face) {
            return vm::dot(face.normal(), normal) > VertexEpsilon
                   && std::abs(
                        vm::dot(face.normal(), position) - face.boundary().distance)
                        <= VertexEpsilon;
          });
        if (!onFrontSurface)
        {
          // A shared vertex on another brush's back surface must stay fixed in both.
          displacements.emplace(key, 0.0);
        }
        auto limit = std::numeric_limits<double>::max();
        for (const auto& face : brush.faces())
        {
          const auto opposing = -vm::dot(face.boundary().normal, normal);
          if (opposing > VertexEpsilon)
          {
            const auto depth =
              (face.boundary().distance - vm::dot(face.boundary().normal, position))
              / opposing;
            if (depth > VertexEpsilon)
            {
              limit = std::min(limit, std::max(0.0, depth - 1.0));
            }
          }
        }
        const auto [it, inserted] = thicknessLimits.emplace(key, limit);
        if (!inserted)
        {
          it->second = std::min(it->second, limit);
        }
      }
    }
  }

  for (auto* brushNode : brushNodes)
  {
    const auto& sourceBrush = brushNode->brush();
    auto points = std::vector<vm::vec3d>{};
    points.reserve(sourceBrush.vertexCount());

    auto changed = false;
    for (const auto* vertex : sourceBrush.vertices())
    {
      const auto& position = vertex->position();
      auto sculpted =
        m_mode == Mode::Smooth
          ? smoothedPosition(
              position,
              center,
              normal,
              m_radius,
              m_intensity,
              smoothSurfaceDepth,
              smoothSamples)
          : sculptedPosition(
              position, center, normal, m_radius, m_intensity, surfaceDepth, sign);
      if (sculpted && m_preventCollapse)
      {
        const auto onFrontSurface =
          std::ranges::any_of(sourceBrush.faces(), [&](const auto& face) {
            return vm::dot(face.boundary().normal, normal) > VertexEpsilon
                   && std::abs(
                        vm::dot(face.boundary().normal, position)
                        - face.boundary().distance)
                        <= VertexEpsilon;
          });
        if (!onFrontSurface)
        {
          sculpted.reset();
        }
        else
        {
          const auto key = vertexKey(position);
          auto displacement = vm::dot(*sculpted - position, normal);
          displacement = std::max(displacement, -thicknessLimits.at(key));
          const auto [it, inserted] = displacements.emplace(key, displacement);
          (void)inserted;
          sculpted = position + normal * it->second;
        }
      }
      if (sculpted)
      {
        points.push_back(*sculpted);
        changed =
          changed
          || vm::squared_length(*sculpted - position) > VertexEpsilon * VertexEpsilon;
      }
      else
      {
        points.push_back(position);
      }
    }

    if (!changed)
    {
      continue;
    }

    const auto stablePoints = stableHullInput(points);
    if (!stablePoints)
    {
      if (m_preventCollapse)
      {
        return false;
      }
      continue;
    }

    const auto materialName = sourceBrush.faceCount() > 0u
                                ? sourceBrush.face(0).attributes().materialName()
                                : map.currentMaterialName();
    auto result = builder.createBrush(*stablePoints, materialName);
    if (!result)
    {
      if (m_preventCollapse)
      {
        return false;
      }
      result | kdl::if_error([&](const auto& e) {
        map.logger().error() << "Could not sculpt brush: " << e.msg;
      }) | kdl::ignore();
      continue;
    }

    auto brush = std::move(result).value();
    if (m_preventCollapse && brush.vertexCount() != sourceBrush.vertexCount())
    {
      return false;
    }
    brush.cloneFaceAttributesFrom(sourceBrush);
    nodesToUpdate.emplace_back(brushNode, mdl::NodeContents{std::move(brush)});
  }

  if (nodesToUpdate.empty())
  {
    return false;
  }

  return mdl::updateNodeContents(map, "Sculpt Brushes", std::move(nodesToUpdate));
}

bool SculptTool::doActivate()
{
  mdl::deselectAll(m_document.map());
  return true;
}

bool SculptTool::doDeactivate()
{
  cancelStroke();
  clearBrush();
  return true;
}

QWidget* SculptTool::doCreatePage(QWidget* parent)
{
  return new SculptToolPage{*this, parent};
}

} // namespace tb::ui
