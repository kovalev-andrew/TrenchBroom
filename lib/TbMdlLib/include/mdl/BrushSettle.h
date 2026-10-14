/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "mdl/Brush.h"

#include <array>
#include <cstdint>
#include <memory>

namespace tb::mdl
{
using SettleTriangle = std::array<vm::vec3d, 3>;

struct BrushSettleOptions
{
  double friction = 0.8;
  double restitution = 0.05;
  double kick = 32.0; // Map units / second. Zero starts at rest.
  double duration = 8.0;
  uint32_t seed = 1;
};

// An isolated, fixed-step simulation. No document nodes or commands are retained.
class BrushSettleSimulation
{
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  explicit BrushSettleSimulation(std::unique_ptr<Impl> impl);

public:
  static constexpr size_t MaxBodies = 128;
  static constexpr size_t MaxObstacles = 4096;
  static Result<std::unique_ptr<BrushSettleSimulation>> create(
    std::vector<Brush> brushes,
    const std::vector<Brush>& obstacles,
    const vm::bbox3d& worldBounds,
    const BrushSettleOptions& options,
    const std::vector<SettleTriangle>& patchTriangles = {});
  ~BrushSettleSimulation();
  void step(); // Advances 1/60 second using four 1/240 second physics steps.
  bool finished() const;
  bool sleeping() const;
  double elapsed() const;
  std::vector<vm::mat4x4d> transforms() const;
  // Validates bounds, contacts and transformed brush geometry before returning copies.
  Result<std::vector<Brush>> result();
};
} // namespace tb::mdl
