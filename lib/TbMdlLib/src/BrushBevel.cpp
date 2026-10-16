/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mdl/BrushBevel.h"

#include "mdl/BrushFace.h"
#include "mdl/BrushGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace tb::mdl
{
namespace
{
constexpr double Margin = 0.05;
struct Cut
{
  vm::vec3d anchor;
  vm::vec3d normal;
  double offset;
  size_t materialFace;
};
bool near(const vm::vec3d& a, const vm::vec3d& b)
{
  return vm::squared_distance(a, b) < 1e-10;
}
bool sameEdge(const vm::segment3d& a, const vm::segment3d& b)
{
  return (near(a.start(), b.start()) && near(a.end(), b.end()))
         || (near(a.start(), b.end()) && near(a.end(), b.start()));
}
vm::segment3d position(const BrushEdge* edge)
{
  return {edge->firstVertex()->position(), edge->secondVertex()->position()};
}

// Solve a small least-squares system without depending on a rendering math API.
std::optional<vm::vec3d> cornerCenter(const std::vector<vm::vec3d>& directions)
{
  double a[3][4]{};
  for (const auto& d : directions)
    for (size_t i = 0; i < 3; ++i)
    {
      a[i][3] += d[i];
      for (size_t j = 0; j < 3; ++j)
        a[i][j] += d[i] * d[j];
    }
  for (size_t i = 0; i < 3; ++i)
  {
    size_t pivot = i;
    for (size_t j = i + 1; j < 3; ++j)
      if (std::abs(a[j][i]) > std::abs(a[pivot][i]))
        pivot = j;
    if (std::abs(a[pivot][i]) < 1e-10)
      return std::nullopt;
    for (size_t j = 0; j < 4; ++j)
      std::swap(a[i][j], a[pivot][j]);
    const auto divisor = a[i][i];
    for (size_t j = i; j < 4; ++j)
      a[i][j] /= divisor;
    for (size_t k = 0; k < 3; ++k)
      if (k != i)
      {
        const auto factor = a[k][i];
        for (size_t j = i; j < 4; ++j)
          a[k][j] -= factor * a[i][j];
      }
  }
  return vm::vec3d{a[0][3], a[1][3], a[2][3]};
}

Result<std::vector<Cut>> makeCuts(
  const Brush& source, const BevelSelection& selection, const BevelParameters& p)
{
  auto cuts = std::vector<Cut>{};
  if (selection.edges.size() + selection.vertices.size() > 128)
    return Error{"Select at most 128 elements per brush"};
  for (const auto& selected : selection.edges)
  {
    const auto it = std::ranges::find_if(
      source.edges(), [&](const auto& e) { return sameEdge(position(e), selected); });
    if (it == source.edges().end())
      return Error{"A selected edge no longer exists"};
    const auto& edge = *it;
    const auto ia = *edge->firstFace()->payload();
    const auto ib = *edge->secondFace()->payload();
    const auto a = source.face(ia).boundary().normal;
    const auto b = source.face(ib).boundary().normal;
    const auto cosine = vm::dot(a, b);
    if (cosine > 1.0 - 1e-8)
      continue;
    if (cosine < -1.0 + 1e-8)
      return Error{"The edge angle is too narrow"};
    const auto center = -(a + b) / (1.0 + cosine);
    const auto start = center + a;
    const auto end = center + b;
    const auto scale = p.sizeType == BevelSize::Radius ? 1.0 : 1.0 / vm::length(start);
    auto points = std::vector<vm::vec3d>{};
    for (size_t i = 0; i <= p.segments; ++i)
    {
      const auto t = double(i) / double(p.segments);
      const auto angle = std::acos(std::clamp(cosine, -1.0, 1.0));
      const auto normal =
        (a * std::sin((1.0 - t) * angle) + b * std::sin(t * angle)) / std::sin(angle);
      const auto circular = center + normal;
      const auto flat = start * (1.0 - t) + end * t;
      const auto square = t <= 0.5 ? start * (1.0 - 2.0 * t) : end * (2.0 * t - 1.0);
      points.push_back(
        scale
        * (p.profile <= 0.5 ? flat * (1.0 - 2.0 * p.profile) + circular * (2.0 * p.profile) : circular * (2.0 - 2.0 * p.profile) + square * (2.0 * p.profile - 1.0)));
    }
    const auto axis = vm::normalize(selected.end() - selected.start());
    for (size_t i = 0; i < p.segments; ++i)
    {
      auto n = vm::cross(points[i + 1] - points[i], axis);
      if (vm::length(n) < 1e-9)
        continue;
      n = vm::normalize(n);
      if (vm::dot(n, a + b) < 0)
        n = -n;
      const auto offset = vm::dot(n, points[i]);
      if (offset < -1e-9)
        cuts.push_back({selected.start(), n, offset, ia});
    }
  }
  auto corners = selection.vertices;
  for (const auto& vertex : source.vertexPositions())
  {
    const auto count = std::ranges::count_if(selection.edges, [&](const auto& e) {
      return near(e.start(), vertex) || near(e.end(), vertex);
    });
    if (count >= 2 && std::ranges::find(corners, vertex) == corners.end())
      corners.push_back(vertex);
  }
  for (const auto& vertex : corners)
  {
    if (!source.hasVertex(vertex, 1e-5))
      return Error{"A selected vertex no longer exists"};
    auto directions = std::vector<vm::vec3d>{};
    for (const auto& e : source.edges())
    {
      const auto segment = position(e);
      if (near(segment.start(), vertex))
        directions.push_back(vm::normalize(segment.end() - vertex));
      else if (near(segment.end(), vertex))
        directions.push_back(vm::normalize(segment.start() - vertex));
    }
    const auto edgeJunction =
      std::ranges::find(selection.vertices, vertex) == selection.vertices.end();
    if (edgeJunction)
    {
      directions.clear();
      for (const auto& face : source.faces())
        if (std::abs(face.boundary().point_distance(vertex)) < 1e-5)
          directions.push_back(-face.boundary().normal);
    }
    const auto c = cornerCenter(directions);
    if (!c)
      return Error{"Cannot construct a stable corner at this vertex"};
    double radius = 0;
    for (const auto& d : directions)
      radius = std::max(radius, vm::length(*c - d * vm::dot(*c, d)));
    if (edgeJunction)
      radius = 1.0;
    if (radius < 1e-8)
      return Error{"The vertex angle is too narrow"};
    auto normals = std::vector<vm::vec3d>{};
    size_t materialFace = 0;
    for (size_t i = 0; i < source.faceCount(); ++i)
      if (std::abs(source.face(i).boundary().point_distance(vertex)) < 1e-5)
      {
        normals.push_back(source.face(i).boundary().normal);
        materialFace = i;
      }
    auto average = vm::vec3d{};
    for (const auto& n : normals)
      average = average + n;
    average = vm::normalize(average);
    auto factor = p.sizeType == BevelSize::Radius ? 1.0 / radius : 1.0;
    if (edgeJunction && p.sizeType == BevelSize::Offset)
    {
      for (size_t i = 0; i < normals.size(); ++i)
        for (size_t j = i + 1; j < normals.size(); ++j)
        {
          const auto cosine =
            std::clamp(vm::dot(normals[i], normals[j]), -0.999999, 0.999999);
          factor = std::min(factor, std::sqrt((1.0 + cosine) / (1.0 - cosine)));
        }
    }
    const auto add = [&](vm::vec3d n) {
      n = vm::normalize(n);
      const auto spread = std::min(1.0, 2.0 * p.profile);
      n = vm::normalize(average * (1.0 - spread) + n * spread);
      auto offset = (vm::dot(n, *c) + radius) * factor;
      if (p.profile > 0.5)
        offset *= 2.0 - 2.0 * p.profile;
      if (offset < -1e-8)
        cuts.push_back({vertex, n, offset, materialFace});
    };
    add(average);
    const auto samples = normals.size() * (normals.size() - 1) * (normals.size() - 2) / 6
                         * (p.segments + 1) * (p.segments + 2) / 2;
    if (samples + cuts.size() > 4096)
      return Error{"Bevel corner is too complex; reduce the segment count"};
    if (p.segments > 1)
      for (size_t i = 0; i < normals.size(); ++i)
        for (size_t j = i + 1; j < normals.size(); ++j)
          for (size_t k = j + 1; k < normals.size(); ++k)
            for (size_t u = 0; u <= p.segments; ++u)
              for (size_t v = 0; v <= p.segments - u; ++v)
              {
                if (edgeJunction && (u == 0 || v == 0 || u + v == p.segments))
                  continue;
                add(
                  normals[i] * double(u) + normals[j] * double(v)
                  + normals[k] * double(p.segments - u - v));
              }
  }
  if (cuts.size() > 4096)
    return Error{"Bevel is too complex; use fewer segments or select fewer elements"};
  if (cuts.empty())
    return Error{"Select a face boundary, edge or vertex to bevel"};
  return cuts;
}

bool facesSurvive(
  const Brush& source,
  const std::vector<Cut>& cuts,
  const double size,
  const BevelSelection& selection)
{
  // Non-beveled edges must retain a positive interval. This catches vertex caps
  // meeting along an edge before either adjacent face disappears.
  for (const auto* edge : source.edges())
  {
    const auto segment = position(edge);
    if (std::ranges::any_of(selection.edges, [&](const auto& e) {
          return sameEdge(e, segment);
        }))
      continue;
    double lo = 0, hi = 1;
    for (const auto& cut : cuts)
    {
      const auto a =
        vm::dot(cut.normal, segment.start() - cut.anchor) - cut.offset * size;
      const auto b = vm::dot(cut.normal, segment.end() - cut.anchor) - cut.offset * size;
      if (a > 0 && b > 0)
        return false;
      if (a > 0)
        lo = std::max(lo, a / (a - b));
      if (b > 0)
        hi = std::min(hi, a / (a - b));
    }
    if ((hi - lo) * vm::length(segment.end() - segment.start()) < Margin)
      return false;
  }
  // Intersect the original face polygons with all bevel half-spaces. Keeping a
  // positive area prevents opposing bevel fronts from crossing within a face.
  for (const auto& face : source.faces())
  {
    auto polygon = face.vertexPositions();
    for (const auto& cut : cuts)
    {
      auto clipped = std::vector<vm::vec3d>{};
      for (size_t i = 0; i < polygon.size(); ++i)
      {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        const auto da = vm::dot(cut.normal, a - cut.anchor) - cut.offset * size;
        const auto db = vm::dot(cut.normal, b - cut.anchor) - cut.offset * size;
        if (da <= 0)
          clipped.push_back(a);
        if ((da < 0 && db > 0) || (da > 0 && db < 0))
          clipped.push_back(a + (b - a) * (da / (da - db)));
      }
      polygon = std::move(clipped);
      if (polygon.size() < 3)
        return false;
    }
    auto area = vm::vec3d{};
    for (size_t i = 1; i + 1 < polygon.size(); ++i)
      area = area + vm::cross(polygon[i] - polygon[0], polygon[i + 1] - polygon[0]);
    if (vm::length(area) < Margin * Margin)
      return false;
  }
  return true;
}

Result<Brush> build(
  const Brush& source,
  const vm::bbox3d& bounds,
  const MapFormat format,
  const std::vector<Cut>& cuts,
  const double size)
{
  auto faces = source.faces();
  const auto extent = std::max(64.0, vm::length(source.bounds().size()));
  for (const auto& cut : cuts)
  {
    const auto axis =
      std::abs(cut.normal.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{0, 1, 0};
    const auto u = vm::normalize(vm::cross(cut.normal, axis)) * extent;
    const auto v = vm::cross(cut.normal, u);
    const auto origin = cut.anchor + cut.normal * (cut.offset * size);
    auto face = BrushFace::create(
      origin, origin + v, origin + u, source.face(cut.materialFace).attributes(), format);
    if (!face)
      return Error{"Cannot construct a bevel plane"};
    faces.push_back(std::move(face).value());
  }
  auto result = Brush::create(bounds, std::move(faces));
  if (!result)
    return result;
  auto brush = std::move(result).value();
  if (!brush.closed() || !brush.fullySpecified())
    return Error{"Bevel produced incomplete geometry"};
  for (const auto& face : source.faces())
    if (!brush.findFace(face.boundary().normal))
      return Error{"Bevel would remove an original face"};
  brush.cloneFaceAttributesFrom(source);
  return brush;
}
} // namespace

std::vector<vm::segment3d> bevelFaceBoundary(
  const Brush& brush, const std::vector<size_t>& faces)
{
  auto result = std::vector<vm::segment3d>{};
  for (const auto& edge : brush.edges())
  {
    const auto selected = [&](const auto index) {
      return index && std::ranges::find(faces, *index) != faces.end();
    };
    if (selected(edge->firstFace()->payload()) != selected(edge->secondFace()->payload()))
      result.push_back(position(edge));
  }
  return result;
}

Result<BevelResult> bevelBrush(
  const Brush& source,
  const vm::bbox3d& bounds,
  const MapFormat format,
  const BevelSelection& selection,
  const BevelParameters& p)
{
  if (
    !std::isfinite(p.size) || p.size < 0 || !std::isfinite(p.profile) || p.profile < 0
    || p.profile >= 1 || p.segments < 1 || p.segments > 16)
    return Error{"Use a nonnegative size, 1-16 segments and a convex profile below 1"};
  if (p.size == 0)
    return BevelResult{source, 0, false};
  auto generated = makeCuts(source, selection, p);
  if (!generated)
    return std::get<Error>(generated.error());
  const auto& cuts = generated.value();
  auto size = p.size;
  if (!facesSurvive(source, cuts, size, selection))
  {
    if (!p.clamp)
      return Error{"Bevel overlaps within this brush; reduce the size or enable Clamp"};
    double lo = 0, hi = size;
    for (int i = 0; i < 48; ++i)
    {
      const auto mid = lo + (hi - lo) * 0.5;
      if (facesSurvive(source, cuts, mid, selection))
        lo = mid;
      else
        hi = mid;
    }
    size = std::max(0.0, lo - Margin);
  }
  if (size < Margin)
    return Error{"Not enough room for this bevel"};
  auto brush = build(source, bounds, format, cuts, size);
  if (!brush)
    return Error{"Bevel is below brush precision; reduce segments or change the size"};
  return BevelResult{std::move(brush).value(), size, size < p.size - 1e-6};
}
} // namespace tb::mdl
