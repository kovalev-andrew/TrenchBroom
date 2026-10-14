/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "mdl/Brush.h"

#include <cstdint>

namespace tb::mdl
{
struct PerforateOptions
{
  size_t count = 4;
  size_t sides = 6;
  double minSize = 16;
  double maxSize = 32;
  double spacing = 4;
  uint32_t seed = 1;
};
struct Perforation
{
  std::vector<Brush> remainder;
  std::vector<Brush> plugs;
  kdl_reflect_decl(Perforation, remainder, plugs);
};
// Size is the maximum pairwise distance of the 2D contour, in map units.
Result<Perforation> perforateBrush(
  const Brush& source,
  size_t faceIndex,
  const vm::bbox3d& worldBounds,
  MapFormat format,
  const PerforateOptions& options);
} // namespace tb::mdl
