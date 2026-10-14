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

#include "ui/SubdivisionTool.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include "Logger.h"
#include "NotifierConnection.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/contracts.h"
#include "kd/map_utils.h"
#include "kd/result.h"

#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto MinSegments = 1;
constexpr auto MaxSegments = 64;
constexpr auto DefaultUSegments = 2;
constexpr auto DefaultVSegments = 2;
constexpr auto DefaultPlane = SubdivisionTool::Plane::XY;
constexpr auto MaxUndoSteps = 64u;
constexpr auto SubdivisionEpsilon = 0.01;
constexpr auto SurfacePanelMinThickness = 1.0;
constexpr auto SurfacePanelThicknessGridFactor = 0.125;
constexpr auto SurfaceAreaEpsilon = 0.01;
constexpr auto SmoothPointQuantum = 0.01;

struct SubdivisionBasis
{
  vm::vec3d u;
  vm::vec3d v;
  double uOrigin = 0.0;
  double vOrigin = 0.0;
  double uScale = 1.0;
  double vScale = 1.0;
};

struct Point2
{
  double x = 0.0;
  double y = 0.0;
};

struct GeneratedBrush
{
  mdl::Node* parent = nullptr;
  mdl::Brush brush;
};

struct PointKey
{
  long long x = 0;
  long long y = 0;
  long long z = 0;

  bool operator<(const PointKey& other) const
  {
    if (x != other.x)
    {
      return x < other.x;
    }
    if (y != other.y)
    {
      return y < other.y;
    }
    return z < other.z;
  }
};

double projected(const vm::vec3d& point, const vm::vec3d& axis)
{
  return vm::dot(point, axis);
}

std::pair<double, double> projectionRange(
  const std::vector<vm::vec3d>& points, const vm::vec3d& axis)
{
  auto minValue = std::numeric_limits<double>::max();
  auto maxValue = std::numeric_limits<double>::lowest();

  for (const auto& point : points)
  {
    const auto value = projected(point, axis);
    minValue = std::min(minValue, value);
    maxValue = std::max(maxValue, value);
  }

  return {minValue, maxValue};
}

std::optional<SubdivisionBasis> basisForPlane(const SubdivisionTool::Plane plane)
{
  switch (plane)
  {
  case SubdivisionTool::Plane::XY:
    return SubdivisionBasis{vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};
  case SubdivisionTool::Plane::XZ:
    return SubdivisionBasis{vm::vec3d{1, 0, 0}, vm::vec3d{0, 0, 1}};
  case SubdivisionTool::Plane::YZ:
    return SubdivisionBasis{vm::vec3d{0, 1, 0}, vm::vec3d{0, 0, 1}};
    switchDefault();
  }
}

std::optional<SubdivisionBasis> basisForFace(const mdl::BrushFace& face)
{
  const auto polygon = face.vertexPositions();
  if (polygon.size() < 3u)
  {
    return std::nullopt;
  }

  auto bestStart = polygon[0];
  auto bestEnd = polygon[1];
  auto bestLength = 0.0;
  for (size_t i = 0u; i < polygon.size(); ++i)
  {
    const auto& start = polygon[i];
    const auto& end = polygon[(i + 1u) % polygon.size()];
    const auto length = vm::squared_length(end - start);
    if (length > bestLength)
    {
      bestStart = start;
      bestEnd = end;
      bestLength = length;
    }
  }

  if (vm::is_zero(bestLength, vm::Cd::almost_zero()))
  {
    return std::nullopt;
  }

  const auto u = vm::normalize(bestEnd - bestStart);
  const auto v = vm::normalize(vm::cross(face.normal(), u));
  if (
    vm::is_zero(vm::squared_length(u), vm::Cd::almost_zero())
    || vm::is_zero(vm::squared_length(v), vm::Cd::almost_zero()))
  {
    return std::nullopt;
  }

  return SubdivisionBasis{u, v};
}

std::optional<SubdivisionBasis> basisForTexture(const mdl::BrushFace& face)
{
  const auto matrix =
    face.toUVCoordSystemMatrix(face.attributes().offset(), face.attributes().scale());
  const auto origin = matrix * vm::vec3d{0, 0, 0};
  const auto x = matrix * vm::vec3d{1, 0, 0} - origin;
  const auto y = matrix * vm::vec3d{0, 1, 0} - origin;
  const auto z = matrix * vm::vec3d{0, 0, 1} - origin;
  const auto rawU = vm::vec3d{x.x(), y.x(), z.x()};
  const auto rawV = vm::vec3d{x.y(), y.y(), z.y()};
  const auto normal = face.normal();
  const auto u = rawU - normal * vm::dot(rawU, normal);
  const auto v = rawV - normal * vm::dot(rawV, normal);
  const auto uLength = vm::length(u);
  const auto vLength = vm::length(v);
  if (
    !std::isfinite(uLength) || !std::isfinite(vLength) || !std::isfinite(origin.x())
    || !std::isfinite(origin.y()) || uLength < 0.000001 || vLength < 0.000001
    || vm::length(vm::cross(u / uLength, v / vLength)) < 0.000001)
  {
    return std::nullopt;
  }
  return SubdivisionBasis{
    u / uLength,
    v / vLength,
    -(origin.x() + vm::dot(rawU, normal) * face.boundary().distance) / uLength,
    -(origin.y() + vm::dot(rawV, normal) * face.boundary().distance) / vLength,
    1.0 / uLength,
    1.0 / vLength};
}

std::vector<double> subdivisionPositions(
  const double min,
  const double max,
  const int segments,
  const std::optional<double> step,
  const double origin)
{
  auto result = std::vector<double>{min};
  if (step)
  {
    const auto first = std::floor((min - origin) / *step) + 1.0;
    const auto last = std::ceil((max - origin) / *step) - 1.0;
    if (last - first > 65536.0)
    {
      return {};
    }
    for (auto i = first; i <= last; i += 1.0)
    {
      const auto position = origin + i * *step;
      if (position > min + SubdivisionEpsilon && position < max - SubdivisionEpsilon)
      {
        result.push_back(position);
      }
    }
  }
  else
  {
    for (int i = 1; i < segments; ++i)
    {
      result.push_back(min + (max - min) * i / segments);
    }
  }
  result.push_back(max);
  return result;
}

double cross(const Point2& lhs, const Point2& rhs)
{
  return lhs.x * rhs.y - lhs.y * rhs.x;
}

Point2 operator+(const Point2& lhs, const Point2& rhs)
{
  return {lhs.x + rhs.x, lhs.y + rhs.y};
}

Point2 operator-(const Point2& lhs, const Point2& rhs)
{
  return {lhs.x - rhs.x, lhs.y - rhs.y};
}

Point2 operator*(const Point2& point, const double scalar)
{
  return {point.x * scalar, point.y * scalar};
}

double polygonArea(const std::vector<Point2>& polygon)
{
  auto area = 0.0;
  for (size_t i = 0u; i < polygon.size(); ++i)
  {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1u) % polygon.size()];
    area += cross(a, b);
  }
  return area * 0.5;
}

void removeDuplicatePoints(std::vector<Point2>& polygon)
{
  auto result = std::vector<Point2>{};
  result.reserve(polygon.size());

  for (const auto& point : polygon)
  {
    if (
      result.empty()
      || std::hypot(result.back().x - point.x, result.back().y - point.y)
           > SubdivisionEpsilon)
    {
      result.push_back(point);
    }
  }

  if (
    result.size() > 1u
    && std::hypot(result.front().x - result.back().x, result.front().y - result.back().y)
         <= SubdivisionEpsilon)
  {
    result.pop_back();
  }

  polygon = std::move(result);
}

std::vector<Point2> clipPolygonAgainstEdge(
  const std::vector<Point2>& subject,
  const Point2& a,
  const Point2& b,
  const double orientation)
{
  if (subject.empty())
  {
    return {};
  }

  const auto edge = b - a;
  const auto inside = [&](const Point2& point) {
    return orientation * cross(edge, point - a) >= -SubdivisionEpsilon;
  };
  const auto intersection = [&](const Point2& start, const Point2& end) {
    const auto segment = end - start;
    const auto denominator = cross(edge, segment);
    if (std::abs(denominator) <= vm::Cd::almost_zero())
    {
      return end;
    }
    const auto t = cross(edge, a - start) / denominator;
    return start + segment * std::clamp(t, 0.0, 1.0);
  };

  auto result = std::vector<Point2>{};
  result.reserve(subject.size() + 1u);

  auto previous = subject.back();
  auto previousInside = inside(previous);
  for (const auto& current : subject)
  {
    const auto currentInside = inside(current);
    if (currentInside)
    {
      if (!previousInside)
      {
        result.push_back(intersection(previous, current));
      }
      result.push_back(current);
    }
    else if (previousInside)
    {
      result.push_back(intersection(previous, current));
    }

    previous = current;
    previousInside = currentInside;
  }

  removeDuplicatePoints(result);
  return result;
}

std::vector<Point2> clipPolygonToConvexPolygon(
  std::vector<Point2> subject, const std::vector<Point2>& clipPolygon)
{
  if (clipPolygon.size() < 3u)
  {
    return {};
  }

  const auto orientation = polygonArea(clipPolygon) >= 0.0 ? 1.0 : -1.0;
  for (size_t i = 0u; i < clipPolygon.size(); ++i)
  {
    subject = clipPolygonAgainstEdge(
      subject, clipPolygon[i], clipPolygon[(i + 1u) % clipPolygon.size()], orientation);
    if (subject.size() < 3u)
    {
      return {};
    }
  }

  return subject;
}

Point2 projectToBasis(const vm::vec3d& point, const SubdivisionBasis& basis)
{
  return {vm::dot(point, basis.u), vm::dot(point, basis.v)};
}

vm::vec3d unprojectFromBasis(
  const Point2& point,
  const SubdivisionBasis& basis,
  const vm::vec3d& normal,
  const double normalDistance)
{
  return basis.u * point.x + basis.v * point.y + normal * normalDistance;
}

std::optional<mdl::Brush> createSurfacePanelBrush(
  const mdl::BrushBuilder& builder,
  const mdl::Brush& sourceBrush,
  const mdl::BrushFace& sourceFace,
  const SubdivisionBasis& basis,
  const std::vector<Point2>& polygon,
  const double thickness,
  const std::string& materialName)
{
  if (polygon.size() < 3u || std::abs(polygonArea(polygon)) <= SurfaceAreaEpsilon)
  {
    return std::nullopt;
  }

  const auto normal = vm::normalize(sourceFace.normal());
  const auto normalDistance = vm::dot(sourceFace.vertexPositions().front(), normal);
  auto points = std::vector<vm::vec3d>{};
  points.reserve(polygon.size() * 2u);

  for (const auto& point : polygon)
  {
    points.push_back(
      unprojectFromBasis(point, basis, normal, normalDistance) + normal * thickness);
  }
  for (const auto& point : polygon)
  {
    points.push_back(unprojectFromBasis(point, basis, normal, normalDistance));
  }

  auto result = builder.createBrush(points, materialName);
  if (!result)
  {
    return std::nullopt;
  }

  auto brush = std::move(result).value();
  brush.cloneFaceAttributesFrom(sourceBrush);
  return brush;
}

std::vector<mdl::Brush> subdivideFaceSurface(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const mdl::BrushFace& sourceFace,
  const SubdivisionBasis& basis,
  const int uSegments,
  const int vSegments)
{
  const auto sourcePoints = sourceFace.vertexPositions();
  if (sourcePoints.size() < 3u)
  {
    return {};
  }

  auto facePolygon = std::vector<Point2>{};
  facePolygon.reserve(sourcePoints.size());
  for (const auto& point : sourcePoints)
  {
    facePolygon.push_back(projectToBasis(point, basis));
  }

  removeDuplicatePoints(facePolygon);
  if (facePolygon.size() < 3u)
  {
    return {};
  }

  auto uMin = std::numeric_limits<double>::max();
  auto uMax = std::numeric_limits<double>::lowest();
  auto vMin = std::numeric_limits<double>::max();
  auto vMax = std::numeric_limits<double>::lowest();
  for (const auto& point : facePolygon)
  {
    uMin = std::min(uMin, point.x);
    uMax = std::max(uMax, point.x);
    vMin = std::min(vMin, point.y);
    vMax = std::max(vMax, point.y);
  }

  const auto uCount = std::max(uSegments, 1);
  const auto vCount = std::max(vSegments, 1);
  const auto uExtent = uMax - uMin;
  const auto vExtent = vMax - vMin;
  if (uExtent <= SubdivisionEpsilon || vExtent <= SubdivisionEpsilon)
  {
    return {};
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};
  const auto materialName = sourceFace.attributes().materialName().empty()
                              ? map.currentMaterialName()
                              : sourceFace.attributes().materialName();
  const auto thickness = std::max(
    SurfacePanelMinThickness, map.grid().actualSize() * SurfacePanelThicknessGridFactor);

  auto result = std::vector<mdl::Brush>{};
  result.reserve(static_cast<size_t>(uCount * vCount));

  for (int uIndex = 0; uIndex < uCount; ++uIndex)
  {
    const auto uStart = uMin + uExtent * static_cast<double>(uIndex) / uCount;
    const auto uEnd = uMin + uExtent * static_cast<double>(uIndex + 1) / uCount;

    for (int vIndex = 0; vIndex < vCount; ++vIndex)
    {
      const auto vStart = vMin + vExtent * static_cast<double>(vIndex) / vCount;
      const auto vEnd = vMin + vExtent * static_cast<double>(vIndex + 1) / vCount;
      auto cell = std::vector<Point2>{
        {uStart, vStart}, {uEnd, vStart}, {uEnd, vEnd}, {uStart, vEnd}};
      cell = clipPolygonToConvexPolygon(std::move(cell), facePolygon);
      if (cell.size() < 3u)
      {
        continue;
      }

      if (
        auto brush = createSurfacePanelBrush(
          builder, sourceBrush, sourceFace, basis, cell, thickness, materialName))
      {
        result.push_back(std::move(*brush));
      }
    }
  }

  return result;
}

PointKey pointKey(const vm::vec3d& point)
{
  return PointKey{
    static_cast<long long>(std::llround(point.x() / SmoothPointQuantum)),
    static_cast<long long>(std::llround(point.y() / SmoothPointQuantum)),
    static_cast<long long>(std::llround(point.z() / SmoothPointQuantum))};
}

std::vector<vm::vec3d> uniquePoints(const std::vector<vm::vec3d>& points)
{
  auto result = std::vector<vm::vec3d>{};
  auto keys = std::set<PointKey>{};
  result.reserve(points.size());

  for (const auto& point : points)
  {
    if (keys.insert(pointKey(point)).second)
    {
      result.push_back(point);
    }
  }

  return result;
}

std::string materialNameForBrush(const mdl::Brush& brush, mdl::Map& map)
{
  return brush.faceCount() > 0u && !brush.face(0).attributes().materialName().empty()
           ? brush.face(0).attributes().materialName()
           : map.currentMaterialName();
}

void smoothGeneratedBrushes(
  mdl::Map& map, std::vector<GeneratedBrush>& generatedBrushes, const double intensity)
{
  const auto factor = std::clamp(intensity, 0.0, 1.0);
  if (factor <= 0.0 || generatedBrushes.empty())
  {
    return;
  }

  struct Cluster
  {
    vm::vec3d position;
    std::set<size_t> neighbours;
  };

  auto clusters = std::vector<Cluster>{};
  auto clusterByPoint = std::map<PointKey, size_t>{};
  const auto clusterIndex = [&](const vm::vec3d& point) {
    const auto key = pointKey(point);
    if (const auto it = clusterByPoint.find(key); it != std::end(clusterByPoint))
    {
      return it->second;
    }

    const auto index = clusters.size();
    clusters.push_back(Cluster{point, {}});
    clusterByPoint.emplace(key, index);
    return index;
  };

  for (const auto& generated : generatedBrushes)
  {
    for (const auto& point : generated.brush.vertexPositions())
    {
      clusterIndex(point);
    }

    for (const auto* edge : generated.brush.edges())
    {
      const auto first = clusterIndex(edge->firstVertex()->position());
      const auto second = clusterIndex(edge->secondVertex()->position());
      if (first != second)
      {
        clusters[first].neighbours.insert(second);
        clusters[second].neighbours.insert(first);
      }
    }
  }

  auto smoothedPositions = std::vector<vm::vec3d>{};
  smoothedPositions.reserve(clusters.size());
  for (const auto& cluster : clusters)
  {
    if (cluster.neighbours.empty())
    {
      smoothedPositions.push_back(cluster.position);
      continue;
    }

    auto average = vm::vec3d{0, 0, 0};
    for (const auto neighbour : cluster.neighbours)
    {
      average = average + clusters[neighbour].position;
    }
    average = average / static_cast<double>(cluster.neighbours.size());
    smoothedPositions.push_back(cluster.position + (average - cluster.position) * factor);
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  for (auto& generated : generatedBrushes)
  {
    auto points = std::vector<vm::vec3d>{};
    points.reserve(generated.brush.vertexCount());
    for (const auto& point : generated.brush.vertexPositions())
    {
      if (
        const auto it = clusterByPoint.find(pointKey(point));
        it != std::end(clusterByPoint))
      {
        points.push_back(smoothedPositions[it->second]);
      }
      else
      {
        points.push_back(point);
      }
    }

    points = uniquePoints(points);
    if (points.size() < 4u)
    {
      continue;
    }

    const auto sourceBrush = generated.brush;
    auto result = builder.createBrush(points, materialNameForBrush(sourceBrush, map));
    if (!result)
    {
      continue;
    }

    auto brush = std::move(result).value();
    brush.cloneFaceAttributesFrom(sourceBrush);
    generated.brush = std::move(brush);
  }
}

std::optional<mdl::BrushFace> createClipFace(
  const mdl::Map& map, const vm::vec3d& axis, const double position, const bool flipped)
{
  const auto normal = vm::normalize(axis);
  const auto helper =
    std::abs(normal.z()) < 0.75 ? vm::vec3d{0, 0, 1} : vm::vec3d{1, 0, 0};
  auto u = vm::cross(helper, normal);
  if (vm::is_zero(vm::squared_length(u), vm::Cd::almost_zero()))
  {
    u = vm::cross(vm::vec3d{0, 1, 0}, normal);
  }
  if (vm::is_zero(vm::squared_length(u), vm::Cd::almost_zero()))
  {
    return std::nullopt;
  }

  u = vm::normalize(u);
  const auto v = vm::normalize(vm::cross(normal, u));
  const auto origin = normal * position;

  auto p0 = origin;
  auto p1 = origin + u;
  auto p2 = origin + v;
  if (flipped)
  {
    std::swap(p1, p2);
  }

  auto face = mdl::BrushFace::create(
    p0,
    p1,
    p2,
    mdl::BrushFaceAttributes{map.currentMaterialName()},
    map.worldNode().mapFormat());
  if (!face)
  {
    return std::nullopt;
  }

  return std::move(face).value();
}

std::optional<mdl::Brush> clipBrushToProjectionLimit(
  const mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const vm::vec3d& axis,
  const double position,
  const bool keepGreater)
{
  for (const auto flipped : {false, true})
  {
    auto brush = sourceBrush;
    auto face = createClipFace(map, axis, position, flipped);
    if (!face)
    {
      continue;
    }
    if (!brush.clip(map.worldBounds(), std::move(*face)))
    {
      continue;
    }

    const auto [minValue, maxValue] = projectionRange(brush.vertexPositions(), axis);
    if (
      keepGreater ? minValue >= position - SubdivisionEpsilon
                  : maxValue <= position + SubdivisionEpsilon)
    {
      brush.cloneFaceAttributesFrom(sourceBrush);
      return brush;
    }
  }

  return std::nullopt;
}

std::vector<mdl::Brush> subdivideBrush(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const SubdivisionBasis& basis,
  const int uSegments,
  const int vSegments,
  const std::optional<double> cellSize = std::nullopt)
{
  const auto sourcePoints = sourceBrush.vertexPositions();
  if (sourcePoints.empty())
  {
    return {};
  }

  const auto [uMin, uMax] = projectionRange(sourcePoints, basis.u);
  const auto [vMin, vMax] = projectionRange(sourcePoints, basis.v);
  const auto uPositions = subdivisionPositions(
    uMin,
    uMax,
    std::max(uSegments, 1),
    cellSize ? std::optional{*cellSize * basis.uScale} : std::nullopt,
    basis.uOrigin);
  const auto vPositions = subdivisionPositions(
    vMin,
    vMax,
    std::max(vSegments, 1),
    cellSize ? std::optional{*cellSize * basis.vScale} : std::nullopt,
    basis.vOrigin);
  if (
    uPositions.empty() || vPositions.empty()
    || (uPositions.size() - 1) * (vPositions.size() - 1) > 65536u)
  {
    map.logger().error()
      << "Subdivision exceeds 65536 cells per brush; increase cell size";
    return {};
  }
  const auto uCount = static_cast<int>(uPositions.size() - 1);
  const auto vCount = static_cast<int>(vPositions.size() - 1);
  const auto uExtent = uMax - uMin;
  const auto vExtent = vMax - vMin;
  if (uExtent <= SubdivisionEpsilon || vExtent <= SubdivisionEpsilon)
  {
    return {sourceBrush};
  }

  auto result = std::vector<mdl::Brush>{};
  result.reserve(static_cast<size_t>(uCount * vCount));

  for (int uIndex = 0; uIndex < uCount; ++uIndex)
  {
    const auto uStart = uPositions[uIndex];
    const auto uEnd = uPositions[uIndex + 1];

    for (int vIndex = 0; vIndex < vCount; ++vIndex)
    {
      const auto vStart = vPositions[vIndex];
      const auto vEnd = vPositions[vIndex + 1];

      auto cell = sourceBrush;
      if (uIndex > 0)
      {
        if (auto clipped = clipBrushToProjectionLimit(map, cell, basis.u, uStart, true))
        {
          cell = std::move(*clipped);
        }
        else
        {
          continue;
        }
      }
      if (uIndex + 1 < uCount)
      {
        if (auto clipped = clipBrushToProjectionLimit(map, cell, basis.u, uEnd, false))
        {
          cell = std::move(*clipped);
        }
        else
        {
          continue;
        }
      }
      if (vIndex > 0)
      {
        if (auto clipped = clipBrushToProjectionLimit(map, cell, basis.v, vStart, true))
        {
          cell = std::move(*clipped);
        }
        else
        {
          continue;
        }
      }
      if (vIndex + 1 < vCount)
      {
        if (auto clipped = clipBrushToProjectionLimit(map, cell, basis.v, vEnd, false))
        {
          cell = std::move(*clipped);
        }
        else
        {
          continue;
        }
      }

      cell.cloneFaceAttributesFrom(sourceBrush);
      result.push_back(std::move(cell));
    }
  }

  return result;
}

std::vector<vm::plane3d> subdivisionPlanesForFace(
  const mdl::BrushFace& face,
  const SubdivisionBasis& basis,
  const int uSegments,
  const int vSegments)
{
  const auto points = face.vertexPositions();
  if (points.size() < 3u)
  {
    return {};
  }

  const auto [uMin, uMax] = projectionRange(points, basis.u);
  const auto [vMin, vMax] = projectionRange(points, basis.v);
  const auto uCount = std::max(uSegments, 1);
  const auto vCount = std::max(vSegments, 1);
  const auto uExtent = uMax - uMin;
  const auto vExtent = vMax - vMin;
  if (uExtent <= SubdivisionEpsilon || vExtent <= SubdivisionEpsilon)
  {
    return {};
  }

  auto result = std::vector<vm::plane3d>{};
  result.reserve(static_cast<size_t>(std::max(0, uCount - 1) + std::max(0, vCount - 1)));

  for (int i = 1; i < uCount; ++i)
  {
    const auto position = uMin + uExtent * static_cast<double>(i) / uCount;
    result.push_back(vm::plane3d{position, basis.u});
  }

  for (int i = 1; i < vCount; ++i)
  {
    const auto position = vMin + vExtent * static_cast<double>(i) / vCount;
    result.push_back(vm::plane3d{position, basis.v});
  }

  return result;
}

class SubdivisionToolPage : public QWidget
{
private:
  SubdivisionTool& m_tool;
  NotifierConnection m_notifierConnection;
  QSpinBox* m_uSegments = nullptr;
  QSpinBox* m_vSegments = nullptr;
  QComboBox* m_plane = nullptr;
  QComboBox* m_mode = nullptr;
  QComboBox* m_alignment = nullptr;
  QDoubleSpinBox* m_cellSize = nullptr;
  QPushButton* m_default = nullptr;
  QPushButton* m_apply = nullptr;

public:
  explicit SubdivisionToolPage(SubdivisionTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  static QLabel* label(const QString& text) { return new QLabel{text}; }

  void createGui()
  {
    m_mode = new QComboBox{};
    m_mode->addItems({tr("Segments"), tr("Grid size")});
    m_alignment = new QComboBox{};
    m_alignment->addItems({tr("World grid"), tr("Texture pivot")});
    m_cellSize = new QDoubleSpinBox{};
    m_cellSize->setRange(0.125, 65536.0);
    m_cellSize->setDecimals(3);
    m_cellSize->setSingleStep(8.0);
    m_cellSize->setKeyboardTracking(false);
    m_cellSize->setToolTip(
      tr("Square cell size in world units, or texels with Texture pivot alignment"));
    connect(m_mode, qOverload<int>(&QComboBox::activated), this, [&](const int index) {
      m_tool.setMode(static_cast<SubdivisionTool::Mode>(index));
    });
    connect(
      m_alignment, qOverload<int>(&QComboBox::activated), this, [&](const int index) {
        m_tool.setAlignment(static_cast<SubdivisionTool::Alignment>(index));
      });
    connect(
      m_cellSize,
      qOverload<double>(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setCellSize(value); });
    m_uSegments = new QSpinBox{};
    m_uSegments->setRange(MinSegments, MaxSegments);
    m_uSegments->setSingleStep(1);
    m_uSegments->setToolTip(tr("Number of subdivisions along the first grid direction"));

    m_vSegments = new QSpinBox{};
    m_vSegments->setRange(MinSegments, MaxSegments);
    m_vSegments->setSingleStep(1);
    m_vSegments->setToolTip(tr("Number of subdivisions along the second grid direction"));

    m_plane = new QComboBox{};
    m_plane->addItem(tr("XY"));
    m_plane->addItem(tr("XZ"));
    m_plane->addItem(tr("YZ"));
    m_plane->setToolTip(tr("Subdivision plane used when whole brushes are selected"));

    m_default = new QPushButton{tr("Default")};
    m_apply = new QPushButton{tr("Apply")};

    connect(
      m_uSegments,
      static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setUSegments(value); });
    connect(
      m_vSegments,
      static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setVSegments(value); });
    connect(
      m_plane,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int index) {
        m_tool.setPlane(
          index == 0   ? SubdivisionTool::Plane::XY
          : index == 1 ? SubdivisionTool::Plane::XZ
                       : SubdivisionTool::Plane::YZ);
      });
    connect(m_default, &QPushButton::clicked, this, [&]() { m_tool.resetSettings(); });
    connect(m_apply, &QPushButton::clicked, this, [&]() { m_tool.apply(); });

    m_notifierConnection +=
      m_tool.settingsDidChangeNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolActivatedNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolDeactivatedNotifier.connect([&](auto&) { updateControls(); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label(tr("Subdivision")), 0, Qt::AlignVCenter);
    layout->addWidget(m_mode, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Cell")), 0, Qt::AlignVCenter);
    layout->addWidget(m_cellSize, 0, Qt::AlignVCenter);
    layout->addWidget(m_alignment, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("U")), 0, Qt::AlignVCenter);
    layout->addWidget(m_uSegments, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("V")), 0, Qt::AlignVCenter);
    layout->addWidget(m_vSegments, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Plane")), 0, Qt::AlignVCenter);
    layout->addWidget(m_plane, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    layout->addWidget(m_default, 0, Qt::AlignVCenter);
    layout->addWidget(m_apply, 0, Qt::AlignVCenter);
    setLayout(layout);

    updateControls();
  }

  void updateControls()
  {
    {
      const auto modeBlocker = QSignalBlocker{m_mode};
      const auto alignBlocker = QSignalBlocker{m_alignment};
      const auto sizeBlocker = QSignalBlocker{m_cellSize};
      const auto gridMode = m_tool.mode() == SubdivisionTool::Mode::GridSize;
      m_mode->setCurrentIndex(static_cast<int>(m_tool.mode()));
      m_alignment->setCurrentIndex(static_cast<int>(m_tool.alignment()));
      m_cellSize->setValue(m_tool.cellSize());
      m_alignment->setEnabled(gridMode);
      m_cellSize->setEnabled(gridMode);
    }
    {
      const auto blocker = QSignalBlocker{m_uSegments};
      m_uSegments->setValue(m_tool.uSegments());
    }
    {
      const auto blocker = QSignalBlocker{m_vSegments};
      m_vSegments->setValue(m_tool.vSegments());
    }
    {
      const auto blocker = QSignalBlocker{m_plane};
      m_plane->setCurrentIndex(
        m_tool.plane() == SubdivisionTool::Plane::XY   ? 0
        : m_tool.plane() == SubdivisionTool::Plane::XZ ? 1
                                                       : 2);
    }
    const auto active = m_tool.active();
    m_uSegments->setEnabled(active && m_tool.mode() == SubdivisionTool::Mode::Segments);
    m_vSegments->setEnabled(active && m_tool.mode() == SubdivisionTool::Mode::Segments);
    m_plane->setEnabled(active);
    m_default->setEnabled(active);
    m_apply->setEnabled(active && m_tool.canAttachToSelection());
  }
};

} // namespace

SubdivisionTool::SubdivisionTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

SubdivisionTool::~SubdivisionTool() = default;

bool SubdivisionTool::Settings::operator==(const Settings& other) const
{
  return uSegments == other.uSegments && vSegments == other.vSegments
         && plane == other.plane && mode == other.mode && alignment == other.alignment
         && cellSize == other.cellSize;
}

bool SubdivisionTool::canAttachToSelection() const
{
  if (active())
  {
    return !m_sourceTargets.empty();
  }

  const auto& selection = m_document.map().selection();
  return selection.hasBrushFaces() || !selection.allBrushes().empty();
}

bool SubdivisionTool::apply()
{
  auto& map = m_document.map();
  if (!m_previewTransactionOpen)
  {
    return false;
  }

  if (!m_previewHasChanges)
  {
    map.cancelTransaction();
    m_previewTransactionOpen = false;
    return false;
  }

  const auto nodesToSelect = m_previewNodes;
  if (!nodesToSelect.empty())
  {
    mdl::selectNodes(map, nodesToSelect);
  }
  m_previewNodes.clear();
  m_previewHasChanges = false;
  m_previewTransactionOpen = false;

  if (!map.commitTransaction())
  {
    return false;
  }

  m_sourceTargets.clear();
  m_undoStack.clear();
  refreshViews();
  subdivisionDidRequestDeactivateNotifier(*this);
  return true;
}

bool SubdivisionTool::undoStep()
{
  if (!active() || m_undoStack.empty())
  {
    return false;
  }

  const auto settings = m_undoStack.back();
  m_undoStack.pop_back();
  applySettings(settings);
  updatePreview();
  notifySettingsChanged();
  return true;
}

void SubdivisionTool::resetSettings()
{
  const auto defaults = Settings{DefaultUSegments, DefaultVSegments, DefaultPlane};
  if (currentSettings() == defaults)
  {
    return;
  }

  saveUndoState();
  applySettings(defaults);
  updatePreview();
  notifySettingsChanged();
}

int SubdivisionTool::uSegments() const
{
  return m_uSegments;
}

void SubdivisionTool::setUSegments(const int segments)
{
  const auto clamped = std::clamp(segments, MinSegments, MaxSegments);
  if (m_uSegments == clamped)
  {
    return;
  }

  saveUndoState();
  m_uSegments = clamped;
  updatePreview();
  notifySettingsChanged();
}

int SubdivisionTool::vSegments() const
{
  return m_vSegments;
}

void SubdivisionTool::setVSegments(const int segments)
{
  const auto clamped = std::clamp(segments, MinSegments, MaxSegments);
  if (m_vSegments == clamped)
  {
    return;
  }

  saveUndoState();
  m_vSegments = clamped;
  updatePreview();
  notifySettingsChanged();
}

SubdivisionTool::Plane SubdivisionTool::plane() const
{
  return m_plane;
}

void SubdivisionTool::setPlane(const Plane plane)
{
  if (m_plane == plane)
  {
    return;
  }

  saveUndoState();
  m_plane = plane;
  updatePreview();
  notifySettingsChanged();
}

SubdivisionTool::Settings SubdivisionTool::currentSettings() const
{
  return Settings{m_uSegments, m_vSegments, m_plane, m_mode, m_alignment, m_cellSize};
}

void SubdivisionTool::applySettings(const Settings& settings)
{
  m_uSegments = settings.uSegments;
  m_vSegments = settings.vSegments;
  m_plane = settings.plane;
  m_mode = settings.mode;
  m_alignment = settings.alignment;
  m_cellSize = settings.cellSize;
}

SubdivisionTool::Mode SubdivisionTool::mode() const
{
  return m_mode;
}

void SubdivisionTool::setMode(const Mode mode)
{
  if (m_mode == mode)
  {
    return;
  }
  saveUndoState();
  m_mode = mode;
  updatePreview();
  notifySettingsChanged();
}

SubdivisionTool::Alignment SubdivisionTool::alignment() const
{
  return m_alignment;
}

void SubdivisionTool::setAlignment(const Alignment alignment)
{
  if (m_alignment == alignment)
  {
    return;
  }
  saveUndoState();
  m_alignment = alignment;
  updatePreview();
  notifySettingsChanged();
}

double SubdivisionTool::cellSize() const
{
  return m_cellSize;
}

void SubdivisionTool::setCellSize(const double cellSize)
{
  const auto clamped = std::clamp(cellSize, 0.125, 65536.0);
  if (m_cellSize == clamped)
  {
    return;
  }
  saveUndoState();
  m_cellSize = clamped;
  updatePreview();
  notifySettingsChanged();
}

void SubdivisionTool::saveUndoState()
{
  if (!active())
  {
    return;
  }

  const auto settings = currentSettings();
  if (!m_undoStack.empty() && m_undoStack.back() == settings)
  {
    return;
  }

  if (m_undoStack.size() >= MaxUndoSteps)
  {
    m_undoStack.erase(m_undoStack.begin());
  }
  m_undoStack.push_back(settings);
}

void SubdivisionTool::notifySettingsChanged()
{
  settingsDidChangeNotifier(*this);
}

void SubdivisionTool::collectSourceTargets()
{
  m_sourceTargets.clear();

  const auto& selection = m_document.map().selection();
  if (selection.hasBrushFaces())
  {
    for (const auto& handle : selection.brushFaces)
    {
      auto* node = handle.node();
      if (node == nullptr || handle.faceIndex() >= node->brush().faceCount())
      {
        continue;
      }

      auto it = std::ranges::find_if(
        m_sourceTargets, [&](const auto& target) { return target.node == node; });
      if (it == std::end(m_sourceTargets))
      {
        auto target = SourceTarget{};
        target.node = node;
        target.pieceFaceIndex = handle.faceIndex();
        target.faceIndices.push_back(handle.faceIndex());
        m_sourceTargets.push_back(std::move(target));
      }
      else
      {
        if (!it->pieceFaceIndex)
        {
          it->pieceFaceIndex = handle.faceIndex();
        }
        if (!kdl::vec_contains(it->faceIndices, handle.faceIndex()))
        {
          it->faceIndices.push_back(handle.faceIndex());
        }
      }
    }
  }
  else
  {
    for (auto* brushNode : selection.allBrushes())
    {
      if (brushNode != nullptr && brushNode->parent() != nullptr)
      {
        m_sourceTargets.push_back(SourceTarget{brushNode, {}, std::nullopt});
      }
    }
  }
}

bool SubdivisionTool::rebuildPreview()
{
  auto& map = m_document.map();
  m_previewHasChanges = false;
  if (m_sourceTargets.empty())
  {
    return false;
  }

  auto generatedBrushes = std::vector<GeneratedBrush>{};
  auto originalNodes = std::vector<mdl::Node*>{};
  originalNodes.reserve(m_sourceTargets.size());

  for (const auto& target : m_sourceTargets)
  {
    auto* brushNode = target.node;
    if (brushNode == nullptr || brushNode->parent() == nullptr)
    {
      continue;
    }

    auto basis = std::optional<SubdivisionBasis>{};
    if (target.pieceFaceIndex && *target.pieceFaceIndex < brushNode->brush().faceCount())
    {
      basis = basisForFace(brushNode->brush().face(*target.pieceFaceIndex));
    }
    if (!basis)
    {
      basis = basisForPlane(m_plane);
    }
    if (!basis)
    {
      continue;
    }

    if (m_mode == Mode::GridSize)
    {
      basis = basisForPlane(m_plane);
      if (m_alignment == Alignment::WorldGrid)
      {
        basis->u = map.grid().fromGridSpace(basis->u);
        basis->v = map.grid().fromGridSpace(basis->v);
        if (target.pieceFaceIndex)
        {
          const auto normal = brushNode->brush().face(*target.pieceFaceIndex).normal();
          auto axes = std::array{
            map.grid().fromGridSpace(vm::vec3d{1, 0, 0}),
            map.grid().fromGridSpace(vm::vec3d{0, 1, 0}),
            vm::vec3d{0, 0, 1}};
          std::ranges::sort(axes, [&](const auto& a, const auto& b) {
            return std::abs(vm::dot(a, normal)) < std::abs(vm::dot(b, normal));
          });
          basis->u = axes[0];
          basis->v = axes[1];
        }
      }
      else
      {
        auto index = target.pieceFaceIndex;
        if (!index)
        {
          const auto preferredNormal = vm::cross(basis->u, basis->v);
          auto best = -2.0;
          for (size_t i = 0; i < brushNode->brush().faceCount(); ++i)
          {
            const auto alignment =
              vm::dot(brushNode->brush().face(i).normal(), preferredNormal);
            if (alignment > best)
            {
              best = alignment;
              index = i;
            }
          }
        }
        basis = index ? basisForTexture(brushNode->brush().face(*index)) : std::nullopt;
        if (!basis)
        {
          return false;
        }
      }
    }

    auto brushes = subdivideBrush(
      map,
      brushNode->brush(),
      *basis,
      m_uSegments,
      m_vSegments,
      m_mode == Mode::GridSize ? std::optional{m_cellSize} : std::nullopt);
    if (brushes.empty())
    {
      return false;
    }
    for (auto& brush : brushes)
    {
      generatedBrushes.push_back(GeneratedBrush{brushNode->parent(), std::move(brush)});
    }
    originalNodes.push_back(brushNode);
  }

  if (generatedBrushes.empty())
  {
    return false;
  }

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  for (auto& generated : generatedBrushes)
  {
    if (generated.parent != nullptr)
    {
      nodesToAdd[generated.parent].push_back(
        new mdl::BrushNode{std::move(generated.brush)});
    }
  }

  if (nodesToAdd.empty())
  {
    return false;
  }

  mdl::deselectAll(map);
  m_previewNodes = mdl::addNodes(map, nodesToAdd);
  if (!originalNodes.empty())
  {
    mdl::removeNodes(map, originalNodes);
  }
  mdl::selectNodes(map, m_previewNodes);
  m_previewHasChanges = !m_previewNodes.empty();
  return !m_previewNodes.empty();
}

void SubdivisionTool::updatePreview()
{
  if (!m_previewTransactionOpen)
  {
    return;
  }

  m_document.map().rollbackTransaction();
  m_previewNodes.clear();
  m_previewHasChanges = false;
  rebuildPreview();
  refreshViews();
}

void SubdivisionTool::cancelPreview()
{
  if (m_previewTransactionOpen)
  {
    m_document.map().cancelTransaction();
    m_previewTransactionOpen = false;
  }

  m_previewHasChanges = false;
  m_previewNodes.clear();
  m_sourceTargets.clear();
  m_undoStack.clear();
  refreshViews();
}

bool SubdivisionTool::doActivate()
{
  m_undoStack.clear();
  collectSourceTargets();
  if (m_sourceTargets.empty())
  {
    return false;
  }

  auto& map = m_document.map();
  map.startTransaction("Subdivision Tool", mdl::TransactionScope::LongRunning);
  m_previewTransactionOpen = true;
  if (!rebuildPreview())
  {
    cancelPreview();
    return false;
  }

  notifySettingsChanged();
  return true;
}

bool SubdivisionTool::doDeactivate()
{
  cancelPreview();
  notifySettingsChanged();
  return true;
}

QWidget* SubdivisionTool::doCreatePage(QWidget* parent)
{
  return new SubdivisionToolPage{*this, parent};
}

} // namespace tb::ui
