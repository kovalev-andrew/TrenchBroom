/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "Result.h"
#include "mdl/Brush.h"

namespace tb::mdl
{
enum class BevelSize
{
  Radius,
  Offset
};
struct BevelParameters
{
  double size = 8.0;
  size_t segments = 4;
  double profile = 0.5;
  BevelSize sizeType = BevelSize::Radius;
  bool clamp = true;
};
struct BevelSelection
{
  std::vector<vm::segment3d> edges;
  std::vector<vm::vec3d> vertices;
};
struct BevelResult
{
  Brush brush;
  double appliedSize;
  bool clamped;
};

// Edges shared by two selected faces are deliberately excluded.
std::vector<vm::segment3d> bevelFaceBoundary(
  const Brush& brush, const std::vector<size_t>& faces);

Result<BevelResult> bevelBrush(
  const Brush& source,
  const vm::bbox3d& worldBounds,
  MapFormat format,
  const BevelSelection& selection,
  const BevelParameters& parameters);
} // namespace tb::mdl
