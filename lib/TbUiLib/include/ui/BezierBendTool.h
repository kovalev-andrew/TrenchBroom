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

#include "Notifier.h"
#include "mdl/Brush.h"
#include "mdl/HitType.h"
#include "ui/Tool.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <optional>
#include <vector>

class QWidget;

namespace tb::gl
{
class Camera;
} // namespace tb::gl

namespace tb::mdl
{
class BrushNode;
class Grid;
class GroupNode;
class PickResult;
} // namespace tb::mdl

namespace tb::render
{
class RenderBatch;
class RenderContext;
} // namespace tb::render

namespace tb::ui
{
class MapDocument;

class BezierBendTool : public Tool
{
public:
  static const mdl::HitType::Type HandleHitType;

  enum class Axis
  {
    X,
    Y,
    Z
  };

  enum class HandleComponent
  {
    Center,
    XAxis,
    YAxis,
    ZAxis
  };

  enum class ManipulationMode
  {
    Move,
    Scale,
    Rotate
  };

  struct HandleHit
  {
    size_t index = 0u;
    HandleComponent component = HandleComponent::Center;
    vm::vec3d position = vm::vec3d{0, 0, 0};
  };

private:
  struct BendBrush
  {
    mdl::BrushNode* node = nullptr;
    mdl::Brush originalBrush;
    vm::vec3d center = vm::vec3d{0, 0, 0};
  };

  struct BendState
  {
    mdl::GroupNode* group = nullptr;
    std::vector<BendBrush> brushes;
    vm::bbox3d bounds;
    vm::vec3d axisStart = vm::vec3d{0, 0, 0};
    double axisMin = 0.0;
    double axisExtent = 0.0;
  };

  struct BendUndoState
  {
    std::vector<vm::vec3d> controlPoints;
    std::vector<vm::vec3d> controlScales;
    std::vector<vm::vec3d> controlRotations;
    Axis axis = Axis::X;
    ManipulationMode manipulationMode = ManipulationMode::Move;
    int segments = 8;
    int subdivisions = 0;
    bool keepJoints = true;
  };

  MapDocument& m_document;
  std::optional<BendState> m_bend;
  bool m_bendTransactionOpen = false;
  bool m_closingBend = false;
  std::vector<vm::vec3d> m_controlPoints;
  std::vector<vm::vec3d> m_controlScales;
  std::vector<vm::vec3d> m_controlRotations;
  std::vector<BendUndoState> m_undoStack;
  Axis m_axis = Axis::X;
  ManipulationMode m_manipulationMode = ManipulationMode::Move;
  int m_segments = 8;
  int m_subdivisions = 0;
  bool m_keepJoints = true;

public:
  explicit BezierBendTool(MapDocument& document);

  bool canAttachToSelection() const;
  bool bendActive() const;

  int segments() const;
  int subdivisions() const;
  Axis axis() const;
  ManipulationMode manipulationMode() const;
  bool keepJoints() const;
  double handleAxisLength() const;
  const mdl::Grid& grid() const;

  void resetSettings();
  void setSegments(int segments);
  void setSubdivisions(int subdivisions);
  void setAxis(Axis axis);
  void setManipulationMode(ManipulationMode manipulationMode);
  void setKeepJoints(bool keepJoints);
  void applyBendTool();
  void cancelBendTool();
  void pushUndoState();
  bool undoBendStep();

  vm::vec3d handlePosition(size_t index) const;
  vm::vec3d handleScale(size_t index) const;
  vm::vec3d handleRotation(size_t index) const;
  bool moveHandleTo(size_t index, const vm::vec3d& position);
  bool scaleHandleTo(size_t index, HandleComponent component, double scale);
  bool rotateHandleTo(size_t index, HandleComponent component, double rotation);

  void pickHandle(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    double handleRadius,
    mdl::PickResult& pickResult) const;
  void render(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult) const;

  Notifier<BezierBendTool&> settingsDidChangeNotifier;
  Notifier<BezierBendTool&> bendDidRequestDeactivateNotifier;

private:
  bool beginBend();
  void closeBendTransaction(bool commit);
  bool applyPreview();
  void resetControlPoints(const vm::bbox3d& bounds);
  void notifySettingsChanged();

private: // Tool interface
  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};

} // namespace tb::ui
