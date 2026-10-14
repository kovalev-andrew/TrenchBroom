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

#include "NotifierConnection.h"
#include "mdl/HitType.h"
#include "ui/Tool.h"

#include "vm/plane.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <array>
#include <optional>
#include <tuple>
#include <vector>

namespace tb
{
namespace gl
{
class Camera;
}

namespace mdl
{
class Brush;
class Grid;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class MapDocument;

class PolygonCutTool : public Tool
{
public:
  static const mdl::HitType::Type PointHitType;

  enum class CutMode
  {
    RemoveCutout,
    KeepCutout,
  };

  enum class ToolMode
  {
    Cut,
    SpawnBrush,
  };

private:
  struct DragState
  {
    size_t index;
    vm::vec3d originalPoint;
  };

  using Triangle = std::array<size_t, 3>;

private:
  MapDocument& m_document;
  ToolMode m_toolMode = ToolMode::Cut;
  CutMode m_cutMode = CutMode::RemoveCutout;
  int m_cutDepthSteps = 0;
  std::vector<vm::vec3d> m_points;
  std::optional<vm::plane3d> m_cutPlane;
  std::optional<DragState> m_dragState;
  bool m_ignoreNotifications = false;
  bool m_dragging = false;

  NotifierConnection m_notifierConnection;

public:
  explicit PolygonCutTool(MapDocument& document);
  ~PolygonCutTool() override;

  const mdl::Grid& grid() const;

  void pick(
    const vm::ray3d& pickRay, const gl::Camera& camera, mdl::PickResult& pickResult);
  void render(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult);
  void renderFeedback(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const vm::vec3d& point) const;

  bool hasBrushes() const;
  bool canCut() const;
  void performCut();
  bool canSpawnBrush() const;
  void performSpawnBrush();

  ToolMode toolMode() const;
  void setToolMode(ToolMode toolMode);

  CutMode cutMode() const;
  void setCutMode(CutMode cutMode);
  int cutDepthSteps() const;
  void setCutDepthSteps(int cutDepthSteps);
  double cutDepth() const;

  std::optional<vm::vec3d> defaultCutPointPos() const;
  std::optional<vm::plane3d> cutPlane() const;

  bool canAddPoint(const vm::vec3d& point, const vm::plane3d& plane) const;
  bool hasPoints() const;
  void addPoint(const vm::vec3d& point, const vm::plane3d& plane);
  bool canRemoveLastPoint() const;
  bool removeLastPoint();

  std::optional<std::tuple<vm::vec3d, vm::vec3d>> beginDragPoint(
    const mdl::PickResult& pickResult);
  void beginDragLastPoint();
  bool dragPoint(const vm::vec3d& newPosition);
  void endDragPoint();
  void cancelDragPoint();

  bool reset();

private:
  void renderPoints(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult) const;
  void renderHighlight(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult) const;
  void renderHighlight(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    size_t index) const;

  bool pointOnCutPlane(const vm::vec3d& point) const;
  bool polygonClosed() const;
  std::vector<vm::vec3d> polygonPoints() const;
  double signedPolygonArea() const;
  std::vector<size_t> orderedPolygonIndices() const;
  std::vector<vm::vec3d> orderedPolygon() const;
  bool isConvexPolygon(const std::vector<size_t>& indices) const;
  std::vector<Triangle> triangulatePolygon() const;
  std::vector<std::vector<vm::vec3d>> axisAlignedCutterPolygons(
    size_t axis, size_t maxPieces) const;
  std::vector<std::vector<vm::vec3d>> orthogonalCutterPolygons() const;
  std::vector<std::vector<vm::vec3d>> cutterPolygons() const;
  std::vector<std::vector<vm::vec3d>> spawnPolygons() const;
  std::optional<mdl::Brush> createSpawnBrush(const std::vector<vm::vec3d>& polygon) const;
  std::optional<mdl::Brush> createCutterBrush(
    const mdl::Brush& targetBrush, const std::vector<vm::vec3d>& polygon) const;
  std::optional<std::vector<mdl::Brush>> createOrthogonalFragments(
    const mdl::Brush& targetBrush, const std::vector<vm::vec3d>& cutterPolygon) const;
  std::vector<vm::plane3d> axisSplitPlanes() const;
  std::vector<mdl::Brush> splitFragmentsAlongAxes(
    std::vector<mdl::Brush> fragments) const;
  std::optional<std::vector<mdl::Brush>> splitBrushByPlane(
    const mdl::Brush& brush, const vm::plane3d& plane) const;

  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;

  void connectObservers();
  void refresh();
};

} // namespace ui
} // namespace tb
