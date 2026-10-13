/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mdl/BrushFracture.h"

#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace tb::mdl
{
namespace
{
constexpr double SiteMargin = 0.01;
// Brush::create corrects each vertex coordinate by up to correct_epsilon().
// A plane distance may therefore change by sqrt(3) times that amount.
const double GeometryTolerance = std::sqrt(3.0) * vm::Cd::correct_epsilon() + 1e-7;

vm::vec3d vertexCenter(const Brush& brush)
{
  auto center = vm::vec3d{};
  for (const auto& p : brush.vertexPositions())
  {
    center = center + p;
  }
  return center / double(brush.vertexCount());
}

double brushVolume(const Brush& brush)
{
  const auto center = vertexCenter(brush);
  auto volume = 0.0;
  for (const auto& face : brush.faces())
  {
    const auto points = face.vertexPositions();
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
      volume +=
        std::abs(
          vm::dot(
            points[0] - center, vm::cross(points[i] - center, points[i + 1] - center)))
        / 6.0;
    }
  }
  return volume;
}

bool validSite(const Brush& source, const vm::vec3d& site)
{
  return std::isfinite(site.x()) && std::isfinite(site.y()) && std::isfinite(site.z())
         && std::ranges::all_of(source.faces(), [&](const auto& face) {
              return face.boundary().point_distance(site) < -SiteMargin;
            });
}

// Convert raw mt19937 output ourselves instead of relying on implementation-defined
// uniform_real_distribution sequences. Neither endpoint is returned.
double randomUnit(std::mt19937& random)
{
  return (double(random()) + 0.5) / 4294967296.0;
}
} // namespace

Result<std::vector<Brush>> fractureBrush(
  const Brush& source,
  const vm::bbox3d& worldBounds,
  const MapFormat mapFormat,
  const BrushFaceAttributes& cutAttributes,
  const std::vector<vm::vec3d>& sites)
{
  if (sites.empty() || sites.size() > MaxFractureFragments)
  {
    return Error{"Fragment count must be between 1 and 128"};
  }
  for (size_t i = 0; i < sites.size(); ++i)
  {
    if (!validSite(source, sites[i]))
    {
      return Error{"Fracture sites must be finite and strictly inside the brush"};
    }
    for (size_t j = 0; j < i; ++j)
    {
      if (vm::squared_distance(sites[i], sites[j]) < SiteMargin * SiteMargin)
      {
        return Error{"Fracture sites are too close together"};
      }
    }
  }

  auto fragments = std::vector<Brush>{};
  fragments.reserve(sites.size());
  const auto planeSize = std::max(64.0, vm::length(source.bounds().size()));
  for (size_t i = 0; i < sites.size(); ++i)
  {
    auto faces = source.faces();
    for (size_t j = 0; j < sites.size(); ++j)
    {
      if (i == j)
      {
        continue;
      }
      // Construct each pair in a canonical order. The other cell uses the same
      // three points with reversed winding, reducing cracks from plane rounding.
      const auto lo = std::min(i, j);
      const auto hi = std::max(i, j);
      const auto normal = vm::normalize(sites[hi] - sites[lo]);
      const auto midpoint = sites[lo] + (sites[hi] - sites[lo]) * 0.5;
      const auto axis =
        std::abs(normal.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{0, 1, 0};
      const auto u = vm::normalize(vm::cross(normal, axis)) * planeSize;
      const auto v = vm::cross(normal, u);
      // vm::from_points uses cross(p2 - p0, p1 - p0).
      auto face = BrushFace::create(
        midpoint,
        midpoint + (i == lo ? v : u),
        midpoint + (i == lo ? u : v),
        cutAttributes,
        mapFormat);
      if (!face)
      {
        return Error{"Could not construct a fracture plane"};
      }
      faces.push_back(std::move(face).value());
    }
    // Build once: healing intermediate clips can discard a short edge/face
    // that a later cut still needs, particularly in thin brushes.
    auto cell = Brush::create(worldBounds, std::move(faces));
    if (!cell)
    {
      return Error{
        "A fracture produced invalid geometry. Try another seed or fewer fragments"};
    }
    auto fragment = std::move(cell).value();
    if (!fragment.closed() || !fragment.fullySpecified() || brushVolume(fragment) < 0.001)
    {
      return Error{
        "A fragment is too small or incomplete. Try another seed or fewer fragments"};
    }
    for (const auto& vertex : fragment.vertexPositions())
    {
      if (std::ranges::any_of(source.faces(), [&](const auto& face) {
            return face.boundary().point_distance(vertex) > GeometryTolerance;
          }))
      {
        return Error{"A fragment escaped the source brush after geometry correction"};
      }
      for (size_t j = 0; j < sites.size(); ++j)
      {
        if (j != i)
        {
          const auto normal = vm::normalize(sites[j] - sites[i]);
          const auto midpoint = sites[i] + (sites[j] - sites[i]) * 0.5;
          if (vm::dot(vertex - midpoint, normal) > GeometryTolerance)
          {
            return Error{"A fragment escaped its Voronoi cell after geometry correction"};
          }
        }
      }
    }
    fragments.push_back(std::move(fragment));
  }

  const auto originalVolume = brushVolume(source);
  auto totalVolume = 0.0;
  for (const auto& fragment : fragments)
  {
    totalVolume += brushVolume(fragment);
  }
  auto surfaceArea = 0.0;
  for (const auto& face : source.faces())
  {
    const auto points = face.vertexPositions();
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
      surfaceArea +=
        vm::length(vm::cross(points[i] - points[0], points[i + 1] - points[0])) / 2.0;
    }
  }
  if (
    std::abs(totalVolume - originalVolume)
    > std::max(0.001, surfaceArea * GeometryTolerance))
  {
    return Error{
      "Fracture failed the volume check (source " + std::to_string(originalVolume)
      + ", fragments " + std::to_string(totalVolume)
      + "). Try another seed or fewer fragments"};
  }
  return fragments;
}

Result<std::vector<Brush>> fractureBrush(
  const Brush& source,
  const vm::bbox3d& worldBounds,
  const MapFormat mapFormat,
  const BrushFaceAttributes& cutAttributes,
  const size_t count,
  const uint32_t seed)
{
  if (count == 0 || count > MaxFractureFragments)
  {
    return Error{"Fragment count must be between 1 and 128"};
  }

  // A convex brush is partitioned into tetrahedra from an interior point to
  // triangulated faces. Volume-weighted sampling also handles rotated thin brushes
  // without the unbounded rejection rate of bounding-box sampling.
  struct Tetrahedron
  {
    std::array<vm::vec3d, 4> points;
    double cumulativeVolume;
  };
  const auto center = vertexCenter(source);
  auto tetrahedra = std::vector<Tetrahedron>{};
  auto totalVolume = 0.0;
  for (const auto& face : source.faces())
  {
    const auto points = face.vertexPositions();
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
      const auto volume =
        std::abs(
          vm::dot(
            points[0] - center, vm::cross(points[i] - center, points[i + 1] - center)))
        / 6.0;
      if (volume > 0.0)
      {
        totalVolume += volume;
        tetrahedra.push_back(
          {{center, points[0], points[i], points[i + 1]}, totalVolume});
      }
    }
  }
  if (tetrahedra.empty())
  {
    return Error{"Cannot fracture a brush with no volume"};
  }

  auto random = std::mt19937{seed};
  auto sites = std::vector<vm::vec3d>{};
  for (size_t attempt = 0; sites.size() < count && attempt < count * 1024; ++attempt)
  {
    const auto target = randomUnit(random) * totalVolume;
    const auto& tetrahedron = *std::lower_bound(
      tetrahedra.begin(), tetrahedra.end(), target, [](const auto& t, const double v) {
        return t.cumulativeVolume < v;
      });
    auto point = vm::vec3d{};
    auto weightSum = 0.0;
    for (const auto& vertex : tetrahedron.points)
    {
      const auto weight = -std::log(randomUnit(random));
      point = point + vertex * weight;
      weightSum += weight;
    }
    point = point / weightSum;
    if (validSite(source, point) && std::ranges::none_of(sites, [&](const auto& site) {
          return vm::squared_distance(point, site) < SiteMargin * SiteMargin;
        }))
    {
      sites.push_back(point);
    }
  }
  if (sites.size() != count)
  {
    return Error{"Brush is too thin to place the requested fracture sites"};
  }
  return fractureBrush(source, worldBounds, mapFormat, cutAttributes, sites);
}
} // namespace tb::mdl
