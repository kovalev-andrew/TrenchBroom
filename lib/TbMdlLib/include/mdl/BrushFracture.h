/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Result.h"
#include "mdl/Brush.h"

#include <cstdint>
#include <vector>

namespace tb::mdl
{
class BrushFaceAttributes;
enum class MapFormat;

constexpr size_t MaxFractureFragments = 128;

// Explicit sites make the geometric operation independently testable. All sites
// must be distinct and strictly inside the source brush. Failure is all-or-nothing.
Result<std::vector<Brush>> fractureBrush(
  const Brush& source,
  const vm::bbox3d& worldBounds,
  MapFormat mapFormat,
  const BrushFaceAttributes& cutAttributes,
  const std::vector<vm::vec3d>& sites);

Result<std::vector<Brush>> fractureBrush(
  const Brush& source,
  const vm::bbox3d& worldBounds,
  MapFormat mapFormat,
  const BrushFaceAttributes& cutAttributes,
  size_t count,
  uint32_t seed);
} // namespace tb::mdl
