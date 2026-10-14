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

#pragma once

#include "ui/BrushHandleToolBase.h"
#include "mdl/Brush.h"
#include "mdl/HitType.h"

#include "vm/polygon.h"

#include <optional>
#include <string>
#include <vector>

class QWidget;

namespace tb::render
{
class RenderBatch;
class RenderContext;
} // namespace tb::render

namespace tb::ui
{
class MapDocument;

class FaceTool : public BrushHandleToolBase<mdl::FaceHandle>
{
public:
  static const mdl::HitType::Type SpinHandleHitType;

  enum class SpinHandleComponent
  {
    Center,
    XAxis,
    YAxis,
    ZAxis,
    XRotation,
    YRotation,
    ZRotation
  };

  struct SpinHandleHit
  {
    SpinHandleComponent component = SpinHandleComponent::Center;
    vm::vec3d position = vm::vec3d{0, 0, 0};
  };

  explicit FaceTool(MapDocument& document);

  using BrushHandleToolBase::findIncidentNodes;

  void pick(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    double handleRadius,
    mdl::PickResult& pickResult) const override;

public:
  std::tuple<vm::vec3d, vm::vec3d> handlePositionAndHitPoint(
    const std::vector<mdl::Hit>& hits) const override;

  MoveResult move(const vm::vec3d& delta) override;

  std::string actionName() const override;

  void removeSelection();

  bool insetActive() const;
  bool adjustInsetAmount(int delta);
  void finishInset();
  void cancelInset();

  bool insetToolActive() const;
  bool insetFaceSelected() const;
  bool selectInsetFace(const mdl::PickResult& pickResult);
  double insetAmount() const;
  double insetAmountStep() const;
  bool insetSnapToGrid() const;
  void activateInsetTool();
  void cancelInsetTool();
  void applyInsetTool();
  void resetInsetSettings();
  void setInsetAmount(double amount);
  void setInsetSnapToGrid(bool snapToGrid);

  Notifier<FaceTool&> insetSettingsDidChangeNotifier;

  bool spinToolActive() const;
  bool spinActive() const;
  bool spinFacesSelected() const;
  int spinSegments() const;
  double spinFlexibility() const;
  double spinEndScale() const;
  vm::vec3d spinEndPosition() const;
  vm::vec3d spinEndRotation() const;
  double spinHandleAxisLength() const;
  double spinHandleRotationRadius() const;
  void activateSpinTool();
  void cancelSpinTool();
  void applySpinTool(bool snapVerticesToInteger);
  void resetSpinSettings();
  void saveSpinUndoState();
  bool undoSpinStep();
  void setSpinSegments(int segments);
  void setSpinFlexibility(double flexibility);
  void setSpinEndScale(double endScale);
  bool moveSpinEndHandleTo(const vm::vec3d& position);
  bool setSpinEndRotation(const vm::vec3d& rotation);
  void pickSpinEndHandle(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    double handleRadius,
    mdl::PickResult& pickResult) const;
  void renderSpinTool(
    render::RenderContext& renderContext,
    render::RenderBatch& renderBatch,
    const mdl::PickResult& pickResult) const;

  Notifier<FaceTool&> spinSettingsDidChangeNotifier;

private:
  struct InsetFace
  {
    size_t faceIndex = 0u;
    double maxAmount = 0.0;
  };

  struct InsetBrush
  {
    mdl::BrushNode* node = nullptr;
    mdl::Brush originalBrush;
    std::vector<InsetFace> faces;
  };

  struct InsetState
  {
    std::vector<InsetBrush> brushes;
    std::vector<vm::polygon3d> originalHandles;
    double amount = 0.0;
    double maxAmount = 0.0;
    double step = 1.0;
  };

  std::optional<InsetState> m_inset;
  double m_insetAmount = 0.0;
  bool m_insetSnapToGrid = false;
  bool m_insetToolActive = false;

  bool beginInset();
  bool applyInsetPreview(double targetAmount);
  bool applyInsetSettingsPreview();
  void updateInsetSettingsFromState();
  void notifyInsetSettingsChanged();

public:
  struct SpinFace
  {
    size_t faceIndex = 0u;
    vm::polygon3d polygon;
    vm::vec3d normal = vm::vec3d{0, 0, 1};
    vm::vec3d center = vm::vec3d{0, 0, 0};
  };

  struct SpinBrush
  {
    mdl::BrushNode* node = nullptr;
    mdl::Brush originalBrush;
    std::vector<SpinFace> faces;
  };

  struct SpinState
  {
    std::vector<SpinBrush> brushes;
    std::vector<vm::polygon3d> originalHandles;
    std::vector<mdl::BrushNode*> previewBrushNodes;
    vm::vec3d baseCenter = vm::vec3d{0, 0, 0};
    vm::vec3d defaultNormal = vm::vec3d{0, 0, 1};
    vm::vec3d endPosition = vm::vec3d{0, 0, 0};
  };

  struct SpinUndoState
  {
    std::vector<vm::polygon3d> handles;
    int segments = 6;
    double flexibility = 0.0;
    double endScale = 1.0;
    std::optional<vm::vec3d> endPosition;
    vm::vec3d endRotation = vm::vec3d{0, 0, 0};
  };

private:
  std::optional<SpinState> m_spin;
  int m_spinSegments = 6;
  double m_spinFlexibility = 0.0;
  double m_spinEndScale = 1.0;
  std::optional<vm::vec3d> m_customSpinEndPosition;
  vm::vec3d m_spinEndRotation = vm::vec3d{0, 0, 0};
  bool m_spinToolActive = false;
  bool m_spinSelectionChangeLocked = false;
  bool m_spinPreviewChangeLocked = false;
  std::vector<SpinUndoState> m_spinUndoStack;
  NotifierConnection m_spinSelectionConnection;

  bool beginSpin();
  bool beginSpin(const std::vector<vm::polygon3d>& handles);
  bool applySpinSettingsPreview();
  void finishSpin(bool snapVerticesToInteger);
  void cancelSpin();
  void updateSpinStateSettings();
  void notifySpinSettingsChanged();
  vm::vec3d defaultSpinEndPosition(const SpinState& state) const;

private: // Tool interface
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};

} // namespace tb::ui
