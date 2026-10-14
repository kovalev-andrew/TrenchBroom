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

#include "ui/EdgeToolController.h"

#include "ui/GestureTracker.h"
#include "ui/EdgeTool.h"
#include "ui/NodeHandleToolControllerParts.h"

#include <cmath>
#include <memory>

namespace tb::ui
{
namespace
{
constexpr auto ChamferCurveDragSensitivity = 0.005;
constexpr auto ChamferProportionDragSensitivity = 0.005;

class ChamferCurveDragTracker : public GestureTracker
{
private:
  EdgeTool& m_tool;

public:
  explicit ChamferCurveDragTracker(EdgeTool& tool)
    : m_tool{tool}
  {
  }

  bool update(const InputState& inputState) override
  {
    if (
      !m_tool.chamferCurveAdjustActive()
      || !inputState.modifierKeysDown(ModifierKeys::CtrlCmd)
      || !inputState.mouseButtonsPressed(MouseButtons::Right))
    {
      return false;
    }

    const auto dragAmount =
      double(inputState.mouseDX() - inputState.mouseDY()) * ChamferCurveDragSensitivity;
    if (std::abs(dragAmount) <= 0.000001)
    {
      return true;
    }

    m_tool.adjustChamferCurve(dragAmount);
    return true;
  }

  void end(const InputState&) override {}
  void cancel() override {}
};

class ChamferProportionDragTracker : public GestureTracker
{
private:
  EdgeTool& m_tool;

public:
  explicit ChamferProportionDragTracker(EdgeTool& tool)
    : m_tool{tool}
  {
  }

  bool update(const InputState& inputState) override
  {
    if (
      !m_tool.chamferCurveAdjustActive()
      || !inputState.modifierKeysDown(ModifierKeys::CtrlCmd)
      || !inputState.mouseButtonsPressed(MouseButtons::Middle))
    {
      return false;
    }

    const auto dragAmount = double(inputState.mouseDX() - inputState.mouseDY())
                            * ChamferProportionDragSensitivity;
    if (std::abs(dragAmount) <= 0.000001)
    {
      return true;
    }

    m_tool.adjustChamferProportion(dragAmount);
    return true;
  }

  void end(const InputState&) override {}
  void cancel() override {}
};

class ChamferEdgePart : public ToolController
{
private:
  EdgeTool& m_tool;

public:
  explicit ChamferEdgePart(EdgeTool& tool)
    : m_tool{tool}
  {
  }

private:
  Tool& tool() override { return m_tool; }

  const Tool& tool() const override { return m_tool; }

  void modifierKeyChange(const InputState& inputState) override
  {
    if (
      m_tool.chamferActive() && !m_tool.bevelToolActive()
      && !inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
    {
      m_tool.finishChamfer();
    }
  }

  void mouseScroll(const InputState& inputState) override
  {
    if (
      inputState.scrollY() == 0.0f || !inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
    {
      return;
    }

    const auto delta = inputState.scrollY() > 0.0f ? 1 : -1;
    if (inputState.modifierKeysDown(ModifierKeys::Shift))
    {
      m_tool.adjustChamferWidth(delta);
    }
    else
    {
      m_tool.adjustChamferSegments(delta);
    }
  }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override
  {
    if (
      !m_tool.chamferCurveAdjustActive()
      || !inputState.modifierKeysDown(ModifierKeys::CtrlCmd))
    {
      return nullptr;
    }

    if (inputState.mouseButtonsPressed(MouseButtons::Right))
    {
      return std::make_unique<ChamferCurveDragTracker>(m_tool);
    }

    if (inputState.mouseButtonsPressed(MouseButtons::Middle))
    {
      return std::make_unique<ChamferProportionDragTracker>(m_tool);
    }

    return nullptr;
  }

  bool cancel() override
  {
    if (m_tool.bevelToolActive())
    {
      m_tool.cancelBevelTool();
      return true;
    }

    if (!m_tool.chamferActive())
    {
      return false;
    }

    m_tool.cancelChamfer();
    return true;
  }
};

class SelectEdgePart : public NodeHandleToolSelectPartBase<EdgeTool, mdl::EdgeHandle>
{
public:
  explicit SelectEdgePart(EdgeTool& tool)
    : NodeHandleToolSelectPartBase{tool, mdl::EdgeHandle::HandleHitType}
  {
  }

private:
  bool equalHandles(const mdl::EdgeHandle& lhs, const mdl::EdgeHandle& rhs) const override
  {
    return compare(lhs.position, rhs.position, MaxHandleDistance) == 0;
  }
};

class MoveEdgePart : public NodeHandleToolMovePartBase<EdgeTool>
{
public:
  explicit MoveEdgePart(EdgeTool& tool)
    : NodeHandleToolMovePartBase{tool, mdl::EdgeHandle::HandleHitType}
  {
  }
};

} // namespace

EdgeToolController::EdgeToolController(EdgeTool& tool)
  : NodeHandleToolControllerBase{tool}
{
  addController(std::make_unique<ChamferEdgePart>(tool));
  addController(std::make_unique<MoveEdgePart>(tool));
  addController(std::make_unique<SelectEdgePart>(tool));
}

} // namespace tb::ui
