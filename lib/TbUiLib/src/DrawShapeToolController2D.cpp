/*
 Copyright (C) 2010 Kristian Duske

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

#include "ui/DrawShapeToolController2D.h"

#include "gl/Camera.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "ui/DrawShapeTool.h"
#include "ui/HandleDragTracker.h"
#include "ui/InputState.h"
#include "ui/MapDocument.h"

#include "vm/intersection.h"
#include "vm/mat_ext.h"

namespace tb::ui
{
namespace
{

class DrawShapeDragDelegate : public HandleDragTrackerDelegate
{
private:
  DrawShapeTool& m_tool;
  vm::bbox3d m_worldBounds;
  vm::bbox3d m_referenceBounds;

public:
  DrawShapeDragDelegate(
    DrawShapeTool& tool, const vm::bbox3d& worldBounds, const vm::bbox3d& referenceBounds)
    : m_tool{tool}
    , m_worldBounds{worldBounds}
    , m_referenceBounds{referenceBounds}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) override
  {
    const auto currentBounds =
      makeBounds(inputState, initialHandlePosition, initialHandlePosition);

    m_tool.updateGridAligned(currentBounds);
    m_tool.refreshViews();

    const auto& camera = inputState.camera();
    const auto plane = vm::plane3d{
      initialHandlePosition,
      vm::vec3d{vm::get_abs_max_component_axis(camera.direction())}};

    return makeHandlePositionProposer(
      makePlaneHandlePicker(plane, handleOffset), makeIdentityHandleSnapper());
  }

  DragStatus update(
    const InputState& inputState,
    const DragState& dragState,
    const vm::vec3d& proposedHandlePosition) override
  {
    if (updateBounds(
          inputState,
          dragState.initialHandlePosition,
          dragState.currentHandlePosition,
          proposedHandlePosition))
    {
      m_tool.refreshViews();
      return DragStatus::Continue;
    }
    return DragStatus::Deny;
  }

  void end(const InputState&, const DragState&) override { m_tool.createBrushes(); }

  void cancel(const DragState&) override { m_tool.cancel(); }

  std::optional<UpdateDragConfig> modifierKeyChange(
    const InputState& inputState, const DragState& dragState) override
  {
    const auto currentBounds = makeBounds(
      inputState, dragState.initialHandlePosition, dragState.currentHandlePosition);

    if (!currentBounds.is_empty())
    {
      m_tool.updateGridAligned(currentBounds);
      m_tool.refreshViews();
    }

    return std::nullopt;
  }

  void render(
    const InputState&,
    const DragState&,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) const override
  {
    m_tool.render(renderContext, renderBatch);
  }

private:
  bool updateBounds(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& lastHandlePosition,
    const vm::vec3d& currentHandlePosition)
  {
    const auto lastBounds =
      makeBounds(inputState, initialHandlePosition, lastHandlePosition);
    const auto currentBounds =
      makeBounds(inputState, initialHandlePosition, currentHandlePosition);

    if (currentBounds.is_empty() || currentBounds == lastBounds)
    {
      return false;
    }

    m_tool.updateGridAligned(currentBounds);
    return true;
  }

  vm::bbox3d makeBounds(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& currentHandlePosition) const
  {
    const auto& grid = m_tool.grid();
    const auto initialGridPosition = grid.toGridSpace(initialHandlePosition);
    const auto currentGridPosition = grid.toGridSpace(currentHandlePosition);

    auto bounds = snapBounds(
      inputState,
      vm::merge(
        vm::bbox3d{initialGridPosition, initialGridPosition}, currentGridPosition));

    if (inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      const auto viewAxis = vm::abs(
        vm::vec3d{vm::get_abs_max_component_axis(
          grid.toGridSpace(vm::vec3d{inputState.camera().direction()}))});
      const auto orthoAxes = vm::vec3d{1, 1, 1} - viewAxis;

      // The max length of the bounds along any of the ortho axes:
      const auto maxLength = vm::get_abs_max_component(bounds.size() * orthoAxes);

      if (inputState.modifierKeysDown(ModifierKeys::Alt))
      {
        const auto lengthDiff = vm::vec3d{maxLength, maxLength, maxLength};

        // The direction in which the user is dragging per component:
        const auto dragDir = vm::step(initialGridPosition, currentGridPosition);
        bounds = vm::bbox3d{
          vm::mix(bounds.min, bounds.max - lengthDiff, vm::vec3d{1, 1, 1} - dragDir),
          vm::mix(bounds.max, bounds.min + lengthDiff, dragDir)};
      }
      else
      {
        // A vector where the ortho axes have maxLength and the view axis has the size of
        // the bounds in that direction
        const auto lengthDiff = viewAxis * bounds.size() + orthoAxes * maxLength;

        // The direction in which the user is dragging per component:
        const auto dragDir = vm::step(initialGridPosition, currentGridPosition);
        bounds = vm::bbox3d{
          vm::mix(bounds.min, bounds.max - lengthDiff, vm::vec3d{1, 1, 1} - dragDir),
          vm::mix(bounds.max, bounds.min + lengthDiff, dragDir)};
      }
    }

    return vm::intersect(bounds, m_worldBounds);
  }

  vm::bbox3d snapBounds(const InputState& inputState, const vm::bbox3d& bounds) const
  {
    const auto& grid = m_tool.grid();
    auto min = bounds.min;
    auto max = bounds.max;
    for (size_t i = 0; i < 3; ++i)
    {
      min[i] = grid.snapDown(min[i], false);
      max[i] = grid.snapUp(max[i], false);
    }

    const auto& camera = inputState.camera();
    const auto worldToGrid =
      vm::rotation_matrix(vm::vec3d{0.0, 0.0, 1.0}, grid.rotation());
    const auto refBounds = m_referenceBounds.transform(worldToGrid);
    const auto factors = vm::vec3d{vm::abs(
      vm::get_abs_max_component_axis(grid.toGridSpace(vm::vec3d{camera.direction()})))};
    return vm::bbox3d{
      vm::mix(min, refBounds.min, factors), vm::mix(max, refBounds.max, factors)};
  }
};

} // namespace

DrawShapeToolController2D::DrawShapeToolController2D(
  DrawShapeTool& tool, MapDocument& document)
  : m_tool{tool}
  , m_document{document}
{
}

Tool& DrawShapeToolController2D::tool()
{
  return m_tool;
}

const Tool& DrawShapeToolController2D::tool() const
{
  return m_tool;
}

std::unique_ptr<GestureTracker> DrawShapeToolController2D::acceptMouseDrag(
  const InputState& inputState)
{
  if (!inputState.mouseButtonsPressed(MouseButtons::Left))
  {
    return nullptr;
  }

  if (!inputState.checkModifierKeys(
        ModifierKeyPressed::No,
        ModifierKeyPressed::DontCare,
        ModifierKeyPressed::DontCare))
  {
    return nullptr;
  }

  auto& map = m_document.map();
  if (map.selection().hasAny())
  {
    return nullptr;
  }

  const auto& bounds = map.referenceBounds();
  const auto& camera = inputState.camera();
  const auto plane = vm::plane3d{
    bounds.min, vm::vec3d{vm::get_abs_max_component_axis(camera.direction())}};

  if (const auto distance = vm::intersect_ray_plane(inputState.pickRay(), plane))
  {
    const auto initialHandlePosition =
      vm::point_at_distance(inputState.pickRay(), *distance);
    return createHandleDragTracker(
      DrawShapeDragDelegate{m_tool, map.worldBounds(), map.referenceBounds()},
      inputState,
      initialHandlePosition,
      initialHandlePosition);
  }

  return nullptr;
}

bool DrawShapeToolController2D::cancel()
{
  return m_tool.cancel();
}

} // namespace tb::ui
