/*
 Copyright (C) 2026 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "ui/Tool.h"

#include "vm/bbox.h"
#include "vm/plane.h"
#include "vm/ray.h"

#include <optional>
#include <string>
#include <vector>

namespace tb
{
namespace mdl
{
class Node;
class PickResult;
} // namespace mdl

namespace ui
{
class MapDocument;

class PrefabTool : public Tool
{
private:
  MapDocument& m_document;
  std::vector<mdl::Node*> m_prefabNodes;
  vm::bbox3d m_referenceBounds;
  std::optional<vm::plane3d> m_lockedPlacementPlane;

public:
  explicit PrefabTool(MapDocument& document);

  bool createPrefab(const std::string& payload);
  void removePrefab();
  void commitPrefab();

  void updatePrefabPosition2D(const vm::ray3d& pickRay);
  void updatePrefabPosition3D(
    const vm::ray3d& pickRay, const mdl::PickResult& pickResult);

private:
  bool prefabActive() const;
};

} // namespace ui
} // namespace tb
