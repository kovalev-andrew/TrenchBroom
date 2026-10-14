/*
 Copyright (C) 2025 Kristian Duske

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

#include "mdl/Map_Geometry.h"

#include "Logger.h"
#include "mdl/AddRemoveNodesCommand.h"
#include "mdl/ApplyAndSwap.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/BrushVertexCommands.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/BrushPerforate.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeHandles.h"
#include "mdl/PatchNode.h"
#include "mdl/Polyhedron3.h"
#include "mdl/SetLinkIdsCommand.h"
#include "mdl/SwapNodeContentsCommand.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/as_rvalue_view.h"
#include "kd/ranges/to.h"
#include "kd/reflection_impl.h"
#include "kd/result_fold.h"
#include "kd/string_format.h"
#include "kd/task_manager.h"
#include "kd/vector_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace tb::mdl
{

kdl_reflect_impl(TransformVerticesResult);

namespace
{
struct BrushConversionCluster
{
  Node* parent = nullptr;
  std::vector<BrushNode*> brushes;
};

bool planesEqual(const vm::plane3d& lhs, const vm::plane3d& rhs)
{
  return 1.0 - vm::dot(lhs.normal, rhs.normal) < 0.001
         && std::abs(lhs.distance - rhs.distance) < 0.1;
}

Result<Brush> createBrushFromUniqueHullPlanes(
  const Polyhedron3& polyhedron, const Map& map, const std::string& materialName)
{
  if (polyhedron.empty())
  {
    return Error{"Cannot create brush from empty polyhedron"};
  }

  auto faces = std::vector<BrushFace>{};
  const auto defaultAttribs = BrushFaceAttributes{
    materialName, map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  for (const auto* face : polyhedron.faces())
  {
    const auto& plane = face->plane();
    if (std::ranges::any_of(faces, [&](const auto& candidate) {
          return planesEqual(candidate.boundary(), plane);
        }))
    {
      continue;
    }

    auto bIt = std::begin(face->boundary());
    const auto* edge1 = *bIt++;
    const auto* edge2 = *bIt++;
    const auto* edge3 = *bIt++;

    const auto& p1 = edge1->origin()->position();
    const auto& p2 = edge2->origin()->position();
    const auto& p3 = edge3->origin()->position();

    auto brushFace =
      BrushFace::create(p1, p3, p2, defaultAttribs, map.worldNode().mapFormat());
    if (!brushFace)
    {
      return std::get<Error>(brushFace.error());
    }
    faces.push_back(std::move(brushFace).value());
  }

  return Brush::create(map.worldBounds(), std::move(faces));
}

Result<Brush> createBrushFromUniqueSourcePlanes(
  const Brush& sourceBrush, const Map& map, const std::string& materialName)
{
  auto faces = std::vector<BrushFace>{};
  faces.reserve(sourceBrush.faceCount());

  for (const auto& face : sourceBrush.faces())
  {
    if (std::ranges::any_of(faces, [&](const auto& candidate) {
          return planesEqual(candidate.boundary(), face.boundary());
        }))
    {
      continue;
    }
    faces.push_back(face);
  }

  if (faces.size() < 4u)
  {
    return Error{"Not enough unique planes to create brush"};
  }

  auto brushResult = Brush::create(map.worldBounds(), std::move(faces));
  if (brushResult)
  {
    auto brush = std::move(brushResult).value();
    brush.cloneFaceAttributesFrom(sourceBrush);
    return brush;
  }

  return BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}.createBrush(
    sourceBrush.vertexPositions(), materialName);
}

Result<Brush> createBestEffortTriangulatedBrush(
  const BrushNode& brushNode, const Map& map, const std::string& materialName)
{
  const auto& sourceBrush = brushNode.brush();
  const auto usefulSimplification = [&](const Brush& brush) {
    return sourceBrush.faceCount() <= 6u || brush.faceCount() < sourceBrush.faceCount();
  };

  auto sourcePlaneBrushResult =
    createBrushFromUniqueSourcePlanes(sourceBrush, map, materialName);
  if (sourcePlaneBrushResult)
  {
    auto brush = std::move(sourcePlaneBrushResult).value();
    if (usefulSimplification(brush))
    {
      return brush;
    }
  }

  auto points = std::vector<vm::vec3d>{};
  points.reserve(sourceBrush.vertices().size());
  for (const auto* vertex : sourceBrush.vertices())
  {
    points.push_back(vertex->position());
  }

  if (points.size() >= 4u)
  {
    const auto polyhedron = Polyhedron3{std::move(points)};
    if (polyhedron.polyhedron() && polyhedron.closed())
    {
      auto brushResult = createBrushFromUniqueHullPlanes(polyhedron, map, materialName);
      if (brushResult)
      {
        auto brush = std::move(brushResult).value();
        if (usefulSimplification(brush))
        {
          return brush;
        }
      }

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
      brushResult = builder.createBrush(polyhedron, materialName);
      if (brushResult)
      {
        auto brush = std::move(brushResult).value();
        if (usefulSimplification(brush))
        {
          return brush;
        }
      }
    }
  }

  return Error{"Could not simplify triangulated brush without changing its shape"};
}

Result<Brush> createBestEffortTriangulatedBrush(
  const std::vector<BrushNode*>& brushNodes,
  const Map& map,
  const std::string& materialName)
{
  if (brushNodes.empty())
  {
    return Error{"Cannot convert empty brush selection"};
  }
  if (brushNodes.size() == 1u)
  {
    return createBestEffortTriangulatedBrush(*brushNodes.front(), map, materialName);
  }

  auto sourceFaceCount = size_t{0u};
  auto points = std::vector<vm::vec3d>{};
  for (const auto* brushNode : brushNodes)
  {
    const auto& brush = brushNode->brush();
    sourceFaceCount += brush.faceCount();
    points.reserve(points.size() + brush.vertices().size());
    for (const auto* vertex : brush.vertices())
    {
      points.push_back(vertex->position());
    }
  }

  const auto usefulSimplification = [&](const Brush& brush) {
    return brush.faceCount() < sourceFaceCount;
  };

  if (points.size() >= 4u)
  {
    const auto polyhedron = Polyhedron3{std::move(points)};
    if (polyhedron.polyhedron() && polyhedron.closed())
    {
      auto brushResult = createBrushFromUniqueHullPlanes(polyhedron, map, materialName);
      if (brushResult)
      {
        auto brush = std::move(brushResult).value();
        if (usefulSimplification(brush))
        {
          return brush;
        }
      }

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
      brushResult = builder.createBrush(polyhedron, materialName);
      if (brushResult)
      {
        auto brush = std::move(brushResult).value();
        if (usefulSimplification(brush))
        {
          return brush;
        }
      }
    }
  }

  return Error{
    "Could not simplify triangulated brush cluster without changing its shape"};
}

std::vector<const Brush*> toSourceBrushes(const std::vector<BrushNode*>& brushNodes)
{
  auto result = std::vector<const Brush*>{};
  result.reserve(brushNodes.size());
  for (const auto* brushNode : brushNodes)
  {
    result.push_back(&brushNode->brush());
  }
  return result;
}

std::vector<BrushNode*> collectBrushesInSubtree(Node& node)
{
  auto result = std::vector<BrushNode*>{};
  node.accept(
    kdl::overload(
      [&](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [&](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](
        auto&& thisLambda, GroupNode& groupNode) { groupNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, EntityNode& entityNode) {
        entityNode.visitChildren(thisLambda);
      },
      [&](BrushNode& brushNode) { result.push_back(&brushNode); },
      [&](PatchNode&) {}));
  return kdl::vec_sort_and_remove_duplicates(std::move(result));
}

std::vector<BrushNode*> collectAllBrushes(Map& map)
{
  return collectBrushesInSubtree(map.worldNode());
}

std::vector<BrushNode*> collectSelectedBrushesForConversion(Map& map)
{
  auto result = std::vector<BrushNode*>{};
  const auto& selection = map.selection();
  const auto& allBrushes = selection.allBrushes();
  result.insert(result.end(), allBrushes.begin(), allBrushes.end());
  for (const auto& faceHandle : selection.brushFaces)
  {
    result.push_back(faceHandle.node());
  }

  const auto collect = [&]<typename Handle>() {
    if (!map.nodeHandles().anyHandleSelected<Handle>())
      return;
    for (auto* brush : collectAllBrushes(map))
      for (const auto& handle : Handle::getHandles(*brush))
        if (map.nodeHandles().isHandleSelected<Handle>(handle))
        {
          result.push_back(brush);
          break;
        }
  };
  collect.template operator()<VertexHandle>();
  collect.template operator()<EdgeHandle>();
  collect.template operator()<FaceHandle>();

  return kdl::vec_sort_and_remove_duplicates(std::move(result));
}

std::vector<BrushConversionCluster> collectBrushConversionClusters(Map& map)
{
  auto result = std::vector<BrushConversionCluster>{};
  auto claimedBrushes = std::vector<BrushNode*>{};

  for (auto* node : map.selection().nodes)
  {
    node->accept(
      kdl::overload(
        [&](GroupNode& groupNode) {
          auto brushes = collectBrushesInSubtree(groupNode);
          if (!brushes.empty())
          {
            claimedBrushes.insert(
              std::end(claimedBrushes), std::begin(brushes), std::end(brushes));
            result.push_back(BrushConversionCluster{&groupNode, std::move(brushes)});
          }
        },
        [&](EntityNode& entityNode) {
          auto brushes = collectBrushesInSubtree(entityNode);
          if (!brushes.empty())
          {
            claimedBrushes.insert(
              std::end(claimedBrushes), std::begin(brushes), std::end(brushes));
            result.push_back(BrushConversionCluster{&entityNode, std::move(brushes)});
          }
        },
        [&](WorldNode&) {},
        [&](LayerNode&) {},
        [&](BrushNode&) {},
        [&](PatchNode&) {}));
  }

  claimedBrushes = kdl::vec_sort_and_remove_duplicates(std::move(claimedBrushes));

  for (auto* brushNode : collectSelectedBrushesForConversion(map))
  {
    if (!kdl::vec_contains(claimedBrushes, brushNode))
    {
      result.push_back(BrushConversionCluster{brushNode->parent(), {brushNode}});
    }
  }

  return result;
}
} // namespace

bool transformSelection(
  Map& map, const std::string& commandName, const vm::mat4x4d& transformation)
{
  if (map.nodeHandles().anyHandleSelected<VertexHandle>())
  {
    const auto selectedVertexPositions =
      VertexHandle::getPositions(map.nodeHandles().selectedHandles<VertexHandle>());
    return transformVertices(map, selectedVertexPositions, transformation).success;
  }

  auto nodesToTransform = std::vector<Node*>{};
  auto entitiesToTransform = std::unordered_map<EntityNodeBase*, size_t>{};

  for (auto* node : map.selection().nodes)
  {
    node->accept(kdl::overload(
      [&](
        auto&& thisLambda, WorldNode& worldNode) { worldNode.visitChildren(thisLambda); },
      [&](
        auto&& thisLambda, LayerNode& layerNode) { layerNode.visitChildren(thisLambda); },
      [&](auto&& thisLambda, GroupNode& groupNode) {
        nodesToTransform.push_back(&groupNode);
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, EntityNode& entityNode) {
        if (!entityNode.hasChildren())
        {
          nodesToTransform.push_back(&entityNode);
        }
        else
        {
          entityNode.visitChildren(thisLambda);
        }
      },
      [&](BrushNode& brushNode) {
        nodesToTransform.push_back(&brushNode);
        entitiesToTransform[brushNode.entity()]++;
      },
      [&](PatchNode& patchNode) {
        nodesToTransform.push_back(&patchNode);
        entitiesToTransform[patchNode.entity()]++;
      }));
  }

  // add entities if all of their children are transformed
  for (const auto& [entityNode, transformedChildCount] : entitiesToTransform)
  {
    if (
      transformedChildCount == entityNode->childCount()
      && !isWorldspawn(entityNode->entity().classname()))
    {
      nodesToTransform.push_back(entityNode);
    }
  }

  using TransformResult = Result<std::pair<Node*, NodeContents>>;

  const auto alignmentLock = map.editorContext().alignmentLock();
  const auto updateAngleProperty =
    map.worldNode().entityPropertyConfig().updateAnglePropertyAfterTransform;

  auto tasks =
    nodesToTransform | std::views::transform([&](auto& node) {
      return std::function{[&]() {
        return node->accept(kdl::overload(
          [&](WorldNode&) -> TransformResult { contract_assert(false); },
          [&](LayerNode&) -> TransformResult { contract_assert(false); },
          [&](GroupNode& groupNode) -> TransformResult {
            auto group = groupNode.group();
            group.transform(transformation);
            return std::make_pair(&groupNode, NodeContents{std::move(group)});
          },
          [&](EntityNode& entityNode) -> TransformResult {
            auto entity = entityNode.entity();
            entity.transform(transformation, updateAngleProperty);
            return std::make_pair(&entityNode, NodeContents{std::move(entity)});
          },
          [&](BrushNode& brushNode) -> TransformResult {
            const auto* containingGroup = brushNode.containingGroup();
            const bool lockAlignment =
            alignmentLock
            || (containingGroup && containingGroup->closed() && collectLinkedNodes({&map.worldNode()}, brushNode).size() > 1);

            auto brush = brushNode.brush();
            return brush.transform(map.worldBounds(), transformation, lockAlignment)
                   | kdl::and_then([&]() -> TransformResult {
                       return std::make_pair(&brushNode, NodeContents{std::move(brush)});
                     });
          },
          [&](PatchNode& patchNode) -> TransformResult {
            auto patch = patchNode.patch();
            patch.transform(transformation);
            return std::make_pair(&patchNode, NodeContents{std::move(patch)});
          }));
      }};
    });

  const auto success = map.taskManager().run_tasks_and_wait(tasks) | kdl::fold
                       | kdl::transform([&](auto nodesToUpdate) {
                           return updateNodeContents(
                             map,
                             commandName,
                             std::move(nodesToUpdate),
                             collectContainingGroups(map.selection().nodes));
                         })
                       | kdl::value_or(false);

  if (success)
  {
    map.pushRepeatableCommand([&, commandName, transformation]() {
      transformSelection(map, commandName, transformation);
    });
  }

  return success;
}

bool translateSelection(mdl::Map& map, const vm::vec3d& delta)
{
  return transformSelection(map, "Translate Objects", vm::translation_matrix(delta));
}

bool rotateSelection(
  mdl::Map& map, const vm::vec3d& center, const vm::vec3d& axis, double angle)
{
  const auto transformation = vm::translation_matrix(center)
                              * vm::rotation_matrix(axis, angle)
                              * vm::translation_matrix(-center);
  return transformSelection(map, "Rotate Objects", transformation);
}

bool scaleSelection(mdl::Map& map, const vm::bbox3d& oldBBox, const vm::bbox3d& newBBox)
{
  const auto transformation = vm::scale_bbox_matrix(oldBBox, newBBox);
  return transformSelection(map, "Scale Objects", transformation);
}

bool scaleSelection(mdl::Map& map, const vm::vec3d& center, const vm::vec3d& scaleFactors)
{
  const auto transformation = vm::translation_matrix(center)
                              * vm::scaling_matrix(scaleFactors)
                              * vm::translation_matrix(-center);
  return transformSelection(map, "Scale Objects", transformation);
}

bool shearSelection(
  mdl::Map& map,
  const vm::bbox3d& box,
  const vm::vec3d& sideToShear,
  const vm::vec3d& delta)
{
  const auto transformation = vm::shear_bbox_matrix(box, sideToShear, delta);
  return transformSelection(map, "Scale Objects", transformation);
}

bool flipSelection(mdl::Map& map, const vm::vec3d& center, const vm::axis::type axis)
{
  const auto transformation = vm::translation_matrix(center)
                              * vm::mirror_matrix<double>(axis)
                              * vm::translation_matrix(-center);
  return transformSelection(map, "Flip Objects", transformation);
}


TransformVerticesResult transformVertices(
  Map& map, const std::vector<vm::vec3d>& vertexPositions, const vm::mat4x4d& transform)
{
  auto newVertexPositions = std::vector<vm::vec3d>{};
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto verticesToMove = vertexPositions
                                    | std::views::filter([&](const auto& vertex) {
                                        return brush.hasVertex(vertex);
                                      })
                                    | kdl::ranges::to<std::vector>();
        if (verticesToMove.empty())
        {
          return true;
        }

        if (!brush.canTransformVertices(map.worldBounds(), verticesToMove, transform))
        {
          return false;
        }

        return brush.transformVertices(
                 map.worldBounds(),
                 verticesToMove,
                 transform,
                 map.editorContext().uvLock())
               | kdl::transform([&]() {
                   auto newPositions =
                     brush.findClosestVertexPositions(transform * verticesToMove);
                   kdl::vec_append(newVertexPositions, std::move(newPositions));
                 })
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not move brush vertices: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (!newNodes)
  {
    return TransformVerticesResult{false, false};
  }

  kdl::vec_sort_and_remove_duplicates(newVertexPositions);
  const auto hasRemainingVertices = !newVertexPositions.empty();

  auto commandName =
    kdl::str_plural(vertexPositions.size(), "Move Brush Vertex", "Move Brush Vertices");
  auto transaction = Transaction{map, commandName};

  const auto changedLinkedGroups = collectContainingGroups(
    *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

  auto command = std::make_unique<BrushVertexCommand>(
    std::move(commandName), std::move(*newNodes), vertexPositions, newVertexPositions);

  if (!map.executeAndStore(std::move(command)))
  {
    transaction.cancel();
    return TransformVerticesResult{false, false};
  }

  setHasPendingChanges(changedLinkedGroups, true);

  if (!transaction.commit())
  {
    return TransformVerticesResult{false, false};
  }

  return {true, hasRemainingVertices};
}

bool transformEdges(
  Map& map, const std::vector<vm::segment3d>& edgePositions, const vm::mat4x4d& transform)
{
  auto newEdgePositions = std::vector<vm::segment3d>{};
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto edgesToMove =
          edgePositions
          | std::views::filter([&](const auto& edge) { return brush.hasEdge(edge); })
          | kdl::ranges::to<std::vector>();
        if (edgesToMove.empty())
        {
          return true;
        }

        if (!brush.canTransformEdges(map.worldBounds(), edgesToMove, transform))
        {
          return false;
        }

        return brush.transformEdges(
                 map.worldBounds(), edgesToMove, transform, map.editorContext().uvLock())
               | kdl::transform([&]() {
                   auto newPositions = brush.findClosestEdgePositions(
                     edgesToMove | std::views::transform([&](const auto& edge) {
                       return edge.transform(transform);
                     })
                     | kdl::ranges::to<std::vector>());
                   kdl::vec_append(newEdgePositions, std::move(newPositions));
                 })
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not move brush edges: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    kdl::vec_sort_and_remove_duplicates(newEdgePositions);

    const auto commandName =
      kdl::str_plural(edgePositions.size(), "Move Brush Edge", "Move Brush Edges");
    auto transaction = Transaction{map, commandName};

    const auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushEdgeCommand>(
      commandName, std::move(*newNodes), edgePositions, newEdgePositions));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool transformFaces(
  Map& map, const std::vector<vm::polygon3d>& facePositions, const vm::mat4x4d& transform)
{
  auto newFacePositions = std::vector<vm::polygon3d>{};
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto facesToMove =
          facePositions
          | std::views::filter([&](const auto& face) { return brush.hasFace(face); })
          | kdl::ranges::to<std::vector>();
        if (facesToMove.empty())
        {
          return true;
        }

        if (!brush.canTransformFaces(map.worldBounds(), facesToMove, transform))
        {
          return false;
        }

        return brush.transformFaces(
                 map.worldBounds(), facesToMove, transform, map.editorContext().uvLock())
               | kdl::transform([&]() {
                   auto newPositions = brush.findClosestFacePositions(
                     facesToMove | std::views::transform([&](const auto& face) {
                       return face.transform(transform);
                     })
                     | kdl::ranges::to<std::vector>());
                   kdl::vec_append(newFacePositions, std::move(newPositions));
                 })
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not move brush faces: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    kdl::vec_sort_and_remove_duplicates(newFacePositions);

    const auto commandName =
      kdl::str_plural(facePositions.size(), "Move Brush Face", "Move Brush Faces");
    auto transaction = Transaction{map, commandName};

    auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushFaceCommand>(
      commandName, std::move(*newNodes), facePositions, newFacePositions));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool addVertex(Map& map, const vm::vec3d& vertexPosition)
{
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        if (!brush.canAddVertex(map.worldBounds(), vertexPosition))
        {
          return false;
        }

        return brush.addVertex(map.worldBounds(), vertexPosition)
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not add brush vertex: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    const auto commandName = "Add Brush Vertex";
    auto transaction = Transaction{map, commandName};

    const auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushVertexCommand>(
      commandName,
      std::move(*newNodes),
      std::vector<vm::vec3d>{},
      std::vector<vm::vec3d>{vertexPosition}));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool removeVertices(
  Map& map, const std::string& commandName, std::vector<vm::vec3d> vertexPositions)
{
  auto newNodes = applyToNodeContents(
    map.selection().nodes,
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto verticesToRemove = vertexPositions
                                      | std::views::filter([&](const auto& vertex) {
                                          return brush.hasVertex(vertex);
                                        })
                                      | kdl::ranges::to<std::vector>();
        if (verticesToRemove.empty())
        {
          return true;
        }

        if (!brush.canRemoveVertices(map.worldBounds(), verticesToRemove))
        {
          return false;
        }

        return brush.removeVertices(map.worldBounds(), verticesToRemove)
               | kdl::if_error([&](auto e) {
                   map.logger().error() << "Could not remove brush vertices: " << e.msg;
                 })
               | kdl::is_success();
      },
      [](BezierPatch&) { return true; }));

  if (newNodes)
  {
    auto transaction = Transaction{map, commandName};

    auto changedLinkedGroups = collectContainingGroups(
      *newNodes | std::views::keys | kdl::ranges::to<std::vector>());

    const auto result = map.executeAndStore(std::make_unique<BrushVertexCommand>(
      commandName, std::move(*newNodes), vertexPositions, std::vector<vm::vec3d>{}));

    if (!result)
    {
      transaction.cancel();
      return false;
    }

    setHasPendingChanges(changedLinkedGroups, true);
    return transaction.commit();
  }

  return false;
}

bool snapVertices(Map& map, const double snapTo)
{
  size_t succeededBrushCount = 0;
  size_t failedBrushCount = 0;

  const auto allSelectedBrushes = map.selection().allBrushes();
  const bool applyAndSwapSuccess = applyAndSwap(
    map,
    "Snap Brush Vertices",
    allSelectedBrushes,
    collectContainingGroups(kdl::vec_static_cast<Node*>(allSelectedBrushes)),
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& originalBrush) {
        if (originalBrush.canSnapVertices(map.worldBounds(), snapTo))
        {
          originalBrush.snapVertices(
            map.worldBounds(), snapTo, map.editorContext().uvLock())
            | kdl::transform([&]() { succeededBrushCount += 1; })
            | kdl::transform_error([&](auto e) {
                map.logger().error() << "Could not snap vertices: " << e.msg;
                failedBrushCount += 1;
              });
        }
        else
        {
          failedBrushCount += 1;
        }
        return true;
      },
      [](BezierPatch&) { return true; }));

  if (!applyAndSwapSuccess)
  {
    return false;
  }
  if (succeededBrushCount > 0)
  {
    map.logger().info() << fmt::format(
      "Snapped vertices of {} {}",
      succeededBrushCount,
      kdl::str_plural(succeededBrushCount, "brush", "brushes"));
  }
  if (failedBrushCount > 0)
  {
    map.logger().info() << fmt::format(
      "Failed to snap vertices of {} {}",
      failedBrushCount,
      kdl::str_plural(failedBrushCount, "brush", "brushes"));
  }

  return true;
}

bool csgConvexMerge(Map& map)
{
  if (!map.selection().hasBrushFaces() && !map.selection().hasOnlyBrushes())
  {
    return false;
  }

  auto points = std::vector<vm::vec3d>{};

  if (map.selection().hasBrushFaces())
  {
    for (const auto& handle : map.selection().brushFaces)
    {
      for (const auto* vertex : handle.face().vertices())
      {
        points.push_back(vertex->position());
      }
    }
  }
  else if (map.selection().hasOnlyBrushes())
  {
    for (const auto* brushNode : map.selection().brushes)
    {
      for (const auto* vertex : brushNode->brush().vertices())
      {
        points.push_back(vertex->position());
      }
    }
  }

  auto polyhedron = Polyhedron3{std::move(points)};
  if (!polyhedron.polyhedron() || !polyhedron.closed())
  {
    return false;
  }

  const auto builder = BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};
  return builder.createBrush(polyhedron, map.currentMaterialName())
         | kdl::transform([&](auto b) {
             b.cloneFaceAttributesFrom(
               map.selection().brushes | std::views::transform([](const auto* brushNode) {
                 return &brushNode->brush();
               })
               | kdl::ranges::to<std::vector>());

             // The nodelist is either empty or contains only brushes.
             const auto toRemove = map.selection().nodes;

             // We could be merging brushes that have different parents; use the parent
             // of the first brush.
             auto* parentNode = static_cast<Node*>(nullptr);
             if (!map.selection().brushes.empty())
             {
               parentNode = map.selection().brushes.front()->parent();
             }
             else if (!map.selection().brushFaces.empty())
             {
               parentNode = map.selection().brushFaces.front().node()->parent();
             }
             else
             {
               parentNode = parentForNodes(map);
             }

             auto* brushNode = new BrushNode{std::move(b)};

             auto transaction = Transaction{map, "CSG Convex Merge"};
             deselectAll(map);
             if (addNodes(map, {{parentNode, {brushNode}}}).empty())
             {
               transaction.cancel();
               return;
             }
             removeNodes(map, toRemove);
             selectNodes(map, {brushNode});
             transaction.commit();
           })
         | kdl::if_error(
           [&](auto e) { map.logger().error() << "Could not create brush: " << e.msg; })
         | kdl::is_success();
}

bool convertTriangulatedBrushes(Map& map)
{
  const auto conversionClusters = collectBrushConversionClusters(map);
  if (conversionClusters.empty())
  {
    return false;
  }

  auto toAdd = std::map<Node*, std::vector<Node*>>{};
  auto toRemove = std::vector<Node*>{};

  for (const auto& cluster : conversionClusters)
  {
    auto brushResult =
      createBestEffortTriangulatedBrush(cluster.brushes, map, map.currentMaterialName());
    if (!brushResult)
    {
      static_cast<void>(brushResult | kdl::if_error([&](auto e) {
                          map.logger().error()
                            << "Could not convert triangulated brush: " << e.msg;
                        }));
      continue;
    }

    auto brush = std::move(brushResult).value();
    brush.cloneFaceAttributesFrom(toSourceBrushes(cluster.brushes));

    toAdd[cluster.parent].push_back(new BrushNode{std::move(brush)});
    toRemove.insert(
      std::end(toRemove), std::begin(cluster.brushes), std::end(cluster.brushes));
  }

  toRemove = kdl::vec_sort_and_remove_duplicates(std::move(toRemove));
  if (toRemove.empty())
  {
    return false;
  }

  auto transaction = Transaction{map, "Convert Triangulated Brushes"};
  deselectAll(map);
  const auto added = addNodes(map, toAdd);
  if (added.empty())
  {
    transaction.cancel();
    return false;
  }

  removeNodes(map, toRemove);
  selectNodes(map, added);
  transaction.commit();
  return true;
}

bool csgSubtract(Map& map)
{
  const auto subtrahendNodes = std::vector<BrushNode*>{map.selection().brushes};
  if (subtrahendNodes.empty())
  {
    return false;
  }

  auto transaction = Transaction{map, "CSG Subtract"};
  // Select touching, but don't delete the subtrahends yet
  selectTouchingNodes(map, false);

  const auto minuendNodes = std::vector<BrushNode*>{map.selection().brushes};
  const auto subtrahends = subtrahendNodes
                           | std::views::transform([](const auto* subtrahendNode) {
                               return &subtrahendNode->brush();
                             })
                           | kdl::ranges::to<std::vector>();

  auto toAdd = std::map<Node*, std::vector<Node*>>{};
  auto toRemove =
    std::vector<Node*>{std::begin(subtrahendNodes), std::end(subtrahendNodes)};

  return minuendNodes | std::views::transform([&](auto* minuendNode) {
           const auto& minuend = minuendNode->brush();
           auto currentSubtractionResults = minuend.subtract(
             map.worldNode().mapFormat(),
             map.worldBounds(),
             map.currentMaterialName(),
             subtrahends);

           return currentSubtractionResults
                  | std::views::filter([](const auto r) { return r | kdl::is_success(); })
                  | kdl::views::as_rvalue | kdl::fold
                  | kdl::transform([&](auto currentBrushes) {
                      if (!currentBrushes.empty())
                      {
                        auto resultNodes = currentBrushes | kdl::views::as_rvalue
                                           | std::views::transform([&](auto b) {
                                               return new BrushNode{std::move(b)};
                                             })
                                           | kdl::ranges::to<std::vector>();
                        auto& toAddForParent = toAdd[minuendNode->parent()];
                        kdl::vec_append(toAddForParent, std::move(resultNodes));
                      }

                      toRemove.push_back(minuendNode);
                    });
         })
         | kdl::fold | kdl::transform([&]() {
             deselectAll(map);
             const auto added = addNodes(map, toAdd);
             removeNodes(map, toRemove);
             selectNodes(map, added);

             return transaction.commit();
           })
         | kdl::transform_error([&](const auto& e) {
             map.logger().error() << "Could not subtract brushes: " << e;
             transaction.cancel();
             return false;
           })
         | kdl::value();
}

namespace
{
constexpr auto Subtract2Epsilon = 0.001;
constexpr auto MaxSubtract2FragmentsPerBrush = size_t{512};

bool nearEqual(const double lhs, const double rhs)
{
  return std::abs(lhs - rhs) <= Subtract2Epsilon;
}

void addUniqueCoordinate(
  std::vector<double>& coordinates,
  const double value,
  const double minValue,
  const double maxValue)
{
  if (value <= minValue + Subtract2Epsilon || value >= maxValue - Subtract2Epsilon)
  {
    return;
  }
  if (std::ranges::none_of(coordinates, [&](const auto candidate) {
        return nearEqual(candidate, value);
      }))
  {
    coordinates.push_back(value);
  }
}

bool isAxisAlignedCuboid(const Brush& brush)
{
  if (brush.faceCount() != 6u || brush.vertexCount() != 8u)
  {
    return false;
  }

  const auto& bounds = brush.bounds();
  for (size_t axis = 0; axis < 3u; ++axis)
  {
    if (bounds.max[axis] <= bounds.min[axis] + Subtract2Epsilon)
    {
      return false;
    }
  }

  for (const auto& point : brush.vertexPositions())
  {
    for (size_t axis = 0; axis < 3u; ++axis)
    {
      if (
        !nearEqual(point[axis], bounds.min[axis])
        && !nearEqual(point[axis], bounds.max[axis]))
      {
        return false;
      }
    }
  }

  return true;
}

bool pointInsideAnyBounds(const vm::vec3d& point, const std::vector<vm::bbox3d>& bounds)
{
  return std::ranges::any_of(bounds, [&](const auto& box) {
    return point.x() > box.min.x() + Subtract2Epsilon
           && point.x() < box.max.x() - Subtract2Epsilon
           && point.y() > box.min.y() + Subtract2Epsilon
           && point.y() < box.max.y() - Subtract2Epsilon
           && point.z() > box.min.z() + Subtract2Epsilon
           && point.z() < box.max.z() - Subtract2Epsilon;
  });
}

std::optional<std::vector<Brush>> csgSubtract2Fragments(
  Map& map, const Brush& minuend, const std::vector<const Brush*>& subtrahends)
{
  if (std::ranges::any_of(subtrahends, [](const auto* brush) {
        return !isAxisAlignedCuboid(*brush);
      }))
  {
    return std::nullopt;
  }

  const auto& minuendBounds = minuend.bounds();
  auto subtrahendBounds = std::vector<vm::bbox3d>{};
  subtrahendBounds.reserve(subtrahends.size());
  for (const auto* subtrahend : subtrahends)
  {
    if (minuendBounds.intersects(subtrahend->bounds()) && minuend.intersects(*subtrahend))
    {
      subtrahendBounds.push_back(subtrahend->bounds());
    }
  }

  if (subtrahendBounds.empty())
  {
    return std::vector<Brush>{minuend};
  }

  auto coordinates = std::array<std::vector<double>, 3u>{
    std::vector<double>{minuendBounds.min.x(), minuendBounds.max.x()},
    std::vector<double>{minuendBounds.min.y(), minuendBounds.max.y()},
    std::vector<double>{minuendBounds.min.z(), minuendBounds.max.z()}};

  for (const auto& bounds : subtrahendBounds)
  {
    for (size_t axis = 0; axis < 3u; ++axis)
    {
      addUniqueCoordinate(
        coordinates[axis],
        std::clamp(bounds.min[axis], minuendBounds.min[axis], minuendBounds.max[axis]),
        minuendBounds.min[axis],
        minuendBounds.max[axis]);
      addUniqueCoordinate(
        coordinates[axis],
        std::clamp(bounds.max[axis], minuendBounds.min[axis], minuendBounds.max[axis]),
        minuendBounds.min[axis],
        minuendBounds.max[axis]);
    }
  }

  for (auto& axisCoordinates : coordinates)
  {
    std::ranges::sort(axisCoordinates);
  }

  const auto cellCount = (coordinates[0].size() - 1u) * (coordinates[1].size() - 1u)
                         * (coordinates[2].size() - 1u);
  if (cellCount > MaxSubtract2FragmentsPerBrush)
  {
    return std::nullopt;
  }

  const auto builder = BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  auto result = std::vector<Brush>{};
  result.reserve(cellCount);
  for (size_t x = 0; x + 1u < coordinates[0].size(); ++x)
  {
    for (size_t y = 0; y + 1u < coordinates[1].size(); ++y)
    {
      for (size_t z = 0; z + 1u < coordinates[2].size(); ++z)
      {
        const auto min =
          vm::vec3d{coordinates[0][x], coordinates[1][y], coordinates[2][z]};
        const auto max = vm::vec3d{
          coordinates[0][x + 1u], coordinates[1][y + 1u], coordinates[2][z + 1u]};
        if (
          max.x() <= min.x() + Subtract2Epsilon || max.y() <= min.y() + Subtract2Epsilon
          || max.z() <= min.z() + Subtract2Epsilon)
        {
          continue;
        }

        const auto center = (min + max) / 2.0;
        if (pointInsideAnyBounds(center, subtrahendBounds))
        {
          continue;
        }

        auto cellBrush =
          builder.createCuboid(vm::bbox3d{min, max}, map.currentMaterialName());
        if (!cellBrush)
        {
          return std::nullopt;
        }

        auto fragment = std::move(cellBrush).value();
        if (!fragment.intersects(minuend))
        {
          continue;
        }

        if (!fragment.intersect(map.worldBounds(), minuend))
        {
          return std::nullopt;
        }

        fragment.cloneFaceAttributesFrom(minuend);
        result.push_back(std::move(fragment));
      }
    }
  }

  return result;
}

std::vector<Brush> csgSubtractFallbackFragments(
  Map& map, const Brush& minuend, const std::vector<const Brush*>& subtrahends)
{
  auto result = std::vector<Brush>{};
  auto subtractionResults = minuend.subtract(
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.currentMaterialName(),
    subtrahends);

  for (auto& subtractionResult : subtractionResults)
  {
    if (subtractionResult)
    {
      result.push_back(std::move(subtractionResult).value());
    }
    else
    {
      subtractionResult | kdl::if_error([&](const auto& e) {
        map.logger().error() << "Could not subtract brushes: " << e.msg;
      }) | kdl::ignore();
    }
  }

  return result;
}
} // namespace

bool csgSubtract2(Map& map)
{
  const auto subtrahendNodes = std::vector<BrushNode*>{map.selection().brushes};
  if (subtrahendNodes.empty())
  {
    return false;
  }

  auto transaction = Transaction{map, "CSG Subtract 2"};
  selectTouchingNodes(map, false);

  const auto minuendNodes = std::vector<BrushNode*>{map.selection().brushes};
  const auto subtrahends = subtrahendNodes
                           | std::views::transform([](const auto* subtrahendNode) {
                               return &subtrahendNode->brush();
                             })
                           | kdl::ranges::to<std::vector>();

  auto toAdd = std::map<Node*, std::vector<Node*>>{};
  auto toRemove =
    std::vector<Node*>{std::begin(subtrahendNodes), std::end(subtrahendNodes)};

  for (auto* minuendNode : minuendNodes)
  {
    const auto& minuend = minuendNode->brush();
    auto betterFragments = csgSubtract2Fragments(map, minuend, subtrahends);
    auto currentBrushes = betterFragments
                            ? std::move(*betterFragments)
                            : csgSubtractFallbackFragments(map, minuend, subtrahends);

    if (!currentBrushes.empty())
    {
      auto resultNodes =
        currentBrushes | kdl::views::as_rvalue
        | std::views::transform([&](auto b) { return new BrushNode{std::move(b)}; })
        | kdl::ranges::to<std::vector>();
      auto& toAddForParent = toAdd[minuendNode->parent()];
      kdl::vec_append(toAddForParent, std::move(resultNodes));
    }

    toRemove.push_back(minuendNode);
  }

  deselectAll(map);
  const auto added = addNodes(map, toAdd);
  removeNodes(map, toRemove);
  selectNodes(map, added);

  if (!transaction.commit())
  {
    transaction.cancel();
    return false;
  }

  return true;
}

bool csgIntersect(Map& map)
{
  const auto brushes = map.selection().brushes;
  if (brushes.size() < 2u)
  {
    return false;
  }

  auto intersection = brushes.front()->brush();

  bool valid = true;
  for (auto it = std::next(std::begin(brushes)), end = std::end(brushes);
       it != end && valid;
       ++it)
  {
    auto* brushNode = *it;
    const auto& brush = brushNode->brush();
    valid = intersection.intersect(map.worldBounds(), brush) | kdl::if_error([&](auto e) {
              map.logger().error() << "Could not intersect brushes: " << e.msg;
            })
            | kdl::is_success();
  }

  const auto toRemove = std::vector<Node*>{std::begin(brushes), std::end(brushes)};

  auto transaction = Transaction{map, "CSG Intersect"};
  deselectNodes(map, toRemove);

  if (valid)
  {
    auto* intersectionNode = new BrushNode{std::move(intersection)};
    if (addNodes(map, {{parentForNodes(map, toRemove), {intersectionNode}}}).empty())
    {
      transaction.cancel();
      return false;
    }
    removeNodes(map, toRemove);
    selectNodes(map, {intersectionNode});
  }
  else
  {
    removeNodes(map, toRemove);
  }

  return transaction.commit();
}

namespace
{
bool replaceWithPieces(
  Map& map,
  BrushNode* original,
  std::vector<Brush> ordinary,
  std::vector<Brush> walls,
  const std::string& name)
{
  for (const auto* pieces : {&ordinary, &walls})
    for (const auto& brush : *pieces)
      if (
        !brush.closed() || !brush.fullySpecified()
        || !map.worldBounds().contains(brush.bounds()))
        return false;
  auto toAdd = std::map<Node*, std::vector<Node*>>{};
  auto selected = std::vector<Node*>{};
  auto* parent = original->parent();
  auto* entityParent = dynamic_cast<EntityNode*>(parent) ? parent->parent() : parent;
  for (auto& brush : ordinary)
  {
    auto* node = new BrushNode{std::move(brush)};
    toAdd[parent].push_back(node);
    if (walls.empty())
      selected.push_back(node);
  }
  for (auto& brush : walls)
  {
    auto* entity = new EntityNode{Entity{{{"classname", "func_wall"}}}};
    auto* node = new BrushNode{std::move(brush)};
    entity->addChild(node);
    toAdd[entityParent].push_back(entity);
    selected.push_back(node);
  }
  if (toAdd.empty())
    return false;
  auto transaction = Transaction{map, name};
  deselectAll(map);
  if (addNodes(map, toAdd).empty())
  {
    transaction.cancel();
    return false;
  }
  removeNodes(map, {original});
  selectNodes(map, selected);
  return transaction.commit();
}
} // namespace

bool replaceWithFracture(Map& map, std::vector<Brush> fragments, const bool funcWalls)
{
  const auto& selection = map.selection();
  if (
    !selection.hasOnlyBrushes() || selection.brushes.size() != 1 || fragments.size() < 2)
    return false;
  return funcWalls
           ? replaceWithPieces(
               map, selection.brushes.front(), {}, std::move(fragments), "Fracture Brush")
           : replaceWithPieces(
               map,
               selection.brushes.front(),
               std::move(fragments),
               {},
               "Fracture Brush");
}

bool replaceWithPerforation(Map& map, Perforation perforation, const bool keepPlugs)
{
  if (
    map.selection().brushFaces.size() != 1 || perforation.remainder.empty()
    || perforation.plugs.empty())
    return false;
  return replaceWithPieces(
    map,
    map.selection().brushFaces.front().node(),
    std::move(perforation.remainder),
    keepPlugs ? std::move(perforation.plugs) : std::vector<Brush>{},
    "Perforate Brush");
}

bool applySettledBrushes(Map& map, std::vector<Brush> brushes)
{
  const auto& selection = map.selection();
  if (
    !selection.hasOnlyBrushes() || brushes.empty()
    || selection.brushes.size() != brushes.size())
  {
    return false;
  }
  auto changes = std::vector<std::pair<Node*, NodeContents>>{};
  for (size_t i = 0; i < brushes.size(); ++i)
  {
    if (
      !brushes[i].closed() || !brushes[i].fullySpecified()
      || !map.worldBounds().contains(brushes[i].bounds()))
    {
      return false;
    }
    changes.emplace_back(selection.brushes[i], NodeContents{std::move(brushes[i])});
  }
  return updateNodeContents(map, "Settle Brushes", std::move(changes));
}

bool csgHollow(Map& map)
{
  const auto brushNodes = map.selection().brushes;
  if (brushNodes.empty())
  {
    return false;
  }

  bool didHollowAnything = false;
  auto toAdd = std::map<Node*, std::vector<Node*>>{};
  auto toRemove = std::vector<Node*>{};

  for (auto* brushNode : brushNodes)
  {
    const auto& originalBrush = brushNode->brush();

    auto shrunkenBrush = originalBrush;
    shrunkenBrush.expand(map.worldBounds(), -double(map.grid().actualSize()), true)
      | kdl::and_then([&]() {
          didHollowAnything = true;

          return originalBrush.subtract(
                   map.worldNode().mapFormat(),
                   map.worldBounds(),
                   map.currentMaterialName(),
                   shrunkenBrush)
                 | kdl::fold | kdl::transform([&](auto fragments) {
                     auto fragmentNodes =
                       fragments | kdl::views::as_rvalue
                       | std::views::transform([](auto&& b) {
                           return new BrushNode{std::forward<decltype(b)>(b)};
                         })
                       | kdl::ranges::to<std::vector>();

                     auto& toAddForParent = toAdd[brushNode->parent()];
                     kdl::vec_append(toAddForParent, fragmentNodes);
                     toRemove.push_back(brushNode);
                   });
        })
      | kdl::transform_error(
        [&](const auto& e) { map.logger().error() << "Could not hollow brush: " << e; });
  }

  if (!didHollowAnything)
  {
    return false;
  }

  auto transaction = Transaction{map, "CSG Hollow"};
  deselectAll(map);
  const auto added = addNodes(map, toAdd);
  if (added.empty())
  {
    transaction.cancel();
    return false;
  }
  removeNodes(map, toRemove);
  selectNodes(map, added);

  return transaction.commit();
}

bool extrudeBrushes(
  Map& map, const std::vector<vm::polygon3d>& faces, const vm::vec3d& delta)
{
  const auto nodes = map.selection().nodes;
  return applyAndSwap(
    map,
    "Resize Brushes",
    nodes,
    collectContainingGroups(nodes),
    kdl::overload(
      [](Layer&) { return true; },
      [](Group&) { return true; },
      [](Entity&) { return true; },
      [&](Brush& brush) {
        const auto faceIndex = brush.findFace(faces);
        if (!faceIndex)
        {
          // we allow resizing only some of the brushes
          return true;
        }

        return brush.moveBoundary(
                 map.worldBounds(),
                 *faceIndex,
                 delta,
                 map.editorContext().alignmentLock())
               | kdl::transform(
                 [&]() { return map.worldBounds().contains(brush.bounds()); })
               | kdl::transform_error([&](auto e) {
                   map.logger().error() << "Could not resize brush: " << e.msg;
                   return false;
                 })
               | kdl::value();
      },
      [](BezierPatch&) { return true; }));
}

} // namespace tb::mdl
