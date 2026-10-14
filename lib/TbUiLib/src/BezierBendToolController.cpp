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

#include "ui/BezierBendToolController.h"

#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/Hit.h"
#include "mdl/HitFilter.h"
#include "ui/BezierBendTool.h"
#include "ui/HandleDragTracker.h"

#include "vm/intersection.h"
#include "vm/plane.h"

#include <algorithm>
#include <cmath>

namespace tb::ui
{
namespace
{
vm::vec3d bendHandleAxis(const BezierBendTool::HandleComponent component)
{
  switch (component)
  {
  case BezierBendTool::HandleComponent::XAxis:
    return vm::vec3d{1, 0, 0};
  case BezierBendTool::HandleComponent::YAxis:
    return vm::vec3d{0, 1, 0};
  case BezierBendTool::HandleComponent::ZAxis:
    return vm::vec3d{0, 0, 1};
  case BezierBendTool::HandleComponent::Center:
    return vm::vec3d{0, 0, 0};
    switchDefault();
  }
}

size_t bendHandleComponentIndex(const BezierBendTool::HandleComponent component)
{
  switch (component)
  {
  case BezierBendTool::HandleComponent::XAxis:
    return 0u;
  case BezierBendTool::HandleComponent::YAxis:
    return 1u;
  case BezierBendTool::HandleComponent::ZAxis:
    return 2u;
  case BezierBendTool::HandleComponent::Center:
    return 0u;
    switchDefault();
  }
}

size_t bendAxisIndex(const BezierBendTool::Axis axis)
{
  switch (axis)
  {
  case BezierBendTool::Axis::X:
    return 0u;
  case BezierBendTool::Axis::Y:
    return 1u;
  case BezierBendTool::Axis::Z:
    return 2u;
    switchDefault();
  }
}

vm::vec3d bendAxisVector(const BezierBendTool::Axis axis)
{
  switch (axis)
  {
  case BezierBendTool::Axis::X:
    return vm::vec3d{1, 0, 0};
  case BezierBendTool::Axis::Y:
    return vm::vec3d{0, 1, 0};
  case BezierBendTool::Axis::Z:
    return vm::vec3d{0, 0, 1};
    switchDefault();
  }
}

vm::vec3d projectOnBendRotationPlane(const vm::vec3d& vector, const vm::vec3d& axis)
{
  return vector - axis * vm::dot(vector, axis);
}

vm::vec3d fallbackBendRotationVector(const vm::vec3d& axis)
{
  const auto helper = std::abs(vm::dot(axis, vm::vec3d{0, 0, 1})) > 0.95
                        ? vm::vec3d{0, 1, 0}
                        : vm::vec3d{0, 0, 1};
  return vm::normalize(vm::cross(axis, helper));
}

double signedBendRotationAngle(
  const vm::vec3d& from, const vm::vec3d& to, const vm::vec3d& axis)
{
  const auto clampedDot = std::clamp(vm::dot(from, to), -1.0, 1.0);
  return std::atan2(vm::dot(vm::cross(from, to), axis), clampedDot);
}

HandlePositionProposer makeBendRotationHandleProposer(
  const vm::vec3d& center, const vm::vec3d& axis, const double radius)
{
  return [center, axis, radius](
           const InputState& inputState, const DragState&) -> std::optional<vm::vec3d> {
    const auto plane = vm::plane3d{center, axis};
    const auto distance = vm::intersect_ray_plane(inputState.pickRay(), plane);
    if (!distance)
    {
      return std::nullopt;
    }

    const auto hitPoint = vm::point_at_distance(inputState.pickRay(), *distance);
    const auto projected = projectOnBendRotationPlane(hitPoint - center, axis);
    if (vm::squared_length(projected) <= 0.000001)
    {
      return center + fallbackBendRotationVector(axis) * radius;
    }
    return center + vm::normalize(projected) * radius;
  };
}
} // namespace

BezierBendToolController::BezierBendToolController(BezierBendTool& tool)
  : m_tool{tool}
{
}

Tool& BezierBendToolController::tool()
{
  return m_tool;
}

const Tool& BezierBendToolController::tool() const
{
  return m_tool;
}

void BezierBendToolController::pick(
  const InputState& inputState, mdl::PickResult& pickResult)
{
  m_tool.pickHandle(
    inputState.pickRay(),
    inputState.camera(),
    pref(Preferences::HandleRadius),
    pickResult);
}

std::unique_ptr<GestureTracker> BezierBendToolController::acceptMouseDrag(
  const InputState& inputState)
{
  using namespace mdl::HitFilters;

  if (!m_tool.active() || !inputState.mouseButtonsPressed(MouseButtons::Left))
  {
    return nullptr;
  }

  const auto& hit = inputState.pickResult().first(type(BezierBendTool::HandleHitType));
  if (!hit.isMatch())
  {
    return nullptr;
  }

  const auto hitData = hit.target<BezierBendTool::HandleHit>();
  m_tool.pushUndoState();

  class BendHandleDragDelegate : public HandleDragTrackerDelegate
  {
  private:
    BezierBendTool& m_tool;
    size_t m_index;
    BezierBendTool::HandleComponent m_component;
    vm::vec3d m_startPosition = vm::vec3d{0, 0, 0};
    vm::vec3d m_startScale = vm::vec3d::fill(1.0);
    vm::vec3d m_startRotation = vm::vec3d{0, 0, 0};
    vm::vec3d m_rotationAxis = vm::vec3d{0, 0, 1};
    vm::vec3d m_previousRotationVector = vm::vec3d{1, 0, 0};
    double m_accumulatedRotation = 0.0;

  public:
    explicit BendHandleDragDelegate(
      BezierBendTool& tool,
      const size_t index,
      const BezierBendTool::HandleComponent component)
      : m_tool{tool}
      , m_index{index}
      , m_component{component}
    {
    }

    HandlePositionProposer start(
      const InputState& inputState,
      const vm::vec3d& initialHandlePosition,
      const vm::vec3d& handleOffset) override
    {
      m_startPosition = initialHandlePosition;
      m_startScale = m_tool.handleScale(m_index);
      m_startRotation = m_tool.handleRotation(m_index);
      if (m_tool.manipulationMode() == BezierBendTool::ManipulationMode::Rotate)
      {
        m_rotationAxis = m_component == BezierBendTool::HandleComponent::Center
                           ? bendAxisVector(m_tool.axis())
                           : bendHandleAxis(m_component);
        const auto initialHitPoint = initialHandlePosition - handleOffset;
        const auto initialRotationVector = projectOnBendRotationPlane(
          initialHitPoint - initialHandlePosition, m_rotationAxis);
        m_previousRotationVector = vm::squared_length(initialRotationVector) <= 0.000001
                                     ? fallbackBendRotationVector(m_rotationAxis)
                                     : vm::normalize(initialRotationVector);
        m_accumulatedRotation = 0.0;
        const auto radius =
          std::max(16.0, vm::length(initialHitPoint - initialHandlePosition));
        return makeBendRotationHandleProposer(
          initialHandlePosition, m_rotationAxis, radius);
      }

      if (m_component != BezierBendTool::HandleComponent::Center)
      {
        const auto line = vm::line3d{m_startPosition, bendHandleAxis(m_component)};
        return makeHandlePositionProposer(
          makeLineHandlePicker(line, handleOffset),
          makeAbsoluteLineHandleSnapper(m_tool.grid(), line));
      }

      const auto plane = vm::orthogonal_plane(
        initialHandlePosition, vm::vec3d{inputState.camera().direction()});
      return makeHandlePositionProposer(
        makePlaneHandlePicker(plane, handleOffset),
        makeAbsoluteHandleSnapper(m_tool.grid()));
    }

    DragStatus update(
      const InputState& inputState,
      const DragState&,
      const vm::vec3d& proposedHandlePosition) override
    {
      const auto mode = m_tool.manipulationMode();
      if (mode == BezierBendTool::ManipulationMode::Move)
      {
        return m_tool.moveHandleTo(m_index, proposedHandlePosition) ? DragStatus::Continue
                                                                    : DragStatus::Deny;
      }

      const auto delta = proposedHandlePosition - m_startPosition;
      const auto dragAxis = m_component == BezierBendTool::HandleComponent::Center
                              ? vm::vec3d{inputState.camera().up()}
                              : bendHandleAxis(m_component);
      const auto amount = vm::dot(delta, dragAxis);
      const auto sensitivity = std::max(16.0, m_tool.handleAxisLength());

      if (mode == BezierBendTool::ManipulationMode::Scale)
      {
        const auto componentIndex = m_component == BezierBendTool::HandleComponent::Center
                                      ? 0u
                                      : bendHandleComponentIndex(m_component);
        const auto baseScale =
          m_component == BezierBendTool::HandleComponent::Center
            ? (m_startScale.x() + m_startScale.y() + m_startScale.z()) / 3.0
            : m_startScale[componentIndex];
        return m_tool.scaleHandleTo(
                 m_index, m_component, baseScale + amount / sensitivity)
                 ? DragStatus::Continue
                 : DragStatus::Deny;
      }

      const auto componentIndex = m_component == BezierBendTool::HandleComponent::Center
                                    ? bendAxisIndex(m_tool.axis())
                                    : bendHandleComponentIndex(m_component);
      const auto currentRotationVector = projectOnBendRotationPlane(
        proposedHandlePosition - m_startPosition, m_rotationAxis);
      if (vm::squared_length(currentRotationVector) <= 0.000001)
      {
        return DragStatus::Continue;
      }

      const auto normalizedRotationVector = vm::normalize(currentRotationVector);
      m_accumulatedRotation += signedBendRotationAngle(
        m_previousRotationVector, normalizedRotationVector, m_rotationAxis);
      m_previousRotationVector = normalizedRotationVector;

      const auto rotation = m_startRotation[componentIndex] + m_accumulatedRotation;
      return m_tool.rotateHandleTo(m_index, m_component, rotation) ? DragStatus::Continue
                                                                   : DragStatus::Deny;
    }

    void end(const InputState&, const DragState&) override {}

    void cancel(const DragState&) override
    {
      switch (m_tool.manipulationMode())
      {
      case BezierBendTool::ManipulationMode::Move:
        m_tool.moveHandleTo(m_index, m_startPosition);
        break;
      case BezierBendTool::ManipulationMode::Scale:
        if (m_component == BezierBendTool::HandleComponent::Center)
        {
          m_tool.scaleHandleTo(
            m_index, BezierBendTool::HandleComponent::XAxis, m_startScale.x());
          m_tool.scaleHandleTo(
            m_index, BezierBendTool::HandleComponent::YAxis, m_startScale.y());
          m_tool.scaleHandleTo(
            m_index, BezierBendTool::HandleComponent::ZAxis, m_startScale.z());
        }
        else
        {
          m_tool.scaleHandleTo(
            m_index, m_component, m_startScale[bendHandleComponentIndex(m_component)]);
        }
        break;
      case BezierBendTool::ManipulationMode::Rotate:
        if (m_component == BezierBendTool::HandleComponent::Center)
        {
          m_tool.rotateHandleTo(
            m_index, BezierBendTool::HandleComponent::XAxis, m_startRotation.x());
          m_tool.rotateHandleTo(
            m_index, BezierBendTool::HandleComponent::YAxis, m_startRotation.y());
          m_tool.rotateHandleTo(
            m_index, BezierBendTool::HandleComponent::ZAxis, m_startRotation.z());
        }
        else
        {
          m_tool.rotateHandleTo(
            m_index, m_component, m_startRotation[bendHandleComponentIndex(m_component)]);
        }
        break;
      }
    }
  };

  return createHandleDragTracker(
    BendHandleDragDelegate{m_tool, hitData.index, hitData.component},
    inputState,
    m_tool.handlePosition(hitData.index),
    hit.hitPoint());
}

void BezierBendToolController::render(
  const InputState& inputState,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  m_tool.render(renderContext, renderBatch, inputState.pickResult());
}

bool BezierBendToolController::cancel()
{
  if (!m_tool.active())
  {
    return false;
  }

  m_tool.cancelBendTool();
  return true;
}

} // namespace tb::ui
