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

#include "ui/FaceToolController.h"

#include "mdl/NodeHandles.h"
#include "ui/FaceTool.h"
#include "ui/NodeHandleToolControllerParts.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/Hit.h"
#include "mdl/HitFilter.h"
#include "ui/HandleDragTracker.h"

#include "vm/intersection.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tb::ui
{
namespace
{
vm::vec3d spinHandleAxis(const FaceTool::SpinHandleComponent component)
{
  switch (component)
  {
  case FaceTool::SpinHandleComponent::XAxis:
  case FaceTool::SpinHandleComponent::XRotation:
    return vm::vec3d{1, 0, 0};
  case FaceTool::SpinHandleComponent::YAxis:
  case FaceTool::SpinHandleComponent::YRotation:
    return vm::vec3d{0, 1, 0};
  case FaceTool::SpinHandleComponent::ZAxis:
  case FaceTool::SpinHandleComponent::ZRotation:
    return vm::vec3d{0, 0, 1};
  case FaceTool::SpinHandleComponent::Center:
    return vm::vec3d{0, 0, 0};
    switchDefault();
  }
}

bool isSpinTranslationComponent(const FaceTool::SpinHandleComponent component)
{
  return component == FaceTool::SpinHandleComponent::Center
         || component == FaceTool::SpinHandleComponent::XAxis
         || component == FaceTool::SpinHandleComponent::YAxis
         || component == FaceTool::SpinHandleComponent::ZAxis;
}

size_t spinRotationComponentIndex(const FaceTool::SpinHandleComponent component)
{
  switch (component)
  {
  case FaceTool::SpinHandleComponent::XRotation:
    return 0u;
  case FaceTool::SpinHandleComponent::YRotation:
    return 1u;
  case FaceTool::SpinHandleComponent::ZRotation:
    return 2u;
  case FaceTool::SpinHandleComponent::Center:
  case FaceTool::SpinHandleComponent::XAxis:
  case FaceTool::SpinHandleComponent::YAxis:
  case FaceTool::SpinHandleComponent::ZAxis:
    return 0u;
    switchDefault();
  }
}

double signedSpinAngle(const vm::vec3d& from, const vm::vec3d& to, const vm::vec3d& axis)
{
  const auto clampedDot = std::clamp(vm::dot(from, to), -1.0, 1.0);
  return std::atan2(vm::dot(vm::cross(from, to), axis), clampedDot);
}

class InsetFacePart : public ToolController
{
private:
  FaceTool& m_tool;

public:
  explicit InsetFacePart(FaceTool& tool)
    : m_tool{tool}
  {
  }

private:
  Tool& tool() override { return m_tool; }

  const Tool& tool() const override { return m_tool; }

  bool mouseClick(const InputState& inputState) override
  {
    if (!m_tool.insetToolActive() || !inputState.mouseButtonsPressed(MouseButtons::Left))
    {
      return false;
    }
    m_tool.selectInsetFace(inputState.pickResult());
    return true;
  }

  void modifierKeyChange(const InputState& inputState) override
  {
    if (
      m_tool.insetActive() && !m_tool.insetToolActive()
      && !inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
    {
      m_tool.finishInset();
    }
  }

  void mouseScroll(const InputState& inputState) override
  {
    if (
      inputState.scrollY() == 0.0f || !inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
    {
      return;
    }

    m_tool.adjustInsetAmount(inputState.scrollY() > 0.0f ? 1 : -1);
  }

  bool cancel() override
  {
    if (m_tool.insetToolActive())
    {
      m_tool.cancelInsetTool();
      return true;
    }

    if (!m_tool.insetActive())
    {
      return false;
    }

    m_tool.cancelInset();
    return true;
  }
};

class SpinHandlePart : public ToolController
{
private:
  FaceTool& m_tool;

public:
  explicit SpinHandlePart(FaceTool& tool)
    : m_tool{tool}
  {
  }

private:
  Tool& tool() override { return m_tool; }

  const Tool& tool() const override { return m_tool; }

  void pick(const InputState& inputState, mdl::PickResult& pickResult) override
  {
    m_tool.pickSpinEndHandle(
      inputState.pickRay(),
      inputState.camera(),
      pref(Preferences::HandleRadius),
      pickResult);
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    using namespace mdl::HitFilters;

    if (!m_tool.spinToolActive() || !inputState.mouseButtonsPressed(MouseButtons::Left))
    {
      return nullptr;
    }

    const auto& hit = inputState.pickResult().first(type(FaceTool::SpinHandleHitType));
    if (!hit.isMatch())
    {
      return nullptr;
    }
    const auto hitData = hit.target<FaceTool::SpinHandleHit>();

    class SpinEndHandleDragDelegate : public HandleDragTrackerDelegate
    {
    private:
      FaceTool& m_tool;
      FaceTool::SpinHandleComponent m_component;
      vm::vec3d m_startPosition = vm::vec3d{0, 0, 0};
      vm::vec3d m_startRotation = vm::vec3d{0, 0, 0};

    public:
      explicit SpinEndHandleDragDelegate(
        FaceTool& tool, const FaceTool::SpinHandleComponent component)
        : m_tool{tool}
        , m_component{component}
      {
      }

      HandlePositionProposer start(
        const InputState& inputState,
        const vm::vec3d& initialHandlePosition,
        const vm::vec3d& handleOffset) override
      {
        m_startPosition = m_tool.spinEndPosition();
        m_startRotation = m_tool.spinEndRotation();
        m_tool.saveSpinUndoState();

        if (m_component == FaceTool::SpinHandleComponent::Center)
        {
          const auto plane = vm::orthogonal_plane(
            initialHandlePosition, vm::vec3d{inputState.camera().direction()});
          return makeHandlePositionProposer(
            makePlaneHandlePicker(plane, handleOffset),
            makeAbsoluteHandleSnapper(m_tool.grid()));
        }

        if (isSpinTranslationComponent(m_component))
        {
          const auto line = vm::line3d{m_startPosition, spinHandleAxis(m_component)};
          return makeHandlePositionProposer(
            makeLineHandlePicker(line, handleOffset),
            makeAbsoluteLineHandleSnapper(m_tool.grid(), line));
        }

        return makeHandlePositionProposer(
          makeCircleHandlePicker(
            m_startPosition,
            spinHandleAxis(m_component),
            m_tool.spinHandleRotationRadius(),
            handleOffset),
          makeIdentityHandleSnapper());
      }

      DragStatus update(
        const InputState&,
        const DragState& dragState,
        const vm::vec3d& proposedHandlePosition) override
      {
        if (isSpinTranslationComponent(m_component))
        {
          return m_tool.moveSpinEndHandleTo(proposedHandlePosition) ? DragStatus::Continue
                                                                    : DragStatus::Deny;
        }

        const auto center = m_tool.spinEndPosition();
        const auto axis = spinHandleAxis(m_component);
        if (
          vm::is_zero(dragState.initialHandlePosition - center, vm::Cd::almost_zero())
          || vm::is_zero(proposedHandlePosition - center, vm::Cd::almost_zero()))
        {
          return DragStatus::Deny;
        }

        const auto ref = vm::normalize(dragState.initialHandlePosition - center);
        const auto vec = vm::normalize(proposedHandlePosition - center);
        const auto angle = signedSpinAngle(ref, vec, axis);
        auto rotation = m_startRotation;
        rotation[spinRotationComponentIndex(m_component)] += angle;
        return m_tool.setSpinEndRotation(rotation) ? DragStatus::Continue
                                                   : DragStatus::Deny;
      }

      void end(const InputState&, const DragState&) override {}

      void cancel(const DragState&) override
      {
        if (isSpinTranslationComponent(m_component))
        {
          m_tool.moveSpinEndHandleTo(m_startPosition);
        }
        else
        {
          m_tool.setSpinEndRotation(m_startRotation);
        }
      }
    };

    auto initialHandlePosition = m_tool.spinEndPosition();
    auto initialHitPoint = hit.hitPoint();
    if (!isSpinTranslationComponent(hitData.component))
    {
      const auto axis = spinHandleAxis(hitData.component);
      if (
        const auto distance = vm::intersect_ray_plane(
          inputState.pickRay(), vm::plane3d{initialHandlePosition, axis}))
      {
        initialHandlePosition = vm::point_at_distance(inputState.pickRay(), *distance);
        initialHitPoint = initialHandlePosition;
      }
      else
      {
        return nullptr;
      }
    }

    return createHandleDragTracker(
      SpinEndHandleDragDelegate{m_tool, hitData.component},
      inputState,
      initialHandlePosition,
      initialHitPoint);
  }

  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override
  {
    m_tool.renderSpinTool(renderContext, renderBatch, inputState.pickResult());
  }

  bool cancel() override
  {
    if (!m_tool.spinToolActive())
    {
      return false;
    }

    m_tool.cancelSpinTool();
    return true;
  }
};

class SelectFacePart : public NodeHandleToolSelectPartBase<FaceTool, mdl::FaceHandle>
{
public:
  explicit SelectFacePart(FaceTool& tool)
    : NodeHandleToolSelectPartBase{tool, mdl::FaceHandle::HandleHitType}
  {
  }

private:
  bool equalHandles(const mdl::FaceHandle& lhs, const mdl::FaceHandle& rhs) const override
  {
    return compareUnoriented(lhs.position, rhs.position, MaxHandleDistance) == 0;
  }
};

class MoveFacePart : public NodeHandleToolMovePartBase<FaceTool>
{
public:
  explicit MoveFacePart(FaceTool& tool)
    : NodeHandleToolMovePartBase{tool, mdl::FaceHandle::HandleHitType}
  {
  }

private:
  bool shouldStartMove(const InputState& inputState) const override
  {
    return !m_tool.insetToolActive() && !m_tool.spinToolActive()
           && NodeHandleToolMovePartBase::shouldStartMove(inputState);
  }
};

} // namespace

FaceToolController::FaceToolController(FaceTool& tool)
  : NodeHandleToolControllerBase(tool)
{
  addController(std::make_unique<SpinHandlePart>(tool));
  addController(std::make_unique<InsetFacePart>(tool));
  addController(std::make_unique<MoveFacePart>(tool));
  addController(std::make_unique<SelectFacePart>(tool));
}

} // namespace tb::ui
