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

#include "ui/PrefabTool.h"

#include "Logger.h"
#include "SimpleParserStatus.h"
#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/NodeReader.h"
#include "mdl/PickResult.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"
#include "kd/k.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto PrefabPayloadPrefix = "prefab\n";

std::string serializedPrefabFromPayload(const std::string& payload)
{
  const auto prefix = std::string{PrefabPayloadPrefix};
  return payload.rfind(prefix, 0) == 0 ? payload.substr(prefix.size()) : "";
}

vm::bbox3d prefabBounds(const std::vector<mdl::Node*>& nodes)
{
  auto bounds = nodes.front()->logicalBounds();
  for (auto* node : nodes)
  {
    bounds = vm::merge(bounds, node->logicalBounds());
  }
  return bounds;
}

void resetPersistentGroupIds(mdl::Node& node)
{
  if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node))
  {
    groupNode->resetPersistentId();
  }

  for (auto* child : node.children())
  {
    resetPersistentGroupIds(*child);
  }
}

void collectGroupNodes(mdl::Node& node, std::vector<mdl::GroupNode*>& groups)
{
  if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node))
  {
    groups.push_back(groupNode);
  }

  for (auto* child : node.children())
  {
    collectGroupNodes(*child, groups);
  }
}

void collectGroupNames(const mdl::Node& node, std::set<std::string>& names)
{
  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    names.insert(groupNode->group().name());
  }

  for (const auto* child : node.children())
  {
    collectGroupNames(*child, names);
  }
}

std::string makeUniqueGroupName(const std::string& baseName, std::set<std::string>& names)
{
  const auto name = baseName.empty() ? std::string{"Prefab"} : baseName;
  if (names.insert(name).second)
  {
    return name;
  }

  for (auto index = size_t{2};; ++index)
  {
    auto candidate = name + " " + std::to_string(index);
    if (names.insert(candidate).second)
    {
      return candidate;
    }
  }
}

void makePrefabGroupNamesUnique(mdl::Node& node, std::set<std::string>& names)
{
  if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node))
  {
    auto group = groupNode->group();
    group.setName(makeUniqueGroupName(group.name(), names));
    groupNode->setGroup(std::move(group));
  }

  for (auto* child : node.children())
  {
    makePrefabGroupNamesUnique(*child, names);
  }
}

} // namespace

PrefabTool::PrefabTool(MapDocument& document)
  : Tool{K(initiallyActive)}
  , m_document{document}
{
}

bool PrefabTool::createPrefab(const std::string& payload)
{
  const auto serializedPrefab = serializedPrefabFromPayload(payload);
  if (serializedPrefab.empty())
  {
    return false;
  }

  auto& map = m_document.map();
  auto parserStatus = SimpleParserStatus{map.logger()};

  auto readResult = mdl::NodeReader::read(
    serializedPrefab,
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.worldNode().entityPropertyConfig(),
    parserStatus,
    map.taskManager());

  if (!readResult)
  {
    std::visit(
      [&](const auto& error) {
        map.logger().error() << "Could not parse prefab payload: " << error.msg;
      },
      readResult.error());
    return false;
  }

  auto nodes = std::move(readResult).value();
  m_referenceBounds = map.referenceBounds();
  m_lockedPlacementPlane = std::nullopt;

  map.startTransaction("Create Prefab", mdl::TransactionScope::LongRunning);

  auto usedGroupNames = std::set<std::string>{};
  auto prefabGroups = std::vector<mdl::GroupNode*>{};
  collectGroupNames(map.worldNode(), usedGroupNames);
  for (auto* node : nodes)
  {
    resetPersistentGroupIds(*node);
    collectGroupNodes(*node, prefabGroups);
    makePrefabGroupNamesUnique(*node, usedGroupNames);
  }
  mdl::resetLinkIds(prefabGroups);

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{
    {mdl::parentForNodes(map), std::move(nodes)}};
  m_prefabNodes = mdl::addNodes(map, nodesToAdd);
  if (m_prefabNodes.empty())
  {
    map.cancelTransaction();
    return false;
  }

  mdl::deselectAll(map);
  mdl::selectNodes(map, m_prefabNodes);
  return true;
}

bool prefabContainsNode(const std::vector<mdl::Node*>& prefabNodes, const mdl::Node& node)
{
  return std::ranges::any_of(prefabNodes, [&](const auto* prefabNode) {
    return prefabNode == &node || node.isDescendantOf(*prefabNode);
  });
}

void PrefabTool::removePrefab()
{
  contract_pre(prefabActive());

  m_document.map().cancelTransaction();
  m_prefabNodes.clear();
  m_lockedPlacementPlane = std::nullopt;
}

void PrefabTool::commitPrefab()
{
  contract_pre(prefabActive());

  m_document.map().commitTransaction();
  m_prefabNodes.clear();
  m_lockedPlacementPlane = std::nullopt;
}

void PrefabTool::updatePrefabPosition2D(const vm::ray3d& pickRay)
{
  contract_pre(prefabActive());

  const auto toMin = m_referenceBounds.min - pickRay.origin;
  const auto toMax = m_referenceBounds.max - pickRay.origin;
  const auto anchor = dot(toMin, pickRay.direction) > dot(toMax, pickRay.direction)
                        ? m_referenceBounds.min
                        : m_referenceBounds.max;
  const auto dragPlane = vm::plane3d(anchor, -pickRay.direction);

  auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto delta = grid.moveDeltaForBounds(
    dragPlane, prefabBounds(m_prefabNodes), map.worldBounds(), pickRay);

  if (!vm::is_zero(delta, vm::Cd::almost_zero()))
  {
    translateSelection(map, delta);
  }
}

void PrefabTool::updatePrefabPosition3D(
  const vm::ray3d& pickRay, const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  contract_pre(prefabActive());

  auto& map = m_document.map();

  auto delta = vm::vec3d{};
  const auto& grid = map.grid();
  const auto bounds = prefabBounds(m_prefabNodes);
  if (!m_lockedPlacementPlane)
  {
    for (const auto& hit : pickResult.all(type(mdl::BrushNode::BrushHitType)))
    {
      if (const auto candidate = mdl::hitToFaceHandle(hit))
      {
        if (prefabContainsNode(m_prefabNodes, *candidate->node()))
        {
          break;
        }

        m_lockedPlacementPlane = candidate->face().boundary();
        break;
      }
    }
  }

  if (m_lockedPlacementPlane)
  {
    delta = grid.moveDeltaForBounds(
      *m_lockedPlacementPlane, bounds, map.worldBounds(), pickRay);
  }
  else
  {
    const auto newPosition = vm::point_at_distance(
      pickRay, static_cast<double>(gl::Camera::DefaultPointDistance));
    const auto boundsCenter = bounds.center();
    delta = grid.moveDeltaForPoint(boundsCenter, newPosition - boundsCenter);
  }

  if (!vm::is_zero(delta, vm::Cd::almost_zero()))
  {
    translateSelection(map, delta);
  }
}

bool PrefabTool::prefabActive() const
{
  return !m_prefabNodes.empty();
}

} // namespace tb::ui
