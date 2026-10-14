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

#include "ui/Tool.h"

#include "vm/plane.h"
#include "vm/vec.h"

#include <optional>

namespace tb
{
namespace mdl
{
class BrushNode;
class PickResult;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class MapDocument;

class SculptTool : public Tool
{
public:
  enum class Mode
  {
    Raise,
    Lower,
    Smooth,
  };

  struct Brush
  {
    mdl::BrushNode* node = nullptr;
    vm::vec3d position;
    vm::plane3d plane;
  };

private:
  MapDocument& m_document;
  Mode m_mode = Mode::Raise;
  double m_radius = 64.0;
  double m_intensity = 2.0;
  bool m_preventCollapse = true;
  std::optional<Brush> m_brush;
  bool m_transactionOpen = false;
  bool m_transactionChanged = false;

public:
  explicit SculptTool(MapDocument& document);
  ~SculptTool() override;

  Mode mode() const;
  void setMode(Mode mode);

  double radius() const;
  void setRadius(double radius);

  double intensity() const;
  void setIntensity(double intensity);

  bool preventCollapse() const;
  void setPreventCollapse(bool preventCollapse);

  bool hasBrush() const;
  void updateBrush(const mdl::PickResult& pickResult);
  void clearBrush();

  void beginStroke();
  bool applyBrush();
  void endStroke();
  void cancelStroke();

  void render(
    render::RenderContext& renderContext, render::RenderBatch& renderBatch) const;

private:
  bool sculptAt(const vm::vec3d& center, const vm::plane3d& plane);

  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};

} // namespace ui
} // namespace tb
