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

#include <QWidget>

#include "Macros.h"
#include "NotifierConnection.h"
#include "ui/MapView.h"

namespace tb
{
namespace gl
{
class PerspectiveCamera;
}

namespace ui
{
class AppController;
class ClipTool;
class ControlPointTool;
class EdgeTool;
class FaceTool;
class Inspector;
class MapDocument;
class MapViewBar;
enum class MapViewLayout;
class MapViewToolBox;
class MultiPaneMapView;
class PolygonCutTool;
class Tool;
class VertexTool;

class SwitchableMapViewContainer : public QWidget, public MapView
{
  Q_OBJECT
private:
  AppController& m_appController;
  MapDocument& m_document;

  MapViewBar* m_mapViewBar = nullptr;
  std::unique_ptr<MapViewToolBox> m_toolBox;

  MultiPaneMapView* m_mapView = nullptr;
  std::unique_ptr<MapViewActivationTracker> m_activationTracker;

  NotifierConnection m_notifierConnection;

public:
  SwitchableMapViewContainer(
    AppController& appController, MapDocument& document, QWidget* parent = nullptr);
  ~SwitchableMapViewContainer() override;

  void connectTopWidgets(Inspector* inspector);

  void windowActivationStateChanged(bool active);

  bool active() const;
  void switchToMapView(MapViewLayout viewId);

  const MapViewToolBox& toolBox() const;
  MapViewToolBox& toolBox();
  bool anyModalToolActive() const;
  void deactivateCurrentTool();

  bool assembleBrushToolActive() const;
  bool canToggleAssembleBrushTool() const;
  void toggleAssembleBrushTool();

  bool clipToolActive() const;
  bool canToggleClipTool() const;
  void toggleClipTool();
  ClipTool& clipTool();

  bool polygonCutToolActive() const;
  bool canTogglePolygonCutTool() const;
  void togglePolygonCutTool();
  PolygonCutTool& polygonCutTool();
  bool polygonSpawnToolActive() const;
  bool canTogglePolygonSpawnTool() const;
  void togglePolygonSpawnTool();

  bool rotateToolActive() const;
  bool canToggleRotateTool() const;
  void toggleRotateTool();

  bool scaleToolActive() const;
  bool canToggleScaleTool() const;
  void toggleScaleTool();

  bool shearToolActive() const;
  bool canToggleShearTool() const;
  void toggleShearTool();

  bool symmetryToolActive() const;
  bool symmetryToolPanelSelected() const;
  bool symmetryModifierActive() const;
  bool canToggleSymmetryTool() const;
  void toggleSymmetryTool();

  bool canToggleVertexTools() const;
  bool anyVertexToolActive() const;
  bool vertexToolActive() const;
  bool edgeToolActive() const;
  bool bevelToolActive() const;
  bool faceToolActive() const;
  bool insetToolActive() const;
  bool spinToolActive() const;
  bool bezierBendToolActive() const;
  bool sculptToolActive() const;
  bool arrayToolActive() const;
  void toggleVertexTool();
  void toggleEdgeTool();
  bool canToggleBevelTool() const;
  void toggleBevelTool();
  void applyBevelTool();
  void toggleFaceTool();
  bool canToggleInsetTool() const;
  void toggleInsetTool();
  void applyInsetTool();
  bool canToggleSpinTool() const;
  void toggleSpinTool();
  void applySpinTool(bool snapVerticesToInteger);
  bool canToggleBezierBendTool() const;
  void toggleBezierBendTool();
  void applyBezierBendTool();
  bool canToggleSculptTool() const;
  void toggleSculptTool();
  bool canToggleArrayTool() const;
  void toggleArrayTool();
  void applyArrayTool();
  bool subdivisionToolActive() const;
  bool canToggleSubdivisionTool() const;
  void toggleSubdivisionTool();
  void applySubdivisionTool();
  VertexTool& vertexTool();
  EdgeTool& edgeTool();
  FaceTool& faceTool();
  MapViewToolBox& mapViewToolBox();

  bool canMoveCameraToNextTracePoint() const;
  bool canMoveCameraToPreviousTracePoint() const;
  void moveCameraToNextTracePoint();
  void moveCameraToPreviousTracePoint();

  const gl::PerspectiveCamera& perspectiveCamera() const;

  bool canMaximizeCurrentView() const;
  bool currentViewMaximized() const;
  void toggleMaximizeCurrentView();

private:
  void connectObservers();
  void refreshViews(Tool& tool);

public: // implement MapView interface
  void installActivationTracker(MapViewActivationTracker& activationTracker) override;
  bool isCurrent() const override;
  MapViewBase* firstMapViewBase() override;
  bool canSelectTall() override;
  void selectTall() override;
  vm::vec3d pasteObjectsDelta(
    const vm::bbox3d& bounds, const vm::bbox3d& referenceBounds) const override;
  void reset2dCameras(const gl::Camera& masterCamera, bool animate) override;
  void focusCameraOnSelection(bool animate) override;
  void moveCameraToPosition(const vm::vec3f& position, bool animate) override;
  void moveCameraToCurrentTracePoint() override;
  bool cancelMouseDrag() override;
  void flashSelection() override;
  void refreshViews() override;

  deleteCopyAndMove(SwitchableMapViewContainer);
};

} // namespace ui
} // namespace tb
