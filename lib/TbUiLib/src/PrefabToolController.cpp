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

#include "ui/PrefabToolController.h"

#include "ui/DropTracker.h"
#include "ui/InputState.h"
#include "ui/PrefabTool.h"

#include "kd/contracts.h"

#include <functional>
#include <string>

namespace tb::ui
{
namespace
{

constexpr auto PrefabPayloadPrefix = "prefab\n";

bool isPrefabPayload(const std::string& payload)
{
  return payload.rfind(PrefabPayloadPrefix, 0) == 0;
}

class PrefabDropTracker : public DropTracker
{
private:
  using UpdatePrefabPosition = std::function<void(const InputState&, PrefabTool&)>;

  PrefabTool& m_tool;
  UpdatePrefabPosition m_updatePrefabPosition;

public:
  explicit PrefabDropTracker(
    const InputState& inputState,
    PrefabTool& tool,
    UpdatePrefabPosition updatePrefabPosition)
    : m_tool{tool}
    , m_updatePrefabPosition{std::move(updatePrefabPosition)}
  {
    m_updatePrefabPosition(inputState, m_tool);
  }

  bool move(const InputState& inputState) override
  {
    m_updatePrefabPosition(inputState, m_tool);
    return true;
  }

  bool drop(const InputState&) override
  {
    m_tool.commitPrefab();
    return true;
  }

  void leave(const InputState&) override { m_tool.removePrefab(); }
};

} // namespace

PrefabToolController::PrefabToolController(PrefabTool& tool)
  : m_tool{tool}
{
}

PrefabToolController::~PrefabToolController() = default;

Tool& PrefabToolController::tool()
{
  return m_tool;
}

const Tool& PrefabToolController::tool() const
{
  return m_tool;
}

bool PrefabToolController::shouldAcceptDrop(
  const InputState&, const std::string& payload) const
{
  return isPrefabPayload(payload);
}

std::unique_ptr<DropTracker> PrefabToolController::acceptDrop(
  const InputState& inputState, const std::string& payload)
{
  if (!shouldAcceptDrop(inputState, payload))
  {
    return nullptr;
  }

  return m_tool.createPrefab(payload) ? createDropTracker(inputState) : nullptr;
}

bool PrefabToolController::cancel()
{
  return false;
}

PrefabToolController2D::PrefabToolController2D(PrefabTool& tool)
  : PrefabToolController{tool}
{
}

std::unique_ptr<DropTracker> PrefabToolController2D::createDropTracker(
  const InputState& inputState) const
{
  return std::make_unique<PrefabDropTracker>(
    inputState, m_tool, [](const auto& is, auto& t) {
      t.updatePrefabPosition2D(is.pickRay());
    });
}

PrefabToolController3D::PrefabToolController3D(PrefabTool& tool)
  : PrefabToolController{tool}
{
}

std::unique_ptr<DropTracker> PrefabToolController3D::createDropTracker(
  const InputState& inputState) const
{
  return std::make_unique<PrefabDropTracker>(
    inputState, m_tool, [](const auto& is, auto& t) {
      t.updatePrefabPosition3D(is.pickRay(), is.pickResult());
    });
}

} // namespace tb::ui
