/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mdl/BrushPerforate.h"

#include "mdl/BrushFace.h"

#include "kd/reflection_impl.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace tb::mdl
{
kdl_reflect_impl(Perforation);
namespace
{
double volume(const Brush& b)
{
  double result = 0;
  // A bounding-box center can lie outside a thin, rotated convex brush.
  auto center = vm::vec3d{};
  for (const auto& vertex : b.vertexPositions())
    center = center + vertex;
  center = center / double(b.vertexCount());
  for (const auto& f : b.faces())
  {
    const auto p = f.vertexPositions();
    for (size_t i = 1; i + 1 < p.size(); ++i)
      result +=
        std::abs(vm::dot(p[0] - center, vm::cross(p[i] - center, p[i + 1] - center))) / 6;
  }
  return result;
}
} // namespace
Result<Perforation> perforateBrush(
  const Brush& source,
  const size_t faceIndex,
  const vm::bbox3d& worldBounds,
  const MapFormat format,
  const PerforateOptions& options)
{
  if (
    faceIndex >= source.faceCount() || options.count < 1 || options.count > 64
    || options.sides < 3 || options.sides > 32 || !std::isfinite(options.minSize)
    || !std::isfinite(options.maxSize) || !std::isfinite(options.spacing)
    || options.minSize < 4 || options.maxSize < options.minSize || options.spacing < 1)
    return Error{"Use 1-64 holes, 3-32 sides, sizes >= 4, min <= max and spacing >= 1."};

  const auto& face = source.face(faceIndex);
  const auto normal = face.boundary().normal;
  const auto origin = face.vertexPositions().front();
  const auto axis = std::abs(normal.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{0, 1, 0};
  const auto u = vm::normalize(vm::cross(normal, axis));
  const auto v = vm::cross(normal, u);
  const auto project = [&](const vm::vec3d& p) {
    return vm::vec2d{vm::dot(p - origin, u), vm::dot(p - origin, v)};
  };
  auto outline = std::vector<vm::vec2d>{};
  auto min = vm::vec2d{std::numeric_limits<double>::max()};
  auto max = -min;
  auto faceCenter = vm::vec2d{};
  for (const auto& p : face.vertexPositions())
  {
    const auto q = project(p);
    outline.push_back(q);
    min = vm::min(min, q);
    max = vm::max(max, q);
    faceCenter = faceCenter + q;
  }
  faceCenter = faceCenter / double(outline.size());
  struct Inset
  {
    vm::vec2d point;
    vm::vec2d inward;
  };
  auto boundaries = std::vector<Inset>{};
  for (size_t i = 0; i < outline.size(); ++i)
  {
    const auto edge = outline[(i + 1) % outline.size()] - outline[i];
    auto inward = vm::normalize(vm::vec2d{-edge.y(), edge.x()});
    if (vm::dot(faceCenter - outline[i], inward) < 0)
      inward = -inward;
    boundaries.push_back({outline[i], inward});
  }
  auto random = std::mt19937{options.seed};
  const auto unit = [&] { return (double(random()) + 0.5) / 4294967296.0; };
  struct Hole
  {
    std::vector<vm::vec2d> points;
    double radius;
    vm::vec2d center;
  };
  auto holes = std::vector<Hole>{};
  for (size_t i = 0; i < options.count; ++i)
  {
    auto points = std::vector<vm::vec2d>{};
    const auto size = options.minSize + (options.maxSize - options.minSize) * unit();
    const auto eccentricity = 0.65 + 0.35 * unit();
    const auto rotation = unit() * 2 * vm::Cd::pi();
    for (size_t j = 0; j < options.sides; ++j)
    {
      // Ordered points on an ellipse remain strictly convex, with exactly N sides.
      const auto a =
        (double(j) + 0.3 * (unit() - 0.5)) * 2 * vm::Cd::pi() / double(options.sides);
      const auto x = std::cos(a);
      const auto y = eccentricity * std::sin(a);
      points.push_back(
        {x * std::cos(rotation) - y * std::sin(rotation),
         x * std::sin(rotation) + y * std::cos(rotation)});
    }
    double diameter = 0;
    for (const auto& p : points)
      for (const auto& q : points)
        diameter = std::max(diameter, vm::length(p - q));
    double radius = 0;
    for (auto& p : points)
    {
      p = p * (size / diameter);
      radius = std::max(radius, vm::length(p));
    }
    holes.push_back({std::move(points), radius, {}});
  }
  std::stable_sort(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
    return a.radius > b.radius;
  });
  bool placed = false;
  for (size_t layout = 0; layout < 4 && !placed; ++layout)
  {
    placed = true;
    for (size_t i = 0; i < holes.size(); ++i)
    {
      bool found = false;
      for (size_t attempt = 0; attempt < 512 && !found; ++attempt)
      {
        const auto x = unit();
        const auto y = unit();
        const auto center = min + (max - min) * vm::vec2d{x, y};
        found = std::ranges::all_of(boundaries, [&](const auto& boundary) {
          return std::ranges::all_of(holes[i].points, [&](const auto& p) {
            return vm::dot(p + center - boundary.point, boundary.inward)
                   >= options.spacing + 0.01;
          });
        });
        for (size_t j = 0; j < i && found; ++j)
          found = vm::length(center - holes[j].center)
                  >= holes[i].radius + holes[j].radius + options.spacing + 0.01;
        if (found)
          holes[i].center = center;
      }
      if (!found)
      {
        placed = false;
        break;
      }
    }
  }
  if (!placed)
    return Error{
      "Could not place all holes at these sizes and spacing. Try another seed, fewer "
      "holes or smaller sizes."};
  auto result = Perforation{};
  for (const auto& hole : holes)
  {
    auto faces = source.faces();
    auto contourPlanes = std::vector<vm::plane3d>{};
    for (size_t i = 0; i < hole.points.size(); ++i)
    {
      const auto a = hole.points[i] + hole.center;
      const auto b = hole.points[(i + 1) % hole.points.size()] + hole.center;
      const auto p = origin + u * a.x() + v * a.y();
      const auto q = origin + u * b.x() + v * b.y();
      auto side = BrushFace::create(p, p + normal * 64.0, q, face.attributes(), format);
      if (!side)
        return Error{"Could not construct a hole side."};
      contourPlanes.push_back(side.value().boundary());
      faces.push_back(std::move(side).value());
    }
    auto plug = Brush::create(worldBounds, std::move(faces));
    if (!plug || volume(plug.value()) < 0.001)
      return Error{
        "A hole produced invalid or too small geometry. Try another seed or larger "
        "sizes."};
    // Confirm no requested contour edge was lost to geometric healing.
    // Match the constructed planes: point correction can slightly rotate a plane.
    if (!std::ranges::all_of(contourPlanes, [&](const auto& plane) {
          return plug.value().findFace(plane).has_value();
        }))
      return Error{
        "A hole lost contour sides. Increase its size or reduce the side count."};
    result.plugs.push_back(std::move(plug).value());
  }
  auto cutters = std::vector<const Brush*>{};
  for (const auto& plug : result.plugs)
    cutters.push_back(&plug);
  auto pieces =
    source.subtract(format, worldBounds, face.attributes().materialName(), cutters);
  if (pieces.empty() || pieces.size() > 4096)
    return Error{
      "The requested perforation produces too many pieces or removes the whole brush."};
  double total = 0;
  for (auto& piece : pieces)
  {
    if (!piece || !piece.value().closed() || !piece.value().fullySpecified())
      return Error{"Subtraction produced invalid geometry. No changes were applied."};
    total += volume(piece.value());
    result.remainder.push_back(std::move(piece).value());
  }
  for (const auto& plug : result.plugs)
    total += volume(plug);
  if (std::abs(total - volume(source)) > std::max(0.01, volume(source) * 1e-4))
    return Error{
      "Perforation failed the volume check. Try another seed or larger spacing."};
  return result;
}
} // namespace tb::mdl
