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

#include "ui/PolygonCutTool.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>

#include "Logger.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Camera.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Hit.h"
#include "mdl/HitFilter.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/SelectionChange.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/set_temp.h"

#include "vm/abstract_line.h"
#include "vm/scalar.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>

namespace tb::ui
{

const mdl::HitType::Type PolygonCutTool::PointHitType = mdl::HitType::freeType();

namespace
{

constexpr auto AreaEpsilon = 0.0001;
constexpr auto CoordinateEpsilon = 0.001;
constexpr auto PlaneEpsilon = 0.01;
constexpr auto MaxOrthogonalCutterPieces = size_t{4};
constexpr auto MaxOrthogonalSpawnPieces = size_t{16};
constexpr auto MaxOrthogonalOutputFragments = size_t{10};
constexpr auto MaxAxisSplitFragments = size_t{8};

vm::vec2d projectToCutPlane(const vm::vec3d& point, const vm::vec3d& normal)
{
  switch (vm::find_abs_max_component(normal))
  {
  case vm::axis::x:
    return {point.y(), point.z()};
  case vm::axis::y:
    return {point.x(), point.z()};
  case vm::axis::z:
    return {point.x(), point.y()};
  }

  return {point.x(), point.y()};
}

vm::vec2d rotateCutPlaneCoords(const vm::vec2d& point, const double rotation)
{
  const auto c = std::cos(rotation);
  const auto s = std::sin(rotation);
  return {
    c * point.x() - s * point.y(),
    s * point.x() + c * point.y(),
  };
}

vm::vec2d projectToCutPlane(
  const vm::vec3d& point, const vm::vec3d& normal, const double gridRotation)
{
  return rotateCutPlaneCoords(projectToCutPlane(point, normal), gridRotation);
}

vm::vec3d unprojectFromCutPlane(const vm::vec2d& point, const vm::plane3d& plane)
{
  const auto& normal = plane.normal;
  switch (vm::find_abs_max_component(normal))
  {
  case vm::axis::x: {
    const auto x =
      (plane.distance - normal.y() * point.x() - normal.z() * point.y()) / normal.x();
    return {x, point.x(), point.y()};
  }
  case vm::axis::y: {
    const auto y =
      (plane.distance - normal.x() * point.x() - normal.z() * point.y()) / normal.y();
    return {point.x(), y, point.y()};
  }
  case vm::axis::z: {
    const auto z =
      (plane.distance - normal.x() * point.x() - normal.y() * point.y()) / normal.z();
    return {point.x(), point.y(), z};
  }
  }

  return {point.x(), point.y(), 0.0};
}

vm::vec3d unprojectFromCutPlane(
  const vm::vec2d& point, const vm::plane3d& plane, const double gridRotation)
{
  return unprojectFromCutPlane(rotateCutPlaneCoords(point, -gridRotation), plane);
}

double cross2D(const vm::vec2d& a, const vm::vec2d& b, const vm::vec2d& c)
{
  const auto ab = b - a;
  const auto ac = c - a;
  return ab.x() * ac.y() - ab.y() * ac.x();
}

double signedArea2D(const std::vector<vm::vec2d>& points)
{
  auto area = 0.0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    const auto& a = points[i];
    const auto& b = points[(i + 1) % points.size()];
    area += a.x() * b.y() - b.x() * a.y();
  }
  return area / 2.0;
}

bool nearEqual(const double lhs, const double rhs)
{
  return std::abs(lhs - rhs) <= CoordinateEpsilon;
}

bool nearEqual(const vm::vec2d& lhs, const vm::vec2d& rhs)
{
  return nearEqual(lhs.x(), rhs.x()) && nearEqual(lhs.y(), rhs.y());
}

std::vector<vm::vec2d> cleanupPolygon(std::vector<vm::vec2d> polygon)
{
  auto result = std::vector<vm::vec2d>{};
  result.reserve(polygon.size());
  for (const auto& point : polygon)
  {
    if (result.empty() || !nearEqual(result.back(), point))
    {
      result.push_back(point);
    }
  }

  if (result.size() > 1 && nearEqual(result.front(), result.back()))
  {
    result.pop_back();
  }

  auto removedColinearPoint = true;
  while (removedColinearPoint && result.size() > 3)
  {
    removedColinearPoint = false;
    for (size_t i = 0; i < result.size(); ++i)
    {
      const auto& prev = result[(i + result.size() - 1) % result.size()];
      const auto& cur = result[i];
      const auto& next = result[(i + 1) % result.size()];
      if (std::abs(cross2D(prev, cur, next)) <= AreaEpsilon)
      {
        result.erase(std::next(std::begin(result), static_cast<std::ptrdiff_t>(i)));
        removedColinearPoint = true;
        break;
      }
    }
  }

  return result;
}

std::vector<vm::vec2d> clipPolygonByHalfPlane(
  const std::vector<vm::vec2d>& polygon,
  const size_t axis,
  const double value,
  const bool keepGreater)
{
  if (polygon.empty())
  {
    return {};
  }

  const auto inside = [&](const auto& point) {
    return keepGreater ? point[axis] >= value - CoordinateEpsilon
                       : point[axis] <= value + CoordinateEpsilon;
  };

  auto result = std::vector<vm::vec2d>{};
  auto previous = polygon.back();
  auto previousInside = inside(previous);

  for (const auto& current : polygon)
  {
    const auto currentInside = inside(current);
    if (currentInside != previousInside)
    {
      const auto delta = current[axis] - previous[axis];
      if (std::abs(delta) > CoordinateEpsilon)
      {
        const auto t = (value - previous[axis]) / delta;
        result.push_back(previous + (current - previous) * t);
      }
    }

    if (currentInside)
    {
      result.push_back(current);
    }

    previous = current;
    previousInside = currentInside;
  }

  return cleanupPolygon(std::move(result));
}

std::vector<vm::vec2d> clipPolygonByAxisRange(
  const std::vector<vm::vec2d>& polygon,
  const size_t axis,
  const double min,
  const double max)
{
  auto result = clipPolygonByHalfPlane(polygon, axis, min, true);
  result = clipPolygonByHalfPlane(result, axis, max, false);
  return cleanupPolygon(std::move(result));
}

std::vector<double> uniqueSortedCoordinates(
  const std::vector<vm::vec2d>& points, const size_t axis)
{
  auto result = points
                | std::views::transform([&](const auto& point) { return point[axis]; })
                | kdl::ranges::to<std::vector>();
  std::ranges::sort(result);

  auto unique = std::vector<double>{};
  unique.reserve(result.size());
  for (const auto coordinate : result)
  {
    if (unique.empty() || !nearEqual(unique.back(), coordinate))
    {
      unique.push_back(coordinate);
    }
  }

  return unique;
}

void addUniqueCoordinate(std::vector<double>& coordinates, const double coordinate)
{
  if (std::ranges::none_of(coordinates, [&](const auto existing) {
        return nearEqual(existing, coordinate);
      }))
  {
    coordinates.push_back(coordinate);
  }
}

std::optional<vm::vec3d> projectedAxisNormal(
  const vm::vec3d& cutPlaneNormal, const size_t projectedAxis, const double gridRotation)
{
  auto axis0 = size_t{};
  auto axis1 = size_t{};
  switch (vm::find_abs_max_component(cutPlaneNormal))
  {
  case vm::axis::x:
    axis0 = vm::axis::y;
    axis1 = vm::axis::z;
    break;
  case vm::axis::y:
    axis0 = vm::axis::x;
    axis1 = vm::axis::z;
    break;
  case vm::axis::z:
    axis0 = vm::axis::x;
    axis1 = vm::axis::y;
    break;
  default:
    return std::nullopt;
  }

  const auto c = std::cos(gridRotation);
  const auto s = std::sin(gridRotation);
  auto normal = vm::vec3d{0, 0, 0};
  if (projectedAxis == 0)
  {
    normal[axis0] = c;
    normal[axis1] = -s;
  }
  else
  {
    normal[axis0] = s;
    normal[axis1] = c;
  }

  return normal;
}

std::optional<mdl::BrushFace> createFaceForPlane(
  const vm::plane3d& plane,
  const mdl::BrushFaceAttributes& attributes,
  const mdl::MapFormat mapFormat)
{
  const auto normal = vm::normalize(plane.normal);
  const auto anchor = normal * plane.distance;
  const auto helper =
    std::abs(normal.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{0, 1, 0};
  const auto u = vm::normalize(vm::cross(helper, normal));
  const auto v = vm::cross(normal, u);
  constexpr auto Size = 64.0;

  auto result = mdl::BrushFace::create(
    anchor, anchor + v * Size, anchor + u * Size, attributes, mapFormat);
  if (!result)
  {
    return std::nullopt;
  }

  return std::move(result).value();
}

bool isConvexPolygon2D(const std::vector<vm::vec2d>& polygon)
{
  if (polygon.size() < 3 || std::abs(signedArea2D(polygon)) <= AreaEpsilon)
  {
    return false;
  }

  const auto positive = signedArea2D(polygon) > 0.0;
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    const auto cross = cross2D(
      polygon[i], polygon[(i + 1) % polygon.size()], polygon[(i + 2) % polygon.size()]);
    if (positive && cross < -AreaEpsilon)
    {
      return false;
    }
    if (!positive && cross > AreaEpsilon)
    {
      return false;
    }
  }

  return true;
}

std::optional<std::array<double, 2>> convexPolygonYRangeAtX(
  const std::vector<vm::vec2d>& polygon, const double x)
{
  auto intersections = std::vector<double>{};
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1) % polygon.size()];
    const auto minX = std::min(a.x(), b.x());
    const auto maxX = std::max(a.x(), b.x());

    if (nearEqual(a.x(), b.x()))
    {
      if (nearEqual(x, a.x()))
      {
        intersections.push_back(a.y());
        intersections.push_back(b.y());
      }
      continue;
    }

    if (x < minX - CoordinateEpsilon || x > maxX + CoordinateEpsilon)
    {
      continue;
    }

    const auto t = (x - a.x()) / (b.x() - a.x());
    if (t >= -CoordinateEpsilon && t <= 1.0 + CoordinateEpsilon)
    {
      intersections.push_back(a.y() + (b.y() - a.y()) * std::clamp(t, 0.0, 1.0));
    }
  }

  if (intersections.empty())
  {
    return std::nullopt;
  }

  std::ranges::sort(intersections);
  auto unique = std::vector<double>{};
  for (const auto y : intersections)
  {
    if (unique.empty() || !nearEqual(unique.back(), y))
    {
      unique.push_back(y);
    }
  }

  return std::array<double, 2>{unique.front(), unique.back()};
}

bool pointInTriangle(
  const vm::vec2d& point, const vm::vec2d& a, const vm::vec2d& b, const vm::vec2d& c)
{
  const auto ab = cross2D(a, b, point);
  const auto bc = cross2D(b, c, point);
  const auto ca = cross2D(c, a, point);
  return ab >= -AreaEpsilon && bc >= -AreaEpsilon && ca >= -AreaEpsilon;
}

std::vector<vm::vec2d> projectedPoints(
  const std::vector<vm::vec3d>& points, const vm::vec3d& normal)
{
  return points | std::views::transform([&](const auto& point) {
           return projectToCutPlane(point, normal);
         })
         | kdl::ranges::to<std::vector>();
}

std::vector<vm::vec2d> projectedPoints(
  const std::vector<vm::vec3d>& points,
  const vm::vec3d& normal,
  const double gridRotation)
{
  return points | std::views::transform([&](const auto& point) {
           return projectToCutPlane(point, normal, gridRotation);
         })
         | kdl::ranges::to<std::vector>();
}

double absoluteArea2D(const std::vector<vm::vec3d>& points, const vm::vec3d& normal)
{
  return std::abs(signedArea2D(projectedPoints(points, normal)));
}

std::optional<size_t> findPointIndex(
  const std::vector<vm::vec3d>& points, const vm::vec3d& point)
{
  for (size_t i = 0; i < points.size(); ++i)
  {
    if (vm::is_equal(points[i], point, vm::Cd::almost_zero()))
    {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<vm::vec3d>> mergeConvexPolygons(
  const std::vector<vm::vec3d>& lhs,
  const std::vector<vm::vec3d>& rhs,
  const vm::vec3d& normal)
{
  auto points = lhs;
  auto edges = std::vector<std::pair<size_t, size_t>>{};

  const auto addPoint = [&](const vm::vec3d& point) {
    if (const auto index = findPointIndex(points, point))
    {
      return *index;
    }
    points.push_back(point);
    return points.size() - 1;
  };

  const auto addEdge = [&](const size_t from, const size_t to) {
    const auto reverseIt = std::ranges::find_if(
      edges, [&](const auto& edge) { return edge.first == to && edge.second == from; });
    if (reverseIt != std::end(edges))
    {
      edges.erase(reverseIt);
    }
    else
    {
      edges.emplace_back(from, to);
    }
  };

  const auto addPolygonEdges = [&](const std::vector<vm::vec3d>& polygon) {
    for (size_t i = 0; i < polygon.size(); ++i)
    {
      addEdge(addPoint(polygon[i]), addPoint(polygon[(i + 1) % polygon.size()]));
    }
  };

  addPolygonEdges(lhs);
  addPolygonEdges(rhs);
  if (edges.size() < 3)
  {
    return std::nullopt;
  }

  auto polygon = std::vector<vm::vec3d>{};
  polygon.reserve(edges.size());

  auto current = edges.front().first;
  const auto start = current;
  while (!edges.empty())
  {
    polygon.push_back(points[current]);
    const auto edgeIt = std::ranges::find_if(
      edges, [&](const auto& edge) { return edge.first == current; });
    if (edgeIt == std::end(edges))
    {
      return std::nullopt;
    }

    current = edgeIt->second;
    edges.erase(edgeIt);
  }

  if (current != start)
  {
    return std::nullopt;
  }

  const auto projectedPolygon = projectedPoints(polygon, normal);
  const auto area = std::abs(signedArea2D(projectedPolygon));
  const auto areaSum = absoluteArea2D(lhs, normal) + absoluteArea2D(rhs, normal);
  const auto areaTolerance = std::max(AreaEpsilon, areaSum * 0.001);
  if (std::abs(area - areaSum) > areaTolerance)
  {
    return std::nullopt;
  }

  if (!isConvexPolygon2D(projectedPolygon))
  {
    return std::nullopt;
  }

  if (signedArea2D(projectedPolygon) < 0.0)
  {
    std::ranges::reverse(polygon);
  }

  return polygon;
}

std::vector<std::vector<vm::vec3d>> mergeConvexPolygonPieces(
  std::vector<std::vector<vm::vec3d>> pieces, const vm::vec3d& normal)
{
  auto merged = true;
  while (merged)
  {
    merged = false;
    for (size_t i = 0; i < pieces.size() && !merged; ++i)
    {
      for (size_t j = i + 1; j < pieces.size(); ++j)
      {
        if (auto polygon = mergeConvexPolygons(pieces[i], pieces[j], normal))
        {
          pieces[i] = std::move(*polygon);
          pieces.erase(std::next(std::begin(pieces), static_cast<std::ptrdiff_t>(j)));
          merged = true;
          break;
        }
      }
    }
  }

  return pieces;
}

class PolygonCutToolPage : public QWidget
{
private:
  PolygonCutTool& m_tool;
  NotifierConnection m_notifierConnection;
  QLabel* m_label = nullptr;
  QComboBox* m_cutMode = nullptr;
  QLabel* m_depthLabel = nullptr;
  QSpinBox* m_cutDepth = nullptr;

public:
  explicit PolygonCutToolPage(PolygonCutTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  void createGui()
  {
    m_label = new QLabel{tr("Mode")};
    m_cutMode = new QComboBox{};
    m_cutMode->addItem(tr("Remove cutout"));
    m_cutMode->addItem(tr("Keep cutout"));
    m_cutMode->setCurrentIndex(
      m_tool.cutMode() == PolygonCutTool::CutMode::RemoveCutout ? 0 : 1);
    m_depthLabel = new QLabel{tr("Depth")};
    m_cutDepth = new QSpinBox{};
    m_cutDepth->setRange(0, 1024);
    m_cutDepth->setSuffix(tr(" grid"));
    m_cutDepth->setToolTip(
      tr("0 cuts through the whole brush; higher values cut by the current grid size."));
    m_cutDepth->setValue(m_tool.cutDepthSteps());

    connect(
      m_cutMode,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int index) {
        m_tool.setCutMode(
          index == 0 ? PolygonCutTool::CutMode::RemoveCutout
                     : PolygonCutTool::CutMode::KeepCutout);
      });
    connect(
      m_cutDepth,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setCutDepthSteps(value); });

    m_notifierConnection +=
      m_tool.toolActivatedNotifier.connect([&](auto&) { updateControls(); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(m_label, 0, Qt::AlignVCenter);
    layout->addWidget(m_cutMode, 0, Qt::AlignVCenter);
    layout->addSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(m_depthLabel, 0, Qt::AlignVCenter);
    layout->addWidget(m_cutDepth, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    setLayout(layout);
    updateControls();
  }

  void updateControls()
  {
    const auto cutMode = m_tool.toolMode() == PolygonCutTool::ToolMode::Cut;
    m_label->setVisible(cutMode);
    m_cutMode->setVisible(cutMode);
    m_depthLabel->setVisible(cutMode);
    m_cutDepth->setVisible(cutMode);
    const auto blocker = QSignalBlocker{m_cutDepth};
    m_cutDepth->setValue(m_tool.cutDepthSteps());
  }
};

} // namespace

PolygonCutTool::PolygonCutTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

PolygonCutTool::~PolygonCutTool() = default;

const mdl::Grid& PolygonCutTool::grid() const
{
  return m_document.map().grid();
}

void PolygonCutTool::pick(
  const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult)
{
  for (size_t i = 0; i < m_points.size(); ++i)
  {
    const auto& point = m_points[i];
    if (
      const auto distance = camera.pickPointHandle(
        pickRay, point, static_cast<double>(pref(Preferences::HandleRadius))))
    {
      const auto hitPoint = vm::point_at_distance(pickRay, *distance);
      pickResult.addHit(mdl::Hit{PointHitType, *distance, hitPoint, i});
    }
  }
}

void PolygonCutTool::render(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult)
{
  renderPoints(renderContext, renderBatch, pickResult);
}

void PolygonCutTool::renderFeedback(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const vm::vec3d& point) const
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setForegroundColor(pref(Preferences::ClipHandleColor));
  renderService.renderHandle(vm::vec3f{point});

  if (!m_points.empty())
  {
    renderService.setShowOccludedObjects();
    renderService.renderLine(vm::vec3f{m_points.back()}, vm::vec3f{point});
  }
}

bool PolygonCutTool::hasBrushes() const
{
  return m_document.map().selection().hasBrushes();
}

bool PolygonCutTool::canCut() const
{
  if (m_toolMode == ToolMode::SpawnBrush)
  {
    return canSpawnBrush();
  }

  return polygonClosed() && m_cutPlane && std::abs(signedPolygonArea()) > AreaEpsilon;
}

void PolygonCutTool::performCut()
{
  if (m_toolMode == ToolMode::SpawnBrush)
  {
    performSpawnBrush();
    return;
  }

  if (m_dragging || !canCut())
  {
    return;
  }

  auto polygons = cutterPolygons();
  if (polygons.empty())
  {
    m_document.logger().error()
      << "Could not polygon cut brush: polygon is invalid or self-intersecting";
    return;
  }

  auto& map = m_document.map();
  const auto brushNodes = std::vector<mdl::BrushNode*>{map.selection().brushes};
  if (brushNodes.empty())
  {
    return;
  }

  auto toAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto toRemove = std::vector<mdl::Node*>{};

  for (const auto* brushNode : brushNodes)
  {
    auto fragments = std::vector<mdl::Brush>{};
    auto keptFragments = std::vector<mdl::Brush>{};
    if (polygons.size() == 1 && m_cutDepthSteps == 0)
    {
      if (
        auto orthogonalFragments =
          createOrthogonalFragments(brushNode->brush(), polygons.front()))
      {
        fragments = std::move(*orthogonalFragments);
      }
    }

    if (fragments.empty())
    {
      fragments = std::vector<mdl::Brush>{brushNode->brush()};
      for (const auto& polygon : polygons)
      {
        const auto cutterBrush = createCutterBrush(brushNode->brush(), polygon);
        if (!cutterBrush)
        {
          return;
        }

        auto nextFragments = std::vector<mdl::Brush>{};
        for (auto& fragment : fragments)
        {
          auto fragmentFailed = false;
          auto subtractionResults = fragment.subtract(
            map.worldNode().mapFormat(),
            map.worldBounds(),
            map.currentMaterialName(),
            *cutterBrush);

          for (auto& result : subtractionResults)
          {
            if (result)
            {
              nextFragments.push_back(std::move(result).value());
            }
            else
            {
              fragmentFailed = true;
              result | kdl::if_error([&](const auto& e) {
                map.logger().error() << "Could not polygon cut brush: " << e.msg;
              }) | kdl::ignore();
            }
          }

          if (fragmentFailed)
          {
            nextFragments.push_back(std::move(fragment));
          }
        }

        fragments = std::move(nextFragments);
        if (fragments.empty())
        {
          break;
        }
      }
    }

    if (m_cutMode == CutMode::KeepCutout)
    {
      for (const auto& polygon : polygons)
      {
        const auto cutterBrush = createCutterBrush(brushNode->brush(), polygon);
        if (!cutterBrush)
        {
          return;
        }

        if (brushNode->brush().intersects(*cutterBrush))
        {
          auto keptFragment = brushNode->brush();
          keptFragment.intersect(map.worldBounds(), *cutterBrush) | kdl::transform([&]() {
            keptFragments.push_back(std::move(keptFragment));
          }) | kdl::if_error([&](const auto& e) {
            map.logger().error() << "Could not keep polygon cutout brush: " << e.msg;
          }) | kdl::ignore();
        }
      }
    }

    if (!fragments.empty())
    {
      auto& nodes = toAdd[brushNode->parent()];
      for (auto& fragment : fragments)
      {
        nodes.push_back(new mdl::BrushNode{std::move(fragment)});
      }
    }

    if (!keptFragments.empty())
    {
      auto& nodes = toAdd[brushNode->parent()];
      for (auto& keptFragment : keptFragments)
      {
        nodes.push_back(new mdl::BrushNode{std::move(keptFragment)});
      }
    }
    toRemove.push_back(const_cast<mdl::BrushNode*>(brushNode));
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto transaction = mdl::Transaction{map, "Polygon Cut Brushes"};

  deselectAll(map);
  const auto addedNodes = addNodes(map, toAdd);
  removeNodes(map, toRemove);
  selectNodes(map, addedNodes);

  if (transaction.commit())
  {
    reset();
  }
}

bool PolygonCutTool::canSpawnBrush() const
{
  return polygonClosed() && m_cutPlane && std::abs(signedPolygonArea()) > AreaEpsilon;
}

void PolygonCutTool::performSpawnBrush()
{
  if (m_dragging || !canSpawnBrush())
  {
    return;
  }

  const auto polygons = spawnPolygons();
  if (polygons.empty())
  {
    m_document.logger().error()
      << "Could not spawn polygon brush: polygon is invalid or self-intersecting";
    return;
  }

  auto& map = m_document.map();
  const auto brushNodes = std::vector<mdl::BrushNode*>{map.selection().brushes};
  if (brushNodes.empty())
  {
    return;
  }

  auto toAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto& nodes = toAdd[brushNodes.front()->parent()];
  for (const auto& polygon : polygons)
  {
    auto brush = createSpawnBrush(polygon);
    if (!brush)
    {
      return;
    }

    nodes.push_back(new mdl::BrushNode{std::move(*brush)});
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto transaction = mdl::Transaction{map, "Spawn Polygon Brushes"};

  deselectAll(map);
  const auto addedNodes = addNodes(map, toAdd);
  selectNodes(map, addedNodes);

  if (transaction.commit())
  {
    reset();
  }
}

PolygonCutTool::ToolMode PolygonCutTool::toolMode() const
{
  return m_toolMode;
}

void PolygonCutTool::setToolMode(const ToolMode toolMode)
{
  if (m_toolMode == toolMode)
  {
    return;
  }

  m_toolMode = toolMode;
  reset();
}

PolygonCutTool::CutMode PolygonCutTool::cutMode() const
{
  return m_cutMode;
}

void PolygonCutTool::setCutMode(const CutMode cutMode)
{
  m_cutMode = cutMode;
}

int PolygonCutTool::cutDepthSteps() const
{
  return m_cutDepthSteps;
}

void PolygonCutTool::setCutDepthSteps(const int cutDepthSteps)
{
  m_cutDepthSteps = std::max(0, cutDepthSteps);
}

double PolygonCutTool::cutDepth() const
{
  return static_cast<double>(m_cutDepthSteps) * grid().actualSize();
}

std::optional<vm::vec3d> PolygonCutTool::defaultCutPointPos() const
{
  return m_document.map().selectionBounds()
         | kdl::optional_transform([](const auto& bounds) { return bounds.center(); });
}

std::optional<vm::plane3d> PolygonCutTool::cutPlane() const
{
  return m_cutPlane;
}

bool PolygonCutTool::canAddPoint(const vm::vec3d& point, const vm::plane3d& plane) const
{
  if (polygonClosed())
  {
    return false;
  }

  if (m_cutPlane)
  {
    if (!pointOnCutPlane(point))
    {
      return false;
    }
  }
  else if (vm::is_zero(vm::squared_length(plane.normal), vm::Cd::almost_zero()))
  {
    return false;
  }

  if (m_points.size() >= 3)
  {
    if (vm::is_equal(m_points.front(), point, vm::Cd::almost_zero()))
    {
      return !vm::is_equal(m_points.back(), point, vm::Cd::almost_zero());
    }
  }

  return std::ranges::none_of(m_points, [&](const auto& p) {
    return vm::is_equal(p, point, vm::Cd::almost_zero());
  });
}

bool PolygonCutTool::hasPoints() const
{
  return !m_points.empty();
}

void PolygonCutTool::addPoint(const vm::vec3d& point, const vm::plane3d& plane)
{
  contract_pre(canAddPoint(point, plane));

  if (!m_cutPlane)
  {
    m_cutPlane = plane;
  }

  m_points.push_back(point);
  refreshViews();
}

bool PolygonCutTool::canRemoveLastPoint() const
{
  return !m_points.empty();
}

bool PolygonCutTool::removeLastPoint()
{
  if (canRemoveLastPoint())
  {
    m_points.pop_back();
    if (m_points.empty())
    {
      m_cutPlane.reset();
    }
    refreshViews();
    return true;
  }

  return false;
}

std::optional<std::tuple<vm::vec3d, vm::vec3d>> PolygonCutTool::beginDragPoint(
  const mdl::PickResult& pickResult)
{
  contract_pre(!m_dragging);

  if (polygonClosed())
  {
    return std::nullopt;
  }

  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(PointHitType));
  if (!hit.isMatch())
  {
    return std::nullopt;
  }

  const auto index = hit.target<size_t>();
  m_dragState = DragState{index, m_points[index]};
  m_dragging = true;
  return {{m_points[index], hit.hitPoint()}};
}

void PolygonCutTool::beginDragLastPoint()
{
  contract_pre(!m_dragging);
  contract_pre(!m_points.empty());

  m_dragState = DragState{m_points.size() - 1, m_points.back()};
  m_dragging = true;
}

bool PolygonCutTool::dragPoint(const vm::vec3d& newPosition)
{
  contract_pre(m_dragging);
  contract_pre(m_dragState != std::nullopt);

  if (!pointOnCutPlane(newPosition))
  {
    return false;
  }

  for (size_t i = 0; i < m_points.size(); ++i)
  {
    const auto allowClosingPoint =
      m_dragState->index == m_points.size() - 1 && i == 0 && m_points.size() >= 4;
    if (
      m_dragState->index != i && !allowClosingPoint
      && vm::is_equal(m_points[i], newPosition, vm::Cd::almost_zero()))
    {
      return false;
    }
  }

  m_points[m_dragState->index] = newPosition;
  refreshViews();
  return true;
}

void PolygonCutTool::endDragPoint()
{
  contract_pre(m_dragging);

  m_dragState = std::nullopt;
  m_dragging = false;
  refreshViews();
}

void PolygonCutTool::cancelDragPoint()
{
  contract_pre(m_dragging);
  contract_pre(m_dragState != std::nullopt);

  m_points[m_dragState->index] = m_dragState->originalPoint;
  m_dragState = std::nullopt;
  m_dragging = false;
  refreshViews();
}

bool PolygonCutTool::reset()
{
  if (!m_points.empty() || m_cutPlane)
  {
    m_points.clear();
    m_cutPlane.reset();
    m_dragState = std::nullopt;
    m_dragging = false;
    refreshViews();
    return true;
  }

  return false;
}

void PolygonCutTool::renderPoints(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult) const
{
  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setForegroundColor(pref(Preferences::ClipHandleColor));
  renderService.setShowOccludedObjects();

  for (size_t i = 1; i < m_points.size(); ++i)
  {
    renderService.renderLine(vm::vec3f{m_points[i - 1]}, vm::vec3f{m_points[i]});
  }

  renderService.setForegroundColor(pref(Preferences::ClipHandleColor));
  renderService.setBackgroundColor(pref(Preferences::InfoOverlayBackgroundColor));

  for (const auto& point : m_points)
  {
    renderService.renderHandle(vm::vec3f{point});
  }

  renderHighlight(renderContext, renderBatch, pickResult);
}

void PolygonCutTool::renderHighlight(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const mdl::PickResult& pickResult) const
{
  if (m_dragState)
  {
    renderHighlight(renderContext, renderBatch, m_dragState->index);
  }
  else
  {
    using namespace mdl::HitFilters;

    const auto& hit = pickResult.first(type(PointHitType));
    if (hit.isMatch())
    {
      renderHighlight(renderContext, renderBatch, hit.target<size_t>());
    }
  }
}

void PolygonCutTool::renderHighlight(
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch,
  const size_t index) const
{
  if (index < m_points.size())
  {
    auto renderService = render::RenderService{renderContext, renderBatch};
    renderService.setForegroundColor(pref(Preferences::SelectedHandleColor));
    renderService.renderHandleHighlight(vm::vec3f{m_points[index]});
  }
}

bool PolygonCutTool::pointOnCutPlane(const vm::vec3d& point) const
{
  if (!m_cutPlane)
  {
    return true;
  }

  return std::abs(vm::dot(point, m_cutPlane->normal) - m_cutPlane->distance)
         <= PlaneEpsilon;
}

bool PolygonCutTool::polygonClosed() const
{
  return m_points.size() >= 4
         && vm::is_equal(m_points.front(), m_points.back(), vm::Cd::almost_zero());
}

std::vector<vm::vec3d> PolygonCutTool::polygonPoints() const
{
  auto result = m_points;
  if (result.size() > 1)
  {
    if (vm::is_equal(result.front(), result.back(), vm::Cd::almost_zero()))
    {
      result.pop_back();
    }
  }
  return result;
}

double PolygonCutTool::signedPolygonArea() const
{
  const auto polygon = polygonPoints();
  if (!m_cutPlane || polygon.size() < 3)
  {
    return 0.0;
  }

  const auto points = projectedPoints(polygon, m_cutPlane->normal);
  auto area = 0.0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    const auto& a = points[i];
    const auto& b = points[(i + 1) % points.size()];
    area += a.x() * b.y() - b.x() * a.y();
  }
  return area / 2.0;
}

std::vector<size_t> PolygonCutTool::orderedPolygonIndices() const
{
  if (!canCut())
  {
    return {};
  }

  const auto polygon = polygonPoints();
  const auto points = projectedPoints(polygon, m_cutPlane->normal);
  auto indices = std::vector<size_t>(points.size());
  std::iota(std::begin(indices), std::end(indices), 0u);

  if (signedPolygonArea() < 0.0)
  {
    std::ranges::reverse(indices);
  }

  auto removedColinearPoint = true;
  while (removedColinearPoint && indices.size() > 3)
  {
    removedColinearPoint = false;
    for (size_t i = 0; i < indices.size(); ++i)
    {
      const auto prev = indices[(i + indices.size() - 1) % indices.size()];
      const auto cur = indices[i];
      const auto next = indices[(i + 1) % indices.size()];
      if (std::abs(cross2D(points[prev], points[cur], points[next])) <= AreaEpsilon)
      {
        indices.erase(std::next(std::begin(indices), static_cast<std::ptrdiff_t>(i)));
        removedColinearPoint = true;
        break;
      }
    }
  }

  return indices;
}

std::vector<vm::vec3d> PolygonCutTool::orderedPolygon() const
{
  const auto polygon = polygonPoints();
  const auto indices = orderedPolygonIndices();
  return indices | std::views::transform([&](const auto index) { return polygon[index]; })
         | kdl::ranges::to<std::vector>();
}

bool PolygonCutTool::isConvexPolygon(const std::vector<size_t>& indices) const
{
  if (!m_cutPlane || indices.size() < 3)
  {
    return false;
  }

  const auto polygon = polygonPoints();
  const auto points = projectedPoints(polygon, m_cutPlane->normal);
  for (size_t i = 0; i < indices.size(); ++i)
  {
    const auto prev = indices[(i + indices.size() - 1) % indices.size()];
    const auto cur = indices[i];
    const auto next = indices[(i + 1) % indices.size()];
    if (cross2D(points[prev], points[cur], points[next]) < -AreaEpsilon)
    {
      return false;
    }
  }

  return true;
}

std::vector<PolygonCutTool::Triangle> PolygonCutTool::triangulatePolygon() const
{
  if (!canCut())
  {
    return {};
  }

  const auto polygon = polygonPoints();
  const auto points = projectedPoints(polygon, m_cutPlane->normal);
  auto indices = orderedPolygonIndices();

  auto triangles = std::vector<Triangle>{};
  auto guard = size_t{0};
  while (indices.size() > 3 && guard++ < points.size() * points.size())
  {
    auto foundEar = false;
    for (size_t i = 0; i < indices.size(); ++i)
    {
      const auto prev = indices[(i + indices.size() - 1) % indices.size()];
      const auto cur = indices[i];
      const auto next = indices[(i + 1) % indices.size()];

      if (cross2D(points[prev], points[cur], points[next]) <= AreaEpsilon)
      {
        continue;
      }

      auto containsPoint = false;
      for (const auto candidate : indices)
      {
        if (candidate == prev || candidate == cur || candidate == next)
        {
          continue;
        }

        if (pointInTriangle(points[candidate], points[prev], points[cur], points[next]))
        {
          containsPoint = true;
          break;
        }
      }

      if (!containsPoint)
      {
        triangles.push_back(Triangle{prev, cur, next});
        indices.erase(std::next(std::begin(indices), static_cast<std::ptrdiff_t>(i)));
        foundEar = true;
        break;
      }
    }

    if (!foundEar)
    {
      return {};
    }
  }

  if (indices.size() == 3)
  {
    triangles.push_back(Triangle{indices[0], indices[1], indices[2]});
  }

  return triangles;
}

std::vector<std::vector<vm::vec3d>> PolygonCutTool::axisAlignedCutterPolygons(
  const size_t axis, const size_t maxPieces) const
{
  if (!m_cutPlane)
  {
    return {};
  }

  const auto polygon = orderedPolygon();
  if (polygon.size() < 4)
  {
    return {};
  }

  const auto gridRotation = grid().rotation();
  auto projectedPolygon =
    cleanupPolygon(projectedPoints(polygon, m_cutPlane->normal, gridRotation));
  if (projectedPolygon.size() < 4)
  {
    return {};
  }

  const auto coordinates = uniqueSortedCoordinates(projectedPolygon, axis);
  if (coordinates.size() < 3 || coordinates.size() - 1 > maxPieces)
  {
    return {};
  }

  const auto originalArea = std::abs(signedArea2D(projectedPolygon));
  auto coveredArea = 0.0;
  auto pieces = std::vector<std::vector<vm::vec3d>>{};
  pieces.reserve(coordinates.size() - 1);

  for (size_t i = 0; i + 1 < coordinates.size(); ++i)
  {
    if (nearEqual(coordinates[i], coordinates[i + 1]))
    {
      continue;
    }

    auto piece =
      clipPolygonByAxisRange(projectedPolygon, axis, coordinates[i], coordinates[i + 1]);
    const auto area = signedArea2D(piece);
    if (std::abs(area) <= AreaEpsilon)
    {
      continue;
    }

    if (!isConvexPolygon2D(piece))
    {
      return {};
    }

    if (area < 0.0)
    {
      std::ranges::reverse(piece);
    }

    coveredArea += std::abs(area);
    pieces.push_back(
      piece | std::views::transform([&](const auto& point) {
        return unprojectFromCutPlane(point, *m_cutPlane, gridRotation);
      })
      | kdl::ranges::to<std::vector>());
  }

  const auto areaTolerance = std::max(AreaEpsilon, originalArea * 0.001);
  if (
    pieces.size() <= 1 || pieces.size() > maxPieces
    || std::abs(coveredArea - originalArea) > areaTolerance)
  {
    return {};
  }

  return pieces;
}

std::vector<std::vector<vm::vec3d>> PolygonCutTool::orthogonalCutterPolygons() const
{
  auto verticalPieces = axisAlignedCutterPolygons(0, MaxOrthogonalCutterPieces);
  auto horizontalPieces = axisAlignedCutterPolygons(1, MaxOrthogonalCutterPieces);

  if (verticalPieces.empty())
  {
    return horizontalPieces;
  }
  if (horizontalPieces.empty())
  {
    return verticalPieces;
  }

  return verticalPieces.size() >= horizontalPieces.size() ? std::move(verticalPieces)
                                                          : std::move(horizontalPieces);
}

std::vector<std::vector<vm::vec3d>> PolygonCutTool::cutterPolygons() const
{
  auto indices = orderedPolygonIndices();
  if (indices.size() < 3)
  {
    return {};
  }

  if (isConvexPolygon(indices))
  {
    return {orderedPolygon()};
  }

  const auto triangles = triangulatePolygon();
  const auto polygon = polygonPoints();
  return triangles | std::views::transform([&](const auto& triangle) {
           return std::vector<vm::vec3d>{
             polygon[triangle[0]],
             polygon[triangle[1]],
             polygon[triangle[2]],
           };
         })
         | kdl::ranges::to<std::vector>();
}

std::vector<std::vector<vm::vec3d>> PolygonCutTool::spawnPolygons() const
{
  if (!m_cutPlane)
  {
    return {};
  }

  auto indices = orderedPolygonIndices();
  if (indices.size() < 3)
  {
    return {};
  }

  if (isConvexPolygon(indices))
  {
    return {orderedPolygon()};
  }

  auto verticalPieces = axisAlignedCutterPolygons(0, MaxOrthogonalSpawnPieces);
  auto horizontalPieces = axisAlignedCutterPolygons(1, MaxOrthogonalSpawnPieces);

  auto orthogonalPieces = std::vector<std::vector<vm::vec3d>>{};
  if (verticalPieces.empty())
  {
    orthogonalPieces = std::move(horizontalPieces);
  }
  else if (horizontalPieces.empty())
  {
    orthogonalPieces = std::move(verticalPieces);
  }
  else
  {
    orthogonalPieces = verticalPieces.size() <= horizontalPieces.size()
                         ? std::move(verticalPieces)
                         : std::move(horizontalPieces);
  }

  if (!orthogonalPieces.empty())
  {
    return mergeConvexPolygonPieces(std::move(orthogonalPieces), m_cutPlane->normal);
  }

  const auto triangles = triangulatePolygon();
  const auto polygon = polygonPoints();
  auto trianglePieces = triangles | std::views::transform([&](const auto& triangle) {
                          return std::vector<vm::vec3d>{
                            polygon[triangle[0]],
                            polygon[triangle[1]],
                            polygon[triangle[2]],
                          };
                        })
                        | kdl::ranges::to<std::vector>();

  return mergeConvexPolygonPieces(std::move(trianglePieces), m_cutPlane->normal);
}

std::optional<mdl::Brush> PolygonCutTool::createSpawnBrush(
  const std::vector<vm::vec3d>& polygon) const
{
  contract_pre(m_cutPlane != std::nullopt);
  contract_pre(polygon.size() >= 3);

  const auto normal = vm::normalize(m_cutPlane->normal);
  const auto height = grid().actualSize();

  auto points = std::vector<vm::vec3d>{};
  points.reserve(2 * polygon.size());
  for (const auto& point : polygon)
  {
    points.push_back(point);
    points.push_back(point + normal * height);
  }

  auto& map = m_document.map();
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  auto result = builder.createBrush(points, map.currentMaterialName());
  if (!result)
  {
    result | kdl::if_error([&](const auto& e) {
      map.logger().error() << "Could not create spawned polygon brush: " << e.msg;
    }) | kdl::ignore();
    return std::nullopt;
  }

  return std::move(result).value();
}

std::optional<mdl::Brush> PolygonCutTool::createCutterBrush(
  const mdl::Brush& targetBrush, const std::vector<vm::vec3d>& polygon) const
{
  contract_pre(m_cutPlane != std::nullopt);
  contract_pre(polygon.size() >= 3);

  const auto normal = vm::normalize(m_cutPlane->normal);
  auto polygonCenter = vm::vec3d{};
  for (const auto& point : polygon)
  {
    polygonCenter = polygonCenter + point;
  }
  polygonCenter = polygonCenter / static_cast<double>(polygon.size());

  auto minDistance = std::numeric_limits<double>::max();
  auto maxDistance = std::numeric_limits<double>::lowest();
  for (const auto* vertex : targetBrush.vertices())
  {
    const auto distance = vm::dot(vertex->position(), normal);
    minDistance = std::min(minDistance, distance);
    maxDistance = std::max(maxDistance, distance);
  }

  if (m_cutDepthSteps > 0)
  {
    const auto cutDistance = vm::dot(polygonCenter, normal);
    const auto distanceToMin = std::abs(cutDistance - minDistance);
    const auto distanceToMax = std::abs(maxDistance - cutDistance);
    const auto inwardSign = distanceToMin <= distanceToMax ? 1.0 : -1.0;
    const auto inward = normal * inwardSign;
    const auto depth = std::max(cutDepth(), CoordinateEpsilon);
    const auto outsideMargin =
      std::max(2.0 * CoordinateEpsilon, 0.05 * grid().actualSize());

    auto points = std::vector<vm::vec3d>{};
    points.reserve(2 * polygon.size());
    for (const auto& point : polygon)
    {
      points.push_back(point - inward * outsideMargin);
      points.push_back(point + inward * depth);
    }

    auto& map = m_document.map();
    const auto builder = mdl::BrushBuilder{
      map.worldNode().mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaults};

    auto result = builder.createBrush(points, map.currentMaterialName());
    if (!result)
    {
      result | kdl::if_error([&](const auto& e) {
        map.logger().error() << "Could not create polygon cutter brush: " << e.msg;
      }) | kdl::ignore();
      return std::nullopt;
    }

    return std::move(result).value();
  }

  const auto margin = std::max(64.0, 2.0 * grid().actualSize());
  auto points = std::vector<vm::vec3d>{};
  points.reserve(2 * polygon.size());
  for (const auto& point : polygon)
  {
    const auto distance = vm::dot(point, normal);
    points.push_back(point + normal * (minDistance - distance - margin));
    points.push_back(point + normal * (maxDistance - distance + margin));
  }

  auto& map = m_document.map();
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  auto result = builder.createBrush(points, map.currentMaterialName());
  if (!result)
  {
    result | kdl::if_error([&](const auto& e) {
      map.logger().error() << "Could not create polygon cutter brush: " << e.msg;
    }) | kdl::ignore();
    return std::nullopt;
  }

  return std::move(result).value();
}

std::optional<std::vector<mdl::Brush>> PolygonCutTool::createOrthogonalFragments(
  const mdl::Brush& targetBrush, const std::vector<vm::vec3d>& cutterPolygon) const
{
  if (!m_cutPlane)
  {
    return std::nullopt;
  }

  const auto cutPlaneNormal = vm::normalize(m_cutPlane->normal);
  const auto cutPlaneAxis = vm::find_abs_max_component(cutPlaneNormal);
  if (std::abs(cutPlaneNormal[cutPlaneAxis]) < 0.99)
  {
    return std::nullopt;
  }

  const auto gridRotation = grid().rotation();
  auto cutter =
    cleanupPolygon(projectedPoints(cutterPolygon, cutPlaneNormal, gridRotation));
  if (cutter.size() < 3 || !isConvexPolygon2D(cutter))
  {
    return std::nullopt;
  }

  if (signedArea2D(cutter) < 0.0)
  {
    std::ranges::reverse(cutter);
  }

  const auto& bounds = targetBrush.bounds();
  const auto boundsVertices = bounds.vertices();
  const auto bounds2D = projectedPoints(
    std::vector<vm::vec3d>{std::begin(boundsVertices), std::end(boundsVertices)},
    cutPlaneNormal,
    gridRotation);
  auto minX = std::numeric_limits<double>::max();
  auto minY = std::numeric_limits<double>::max();
  auto maxX = std::numeric_limits<double>::lowest();
  auto maxY = std::numeric_limits<double>::lowest();
  for (const auto& point : bounds2D)
  {
    minX = std::min(minX, point.x());
    minY = std::min(minY, point.y());
    maxX = std::max(maxX, point.x());
    maxY = std::max(maxY, point.y());
  }

  auto xCoordinates = std::vector<double>{minX, maxX};
  for (const auto& point : cutter)
  {
    if (point.x() > minX + CoordinateEpsilon && point.x() < maxX - CoordinateEpsilon)
    {
      addUniqueCoordinate(xCoordinates, point.x());
    }
  }
  std::ranges::sort(xCoordinates);

  auto result = std::vector<mdl::Brush>{};
  auto& map = m_document.map();

  const auto addCell = [&](std::vector<vm::vec2d> cell2D) -> bool {
    cell2D = cleanupPolygon(std::move(cell2D));
    if (cell2D.size() < 3 || std::abs(signedArea2D(cell2D)) <= AreaEpsilon)
    {
      return true;
    }
    if (!isConvexPolygon2D(cell2D))
    {
      return false;
    }
    if (signedArea2D(cell2D) < 0.0)
    {
      std::ranges::reverse(cell2D);
    }

    const auto cell3D = cell2D | std::views::transform([&](const auto& point) {
                          return unprojectFromCutPlane(point, *m_cutPlane, gridRotation);
                        })
                        | kdl::ranges::to<std::vector>();
    const auto cellBrush = createCutterBrush(targetBrush, cell3D);
    if (!cellBrush)
    {
      return false;
    }

    auto fragment = targetBrush;
    const auto intersection = fragment.intersect(map.worldBounds(), *cellBrush);
    if (!intersection)
    {
      return false;
    }

    result.push_back(std::move(fragment));
    return result.size() <= MaxOrthogonalOutputFragments;
  };

  for (size_t i = 0; i + 1 < xCoordinates.size(); ++i)
  {
    const auto x0 = xCoordinates[i];
    const auto x1 = xCoordinates[i + 1];
    if (nearEqual(x0, x1))
    {
      continue;
    }

    const auto leftRange = convexPolygonYRangeAtX(cutter, x0);
    const auto rightRange = convexPolygonYRangeAtX(cutter, x1);
    if (!leftRange || !rightRange)
    {
      if (!addCell({{x0, minY}, {x1, minY}, {x1, maxY}, {x0, maxY}}))
      {
        return std::nullopt;
      }
      continue;
    }

    const auto cutBottomLeft = std::clamp((*leftRange)[0], minY, maxY);
    const auto cutTopLeft = std::clamp((*leftRange)[1], minY, maxY);
    const auto cutBottomRight = std::clamp((*rightRange)[0], minY, maxY);
    const auto cutTopRight = std::clamp((*rightRange)[1], minY, maxY);

    if (std::max(cutBottomLeft, cutBottomRight) > minY + CoordinateEpsilon)
    {
      if (!addCell({{x0, minY}, {x1, minY}, {x1, cutBottomRight}, {x0, cutBottomLeft}}))
      {
        return std::nullopt;
      }
    }

    if (std::min(cutTopLeft, cutTopRight) < maxY - CoordinateEpsilon)
    {
      if (!addCell({{x0, cutTopLeft}, {x1, cutTopRight}, {x1, maxY}, {x0, maxY}}))
      {
        return std::nullopt;
      }
    }
  }

  if (result.empty())
  {
    return std::nullopt;
  }

  return result;
}

std::vector<vm::plane3d> PolygonCutTool::axisSplitPlanes() const
{
  if (!m_cutPlane)
  {
    return {};
  }

  const auto cutPlaneNormal = vm::normalize(m_cutPlane->normal);
  const auto cutPlaneAxis = vm::find_abs_max_component(cutPlaneNormal);
  if (std::abs(cutPlaneNormal[cutPlaneAxis]) < 0.99)
  {
    return {};
  }

  const auto gridRotation = grid().rotation();
  const auto polygon =
    cleanupPolygon(projectedPoints(orderedPolygon(), cutPlaneNormal, gridRotation));
  if (polygon.size() < 4)
  {
    return {};
  }

  auto coordinatesByProjectedAxis = std::array<std::vector<double>, 2>{};
  for (size_t extremeAxis = 0; extremeAxis < 2; ++extremeAxis)
  {
    const auto otherAxis = 1 - extremeAxis;
    const auto minmax =
      std::ranges::minmax(polygon | std::views::transform([&](const auto& point) {
                            return point[extremeAxis];
                          }));

    const auto addSinglePointExtreme = [&](const double value) {
      auto candidates = std::vector<double>{};
      for (const auto& point : polygon)
      {
        if (nearEqual(point[extremeAxis], value))
        {
          candidates.push_back(point[otherAxis]);
        }
      }

      if (candidates.size() == 1)
      {
        addUniqueCoordinate(coordinatesByProjectedAxis[otherAxis], candidates.front());
      }
    };

    addSinglePointExtreme(minmax.min);
    if (!nearEqual(minmax.min, minmax.max))
    {
      addSinglePointExtreme(minmax.max);
    }
  }

  auto result = std::vector<vm::plane3d>{};
  for (size_t projectedAxis = 0; projectedAxis < 2; ++projectedAxis)
  {
    const auto normal = projectedAxisNormal(cutPlaneNormal, projectedAxis, gridRotation);
    if (!normal)
    {
      continue;
    }

    for (const auto coordinate : coordinatesByProjectedAxis[projectedAxis])
    {
      result.emplace_back(coordinate, *normal);
    }
  }

  return result;
}

std::vector<mdl::Brush> PolygonCutTool::splitFragmentsAlongAxes(
  std::vector<mdl::Brush> fragments) const
{
  const auto splitPlanes = axisSplitPlanes();
  if (splitPlanes.empty())
  {
    return fragments;
  }

  const auto maxFragments = std::max(MaxAxisSplitFragments, fragments.size() + 4);
  for (const auto& splitPlane : splitPlanes)
  {
    auto nextFragments = std::vector<mdl::Brush>{};
    nextFragments.reserve(fragments.size() + 1);

    for (const auto& fragment : fragments)
    {
      if (const auto split = splitBrushByPlane(fragment, splitPlane))
      {
        nextFragments.push_back((*split)[0]);
        nextFragments.push_back((*split)[1]);
      }
      else
      {
        nextFragments.push_back(fragment);
      }

      if (nextFragments.size() > maxFragments)
      {
        return fragments;
      }
    }

    fragments = std::move(nextFragments);
  }

  return fragments;
}

std::optional<std::vector<mdl::Brush>> PolygonCutTool::splitBrushByPlane(
  const mdl::Brush& brush, const vm::plane3d& plane) const
{
  auto hasFront = false;
  auto hasBack = false;
  for (const auto* vertex : brush.vertices())
  {
    const auto distance = plane.point_distance(vertex->position());
    hasFront = hasFront || distance > CoordinateEpsilon;
    hasBack = hasBack || distance < -CoordinateEpsilon;
  }

  if (!hasFront || !hasBack)
  {
    return std::nullopt;
  }

  auto& map = m_document.map();
  const auto attributes = mdl::BrushFaceAttributes{map.currentMaterialName()};
  auto frontFace = createFaceForPlane(plane, attributes, map.worldNode().mapFormat());
  auto backFace =
    createFaceForPlane(plane.flip(), attributes, map.worldNode().mapFormat());
  if (!frontFace || !backFace)
  {
    return std::nullopt;
  }

  auto frontBrush = brush;
  auto backBrush = brush;
  const auto& worldBounds = map.worldBounds();
  const auto frontResult = frontBrush.clip(worldBounds, std::move(*frontFace));
  const auto backResult = backBrush.clip(worldBounds, std::move(*backFace));
  if (!frontResult || !backResult)
  {
    return std::nullopt;
  }

  return std::vector<mdl::Brush>{std::move(frontBrush), std::move(backBrush)};
}

bool PolygonCutTool::doActivate()
{
  if (!m_document.map().selection().hasOnlyBrushes())
  {
    return false;
  }

  connectObservers();
  reset();
  return true;
}

bool PolygonCutTool::doDeactivate()
{
  m_notifierConnection.disconnect();

  m_points.clear();
  m_cutPlane.reset();
  m_dragState = std::nullopt;
  m_dragging = false;

  return true;
}

QWidget* PolygonCutTool::doCreatePage(QWidget* parent)
{
  return new PolygonCutToolPage{*this, parent};
}

void PolygonCutTool::connectObservers()
{
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect([&]() { refresh(); });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&]() { refresh(); });
  m_notifierConnection +=
    m_document.selectionDidChangeNotifier.connect([&](const auto&) { refresh(); });
}

void PolygonCutTool::refresh()
{
  if (!m_ignoreNotifications)
  {
    if (!hasBrushes())
    {
      reset();
    }
    refreshViews();
  }
}

} // namespace tb::ui
