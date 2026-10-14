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

#include <string>

namespace tb::ui
{
class PrefabTool;

class PrefabToolController : public ToolController
{
protected:
  PrefabTool& m_tool;

protected:
  explicit PrefabToolController(PrefabTool& tool);

public:
  ~PrefabToolController() override;

private:
  Tool& tool() override;
  const Tool& tool() const override;

  bool shouldAcceptDrop(
    const InputState& inputState, const std::string& payload) const override;
  std::unique_ptr<DropTracker> acceptDrop(
    const InputState& inputState, const std::string& payload) override;

  bool cancel() override;

private:
  virtual std::unique_ptr<DropTracker> createDropTracker(
    const InputState& inputState) const = 0;
};

class PrefabToolController2D : public PrefabToolController
{
public:
  explicit PrefabToolController2D(PrefabTool& tool);

private:
  std::unique_ptr<DropTracker> createDropTracker(
    const InputState& inputState) const override;
};

class PrefabToolController3D : public PrefabToolController
{
public:
  explicit PrefabToolController3D(PrefabTool& tool);

private:
  std::unique_ptr<DropTracker> createDropTracker(
    const InputState& inputState) const override;
};

} // namespace tb::ui
