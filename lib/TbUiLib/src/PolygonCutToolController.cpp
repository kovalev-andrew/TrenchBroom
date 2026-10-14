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

#include "ui/PolygonCutToolController.h"

#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Grid.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/PickResult.h"
#include "render/RenderContext.h"
#include "ui/HandleDragTracker.h"
#include "ui/PolygonCutTool.h"

#include "kd/contracts.h"
#include "kd/optional_utils.h"

#include "vm/intersection.h"
#include "vm/vec.h"

#include <memory>
#include <optional>

namespace tb::ui
{
namespace
{

using CutPoint = std::tuple<vm::vec3d, vm::plane3d, vm::vec3d>;

DragHandleSnapper makePlaneHandleSnapper(const mdl::Grid& grid, const vm::plane3d& plane)
{
  return [&grid, plane](
           const InputState&, const DragState&, const vm::vec3d& proposedHandlePosition) {
    return grid.snap(proposedHandlePosition, plane);
  };
}

bool selectableCutFace(const mdl::BrushFaceHandle& faceHandle)
{
  return faceHandle.node()->selected() || faceHandle.node()->transitivelySelected()
         || faceHandle.face().selected();
}

class PartDelegateBase
{
protected:
  PolygonCutTool& m_tool;

public:
  explicit PartDelegateBase(PolygonCutTool& tool)
    : m_tool{tool}
  {
  }

  virtual ~PartDelegateBase() = default;

  PolygonCutTool& tool() const { return m_tool; }

  std::optional<std::tuple<vm::vec3d, vm::vec3d>> addCutPoint(
    const InputState& inputState)
  {
    const auto cutPoint = doGetNewCutPoint(inputState);
    if (!cutPoint)
    {
      return std::nullopt;
    }

    const auto& [position, plane, hitPoint] = *cutPoint;
    if (!m_tool.canAddPoint(position, plane))
    {
      return std::nullopt;
    }

    m_tool.addPoint(position, plane);
    return std::tuple{position, hitPoint};
  }

  virtual HandlePositionProposer makeHandlePositionProposer(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) const = 0;

  void renderFeedback(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch)
  {
    if (inputState.anyToolDragging())
    {
      return;
    }

    const auto cutPoint = doGetNewCutPoint(inputState);
    if (!cutPoint)
    {
      return;
    }

    const auto& [position, plane, hitPoint] = *cutPoint;
    (void)hitPoint;
    if (m_tool.canAddPoint(position, plane))
    {
      m_tool.renderFeedback(renderContext, renderBatch, position);
    }
  }

private:
  virtual std::optional<CutPoint> doGetNewCutPoint(
    const InputState& inputState) const = 0;
};

class PartDelegate2D : public PartDelegateBase
{
public:
  explicit PartDelegate2D(PolygonCutTool& tool)
    : PartDelegateBase{tool}
  {
  }

  HandlePositionProposer makeHandlePositionProposer(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) const override
  {
    const auto plane = m_tool.cutPlane().value_or(
      vm::plane3d{initialHandlePosition, vm::vec3d{inputState.camera().direction()}});
    return ui::makeHandlePositionProposer(
      makePlaneHandlePicker(plane, handleOffset),
      makePlaneHandleSnapper(m_tool.grid(), plane));
  }

private:
  std::optional<CutPoint> doGetNewCutPoint(const InputState& inputState) const override
  {
    const auto& camera = inputState.camera();
    const auto viewDir = vm::get_abs_max_component_axis(vm::vec3d(camera.direction()));

    const auto plane = m_tool.cutPlane().value_or(
      m_tool.defaultCutPointPos() | kdl::optional_transform([&](const auto defaultPos) {
        return vm::plane3d{defaultPos, viewDir};
      })
      | kdl::optional_value_or(vm::plane3d{}));

    if (vm::is_zero(vm::squared_length(plane.normal), vm::Cd::almost_zero()))
    {
      return std::nullopt;
    }

    return vm::intersect_ray_plane(inputState.pickRay(), plane)
           | kdl::optional_transform([&](const auto distance) {
               const auto hitPoint =
                 vm::point_at_distance(inputState.pickRay(), distance);
               const auto position = m_tool.grid().snap(hitPoint, plane);
               return CutPoint{position, plane, hitPoint};
             });
  }
};

class PartDelegate3D : public PartDelegateBase
{
public:
  explicit PartDelegate3D(PolygonCutTool& tool)
    : PartDelegateBase{tool}
  {
  }

  HandlePositionProposer makeHandlePositionProposer(
    const InputState&,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) const override
  {
    const auto plane =
      m_tool.cutPlane().value_or(vm::horizontal_plane(initialHandlePosition));
    return ui::makeHandlePositionProposer(
      makePlaneHandlePicker(plane, handleOffset),
      makePlaneHandleSnapper(m_tool.grid(), plane));
  }

private:
  std::optional<CutPoint> doGetNewCutPoint(const InputState& inputState) const override
  {
    if (const auto plane = m_tool.cutPlane())
    {
      using namespace mdl::HitFilters;

      auto hitSelectedBrush = false;
      for (const auto& hit :
           inputState.pickResult().all(type(mdl::BrushNode::BrushHitType)))
      {
        const auto faceHandle = mdl::hitToFaceHandle(hit);
        if (!faceHandle)
        {
          continue;
        }

        if (!selectableCutFace(*faceHandle))
        {
          return std::nullopt;
        }

        hitSelectedBrush = true;
        break;
      }

      if (!hitSelectedBrush)
      {
        return std::nullopt;
      }

      return vm::intersect_ray_plane(inputState.pickRay(), *plane)
             | kdl::optional_transform([&](const auto distance) {
                 const auto hitPoint =
                   vm::point_at_distance(inputState.pickRay(), distance);
                 const auto position = m_tool.grid().snap(hitPoint, *plane);
                 return CutPoint{position, *plane, hitPoint};
               });
    }

    using namespace mdl::HitFilters;

    for (const auto& hit :
         inputState.pickResult().all(type(mdl::BrushNode::BrushHitType)))
    {
      const auto faceHandle = mdl::hitToFaceHandle(hit);
      if (!faceHandle)
      {
        continue;
      }

      if (!selectableCutFace(*faceHandle))
      {
        return std::nullopt;
      }

      const auto plane = faceHandle->face().boundary();
      const auto position = m_tool.grid().snap(hit.hitPoint(), plane);
      return CutPoint{position, plane, hit.hitPoint()};
    }
    return std::nullopt;
  }
};

class PartBase
{
protected:
  std::unique_ptr<PartDelegateBase> m_delegate;

  explicit PartBase(std::unique_ptr<PartDelegateBase> delegate)
    : m_delegate{std::move(delegate)}
  {
    contract_pre(m_delegate != nullptr);
  }

public:
  virtual ~PartBase() = default;
};

class AddCutPointDragDelegate : public HandleDragTrackerDelegate
{
private:
  PartDelegateBase& m_delegate;
  bool m_secondPointSet{false};

public:
  explicit AddCutPointDragDelegate(PartDelegateBase& delegate)
    : m_delegate{delegate}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) override
  {
    return m_delegate.makeHandlePositionProposer(
      inputState, initialHandlePosition, handleOffset);
  }

  DragStatus update(
    const InputState&, const DragState&, const vm::vec3d& proposedHandlePosition) override
  {
    if (!m_secondPointSet)
    {
      m_delegate.tool().beginDragLastPoint();
      m_secondPointSet = true;
      return DragStatus::Continue;
    }

    if (m_delegate.tool().dragPoint(proposedHandlePosition))
    {
      return DragStatus::Continue;
    }
    return DragStatus::Deny;
  }

  void end(const InputState&, const DragState&) override
  {
    if (m_secondPointSet)
    {
      m_delegate.tool().endDragPoint();
    }
  }

  void cancel(const DragState&) override
  {
    if (m_secondPointSet)
    {
      m_delegate.tool().cancelDragPoint();
    }
    m_delegate.tool().removeLastPoint();
  }
};

class AddCutPointPart : public ToolController, protected PartBase
{
public:
  explicit AddCutPointPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  bool mouseClick(const InputState& inputState) override
  {
    if (
      !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return false;
    }

    return m_delegate->addCutPoint(inputState) != std::nullopt;
  }

  bool mouseDoubleClick(const InputState& inputState) override
  {
    if (
      !inputState.mouseButtonsPressed(MouseButtons::Left)
      || !inputState.modifierKeysPressed(ModifierKeys::None))
    {
      return false;
    }

    if (m_delegate->tool().canCut())
    {
      m_delegate->tool().performCut();
      return true;
    }
    return m_delegate->addCutPoint(inputState) != std::nullopt;
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    if (
      inputState.mouseButtons() != MouseButtons::Left
      || inputState.modifierKeys() != ModifierKeys::None)
    {
      return nullptr;
    }

    const auto initialHandlePositionAndHitPoint = m_delegate->addCutPoint(inputState);
    if (!initialHandlePositionAndHitPoint)
    {
      return nullptr;
    }

    const auto [initialHandlePosition, hitPoint] = *initialHandlePositionAndHitPoint;
    return createHandleDragTracker(
      AddCutPointDragDelegate{*m_delegate}, inputState, initialHandlePosition, hitPoint);
  }

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override
  {
    m_delegate->renderFeedback(inputState, renderContext, renderBatch);
  }

  bool cancel() override { return false; }
};

class MoveCutPointDragDelegate : public HandleDragTrackerDelegate
{
private:
  PartDelegateBase& m_delegate;

public:
  explicit MoveCutPointDragDelegate(PartDelegateBase& delegate)
    : m_delegate{delegate}
  {
  }

  HandlePositionProposer start(
    const InputState& inputState,
    const vm::vec3d& initialHandlePosition,
    const vm::vec3d& handleOffset) override
  {
    return m_delegate.makeHandlePositionProposer(
      inputState, initialHandlePosition, handleOffset);
  }

  DragStatus update(
    const InputState&, const DragState&, const vm::vec3d& proposedHandlePosition) override
  {
    if (m_delegate.tool().dragPoint(proposedHandlePosition))
    {
      return DragStatus::Continue;
    }
    return DragStatus::Deny;
  }

  void end(const InputState&, const DragState&) override
  {
    m_delegate.tool().endDragPoint();
  }

  void cancel(const DragState&) override { m_delegate.tool().cancelDragPoint(); }
};

class MoveCutPointPart : public ToolController, protected PartBase
{
public:
  explicit MoveCutPointPart(std::unique_ptr<PartDelegateBase> delegate)
    : PartBase{std::move(delegate)}
  {
  }

private:
  Tool& tool() override { return m_delegate->tool(); }

  const Tool& tool() const override { return m_delegate->tool(); }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    if (
      inputState.mouseButtons() != MouseButtons::Left
      || inputState.modifierKeys() != ModifierKeys::None)
    {
      return nullptr;
    }

    const auto initialHandlePositionAndHitPoint =
      m_delegate->tool().beginDragPoint(inputState.pickResult());
    if (!initialHandlePositionAndHitPoint)
    {
      return nullptr;
    }

    const auto [initialHandlePosition, handleOffset] = *initialHandlePositionAndHitPoint;
    return createHandleDragTracker(
      MoveCutPointDragDelegate{*m_delegate},
      inputState,
      initialHandlePosition,
      handleOffset);
  }

  bool cancel() override { return false; }
};

} // namespace

PolygonCutToolControllerBase::PolygonCutToolControllerBase(PolygonCutTool& tool)
  : m_tool{tool}
{
}

PolygonCutToolControllerBase::~PolygonCutToolControllerBase() = default;

Tool& PolygonCutToolControllerBase::tool()
{
  return m_tool;
}

const Tool& PolygonCutToolControllerBase::tool() const
{
  return m_tool;
}

void PolygonCutToolControllerBase::pick(
  const InputState& inputState, mdl::PickResult& pickResult)
{
  m_tool.pick(inputState.pickRay(), inputState.camera(), pickResult);
}

void PolygonCutToolControllerBase::render(
  const InputState& inputState,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  m_tool.render(renderContext, renderBatch, inputState.pickResult());
  ToolControllerGroup::render(inputState, renderContext, renderBatch);
}

bool PolygonCutToolControllerBase::cancel()
{
  if (m_tool.removeLastPoint())
  {
    if (!m_tool.hasPoints())
    {
      m_tool.reset();
    }
    return true;
  }
  return false;
}

PolygonCutToolController2D::PolygonCutToolController2D(PolygonCutTool& tool)
  : PolygonCutToolControllerBase{tool}
{
  addController(
    std::make_unique<MoveCutPointPart>(std::make_unique<PartDelegate2D>(tool)));
  addController(
    std::make_unique<AddCutPointPart>(std::make_unique<PartDelegate2D>(tool)));
}

PolygonCutToolController3D::PolygonCutToolController3D(PolygonCutTool& tool)
  : PolygonCutToolControllerBase{tool}
{
  addController(
    std::make_unique<MoveCutPointPart>(std::make_unique<PartDelegate3D>(tool)));
  addController(
    std::make_unique<AddCutPointPart>(std::make_unique<PartDelegate3D>(tool)));
}

} // namespace tb::ui
