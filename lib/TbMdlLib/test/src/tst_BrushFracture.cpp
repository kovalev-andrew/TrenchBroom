/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFracture.h"
#include "mdl/MapFormat.h"

#include "vm/mat_ext.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
namespace
{
// Independent signed surface integral, rather than the implementation's sum of
// absolute tetrahedron volumes. Translate to keep far-from-origin tests stable.
double volume(const Brush& brush)
{
  const auto origin = brush.bounds().center();
  auto result = 0.0;
  for (const auto& face : brush.faces())
  {
    const auto points = face.vertexPositions();
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
      result +=
        vm::dot(points[0] - origin, vm::cross(points[i] - origin, points[i + 1] - origin))
        / 6.0;
    }
  }
  return std::abs(result);
}

void checkPartition(const Brush& source, const std::vector<Brush>& fragments)
{
  auto total = 0.0;
  for (const auto& fragment : fragments)
  {
    REQUIRE(fragment.closed());
    REQUIRE(fragment.fullySpecified());
    REQUIRE(volume(fragment) > 0.0);
    total += volume(fragment);
    for (const auto& vertex : fragment.vertexPositions())
    {
      for (const auto& face : source.faces())
      {
        CHECK(face.boundary().point_distance(vertex) <= 0.001);
      }
    }
  }
  CHECK(total == Catch::Approx(volume(source)).epsilon(1e-4));

  // Positive-volume intersections are forbidden; touching faces/edges are fine.
  for (size_t i = 0; i < fragments.size(); ++i)
  {
    for (size_t j = 0; j < i; ++j)
    {
      auto intersection = fragments[i];
      if (intersection.intersect(vm::bbox3d{65536.0}, fragments[j]))
      {
        CHECK(volume(intersection) < 0.001);
      }
    }
  }
}
} // namespace

TEST_CASE("BrushFracture")
{
  const auto format = GENERATE(MapFormat::Standard, MapFormat::Valve);
  const auto worldBounds = vm::bbox3d{65536.0};
  const auto builder = BrushBuilder{format, worldBounds};
  const auto cube = builder.createCube(128.0, "outside").value();
  const auto cut = BrushFaceAttributes{"inside"};

  SECTION("One site preserves the source")
  {
    const auto result =
      fractureBrush(cube, worldBounds, format, cut, std::vector<vm::vec3d>{{0, 0, 0}});
    REQUIRE(result);
    REQUIRE(result.value().size() == 1);
    CHECK(result.value().front() == cube);
  }

  SECTION("Two symmetric sites produce equal halves and preserve exterior UVs")
  {
    auto textured = cube;
    for (size_t i = 0; i < textured.faceCount(); ++i)
    {
      auto attributes = textured.face(i).attributes();
      attributes.setScale(vm::vec2f{0.25f + float(i) * 0.1f, -0.5f});
      attributes.setOffset(vm::vec2f{13.0f, -27.0f});
      attributes.setRotation(17.0f);
      textured.face(i).setAttributes(attributes);
    }
    const auto sites = std::vector<vm::vec3d>{{-32, 0, 0}, {32, 0, 0}};
    const auto result = fractureBrush(textured, worldBounds, format, cut, sites);
    REQUIRE(result);
    const auto& fragments = result.value();
    REQUIRE(fragments.size() == 2);
    CHECK(fragments[0].bounds() == vm::bbox3d{{-64, -64, -64}, {0, 64, 64}});
    CHECK(fragments[1].bounds() == vm::bbox3d{{0, -64, -64}, {64, 64, 64}});
    checkPartition(cube, fragments);
    for (const auto& fragment : fragments)
    {
      auto cutFaces = size_t{0};
      for (const auto& face : fragment.faces())
      {
        if (const auto originalIndex = cube.findFace(face.boundary()))
        {
          const auto& original = textured.face(*originalIndex);
          CHECK(face.attributes() == original.attributes());
          CHECK(face.uAxis() == original.uAxis());
          CHECK(face.vAxis() == original.vAxis());
        }
        else
        {
          ++cutFaces;
          CHECK(face.attributes().materialName() == "inside");
        }
      }
      CHECK(cutFaces == 1);
    }
  }

  SECTION("Eight symmetric sites produce octants")
  {
    auto sites = std::vector<vm::vec3d>{};
    for (const auto x : {-32.0, 32.0})
      for (const auto y : {-32.0, 32.0})
        for (const auto z : {-32.0, 32.0})
          sites.emplace_back(x, y, z);
    const auto result = fractureBrush(cube, worldBounds, format, cut, sites);
    REQUIRE(result);
    REQUIRE(result.value().size() == 8);
    for (const auto& fragment : result.value())
    {
      CHECK(fragment.bounds().size() == vm::vec3d{64, 64, 64});
    }
    checkPartition(cube, result.value());
  }

  SECTION("Invalid inputs are rejected")
  {
    const auto sites = GENERATE(
      std::vector<vm::vec3d>{},
      std::vector<vm::vec3d>{{0, 0, 0}, {0, 0, 0}},
      std::vector<vm::vec3d>{{64, 0, 0}},
      std::vector<vm::vec3d>{{100, 0, 0}},
      std::vector<vm::vec3d>{{std::numeric_limits<double>::quiet_NaN(), 0, 0}});
    CHECK_FALSE(fractureBrush(cube, worldBounds, format, cut, sites));
    CHECK_FALSE(fractureBrush(cube, worldBounds, format, cut, 0, 1));
    CHECK_FALSE(fractureBrush(cube, worldBounds, format, cut, 129, 1));
  }

  SECTION("Random sites are reproducible and form a partition")
  {
    const auto seed = GENERATE(0u, 1u, 17u, 123456u);
    const auto count = GENERATE(size_t{2}, size_t{12}, size_t{32});
    const auto first = fractureBrush(cube, worldBounds, format, cut, count, seed);
    REQUIRE(first);
    REQUIRE(first.value().size() == count);
    const auto second = fractureBrush(cube, worldBounds, format, cut, count, seed);
    REQUIRE(second);
    CHECK(first.value() == second.value());
    checkPartition(cube, first.value());
  }

  SECTION("Thin rotated brush far from origin")
  {
    auto source = builder.createCuboid(vm::vec3d{256, 128, 8}, "outside").value();
    REQUIRE(source.transform(
      worldBounds,
      vm::translation_matrix(vm::vec3d{12000, -8000, 4000})
        * vm::rotation_matrix(vm::vec3d{0, 0, 1}, 0.37),
      true));
    const auto result = fractureBrush(source, worldBounds, format, cut, 12, 17);
    auto errorMessage = std::string{};
    (void)(result | kdl::if_error([&](const auto& error) { errorMessage = error.msg; }));
    INFO(errorMessage);
    REQUIRE(result);
    REQUIRE(result.value().size() == 12);
    checkPartition(source, result.value());
  }

  SECTION("Sites own their Voronoi cells")
  {
    const auto sites = std::vector<vm::vec3d>{
      {-30, -20, 10}, {25, -15, -30}, {10, 35, 20}, {-15, 20, -25}};
    const auto result = fractureBrush(cube, worldBounds, format, cut, sites);
    REQUIRE(result);
    for (size_t i = 0; i < sites.size(); ++i)
    {
      CHECK(result.value()[i].containsPoint(sites[i]));
      for (const auto& p : result.value()[i].vertexPositions())
        for (const auto& site : sites)
          CHECK(
            vm::squared_distance(p, sites[i]) <= vm::squared_distance(p, site) + 0.01);
    }
    checkPartition(cube, result.value());
  }
}
} // namespace tb::mdl
