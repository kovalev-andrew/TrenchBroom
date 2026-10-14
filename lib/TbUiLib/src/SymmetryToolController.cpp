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

#include "ui/SymmetryToolController.h"

#include "ui/SymmetryTool.h"

namespace tb::ui
{

SymmetryToolController::SymmetryToolController(SymmetryTool& tool)
  : m_tool{tool}
{
}

Tool& SymmetryToolController::tool()
{
  return m_tool;
}

const Tool& SymmetryToolController::tool() const
{
  return m_tool;
}

void SymmetryToolController::render(
  const InputState&,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  m_tool.render(renderContext, renderBatch);
}

} // namespace tb::ui
