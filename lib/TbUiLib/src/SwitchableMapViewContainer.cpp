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

#include "ui/SwitchableMapViewContainer.h"

#include <QGridLayout>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/Map.h"
#include "mdl/PointTrace.h"
#include "ui/FourPaneMapView.h"
#include "ui/Inspector.h"
#include "ui/MapDocument.h"
#include "ui/MapViewActivationTracker.h"
#include "ui/MapViewBar.h"
#include "ui/MapViewContainer.h"
#include "ui/MapViewLayout.h"
#include "ui/MapViewToolBox.h"
#include "ui/OnePaneMapView.h"
#include "ui/ThreePaneMapView.h"
#include "ui/TwoPaneMapView.h"

#include "kd/contracts.h"

namespace tb::ui
{
SwitchableMapViewContainer::SwitchableMapViewContainer(
  AppController& appController, MapDocument& document, QWidget* parent)
  : QWidget{parent}
  , m_appController{appController}
  , m_document{document}
  , m_mapViewBar{new MapViewBar(m_document)}
  , m_toolBox{std::make_unique<MapViewToolBox>(m_document, m_mapViewBar->toolBook())}
  , m_activationTracker{std::make_unique<MapViewActivationTracker>()}
{
  setObjectName("SwitchableMapViewContainer");
  switchToMapView(static_cast<MapViewLayout>(pref(Preferences::MapViewLayout)));
  connectObservers();
}

SwitchableMapViewContainer::~SwitchableMapViewContainer()
{
  // we must destroy our children before we destroy our resources because they might still
  // use them in their destructors
  m_activationTracker->clear();
  delete m_mapView;
}

void SwitchableMapViewContainer::connectTopWidgets(Inspector* inspector)
{
  inspector->connectTopWidgets(m_mapViewBar);
}

void SwitchableMapViewContainer::windowActivationStateChanged(const bool active)
{
  m_activationTracker->windowActivationChanged(active);
}

bool SwitchableMapViewContainer::active() const
{
  return m_activationTracker->active();
}

void SwitchableMapViewContainer::switchToMapView(const MapViewLayout viewId)
{
  m_activationTracker->clear();

  // NOTE: not all widgets are deleted so we can't use deleteChildWidgetsAndLayout()
  delete m_mapView;
  m_mapView = nullptr;

  delete layout();

  switch (viewId)
  {
  case MapViewLayout::OnePane:
    m_mapView = new OnePaneMapView{m_appController, m_document, *m_toolBox};
    break;
  case MapViewLayout::TwoPanes:
    m_mapView = new TwoPaneMapView{m_appController, m_document, *m_toolBox};
    break;
  case MapViewLayout::ThreePanes:
    m_mapView = new ThreePaneMapView{m_appController, m_document, *m_toolBox};
    break;
  case MapViewLayout::FourPanes:
    m_mapView = new FourPaneMapView{m_appController, m_document, *m_toolBox};
    break;
    switchDefault();
  }

  installActivationTracker(*m_activationTracker);

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  layout->addWidget(m_mapViewBar);
  layout->addWidget(m_mapView, 1);
  setLayout(layout);

  m_mapView->setFocus();
}

bool SwitchableMapViewContainer::anyModalToolActive() const
{
  return m_toolBox->anyModalToolActive();
}

void SwitchableMapViewContainer::deactivateCurrentTool()
{
  m_toolBox->deactivateCurrentTool();
}

bool SwitchableMapViewContainer::assembleBrushToolActive() const
{
  return m_toolBox->assembleBrushToolActive();
}

bool SwitchableMapViewContainer::canToggleAssembleBrushTool() const
{
  return true;
}

void SwitchableMapViewContainer::toggleAssembleBrushTool()
{
  contract_pre(canToggleAssembleBrushTool());

  m_toolBox->toggleAssembleBrushTool();
}

bool SwitchableMapViewContainer::clipToolActive() const
{
  return m_toolBox->clipToolActive();
}

bool SwitchableMapViewContainer::canToggleClipTool() const
{
  const auto& map = m_document.map();
  return clipToolActive() || map.selection().hasOnlyBrushes();
}

void SwitchableMapViewContainer::toggleClipTool()
{
  contract_pre(canToggleClipTool());

  m_toolBox->toggleClipTool();
}

ClipTool& SwitchableMapViewContainer::clipTool()
{
  return m_toolBox->clipTool();
}

bool SwitchableMapViewContainer::polygonCutToolActive() const
{
  return m_toolBox->polygonCutToolActive();
}

bool SwitchableMapViewContainer::canTogglePolygonCutTool() const
{
  const auto& map = m_document.map();
  return polygonCutToolActive() || polygonSpawnToolActive()
         || map.selection().hasOnlyBrushes();
}

void SwitchableMapViewContainer::togglePolygonCutTool()
{
  contract_pre(canTogglePolygonCutTool());

  m_toolBox->togglePolygonCutTool();
}

PolygonCutTool& SwitchableMapViewContainer::polygonCutTool()
{
  return m_toolBox->polygonCutTool();
}

bool SwitchableMapViewContainer::polygonSpawnToolActive() const
{
  return m_toolBox->polygonSpawnToolActive();
}

bool SwitchableMapViewContainer::canTogglePolygonSpawnTool() const
{
  const auto& map = m_document.map();
  return polygonCutToolActive() || polygonSpawnToolActive()
         || map.selection().hasOnlyBrushes();
}

void SwitchableMapViewContainer::togglePolygonSpawnTool()
{
  contract_pre(canTogglePolygonSpawnTool());

  m_toolBox->togglePolygonSpawnTool();
}

bool SwitchableMapViewContainer::rotateToolActive() const
{
  return m_toolBox->rotateToolActive();
}

bool SwitchableMapViewContainer::canToggleRotateTool() const
{
  const auto& map = m_document.map();
  return rotateToolActive() || map.selection().hasNodes();
}

void SwitchableMapViewContainer::toggleRotateTool()
{
  contract_pre(canToggleRotateTool());

  m_toolBox->toggleRotateTool();
}

bool SwitchableMapViewContainer::scaleToolActive() const
{
  return m_toolBox->scaleToolActive();
}

bool SwitchableMapViewContainer::shearToolActive() const
{
  return m_toolBox->shearToolActive();
}

bool SwitchableMapViewContainer::canToggleScaleTool() const
{
  const auto& map = m_document.map();
  return scaleToolActive() || map.selection().hasNodes();
}

void SwitchableMapViewContainer::toggleScaleTool()
{
  contract_pre(canToggleScaleTool());

  m_toolBox->toggleScaleTool();
}

bool SwitchableMapViewContainer::canToggleShearTool() const
{
  const auto& map = m_document.map();
  return shearToolActive() || map.selection().hasNodes();
}

void SwitchableMapViewContainer::toggleShearTool()
{
  contract_pre(canToggleShearTool());

  m_toolBox->toggleShearTool();
}

bool SwitchableMapViewContainer::symmetryToolActive() const
{
  return m_toolBox->symmetryToolActive();
}

bool SwitchableMapViewContainer::symmetryToolPanelSelected() const
{
  return m_toolBox->symmetryToolPanelSelected();
}

bool SwitchableMapViewContainer::symmetryModifierActive() const
{
  return m_toolBox->symmetryModifierActive();
}

bool SwitchableMapViewContainer::canToggleSymmetryTool() const
{
  return m_toolBox->canToggleSymmetryTool();
}

void SwitchableMapViewContainer::toggleSymmetryTool()
{
  contract_pre(canToggleSymmetryTool());

  m_toolBox->toggleSymmetryTool();
}

bool SwitchableMapViewContainer::canToggleVertexTools() const
{
  const auto& map = m_document.map();
  return vertexToolActive() || edgeToolActive() || faceToolActive()
         || map.selection().hasOnlyBrushes();
}

bool SwitchableMapViewContainer::anyVertexToolActive() const
{
  return vertexToolActive() || edgeToolActive() || faceToolActive();
}

bool SwitchableMapViewContainer::vertexToolActive() const
{
  return m_toolBox->vertexToolActive();
}

bool SwitchableMapViewContainer::edgeToolActive() const
{
  return m_toolBox->edgeToolActive();
}

bool SwitchableMapViewContainer::bevelToolActive() const
{
  return m_toolBox->bevelToolActive();
}

bool SwitchableMapViewContainer::faceToolActive() const
{
  return m_toolBox->faceToolActive();
}

bool SwitchableMapViewContainer::insetToolActive() const
{
  return m_toolBox->insetToolActive();
}

bool SwitchableMapViewContainer::spinToolActive() const
{
  return m_toolBox->spinToolActive();
}

bool SwitchableMapViewContainer::bezierBendToolActive() const
{
  return m_toolBox->bezierBendToolActive();
}

bool SwitchableMapViewContainer::sculptToolActive() const
{
  return m_toolBox->sculptToolActive();
}

bool SwitchableMapViewContainer::arrayToolActive() const
{
  return m_toolBox->arrayToolActive();
}

void SwitchableMapViewContainer::toggleVertexTool()
{
  contract_pre(canToggleVertexTools());

  m_toolBox->toggleVertexTool();
}

void SwitchableMapViewContainer::toggleEdgeTool()
{
  contract_pre(canToggleVertexTools());

  m_toolBox->toggleEdgeTool();
}

bool SwitchableMapViewContainer::canToggleBevelTool() const
{
  return m_toolBox->canToggleBevelTool();
}

void SwitchableMapViewContainer::toggleBevelTool()
{
  contract_pre(canToggleBevelTool());

  m_toolBox->toggleBevelTool();
}

void SwitchableMapViewContainer::applyBevelTool()
{
  contract_pre(bevelToolActive());

  m_toolBox->applyBevelTool();
}

void SwitchableMapViewContainer::toggleFaceTool()
{
  contract_pre(canToggleVertexTools());

  m_toolBox->toggleFaceTool();
}

bool SwitchableMapViewContainer::canToggleInsetTool() const
{
  return m_toolBox->canToggleInsetTool();
}

void SwitchableMapViewContainer::toggleInsetTool()
{
  contract_pre(canToggleInsetTool());

  m_toolBox->toggleInsetTool();
}

void SwitchableMapViewContainer::applyInsetTool()
{
  contract_pre(insetToolActive());

  m_toolBox->applyInsetTool();
}

bool SwitchableMapViewContainer::canToggleSpinTool() const
{
  return m_toolBox->canToggleSpinTool();
}

void SwitchableMapViewContainer::toggleSpinTool()
{
  contract_pre(canToggleSpinTool());

  m_toolBox->toggleSpinTool();
}

void SwitchableMapViewContainer::applySpinTool(const bool snapVerticesToInteger)
{
  contract_pre(spinToolActive());

  m_toolBox->applySpinTool(snapVerticesToInteger);
}

bool SwitchableMapViewContainer::canToggleBezierBendTool() const
{
  return m_toolBox->canToggleBezierBendTool();
}

void SwitchableMapViewContainer::toggleBezierBendTool()
{
  contract_pre(canToggleBezierBendTool());

  m_toolBox->toggleBezierBendTool();
}

void SwitchableMapViewContainer::applyBezierBendTool()
{
  contract_pre(bezierBendToolActive());

  m_toolBox->applyBezierBendTool();
}

bool SwitchableMapViewContainer::canToggleSculptTool() const
{
  return m_toolBox->canToggleSculptTool();
}

void SwitchableMapViewContainer::toggleSculptTool()
{
  contract_pre(canToggleSculptTool());

  m_toolBox->toggleSculptTool();
}

bool SwitchableMapViewContainer::canToggleArrayTool() const
{
  return m_toolBox->canToggleArrayTool();
}

void SwitchableMapViewContainer::toggleArrayTool()
{
  contract_pre(canToggleArrayTool());

  m_toolBox->toggleArrayTool();
}

void SwitchableMapViewContainer::applyArrayTool()
{
  contract_pre(arrayToolActive());

  m_toolBox->applyArrayTool();
}

bool SwitchableMapViewContainer::subdivisionToolActive() const
{
  return m_toolBox->subdivisionToolActive();
}

bool SwitchableMapViewContainer::canToggleSubdivisionTool() const
{
  return m_toolBox->canToggleSubdivisionTool();
}

void SwitchableMapViewContainer::toggleSubdivisionTool()
{
  contract_pre(canToggleSubdivisionTool());

  m_toolBox->toggleSubdivisionTool();
}

void SwitchableMapViewContainer::applySubdivisionTool()
{
  contract_pre(subdivisionToolActive());

  m_toolBox->applySubdivisionTool();
}

VertexTool& SwitchableMapViewContainer::vertexTool()
{
  return m_toolBox->vertexTool();
}

EdgeTool& SwitchableMapViewContainer::edgeTool()
{
  return m_toolBox->edgeTool();
}

FaceTool& SwitchableMapViewContainer::faceTool()
{
  return m_toolBox->faceTool();
}

MapViewToolBox& SwitchableMapViewContainer::mapViewToolBox()
{
  return *m_toolBox;
}

const MapViewToolBox& SwitchableMapViewContainer::toolBox() const
{
  return *m_toolBox;
}

MapViewToolBox& SwitchableMapViewContainer::toolBox()
{
  return KDL_CONST_OVERLOAD(toolBox());
}

bool SwitchableMapViewContainer::canMoveCameraToNextTracePoint() const
{
  if (const auto* pointTrace = m_document.pointTrace())
  {
    return pointTrace->hasNextPoint();
  }
  return false;
}

bool SwitchableMapViewContainer::canMoveCameraToPreviousTracePoint() const
{
  if (const auto* pointTrace = m_document.pointTrace())
  {
    return pointTrace->hasPreviousPoint();
  }
  return false;
}

void SwitchableMapViewContainer::moveCameraToNextTracePoint()
{
  contract_pre(m_document.isPointFileLoaded());

  if (auto* pointTrace = m_document.pointTrace())
  {
    pointTrace->advance();
    m_mapView->moveCameraToCurrentTracePoint();
  }
}

void SwitchableMapViewContainer::moveCameraToPreviousTracePoint()
{
  contract_pre(m_document.isPointFileLoaded());

  if (auto* pointTrace = m_document.pointTrace())
  {
    pointTrace->retreat();
    m_mapView->moveCameraToCurrentTracePoint();
  }
}

const gl::PerspectiveCamera& SwitchableMapViewContainer::perspectiveCamera() const
{
  return m_mapView->perspectiveCamera();
}

bool SwitchableMapViewContainer::canMaximizeCurrentView() const
{
  return m_mapView->canMaximizeCurrentView();
}

bool SwitchableMapViewContainer::currentViewMaximized() const
{
  return m_mapView->currentViewMaximized();
}

void SwitchableMapViewContainer::toggleMaximizeCurrentView()
{
  m_mapView->toggleMaximizeCurrentView();
}

void SwitchableMapViewContainer::connectObservers()
{
  m_notifierConnection +=
    m_toolBox->refreshViewsNotifier.connect([&](Tool& tool) { refreshViews(tool); });
}

void SwitchableMapViewContainer::refreshViews(Tool&)
{
  // NOTE: it doesn't work to call QWidget::update() here. The actual OpenGL view is a
  // QWindow embedded in the widget hierarchy with QWidget::createWindowContainer(), and
  // we need to call QWindow::requestUpdate().
  m_mapView->refreshViews();
}

void SwitchableMapViewContainer::installActivationTracker(
  MapViewActivationTracker& activationTracker)
{
  m_mapView->installActivationTracker(activationTracker);
}

bool SwitchableMapViewContainer::isCurrent() const
{
  return m_mapView->isCurrent();
}

MapViewBase* SwitchableMapViewContainer::firstMapViewBase()
{
  return m_mapView->firstMapViewBase();
}

bool SwitchableMapViewContainer::canSelectTall()
{
  return m_mapView->canSelectTall();
}

void SwitchableMapViewContainer::selectTall()
{
  m_mapView->selectTall();
}

vm::vec3d SwitchableMapViewContainer::pasteObjectsDelta(
  const vm::bbox3d& bounds, const vm::bbox3d& referenceBounds) const
{
  return m_mapView->pasteObjectsDelta(bounds, referenceBounds);
}

void SwitchableMapViewContainer::reset2dCameras(
  const gl::Camera& masterCamera, const bool animate)
{
  m_mapView->reset2dCameras(masterCamera, animate);
}

void SwitchableMapViewContainer::focusCameraOnSelection(const bool animate)
{
  m_mapView->focusCameraOnSelection(animate);
}

void SwitchableMapViewContainer::moveCameraToPosition(
  const vm::vec3f& position, const bool animate)
{
  m_mapView->moveCameraToPosition(position, animate);
}

void SwitchableMapViewContainer::moveCameraToCurrentTracePoint()
{
  m_mapView->moveCameraToCurrentTracePoint();
}

bool SwitchableMapViewContainer::cancelMouseDrag()
{
  return m_mapView->cancelMouseDrag();
}

void SwitchableMapViewContainer::flashSelection()
{
  m_mapView->flashSelection();
}

void SwitchableMapViewContainer::refreshViews()
{
  m_mapView->refreshViews();
}

} // namespace tb::ui
