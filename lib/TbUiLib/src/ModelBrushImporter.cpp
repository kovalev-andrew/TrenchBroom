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

#include "ui/ModelBrushImporter.h"

#include "Error.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto DuplicateVertexDistanceSquared = 0.000000000001;
constexpr auto MinBrushEdgeLength = 0.011;
constexpr auto MaxCoplanarDistance = 0.001;
constexpr auto MinNormalLengthSquared = 0.000000000000000000000001;
constexpr auto MinConvexTurn = 0.000000000001;
constexpr auto SmallModelTargetSize = 64.0;
constexpr auto SmallModelSizeThreshold = 16.0;
constexpr auto MaxSimplifiedBrushes = size_t{4096u};
constexpr auto SimplifiedVoxelResolutions = std::array{32, 24, 16, 12};

struct MeshPolygon
{
  std::vector<unsigned int> indices;
  std::vector<vm::vec3d> points;
};

struct VoxelRun
{
  size_t x = 0u;
  size_t y = 0u;
  size_t z = 0u;
  size_t width = 0u;
  size_t depth = 0u;
  size_t height = 0u;
};

struct VoxelGrid
{
  vm::vec3d origin = vm::vec3d{0, 0, 0};
  double cellSize = 1.0;
  size_t xCount = 0u;
  size_t yCount = 0u;
  size_t zCount = 0u;
  std::vector<std::uint8_t> occupied;

  size_t index(const size_t x, const size_t y, const size_t z) const
  {
    return (z * yCount + y) * xCount + x;
  }

  bool isOccupied(const size_t x, const size_t y, const size_t z) const
  {
    return occupied[index(x, y, z)] != 0u;
  }

  void setOccupied(const size_t x, const size_t y, const size_t z)
  {
    occupied[index(x, y, z)] = 1u;
  }

  vm::vec3d cellMin(const size_t x, const size_t y, const size_t z) const
  {
    return origin + vm::vec3d{double(x), double(y), double(z)} * cellSize;
  }

  vm::vec3d cellCenter(const size_t x, const size_t y, const size_t z) const
  {
    return origin
           + vm::vec3d{double(x) + 0.5, double(y) + 0.5, double(z) + 0.5} * cellSize;
  }
};

struct SimplifiedBrushResult
{
  size_t brushCount = 0u;
  int resolution = 0;
};

struct EdgeKey
{
  unsigned int first;
  unsigned int second;

  EdgeKey(const unsigned int a, const unsigned int b)
    : first{std::min(a, b)}
    , second{std::max(a, b)}
  {
  }

  bool operator==(const EdgeKey& other) const
  {
    return first == other.first && second == other.second;
  }
};

struct EdgeKeyHash
{
  size_t operator()(const EdgeKey& key) const
  {
    return (size_t{key.first} << 32u) ^ size_t { key.second };
  }
};

vm::vec3d aiVecToVm(const aiVector3D& v)
{
  return {double(v.x), double(v.y), double(v.z)};
}

vm::vec3d snapNearGrid(const mdl::Grid& grid, const vm::vec3d& point)
{
  if (!grid.snap())
  {
    return point;
  }

  const auto snapped = grid.snap(point);
  const auto threshold = 0.000001;
  return vm::squared_distance(point, snapped) <= threshold * threshold ? snapped : point;
}

std::optional<vm::bbox3d> computeSceneBounds(const aiScene& scene)
{
  auto builder = vm::bbox3d::builder{};

  for (auto meshIndex = 0u; meshIndex < scene.mNumMeshes; ++meshIndex)
  {
    const auto* mesh = scene.mMeshes[meshIndex];
    if (mesh == nullptr || !mesh->HasPositions())
    {
      continue;
    }

    for (auto vertexIndex = 0u; vertexIndex < mesh->mNumVertices; ++vertexIndex)
    {
      builder.add(aiVecToVm(mesh->mVertices[vertexIndex]));
    }
  }

  return builder.initialized() ? std::optional{builder.bounds()} : std::nullopt;
}

double importScaleForBounds(const vm::bbox3d& bounds)
{
  const auto maxSize = vm::get_abs_max_component(bounds.size());
  if (maxSize > 0.0 && maxSize < SmallModelSizeThreshold)
  {
    return SmallModelTargetSize / maxSize;
  }
  return 1.0;
}

int clampVoxelIndex(const int value, const size_t count)
{
  return std::clamp(value, 0, static_cast<int>(count) - 1);
}

std::optional<VoxelGrid> createVoxelGrid(
  const vm::bbox3d& scaledBounds, const int targetLongestAxisCells)
{
  const auto maxSize = vm::get_abs_max_component(scaledBounds.size());
  if (maxSize <= 0.0)
  {
    return std::nullopt;
  }

  const auto cellSize = maxSize / double(targetLongestAxisCells);
  const auto padding = vm::vec3d{cellSize, cellSize, cellSize};
  const auto bounds = vm::bbox3d{scaledBounds.min - padding, scaledBounds.max + padding};
  const auto size = bounds.size();

  auto grid = VoxelGrid{};
  grid.origin = bounds.min;
  grid.cellSize = cellSize;
  grid.xCount = std::max(size_t{1u}, size_t(std::ceil(size.x() / cellSize)));
  grid.yCount = std::max(size_t{1u}, size_t(std::ceil(size.y() / cellSize)));
  grid.zCount = std::max(size_t{1u}, size_t(std::ceil(size.z() / cellSize)));
  grid.occupied.resize(grid.xCount * grid.yCount * grid.zCount, 0u);
  return grid;
}

vm::vec3d closestPointOnTriangle(
  const vm::vec3d& point, const vm::vec3d& a, const vm::vec3d& b, const vm::vec3d& c)
{
  const auto ab = b - a;
  const auto ac = c - a;
  const auto ap = point - a;
  const auto d1 = vm::dot(ab, ap);
  const auto d2 = vm::dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0)
  {
    return a;
  }

  const auto bp = point - b;
  const auto d3 = vm::dot(ab, bp);
  const auto d4 = vm::dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3)
  {
    return b;
  }

  const auto vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
  {
    const auto v = d1 / (d1 - d3);
    return a + ab * v;
  }

  const auto cp = point - c;
  const auto d5 = vm::dot(ab, cp);
  const auto d6 = vm::dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6)
  {
    return c;
  }

  const auto vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
  {
    const auto w = d2 / (d2 - d6);
    return a + ac * w;
  }

  const auto va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
  {
    const auto w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return b + (c - b) * w;
  }

  const auto denom = 1.0 / (va + vb + vc);
  const auto v = vb * denom;
  const auto w = vc * denom;
  return a + ab * v + ac * w;
}

void markTriangleVoxels(
  VoxelGrid& grid, const vm::vec3d& a, const vm::vec3d& b, const vm::vec3d& c)
{
  if (vm::squared_length(vm::cross(b - a, c - a)) <= MinNormalLengthSquared)
  {
    return;
  }

  const auto threshold = grid.cellSize * 0.9;
  const auto thresholdVector = vm::vec3d{threshold, threshold, threshold};
  const auto minPoint = vm::min(vm::min(a, b), c) - thresholdVector;
  const auto maxPoint = vm::max(vm::max(a, b), c) + thresholdVector;

  const auto minX = clampVoxelIndex(
    int(std::floor((minPoint.x() - grid.origin.x()) / grid.cellSize)), grid.xCount);
  const auto minY = clampVoxelIndex(
    int(std::floor((minPoint.y() - grid.origin.y()) / grid.cellSize)), grid.yCount);
  const auto minZ = clampVoxelIndex(
    int(std::floor((minPoint.z() - grid.origin.z()) / grid.cellSize)), grid.zCount);
  const auto maxX = clampVoxelIndex(
    int(std::floor((maxPoint.x() - grid.origin.x()) / grid.cellSize)), grid.xCount);
  const auto maxY = clampVoxelIndex(
    int(std::floor((maxPoint.y() - grid.origin.y()) / grid.cellSize)), grid.yCount);
  const auto maxZ = clampVoxelIndex(
    int(std::floor((maxPoint.z() - grid.origin.z()) / grid.cellSize)), grid.zCount);

  const auto thresholdSquared = threshold * threshold;
  for (auto z = minZ; z <= maxZ; ++z)
  {
    for (auto y = minY; y <= maxY; ++y)
    {
      for (auto x = minX; x <= maxX; ++x)
      {
        const auto center = grid.cellCenter(size_t(x), size_t(y), size_t(z));
        const auto closest = closestPointOnTriangle(center, a, b, c);
        if (vm::squared_distance(center, closest) <= thresholdSquared)
        {
          grid.setOccupied(size_t(x), size_t(y), size_t(z));
        }
      }
    }
  }
}

std::optional<VoxelGrid> voxelizeScene(
  const aiScene& scene,
  const vm::bbox3d& scaledBounds,
  const double importScale,
  const int resolution)
{
  auto grid = createVoxelGrid(scaledBounds, resolution);
  if (!grid)
  {
    return std::nullopt;
  }

  for (auto meshIndex = 0u; meshIndex < scene.mNumMeshes; ++meshIndex)
  {
    const auto* mesh = scene.mMeshes[meshIndex];
    if (mesh == nullptr || !mesh->HasPositions())
    {
      continue;
    }

    for (auto faceIndex = 0u; faceIndex < mesh->mNumFaces; ++faceIndex)
    {
      const auto& face = mesh->mFaces[faceIndex];
      if (face.mNumIndices < 3u)
      {
        continue;
      }

      const auto first = aiVecToVm(mesh->mVertices[face.mIndices[0]]) * importScale;
      for (auto i = 1u; i + 1u < face.mNumIndices; ++i)
      {
        const auto second = aiVecToVm(mesh->mVertices[face.mIndices[i]]) * importScale;
        const auto third =
          aiVecToVm(mesh->mVertices[face.mIndices[i + 1u]]) * importScale;
        markTriangleVoxels(*grid, first, second, third);
      }
    }
  }

  return grid;
}

bool voxelBlockIsFree(
  const VoxelGrid& grid,
  const std::vector<std::uint8_t>& visited,
  const size_t x,
  const size_t y,
  const size_t z,
  const size_t width,
  const size_t depth,
  const size_t height)
{
  for (auto dz = size_t{0u}; dz < height; ++dz)
  {
    for (auto dy = size_t{0u}; dy < depth; ++dy)
    {
      for (auto dx = size_t{0u}; dx < width; ++dx)
      {
        const auto index = grid.index(x + dx, y + dy, z + dz);
        if (grid.occupied[index] == 0u || visited[index] != 0u)
        {
          return false;
        }
      }
    }
  }

  return true;
}

std::vector<VoxelRun> mergeVoxelRuns(const VoxelGrid& grid)
{
  auto visited = std::vector<std::uint8_t>(grid.occupied.size(), 0u);
  auto runs = std::vector<VoxelRun>{};

  for (auto z = size_t{0u}; z < grid.zCount; ++z)
  {
    for (auto y = size_t{0u}; y < grid.yCount; ++y)
    {
      for (auto x = size_t{0u}; x < grid.xCount; ++x)
      {
        const auto startIndex = grid.index(x, y, z);
        if (grid.occupied[startIndex] == 0u || visited[startIndex] != 0u)
        {
          continue;
        }

        auto width = size_t{1u};
        while (x + width < grid.xCount
               && voxelBlockIsFree(grid, visited, x, y, z, width + 1u, 1u, 1u))
        {
          ++width;
        }

        auto depth = size_t{1u};
        while (y + depth < grid.yCount
               && voxelBlockIsFree(grid, visited, x, y, z, width, depth + 1u, 1u))
        {
          ++depth;
        }

        auto height = size_t{1u};
        while (z + height < grid.zCount
               && voxelBlockIsFree(grid, visited, x, y, z, width, depth, height + 1u))
        {
          ++height;
        }

        for (auto dz = size_t{0u}; dz < height; ++dz)
        {
          for (auto dy = size_t{0u}; dy < depth; ++dy)
          {
            for (auto dx = size_t{0u}; dx < width; ++dx)
            {
              visited[grid.index(x + dx, y + dy, z + dz)] = 1u;
            }
          }
        }

        runs.push_back({x, y, z, width, depth, height});
      }
    }
  }

  return runs;
}

vm::bbox3d boundsForVoxelRun(const VoxelGrid& grid, const VoxelRun& run)
{
  const auto min = grid.cellMin(run.x, run.y, run.z);
  const auto max = grid.cellMin(run.x + run.width, run.y + run.depth, run.z + run.height);
  return {min, max};
}

SimplifiedBrushResult addSimplifiedVoxelBrushes(
  mdl::GroupNode& group,
  const aiScene& scene,
  const vm::bbox3d& scaledSceneBounds,
  const double importScale,
  const vm::bbox3d& worldBounds,
  const mdl::MapFormat mapFormat,
  const std::string& materialName,
  const mdl::BrushFaceAttributes& defaultAttributes)
{
  auto selectedGrid = std::optional<VoxelGrid>{};
  auto selectedRuns = std::vector<VoxelRun>{};
  auto selectedResolution = 0;

  for (const auto resolution : SimplifiedVoxelResolutions)
  {
    auto grid = voxelizeScene(scene, scaledSceneBounds, importScale, resolution);
    if (!grid)
    {
      continue;
    }

    auto runs = mergeVoxelRuns(*grid);
    if (runs.empty())
    {
      continue;
    }

    selectedResolution = resolution;
    selectedGrid = std::move(grid);
    selectedRuns = std::move(runs);
    if (selectedRuns.size() <= MaxSimplifiedBrushes)
    {
      break;
    }
  }

  if (!selectedGrid || selectedRuns.empty())
  {
    return {};
  }

  const auto builder = mdl::BrushBuilder{mapFormat, worldBounds, defaultAttributes};
  auto brushCount = size_t{0u};
  const auto maxRunsToBuild = std::min(selectedRuns.size(), MaxSimplifiedBrushes);
  for (auto i = size_t{0u}; i < maxRunsToBuild; ++i)
  {
    auto brush = builder.createCuboid(
      boundsForVoxelRun(*selectedGrid, selectedRuns[i]), materialName);
    if (brush)
    {
      group.addChild(new mdl::BrushNode{std::move(brush).value()});
      ++brushCount;
    }
  }

  return {brushCount, selectedResolution};
}

std::vector<vm::vec3d> cleanPolygonPoints(const std::vector<vm::vec3d>& points)
{
  auto result = std::vector<vm::vec3d>{};
  result.reserve(points.size());

  for (const auto& point : points)
  {
    const auto duplicate = std::ranges::any_of(result, [&](const auto& existing) {
      return vm::squared_distance(point, existing) <= DuplicateVertexDistanceSquared;
    });
    if (!duplicate)
    {
      result.push_back(point);
    }
  }

  return result;
}

vm::vec3d polygonNormalVector(const std::vector<vm::vec3d>& points)
{
  auto normal = vm::vec3d{0.0, 0.0, 0.0};

  for (size_t i = 0; i < points.size(); ++i)
  {
    const auto& current = points[i];
    const auto& next = points[(i + 1u) % points.size()];
    normal[0] += (current[1] - next[1]) * (current[2] + next[2]);
    normal[1] += (current[2] - next[2]) * (current[0] + next[0]);
    normal[2] += (current[0] - next[0]) * (current[1] + next[1]);
  }

  return normal;
}

std::optional<vm::vec3d> polygonNormal(const std::vector<vm::vec3d>& points)
{
  const auto normal = polygonNormalVector(points);
  if (vm::squared_length(normal) <= MinNormalLengthSquared)
  {
    return std::nullopt;
  }

  return vm::normalize(normal);
}

double maxPointDistance(const std::vector<vm::vec3d>& points)
{
  auto result = 0.0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    for (size_t j = i + 1u; j < points.size(); ++j)
    {
      result = std::max(result, vm::distance(points[i], points[j]));
    }
  }
  return result;
}

bool hasSafePointDistances(const std::vector<vm::vec3d>& points)
{
  for (size_t i = 0; i < points.size(); ++i)
  {
    for (size_t j = i + 1u; j < points.size(); ++j)
    {
      const auto distance = vm::distance(points[i], points[j]);
      if (distance > 0.0 && distance < MinBrushEdgeLength)
      {
        return false;
      }
    }
  }
  return true;
}

bool hasSafePolygonHeight(
  const std::vector<vm::vec3d>& points, const vm::vec3d& normalVector)
{
  const auto maxDistance = maxPointDistance(points);
  if (maxDistance < MinBrushEdgeLength)
  {
    return false;
  }

  const auto projectedHeight = vm::length(normalVector) / maxDistance;
  return projectedHeight >= MinBrushEdgeLength;
}

std::optional<mdl::BrushFace> createInwardFace(
  const vm::vec3d& point0,
  const vm::vec3d& point1,
  const vm::vec3d& point2,
  const vm::vec3d& interiorPoint,
  const mdl::BrushFaceAttributes& attributes,
  const mdl::MapFormat mapFormat)
{
  if (const auto plane = vm::from_points(point0, point1, point2))
  {
    const auto normalPointsInward = vm::dot(plane->normal, interiorPoint - point0) >= 0.0;
    auto face = normalPointsInward
                  ? mdl::BrushFace::create(point0, point1, point2, attributes, mapFormat)
                  : mdl::BrushFace::create(point0, point2, point1, attributes, mapFormat);
    if (face)
    {
      return std::move(face).value();
    }
  }

  return std::nullopt;
}

bool addInwardFace(
  std::vector<mdl::BrushFace>& faces,
  const vm::vec3d& point0,
  const vm::vec3d& point1,
  const vm::vec3d& point2,
  const vm::vec3d& interiorPoint,
  const mdl::BrushFaceAttributes& attributes,
  const mdl::MapFormat mapFormat)
{
  if (
    const auto face =
      createInwardFace(point0, point1, point2, interiorPoint, attributes, mapFormat))
  {
    faces.push_back(*face);
    return true;
  }

  return false;
}

bool coplanarWith(
  const std::vector<vm::vec3d>& points, const vm::vec3d& origin, const vm::vec3d& normal)
{
  return std::ranges::all_of(points, [&](const auto& point) {
    return std::abs(vm::dot(point - origin, normal)) <= MaxCoplanarDistance;
  });
}

bool isConvexPolygon(const std::vector<vm::vec3d>& points, const vm::vec3d& normal)
{
  if (points.size() <= 3u)
  {
    return true;
  }

  auto sign = 0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    const auto& a = points[i];
    const auto& b = points[(i + 1u) % points.size()];
    const auto& c = points[(i + 2u) % points.size()];
    const auto turn = vm::dot(vm::cross(b - a, c - b), normal);

    if (std::abs(turn) <= MinConvexTurn)
    {
      continue;
    }

    const auto turnSign = turn > 0.0 ? 1 : -1;
    if (sign == 0)
    {
      sign = turnSign;
    }
    else if (sign != turnSign)
    {
      return false;
    }
  }

  return true;
}

std::vector<vm::vec3d> sortAroundNormal(
  std::vector<vm::vec3d> points, const vm::vec3d& normal)
{
  auto center = vm::vec3d{0.0, 0.0, 0.0};
  for (const auto& point : points)
  {
    center = center + point;
  }
  center = center / double(points.size());

  auto u = points.front() - center;
  if (vm::squared_length(u) <= vm::Cd::almost_zero())
  {
    return points;
  }
  u = vm::normalize(u);
  const auto v = vm::cross(normal, u);

  std::ranges::sort(points, [&](const auto& lhs, const auto& rhs) {
    const auto lhsDelta = lhs - center;
    const auto rhsDelta = rhs - center;
    const auto lhsAngle = std::atan2(vm::dot(lhsDelta, v), vm::dot(lhsDelta, u));
    const auto rhsAngle = std::atan2(vm::dot(rhsDelta, v), vm::dot(rhsDelta, u));
    return lhsAngle < rhsAngle;
  });

  return points;
}

std::optional<MeshPolygon> mergeTrianglePair(
  const MeshPolygon& lhs, const MeshPolygon& rhs)
{
  auto uniqueIndices = lhs.indices;
  for (const auto index : rhs.indices)
  {
    if (std::ranges::find(uniqueIndices, index) == std::end(uniqueIndices))
    {
      uniqueIndices.push_back(index);
    }
  }

  if (uniqueIndices.size() != 4u)
  {
    return std::nullopt;
  }

  const auto lhsNormal = polygonNormal(lhs.points);
  const auto rhsNormal = polygonNormal(rhs.points);
  if (!lhsNormal || !rhsNormal)
  {
    return std::nullopt;
  }

  if (std::abs(vm::dot(*lhsNormal, *rhsNormal)) < 0.999)
  {
    return std::nullopt;
  }

  auto points = lhs.points;
  for (const auto& point : rhs.points)
  {
    const auto duplicate = std::ranges::any_of(points, [&](const auto& existing) {
      return vm::squared_distance(point, existing) <= DuplicateVertexDistanceSquared;
    });
    if (!duplicate)
    {
      points.push_back(point);
    }
  }

  points = sortAroundNormal(std::move(points), *lhsNormal);
  if (
    points.size() != 4u || !coplanarWith(points, points.front(), *lhsNormal)
    || !isConvexPolygon(points, *lhsNormal))
  {
    return std::nullopt;
  }

  return MeshPolygon{std::move(uniqueIndices), std::move(points)};
}

std::vector<MeshPolygon> mergeCoplanarTrianglePairs(std::vector<MeshPolygon> polygons)
{
  auto triangleEdges = std::unordered_map<EdgeKey, std::vector<size_t>, EdgeKeyHash>{};

  for (size_t i = 0; i < polygons.size(); ++i)
  {
    const auto& polygon = polygons[i];
    if (polygon.indices.size() != 3u)
    {
      continue;
    }

    for (size_t edge = 0; edge < 3u; ++edge)
    {
      triangleEdges[EdgeKey{
                      polygon.indices[edge],
                      polygon.indices[(edge + 1u) % polygon.indices.size()]}]
        .push_back(i);
    }
  }

  auto used = std::vector<bool>(polygons.size(), false);
  auto mergedPolygons = std::vector<MeshPolygon>{};
  mergedPolygons.reserve(polygons.size());

  for (const auto& [edge, triangles] : triangleEdges)
  {
    static_cast<void>(edge);
    if (triangles.size() != 2u)
    {
      continue;
    }

    const auto lhsIndex = triangles[0];
    const auto rhsIndex = triangles[1];
    if (used[lhsIndex] || used[rhsIndex])
    {
      continue;
    }

    if (auto merged = mergeTrianglePair(polygons[lhsIndex], polygons[rhsIndex]))
    {
      used[lhsIndex] = true;
      used[rhsIndex] = true;
      mergedPolygons.push_back(std::move(*merged));
    }
  }

  for (size_t i = 0; i < polygons.size(); ++i)
  {
    if (!used[i])
    {
      mergedPolygons.push_back(std::move(polygons[i]));
    }
  }

  return mergedPolygons;
}

std::vector<MeshPolygon> collectMeshPolygons(
  const aiMesh& mesh, const mdl::Grid& grid, const double importScale)
{
  auto vertices = std::vector<vm::vec3d>{};
  vertices.reserve(mesh.mNumVertices);
  for (auto i = 0u; i < mesh.mNumVertices; ++i)
  {
    vertices.push_back(snapNearGrid(grid, aiVecToVm(mesh.mVertices[i]) * importScale));
  }

  auto polygons = std::vector<MeshPolygon>{};
  polygons.reserve(mesh.mNumFaces);

  for (auto faceIndex = 0u; faceIndex < mesh.mNumFaces; ++faceIndex)
  {
    const auto& face = mesh.mFaces[faceIndex];
    if (face.mNumIndices < 3u)
    {
      continue;
    }

    auto polygon = MeshPolygon{};
    polygon.indices.reserve(face.mNumIndices);
    polygon.points.reserve(face.mNumIndices);

    for (auto indexIndex = 0u; indexIndex < face.mNumIndices; ++indexIndex)
    {
      const auto vertexIndex = face.mIndices[indexIndex];
      if (vertexIndex >= vertices.size())
      {
        polygon.indices.clear();
        polygon.points.clear();
        break;
      }

      polygon.indices.push_back(vertexIndex);
      polygon.points.push_back(vertices[vertexIndex]);
    }

    if (polygon.points.size() >= 3u)
    {
      polygons.push_back(std::move(polygon));
    }
  }

  return mergeCoplanarTrianglePairs(std::move(polygons));
}

std::optional<mdl::Brush> createBrushFromPolygon(
  const vm::bbox3d& worldBounds,
  const mdl::MapFormat mapFormat,
  const std::vector<vm::vec3d>& polygonPoints,
  const std::string& materialName,
  const mdl::BrushFaceAttributes& defaultAttributes,
  const double thickness)
{
  auto points = cleanPolygonPoints(polygonPoints);
  if (points.size() < 3u)
  {
    return std::nullopt;
  }

  const auto normalVector = polygonNormalVector(points);
  const auto normal = polygonNormal(points);
  if (
    !normal || !coplanarWith(points, points.front(), *normal)
    || !hasSafePointDistances(points) || !hasSafePolygonHeight(points, normalVector))
  {
    return std::nullopt;
  }

  const auto halfThickness = *normal * (thickness * 0.5);
  auto bottomPoints = std::vector<vm::vec3d>{};
  auto topPoints = std::vector<vm::vec3d>{};
  bottomPoints.reserve(points.size());
  topPoints.reserve(points.size());

  for (const auto& point : points)
  {
    bottomPoints.push_back(point - halfThickness);
  }
  for (const auto& point : points)
  {
    topPoints.push_back(point + halfThickness);
  }

  auto prismPoints = bottomPoints;
  prismPoints.insert(std::end(prismPoints), std::begin(topPoints), std::end(topPoints));
  if (!hasSafePointDistances(prismPoints))
  {
    return std::nullopt;
  }

  auto center = vm::vec3d{0.0, 0.0, 0.0};
  for (const auto& point : prismPoints)
  {
    center = center + point;
  }
  center = center / double(prismPoints.size());

  const auto attributes = mdl::BrushFaceAttributes{materialName, defaultAttributes};
  auto faces = std::vector<mdl::BrushFace>{};
  faces.reserve(points.size() + 2u);

  if (
    !addInwardFace(
      faces,
      bottomPoints[0],
      bottomPoints[1],
      bottomPoints[2],
      center,
      attributes,
      mapFormat)
    || !addInwardFace(
      faces, topPoints[0], topPoints[2], topPoints[1], center, attributes, mapFormat))
  {
    return std::nullopt;
  }

  for (size_t i = 0u; i < points.size(); ++i)
  {
    const auto next = (i + 1u) % points.size();
    if (!addInwardFace(
          faces,
          bottomPoints[i],
          bottomPoints[next],
          topPoints[next],
          center,
          attributes,
          mapFormat))
    {
      return std::nullopt;
    }
  }

  auto brush = mdl::Brush::create(worldBounds, std::move(faces));
  if (!brush)
  {
    return std::nullopt;
  }

  return std::move(brush).value();
}

size_t addBrushesForPolygon(
  mdl::GroupNode& group,
  const vm::bbox3d& worldBounds,
  const mdl::MapFormat mapFormat,
  const MeshPolygon& polygon,
  const std::string& materialName,
  const mdl::BrushFaceAttributes& defaultAttributes,
  const double thickness)
{
  auto points = cleanPolygonPoints(polygon.points);
  const auto normal = polygonNormal(points);
  if (!normal)
  {
    return 0u;
  }

  if (points.size() > 3u && !isConvexPolygon(points, *normal))
  {
    auto brushCount = size_t{0u};
    for (size_t i = 1u; i + 1u < points.size(); ++i)
    {
      const auto triangle =
        std::vector<vm::vec3d>{points.front(), points[i], points[i + 1u]};
      if (
        auto brush = createBrushFromPolygon(
          worldBounds, mapFormat, triangle, materialName, defaultAttributes, thickness))
      {
        group.addChild(new mdl::BrushNode{std::move(*brush)});
        ++brushCount;
      }
    }
    return brushCount;
  }

  if (
    auto brush = createBrushFromPolygon(
      worldBounds, mapFormat, points, materialName, defaultAttributes, thickness))
  {
    group.addChild(new mdl::BrushNode{std::move(*brush)});
    return 1u;
  }

  return 0u;
}

std::string groupNameForPath(const std::filesystem::path& path)
{
  auto name = path.stem().string();
  if (name.empty())
  {
    name = "Imported Model";
  }
  return fmt::format("Imported {}", name);
}

} // namespace

Result<ModelBrushImportResult> importModelAsBrushes(
  MapDocument& document, const std::filesystem::path& path)
{
  auto importer = Assimp::Importer{};
  const auto* scene = importer.ReadFile(
    path.string(),
    aiProcess_JoinIdenticalVertices | aiProcess_PreTransformVertices
      | aiProcess_SortByPType | aiProcess_ImproveCacheLocality);

  if (scene == nullptr)
  {
    return Error{
      fmt::format("Could not import '{}': {}", path, importer.GetErrorString())};
  }
  if (scene->mNumMeshes == 0u)
  {
    return Error{fmt::format("Model '{}' contains no meshes", path)};
  }
  const auto sceneBounds = computeSceneBounds(*scene);
  if (!sceneBounds)
  {
    return Error{fmt::format("Model '{}' contains no vertices", path)};
  }

  auto& map = document.map();
  const auto mapFormat = map.worldNode().mapFormat();
  const auto worldBounds = map.worldBounds();
  const auto defaultAttributes = map.gameInfo().gameConfig.faceAttribsConfig.defaults;
  const auto materialName = map.currentMaterialName();
  const auto importScale = importScaleForBounds(*sceneBounds);
  const auto scaledSceneBounds =
    vm::bbox3d{sceneBounds->min * importScale, sceneBounds->max * importScale};
  const auto scaledMaxSize = vm::get_abs_max_component(sceneBounds->size() * importScale);
  const auto thickness = std::clamp(scaledMaxSize * 0.002, MinBrushEdgeLength * 2.0, 4.0);
  auto group = std::make_unique<mdl::GroupNode>(mdl::Group{groupNameForPath(path)});

  auto result = ModelBrushImportResult{};
  result.importScale = importScale;
  for (auto meshIndex = 0u; meshIndex < scene->mNumMeshes; ++meshIndex)
  {
    const auto* mesh = scene->mMeshes[meshIndex];
    if (mesh == nullptr || !mesh->HasPositions())
    {
      continue;
    }

    result.sourceFaceCount += mesh->mNumFaces;
    const auto polygons = collectMeshPolygons(*mesh, map.grid(), importScale);
    for (const auto& polygon : polygons)
    {
      const auto before = result.brushCount;
      result.brushCount += addBrushesForPolygon(
        *group,
        worldBounds,
        mapFormat,
        polygon,
        materialName,
        defaultAttributes,
        thickness);
      if (result.brushCount == before)
      {
        ++result.skippedFaceCount;
      }
    }
  }

  if (result.brushCount == 0u)
  {
    if (
      const auto simplifiedResult = addSimplifiedVoxelBrushes(
        *group,
        *scene,
        scaledSceneBounds,
        importScale,
        worldBounds,
        mapFormat,
        materialName,
        defaultAttributes);
      simplifiedResult.brushCount > 0u)
    {
      result.brushCount = simplifiedResult.brushCount;
      result.usedSimplifiedImport = true;
      result.simplifiedResolution = simplifiedResult.resolution;
    }
  }

  if (result.brushCount == 0u)
  {
    return Error{fmt::format(
      "Could not create any valid brushes from '{}'. Read {} mesh face(s), skipped "
      "{} polygon(s), and simplified voxel fallback also produced no valid brushes.",
      path,
      result.sourceFaceCount,
      result.skippedFaceCount)};
  }

  auto transaction = mdl::Transaction{map, "Import Model as Brushes"};
  mdl::deselectAll(map);

  auto* groupNode = static_cast<mdl::Node*>(group.release());
  const auto addedNodes = mdl::addNodes(map, {{mdl::parentForNodes(map), {groupNode}}});
  if (addedNodes.empty())
  {
    transaction.cancel();
    return Error{"Could not add imported model brushes to the map"};
  }

  mdl::selectNodes(map, addedNodes);
  if (!transaction.commit())
  {
    return Error{"Could not commit imported model brushes"};
  }

  return result;
}

} // namespace tb::ui
