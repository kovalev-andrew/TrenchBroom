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

#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h" // IWYU pragma: keep
#include "mdl/GameInfo.h"   // IWYU pragma: keep
#include "mdl/Map_Nodes.h"  // IWYU pragma: keep
#include "mdl/NodeHandleCommand.h"
#include "mdl/Polyhedron3.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h" // IWYU pragma: keep
#include "ui/NodeHandleToolBase.h"

#include "kd/result.h"

namespace tb::ui
{
template <typename HandleType>
class BrushHandleToolBase : public NodeHandleToolBase<HandleType>
{
protected:
  explicit BrushHandleToolBase(MapDocument& document)
    : NodeHandleToolBase<HandleType>{document}
  {
  }

public:
  // Geometry tools keep preview state as positions while the editor owns typed handles.
  class GeometryHandles
  {
    mdl::NodeHandleManager& m_manager;

  public:
    explicit GeometryHandles(mdl::NodeHandleManager& manager)
      : m_manager{manager}
    {
    }
    auto selectedHandles() const
    {
      return HandleType::getPositions(m_manager.template selectedHandles<HandleType>());
    }
    size_t selectedHandleCount() const
    {
      return m_manager.template selectedHandleCount<HandleType>();
    }
    void deselectAll() { m_manager.template deselectAllHandles<HandleType>(); }
    void select(const typename HandleType::Position& position)
    {
      const auto handle = HandleType{position};
      if (m_manager.template containsHandle<HandleType>(handle))
        m_manager.template selectHandle<HandleType>(handle);
    }
    void select(const std::vector<typename HandleType::Position>& positions)
    {
      for (const auto& position : positions)
        select(position);
    }
  };

  GeometryHandles geometryHandles() const
  {
    return GeometryHandles{this->m_document.map().nodeHandles()};
  }

  std::vector<mdl::BrushNode*> findIncidentBrushes(
    const typename HandleType::Position& position) const
  {
    auto result = std::vector<mdl::BrushNode*>{};
    for (auto* node : this->findIncidentNodes(HandleType{position}))
      if (auto* brush = dynamic_cast<mdl::BrushNode*>(node))
        result.push_back(brush);
    return result;
  }

  bool canDoCsgConvexMerge() const
  {
    return this->handleManager().template selectedHandleCount<HandleType>() > 1;
  }

  void csgConvexMerge()
  {
    auto handles = this->handleManager().template selectedHandles<HandleType>();
    const auto vertices = HandleType::getVertices(handles);

    const auto polyhedron = mdl::Polyhedron3{vertices};
    if (!polyhedron.polyhedron() || !polyhedron.closed())
    {
      return;
    }

    auto& map = this->m_document.map();

    const auto builder = mdl::BrushBuilder{
      map.worldNode().mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaults};
    builder.createBrush(polyhedron, map.currentMaterialName())
      | kdl::transform([&](auto b) {
          for (const auto* selectedBrushNode : map.selection().brushes)
          {
            b.cloneFaceAttributesFrom(selectedBrushNode->brush());
          }

          auto* newParent = parentForNodes(map, map.selection().nodes);
          auto transaction = mdl::Transaction{map, "CSG Convex Merge"};
          this->deselectAll();
          if (addNodes(map, {{newParent, {new mdl::BrushNode{std::move(b)}}}}).empty())
          {
            transaction.cancel();
            return;
          }
          transaction.commit();
        })
      | kdl::transform_error(
        [&](auto e) { map.logger().error() << "Could not create brush: " << e.msg; });
  }

  bool canRemoveSelection() const
  {
    return this->handleManager().template selectedHandleCount<HandleType>() > 0;
  }
};

} // namespace tb::ui
