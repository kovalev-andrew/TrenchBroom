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

#include "ui/SculptToolController.h"

#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "ui/GestureTracker.h"
#include "ui/InputState.h"
#include "ui/SculptTool.h"

#include <memory>

namespace tb::ui
{
namespace
{

class SculptDragTracker : public GestureTracker
{
private:
  SculptTool& m_tool;

public:
  SculptDragTracker(SculptTool& tool, const InputState& inputState)
    : m_tool{tool}
  {
    m_tool.beginStroke();
    update(inputState);
  }

  bool update(const InputState& inputState) override
  {
    m_tool.updateBrush(inputState.pickResult());
    m_tool.applyBrush();
    return true;
  }

  void end(const InputState&) override { m_tool.endStroke(); }

  void cancel() override { m_tool.cancelStroke(); }

  void render(
    const InputState&,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) const override
  {
    m_tool.render(renderContext, renderBatch);
  }
};

bool leftButtonDown(const InputState& inputState)
{
  return inputState.mouseButtonsPressed(MouseButtons::Left);
}

} // namespace

SculptToolController::SculptToolController(SculptTool& tool)
  : m_tool{tool}
{
}

SculptToolController::~SculptToolController() = default;

Tool& SculptToolController::tool()
{
  return m_tool;
}

const Tool& SculptToolController::tool() const
{
  return m_tool;
}

void SculptToolController::mouseMove(const InputState& inputState)
{
  if (toolActive() && !inputState.anyToolDragging())
  {
    m_tool.updateBrush(inputState.pickResult());
    refreshViews();
  }
}

bool SculptToolController::mouseClick(const InputState& inputState)
{
  if (!toolActive() || !leftButtonDown(inputState))
  {
    return false;
  }

  m_tool.updateBrush(inputState.pickResult());
  m_tool.beginStroke();
  m_tool.applyBrush();
  m_tool.endStroke();
  refreshViews();
  return true;
}

std::unique_ptr<GestureTracker> SculptToolController::acceptMouseDrag(
  const InputState& inputState)
{
  if (!toolActive() || !leftButtonDown(inputState))
  {
    return nullptr;
  }

  return std::make_unique<SculptDragTracker>(m_tool, inputState);
}

void SculptToolController::render(
  const InputState&,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  if (toolActive())
  {
    m_tool.render(renderContext, renderBatch);
  }
}

bool SculptToolController::cancel()
{
  if (!toolActive())
  {
    return false;
  }

  m_tool.cancelStroke();
  m_tool.clearBrush();
  refreshViews();
  return false;
}

} // namespace tb::ui
