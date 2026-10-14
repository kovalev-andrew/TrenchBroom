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

#include "ui/ToolController.h"

namespace tb::ui
{
class BezierBendTool;

class BezierBendToolController : public ToolController
{
private:
  BezierBendTool& m_tool;

public:
  explicit BezierBendToolController(BezierBendTool& tool);

private:
  Tool& tool() override;
  const Tool& tool() const override;

  void pick(const InputState& inputState, mdl::PickResult& pickResult) override;
  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState& inputState) override;
  void render(
    const InputState& inputState,
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch) override;
  bool cancel() override;
};

} // namespace tb::ui
