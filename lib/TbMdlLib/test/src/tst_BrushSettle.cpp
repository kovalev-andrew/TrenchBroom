/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFracture.h"
#include "mdl/BrushSettle.h"
#include "mdl/MapFormat.h"

#include "vm/mat_ext.h"

#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
TEST_CASE("BrushSettle")
{
  const auto format = GENERATE(MapFormat::Standard, MapFormat::Valve);
  const auto bounds = vm::bbox3d{8192.0};
  const auto builder = BrushBuilder{format, bounds};
  const auto cube =
    builder.createCuboid(vm::bbox3d{{-16, -16, 128}, {16, 16, 160}}, "stone").value();
  const auto floor =
    builder.createCuboid(vm::bbox3d{{-1024, -1024, -32}, {1024, 1024, 0}}, "floor")
      .value();
  auto options = BrushSettleOptions{};
  options.kick = 0;
  const auto run =
    [&](const std::vector<Brush>& sources, const std::vector<Brush>& scenery) {
      auto created = BrushSettleSimulation::create(sources, scenery, bounds, options);
      REQUIRE(created);
      auto sim = std::move(created).value();
      for (int i = 0; !sim->finished() && i < 1201; ++i)
      {
        sim->step();
      }
      REQUIRE(sim->finished());
      return sim;
    };

  SECTION("Triangle patch surfaces catch falling brushes from either winding")
  {
    const auto reverse = GENERATE(false, true);
    auto triangles = std::vector<SettleTriangle>{
      {{{-1024, -1024, 0}, {1024, -1024, 0}, {1024, 1024, 0}}},
      {{{1024, 1024, 0}, {-1024, 1024, 0}, {-1024, -1024, 0}}}};
    if (reverse)
      for (auto& triangle : triangles)
        std::swap(triangle[0], triangle[2]);
    auto created = BrushSettleSimulation::create({cube}, {}, bounds, options, triangles);
    REQUIRE(created);
    auto sim = std::move(created).value();
    for (int i = 0; !sim->finished() && i < 1201; ++i)
      sim->step();
    REQUIRE(sim->sleeping());
    const auto result = sim->result();
    REQUIRE(result);
    CHECK(result.value().front().bounds().min.z() >= -0.25);
    CHECK(result.value().front().bounds().min.z() < 0.25);
  }

  SECTION("Falls onto an existing floor and sleeps")
  {
    auto sim = run({cube}, {floor});
    REQUIRE(sim->sleeping());
    const auto result = sim->result();
    REQUIRE(result);
    REQUIRE(result.value().size() == 1);
    const auto& settled = result.value().front();
    CHECK(settled.bounds().min.z() >= -0.25);
    CHECK(settled.bounds().min.z() < 0.2);
    for (const auto& vertex : cube.vertexPositions())
    {
      auto distance = std::numeric_limits<double>::max();
      for (const auto& actual : settled.vertexPositions())
      {
        distance = std::min(distance, vm::length(actual - sim->transforms()[0] * vertex));
      }
      CHECK(distance < 0.005);
    }
    for (size_t i = 0; i < cube.faceCount(); ++i)
    {
      CHECK(settled.faces()[i].attributes().materialName() == "stone");
      // UV locking follows the rigid body, including rotation.
      if (format == MapFormat::Valve)
      {
        const auto& face = cube.faces()[i];
        const auto transform = sim->transforms()[0];
        const auto normal = transform * face.boundary().normal - transform * vm::vec3d{};
        const auto match = settled.findFace(normal);
        REQUIRE(match);
        const auto p = face.vertexPositions().front();
        const auto difference =
          face.uvCoords(p) - settled.faces()[*match].uvCoords(transform * p);
        // Offsets are normalized to one texture repeat by Brush::transform.
        CHECK(std::abs(difference.x() - std::round(difference.x())) < 0.02);
        CHECK(std::abs(difference.y() - std::round(difference.y())) < 0.02);
      }
    }
  }
  SECTION("Stacked pieces collide with each other")
  {
    auto upper = cube;
    REQUIRE(upper.transform(bounds, vm::translation_matrix(vm::vec3d{0, 0, 48}), true));
    auto sim = run({cube, upper}, {floor});
    auto result = sim->result();
    REQUIRE(result);
    const auto& pieces = result.value();
    CHECK(pieces[0].bounds().min.z() >= -0.25);
    CHECK(pieces[1].bounds().min.z() >= pieces[0].bounds().max.z() - 0.25);
    CHECK(pieces[1].bounds().min.z() < 33);
  }
  SECTION("Fracture output can settle with reproducible kick and rotations")
  {
    options.kick = 96;
    options.duration = 12;
    const auto pieces =
      fractureBrush(cube, bounds, format, BrushFaceAttributes{"inside"}, 8, 17);
    REQUIRE(pieces);
    auto first = run(pieces.value(), {floor});
    auto second = run(pieces.value(), {floor});
    auto result = first->result();
    REQUIRE(result);
    REQUIRE(second->result());
    CHECK(result.value() == second->result().value());
    CHECK(first->transforms() == second->transforms());
    for (const auto& piece : result.value())
    {
      CHECK(piece.closed());
      CHECK(piece.fullySpecified());
      CHECK(piece.bounds().min.z() >= -0.25);
      CHECK(piece.bounds().max.z() < 100);
    }
    CHECK(result.value() != pieces.value());
  }
  SECTION("Thin pieces do not tunnel through a thin floor")
  {
    const auto thin =
      builder.createCuboid(vm::bbox3d{{-16, -16, 1000}, {16, 16, 1001}}, "thin").value();
    const auto thinFloor =
      builder.createCuboid(vm::bbox3d{{-1024, -1024, -1}, {1024, 1024, 0}}, "floor")
        .value();
    auto sim = run({thin}, {thinFloor});
    const auto result = sim->result();
    REQUIRE(result);
    CHECK(result.value()[0].bounds().min.z() >= -0.25);
    CHECK(result.value()[0].bounds().max.z() < 3);
  }
  SECTION("Walls contain pieces with a strong kick")
  {
    options.kick = 256;
    const auto left =
      builder.createCuboid(vm::bbox3d{{-80, -80, 0}, {-64, 80, 512}}, "wall").value();
    const auto right =
      builder.createCuboid(vm::bbox3d{{64, -80, 0}, {80, 80, 512}}, "wall").value();
    const auto front =
      builder.createCuboid(vm::bbox3d{{-64, -80, 0}, {64, -64, 512}}, "wall").value();
    const auto back =
      builder.createCuboid(vm::bbox3d{{-64, 64, 0}, {64, 80, 512}}, "wall").value();
    auto sim = run({cube}, {floor, left, right, front, back});
    const auto result = sim->result();
    REQUIRE(result);
    CHECK(result.value()[0].bounds().min.x() >= -64.25);
    CHECK(result.value()[0].bounds().min.y() >= -64.25);
    CHECK(result.value()[0].bounds().max.x() <= 64.25);
    CHECK(result.value()[0].bounds().max.y() <= 64.25);
    CHECK(result.value()[0].bounds().min.z() >= -0.25);
  }
  SECTION("Initial intersections and invalid parameters are rejected")
  {
    CHECK_FALSE(BrushSettleSimulation::create({cube, cube}, {floor}, bounds, options));
    CHECK_FALSE(BrushSettleSimulation::create({floor}, {floor}, bounds, options));
    CHECK_FALSE(BrushSettleSimulation::create({}, {floor}, bounds, options));
    options.friction = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(BrushSettleSimulation::create({cube}, {floor}, bounds, options));
    options.friction = 0.8;
    options.duration = 21;
    CHECK_FALSE(BrushSettleSimulation::create({cube}, {floor}, bounds, options));
  }
  SECTION("No implicit ground and out of bounds results cannot apply")
  {
    auto created = BrushSettleSimulation::create({cube}, {}, bounds, options);
    REQUIRE(created);
    auto sim = std::move(created).value();
    for (int i = 0; i < 10; ++i)
    {
      sim->step();
    }
    REQUIRE(sim->result());
    CHECK(sim->result().value()[0].bounds().min.z() < cube.bounds().min.z());
    while (!sim->finished())
    {
      sim->step();
    }
    CHECK_FALSE(sim->result());
  }
}
} // namespace tb::mdl
