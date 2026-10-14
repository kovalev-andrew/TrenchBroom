/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushPerforate.h"
#include "mdl/MapFormat.h"
#include "vm/mat_ext.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
namespace tb::mdl
{
TEST_CASE("BrushPerforate")
{
  const auto bounds = vm::bbox3d{8192.0};
  const auto format = GENERATE(MapFormat::Standard, MapFormat::Valve, MapFormat::Quake3);
  const auto source =
    BrushBuilder{format, bounds}
      .createCuboid(vm::bbox3d{{-128, -128, -16}, {128, 128, 16}}, "stone")
      .value();
  const auto face = source.findFace(vm::vec3d{0, 0, 1}).value();
  auto options = PerforateOptions{};
  SECTION("Produces distinct through holes with exact contour counts and bounded sizes")
  {
    options.sides = GENERATE(3u, 6u, 12u, 32u);
    options.minSize = 32;
    options.maxSize = 48;
    for (uint32_t seed = 1; seed <= 5; ++seed)
    {
      options.seed = seed;
      auto result = perforateBrush(source, face, bounds, format, options);
      INFO(seed);
      REQUIRE(result);
      REQUIRE(result.value().plugs.size() == options.count);
      CHECK_FALSE(result.value().remainder.empty());
      for (const auto& plug : result.value().plugs)
      {
        CHECK(plug.faceCount() == options.sides + 2);
        CHECK(plug.bounds().min.z() == -16);
        CHECK(plug.bounds().max.z() == 16);
        const auto points =
          plug.face(plug.findFace(vm::vec3d{0, 0, 1}).value()).vertexPositions();
        double diameter = 0;
        for (const auto& a : points)
          for (const auto& b : points)
            diameter = std::max(diameter, vm::length(a - b));
        CHECK(diameter >= options.minSize - 0.01);
        CHECK(diameter <= options.maxSize + 0.01);
        for (const auto& piece : result.value().remainder)
          CHECK_FALSE(piece.intersects(plug));
      }
      auto repeated = perforateBrush(source, face, bounds, format, options);
      REQUIRE(repeated);
      CHECK(repeated.value().plugs == result.value().plugs);
      CHECK(repeated.value().remainder == result.value().remainder);
    }
  }
  SECTION("Impossible placement and invalid parameters fail without modifying the source")
  {
    const auto before = source;
    options.minSize = 1024;
    options.maxSize = 1024;
    CHECK_FALSE(perforateBrush(source, face, bounds, format, options));
    options = {};
    options.minSize = 64;
    options.maxSize = 16;
    CHECK_FALSE(perforateBrush(source, face, bounds, format, options));
    options = {};
    options.sides = 2;
    CHECK_FALSE(perforateBrush(source, face, bounds, format, options));
    CHECK(source == before);
  }
  SECTION("Direction follows the selected face")
  {
    const auto cube = BrushBuilder{format, bounds}.createCube(256, "stone").value();
    const auto side = cube.findFace(vm::vec3d{1, 0, 0}).value();
    auto result = perforateBrush(cube, side, bounds, format, options);
    REQUIRE(result);
    for (const auto& plug : result.value().plugs)
    {
      CHECK(plug.bounds().min.x() == -128);
      CHECK(plug.bounds().max.x() == 128);
    }
  }
  SECTION("Thin rotated brushes have an interior volume reference point")
  {
    auto rotated = BrushBuilder{format, bounds}
                     .createCuboid(vm::bbox3d{{-512, -512, -16}, {512, 512, 16}}, "stone")
                     .value();
    const auto transform = vm::rotation_matrix(vm::normalize(vm::vec3d{1, 1, 1}), 0.43);
    REQUIRE(rotated.transform(bounds, transform, true));
    const auto direction = transform * vm::vec3d{0, 0, 1};
    const auto selectedFace = rotated.findFace(direction);
    REQUIRE(selectedFace);
    options = {8, 9, 24, 48, 4, 42};
    const auto result = perforateBrush(rotated, *selectedFace, bounds, format, options);
    if (!result)
      result.if_error([](const auto& error) { FAIL(error.msg); });
    REQUIRE(result);
    CHECK(result.value().plugs.size() == 8);
  }
}
} // namespace tb::mdl
