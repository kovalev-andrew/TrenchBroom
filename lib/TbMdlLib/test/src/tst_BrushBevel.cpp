/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mdl/BrushBevel.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/MapFormat.h"

#include "vm/mat_ext.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
TEST_CASE("BrushBevel")
{
  const auto bounds = vm::bbox3d{4096.0};
  const auto format = MapFormat::Valve;
  const auto builder = BrushBuilder{format, bounds};
  auto source =
    builder.createCuboid(vm::bbox3d{{0, 0, 0}, {128, 128, 128}}, "stone").value();
  const auto top = *source.findFace(vm::vec3d{0, 0, 1});
  auto selection = BevelSelection{bevelFaceBoundary(source, {top}), {}};
  auto parameters = BevelParameters{};
  SECTION("Only the external boundary of selected faces is used")
  {
    REQUIRE(selection.edges.size() == 4);
    const auto side = *source.findFace(vm::vec3d{1, 0, 0});
    CHECK(bevelFaceBoundary(source, {top, side}).size() == 6);
    auto all = std::vector<size_t>{0, 1, 2, 3, 4, 5};
    CHECK(bevelFaceBoundary(source, all).empty());
  }
  SECTION("Convex profiles remain inside the original brush")
  {
    parameters.profile = GENERATE(0.0, 0.25, 0.5, 0.75, 0.95);
    parameters.segments = GENERATE(1u, 4u, 8u);
    auto result = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(result.is_success());
    const auto& brush = result.value().brush;
    CHECK(brush.closed());
    CHECK(brush.fullySpecified());
    CHECK_FALSE(result.value().clamped);
    CHECK(brush.faceCount() > source.faceCount());
    for (const auto& point : brush.vertexPositions())
      for (const auto& face : source.faces())
        CHECK(face.boundary().point_distance(point) < 0.003);
  }
  SECTION("Circular edge profile has the requested reference radius")
  {
    selection.edges.resize(1);
    parameters.size = 16;
    auto result = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(result.is_success());
    const auto edge = selection.edges.front();
    auto normals = std::vector<vm::vec3d>{};
    for (const auto& face : source.faces())
      if (
        std::abs(face.boundary().point_distance(edge.start())) < 1e-6
        && std::abs(face.boundary().point_distance(edge.end())) < 1e-6)
        normals.push_back(face.boundary().normal);
    REQUIRE(normals.size() == 2);
    const auto axis = vm::normalize(edge.end() - edge.start());
    const auto center = edge.start() - (normals[0] + normals[1]) * 16.0;
    size_t curved = 0;
    for (const auto& point : result.value().brush.vertexPositions())
      if (!source.hasVertex(point, 0.003))
      {
        const auto delta = point - center;
        CHECK(
          vm::length(delta - axis * vm::dot(delta, axis))
          == Catch::Approx(16).margin(0.003));
        ++curved;
      }
    CHECK(curved >= 8);
  }
  SECTION("Clamp is local and cannot silently apply with Clamp disabled")
  {
    parameters.size = 100;
    auto limited = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(limited.is_success());
    CHECK(limited.value().clamped);
    CHECK(limited.value().appliedSize < 64);
    auto large =
      builder.createCuboid(vm::bbox3d{{0, 0, 0}, {512, 512, 512}}, "stone").value();
    const auto largeSelection =
      BevelSelection{bevelFaceBoundary(large, {*large.findFace(vm::vec3d{0, 0, 1})}), {}};
    auto full = bevelBrush(large, bounds, format, largeSelection, parameters);
    REQUIRE(full.is_success());
    CHECK(full.value().appliedSize == 100);
    parameters.clamp = false;
    CHECK(bevelBrush(source, bounds, format, selection, parameters).is_error());
  }
  SECTION("Vertex mode preserves other original vertices")
  {
    parameters.segments = GENERATE(1u, 3u, 6u);
    selection = {{}, {{128, 128, 128}}};
    auto result = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(result.is_success());
    CHECK_FALSE(result.value().brush.hasVertex({128, 128, 128}, 0.001));
    for (const auto& point : source.vertexPositions())
      if (point != vm::vec3d{128, 128, 128})
        CHECK(result.value().brush.hasVertex(point, 0.003));
  }
  SECTION("Zero size is an exact no-op")
  {
    parameters.size = 0;
    REQUIRE(
      bevelBrush(source, bounds, format, selection, parameters).value().brush == source);
  }
  SECTION("Vertex Clamp detects opposing caps before their shared faces vanish")
  {
    selection = {{}, {{0, 0, 128}, {128, 0, 128}}};
    parameters.size = 256;
    const auto result = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(result.is_success());
    CHECK(result.value().clamped);
    parameters.clamp = false;
    CHECK(bevelBrush(source, bounds, format, selection, parameters).is_error());
  }
  SECTION("Texture scale and offsets survive on original faces")
  {
    for (size_t i = 0; i < source.faceCount(); ++i)
    {
      auto attributes = source.face(i).attributes();
      attributes.setScale({0.25f, 1.75f});
      attributes.setOffset({13.0f, -29.0f});
      source.face(i).setAttributes(attributes);
    }
    const auto result = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(result.is_success());
    for (const auto& face : source.faces())
    {
      const auto index = result.value().brush.findFace(face.boundary().normal);
      REQUIRE(index);
      const auto& after = result.value().brush.face(*index);
      CHECK(after.attributes() == face.attributes());
      CHECK(
        after.uvCoords(face.vertexPositions().front())
        == face.uvCoords(face.vertexPositions().front()));
    }
  }
  SECTION("Oblique and translated brushes remain valid")
  {
    const auto shear = GENERATE(0.2, 0.75, -0.4);
    const auto matrix =
      vm::mat4x4d{1, shear, 0, 123, 0, 1, 0, 217, 0, 0, 1, -89, 0, 0, 0, 1};
    REQUIRE(source.transform(bounds, matrix, true).is_success());
    selection.edges = bevelFaceBoundary(source, {*source.findFace(vm::vec3d{0, 0, 1})});
    parameters.size = 16;
    const auto result = bevelBrush(source, bounds, format, selection, parameters);
    REQUIRE(result.is_success());
    CHECK(result.value().brush.closed());
    for (const auto& point : result.value().brush.vertexPositions())
      for (const auto& face : source.faces())
        CHECK(face.boundary().point_distance(point) < 0.01);
  }
  SECTION("Invalid parameters are rejected")
  {
    parameters.profile = -0.5;
    CHECK(bevelBrush(source, bounds, format, selection, parameters).is_error());
  }
}
} // namespace tb::mdl
