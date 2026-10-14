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

#include "ui/MapViewToolBox.h"

#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "mdl/Map_Selection.h"
#include "ui/ArrayTool.h"
#include "ui/AssembleBrushTool.h"
#include "ui/BezierBendTool.h"
#include "ui/ClipTool.h"
#include "ui/ControlPointTool.h"
#include "ui/CreateEntityTool.h"
#include "ui/DrawShapeTool.h"
#include "ui/EdgeTool.h"
#include "ui/ExtrudeTool.h"
#include "ui/FaceTool.h"
#include "ui/MapDocument.h"
#include "ui/MoveObjectsTool.h"
#include "ui/PolygonCutTool.h"
#include "ui/PrefabTool.h"
#include "ui/RotateTool.h"
#include "ui/ScaleTool.h"
#include "ui/SculptTool.h"
#include "ui/ShearTool.h"
#include "ui/SweepTool.h"
#include "ui/SubdivisionTool.h"
#include "ui/SymmetryTool.h"
#include "ui/VertexTool.h"

#include "kd/contracts.h"

namespace tb::ui
{
MapViewToolBox::MapViewToolBox(MapDocument& document, QStackedLayout* bookCtrl)
  : m_document{document}
{
  createTools(bookCtrl);
  connectObservers();
}

MapViewToolBox::~MapViewToolBox() = default;

ClipTool& MapViewToolBox::clipTool()
{
  return *m_clipTool;
}

ArrayTool& MapViewToolBox::arrayTool()
{
  return *m_arrayTool;
}

BezierBendTool& MapViewToolBox::bezierBendTool()
{
  return *m_bezierBendTool;
}

PolygonCutTool& MapViewToolBox::polygonCutTool()
{
  return *m_polygonCutTool;
}

const AssembleBrushTool& MapViewToolBox::assembleBrushTool() const
{
  return *m_assembleBrushTool;
}

AssembleBrushTool& MapViewToolBox::assembleBrushTool()
{
  return KDL_CONST_OVERLOAD(assembleBrushTool());
}

const CreateEntityTool& MapViewToolBox::createEntityTool() const
{
  return *m_createEntityTool;
}

CreateEntityTool& MapViewToolBox::createEntityTool()
{
  return KDL_CONST_OVERLOAD(createEntityTool());
}

const DrawShapeTool& MapViewToolBox::drawShapeTool() const
{
  return *m_drawShapeTool;
}

PrefabTool& MapViewToolBox::prefabTool()
{
  return *m_prefabTool;
}

DrawShapeTool& MapViewToolBox::drawShapeTool()
{
  return KDL_CONST_OVERLOAD(drawShapeTool());
}

const MoveObjectsTool& MapViewToolBox::moveObjectsTool() const
{
  return *m_moveObjectsTool;
}

MoveObjectsTool& MapViewToolBox::moveObjectsTool()
{
  return KDL_CONST_OVERLOAD(moveObjectsTool());
}

const ExtrudeTool& MapViewToolBox::extrudeTool() const
{
  return *m_extrudeTool;
}

ExtrudeTool& MapViewToolBox::extrudeTool()
{
  return KDL_CONST_OVERLOAD(extrudeTool());
}

const RotateTool& MapViewToolBox::rotateTool() const
{
  return *m_rotateTool;
}

RotateTool& MapViewToolBox::rotateTool()
{
  return KDL_CONST_OVERLOAD(rotateTool());
}

const SweepTool& MapViewToolBox::sweepTool() const
{
  return *m_sweepTool;
}

SweepTool& MapViewToolBox::sweepTool()
{
  return KDL_CONST_OVERLOAD(sweepTool());
}

const ScaleTool& MapViewToolBox::scaleTool() const
{
  return *m_scaleTool;
}

SculptTool& MapViewToolBox::sculptTool()
{
  return *m_sculptTool;
}

ScaleTool& MapViewToolBox::scaleTool()
{
  return KDL_CONST_OVERLOAD(scaleTool());
}

const ShearTool& MapViewToolBox::shearTool() const
{
  return *m_shearTool;
}

SubdivisionTool& MapViewToolBox::subdivisionTool()
{
  return *m_subdivisionTool;
}

SymmetryTool& MapViewToolBox::symmetryTool()
{
  return *m_symmetryTool;
}

ShearTool& MapViewToolBox::shearTool()
{
  return KDL_CONST_OVERLOAD(shearTool());
}

const VertexTool& MapViewToolBox::vertexTool() const
{
  return *m_vertexTool;
}

VertexTool& MapViewToolBox::vertexTool()
{
  return KDL_CONST_OVERLOAD(vertexTool());
}

const EdgeTool& MapViewToolBox::edgeTool() const
{
  return *m_edgeTool;
}

EdgeTool& MapViewToolBox::edgeTool()
{
  return KDL_CONST_OVERLOAD(edgeTool());
}

const FaceTool& MapViewToolBox::faceTool() const
{
  return *m_faceTool;
}

FaceTool& MapViewToolBox::faceTool()
{
  return KDL_CONST_OVERLOAD(faceTool());
}

const ControlPointTool& MapViewToolBox::controlPointTool() const
{
  return *m_controlPointTool;
}

ControlPointTool& MapViewToolBox::controlPointTool()
{
  return KDL_CONST_OVERLOAD(controlPointTool());
}

bool MapViewToolBox::canToggleAssembleBrushTool() const
{
  return true;
}

void MapViewToolBox::toggleAssembleBrushTool()
{
  if (canToggleAssembleBrushTool())
  {
    toggleTool(assembleBrushTool());
  }
}

bool MapViewToolBox::assembleBrushToolActive() const
{
  return m_assembleBrushTool->active();
}

void MapViewToolBox::performAssembleBrush()
{
  contract_pre(assembleBrushToolActive());

  m_assembleBrushTool->createBrushes();
}

bool MapViewToolBox::canToggleClipTool() const
{
  const auto& map = m_document.map();
  return clipToolActive() || map.selection().hasOnlyBrushes();
}

void MapViewToolBox::toggleClipTool()
{
  if (canToggleClipTool())
  {
    toggleTool(clipTool());
  }
}

bool MapViewToolBox::clipToolActive() const
{
  return m_clipTool->active();
}

void MapViewToolBox::toggleClipSide()
{
  contract_pre(clipToolActive());

  m_clipTool->toggleSide();
}

void MapViewToolBox::performClip()
{
  contract_pre(clipToolActive());

  m_clipTool->performClip();
}

void MapViewToolBox::removeLastClipPoint()
{
  contract_pre(clipToolActive());

  m_clipTool->removeLastPoint();
}

bool MapViewToolBox::canToggleRotateTool() const
{
  const auto& map = m_document.map();
  return rotateToolActive() || map.selection().hasNodes();
}

void MapViewToolBox::togglePolygonCutTool()
{
  if (
    m_polygonCutTool->active()
    && m_polygonCutTool->toolMode() != PolygonCutTool::ToolMode::Cut)
  {
    toggleTool(polygonCutTool());
    m_polygonCutTool->setToolMode(PolygonCutTool::ToolMode::Cut);
    toggleTool(polygonCutTool());
    return;
  }

  m_polygonCutTool->setToolMode(PolygonCutTool::ToolMode::Cut);
  toggleTool(polygonCutTool());
}

bool MapViewToolBox::polygonCutToolActive() const
{
  return m_polygonCutTool->active()
         && m_polygonCutTool->toolMode() == PolygonCutTool::ToolMode::Cut;
}

void MapViewToolBox::performPolygonCut()
{
  contract_pre(polygonCutToolActive());

  m_polygonCutTool->performCut();
}

void MapViewToolBox::togglePolygonSpawnTool()
{
  if (
    m_polygonCutTool->active()
    && m_polygonCutTool->toolMode() != PolygonCutTool::ToolMode::SpawnBrush)
  {
    toggleTool(polygonCutTool());
    m_polygonCutTool->setToolMode(PolygonCutTool::ToolMode::SpawnBrush);
    toggleTool(polygonCutTool());
    return;
  }

  m_polygonCutTool->setToolMode(PolygonCutTool::ToolMode::SpawnBrush);
  toggleTool(polygonCutTool());
}

bool MapViewToolBox::polygonSpawnToolActive() const
{
  return m_polygonCutTool->active()
         && m_polygonCutTool->toolMode() == PolygonCutTool::ToolMode::SpawnBrush;
}

void MapViewToolBox::performPolygonSpawn()
{
  contract_pre(polygonSpawnToolActive());

  m_polygonCutTool->performSpawnBrush();
}

bool MapViewToolBox::undoActiveToolStep()
{
  if (anyGeneratorToolActive())
  {
    // A generator owns its source brushes until Apply or Cancel.
    return true;
  }

  if (m_polygonCutTool->active() && m_polygonCutTool->hasPoints())
  {
    if (m_polygonCutTool->removeLastPoint())
    {
      if (!m_polygonCutTool->hasPoints())
      {
        m_polygonCutTool->reset();
      }
      return true;
    }
  }

  if (m_faceTool->spinToolActive())
  {
    if (m_faceTool->undoSpinStep())
    {
      updateEditorContext();
      updateToolPage();
    }
    return true;
  }

  if (m_bezierBendTool->active())
  {
    if (m_bezierBendTool->undoBendStep())
    {
      updateEditorContext();
      updateToolPage();
    }
    return true;
  }

  if (m_arrayTool->active())
  {
    if (m_arrayTool->undoStep())
    {
      updateEditorContext();
      updateToolPage();
    }
    return true;
  }

  if (m_subdivisionTool->active())
  {
    if (m_subdivisionTool->undoStep())
    {
      updateEditorContext();
      updateToolPage();
    }
    return true;
  }

  return false;
}

void MapViewToolBox::toggleRotateTool()
{
  if (canToggleRotateTool())
  {
    toggleTool(rotateTool());
  }
}

bool MapViewToolBox::rotateToolActive() const
{
  return m_rotateTool->active();
}

double MapViewToolBox::rotateToolAngle() const
{
  contract_pre(rotateToolActive());

  return m_rotateTool->angle();
}

vm::vec3d MapViewToolBox::rotateToolCenter() const
{
  contract_pre(rotateToolActive());

  return m_rotateTool->rotationCenter();
}

void MapViewToolBox::moveRotationCenter(const vm::vec3d& delta)
{
  contract_pre(rotateToolActive());

  const vm::vec3d center = m_rotateTool->rotationCenter();
  m_rotateTool->setRotationCenter(center + delta);
}

bool MapViewToolBox::canToggleSweepTool() const
{
  const auto& map = m_document.map();
  return sweepToolActive() || map.selection().hasBrushFaces();
}

void MapViewToolBox::toggleSweepTool()
{
  if (canToggleSweepTool())
  {
    toggleTool(sweepTool());
  }
}

bool MapViewToolBox::sweepToolActive() const
{
  return m_sweepTool->active();
}

void MapViewToolBox::moveSweepCenter(const vm::vec3d& delta)
{
  contract_pre(sweepToolActive());

  const auto center = m_sweepTool->destinationCenter();
  m_sweepTool->setDestinationCenter(center + delta);
}

void MapViewToolBox::rotateSweepCap(const vm::vec3d& axis, const double angle)
{
  contract_pre(sweepToolActive());

  m_sweepTool->rotateDestinationCap(axis, angle);
}

void MapViewToolBox::scaleSweepCap(const double distance)
{
  contract_pre(sweepToolActive());

  m_sweepTool->moveScaleHandle(distance);
}

void MapViewToolBox::performSweep()
{
  contract_pre(sweepToolActive());

  m_sweepTool->commitSweep();
  deactivateCurrentTool();
}

bool MapViewToolBox::canToggleScaleTool() const
{
  const auto& map = m_document.map();
  return scaleToolActive() || map.selection().hasNodes();
}

void MapViewToolBox::toggleScaleTool()
{
  if (canToggleScaleTool())
  {
    toggleTool(scaleTool());
  }
}

bool MapViewToolBox::scaleToolActive() const
{
  return m_scaleTool->active();
}

bool MapViewToolBox::canToggleShearTool() const
{
  const auto& map = m_document.map();
  return shearToolActive() || map.selection().hasNodes();
}

void MapViewToolBox::toggleSculptTool()
{
  toggleTool(sculptTool());
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleGeneratorTool(const GeneratorTool::Kind kind)
{
  toggleTool(*m_generatorTools[static_cast<size_t>(kind)]);
}

bool MapViewToolBox::canToggleGeneratorTool(const GeneratorTool::Kind kind) const
{
  return m_generatorTools[static_cast<size_t>(kind)]->canActivate();
}

bool MapViewToolBox::generatorToolActive(const GeneratorTool::Kind kind) const
{
  return m_generatorTools[static_cast<size_t>(kind)]->active();
}

bool MapViewToolBox::anyGeneratorToolActive() const
{
  return std::ranges::any_of(
    m_generatorTools, [](const auto& tool) { return tool->active(); });
}

bool MapViewToolBox::parameterToolActive() const
{
  return anyGeneratorToolActive() || sculptToolActive() || arrayToolActive()
         || subdivisionToolActive() || bezierBendToolActive() || bevelToolActive()
         || insetToolActive() || spinToolActive() || polygonCutToolActive()
         || polygonSpawnToolActive();
}

bool MapViewToolBox::canToggleSculptTool() const
{
  return true;
}

bool MapViewToolBox::sculptToolActive() const
{
  return m_sculptTool->active();
}

void MapViewToolBox::toggleArrayTool()
{
  toggleTool(arrayTool());
  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleArrayTool() const
{
  return m_arrayTool->active() || m_arrayTool->canAttachToSelection();
}

bool MapViewToolBox::arrayToolActive() const
{
  return m_arrayTool->active();
}

void MapViewToolBox::applyArrayTool()
{
  contract_pre(arrayToolActive());

  m_arrayTool->apply();
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleSubdivisionTool()
{
  toggleTool(subdivisionTool());
  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleSubdivisionTool() const
{
  return m_subdivisionTool->active() || m_subdivisionTool->canAttachToSelection();
}

bool MapViewToolBox::subdivisionToolActive() const
{
  return m_subdivisionTool->active();
}

void MapViewToolBox::applySubdivisionTool()
{
  contract_pre(subdivisionToolActive());

  m_subdivisionTool->apply();
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleShearTool()
{
  if (canToggleShearTool())
  {
    toggleTool(shearTool());
  }
}

bool MapViewToolBox::shearToolActive() const
{
  return m_shearTool->active();
}

bool MapViewToolBox::canToggleAnyVertexTool() const
{
  const auto& map = m_document.map();
  return vertexToolActive() || edgeToolActive() || faceToolActive()
         || map.selection().hasOnlyBrushes();
}

void MapViewToolBox::toggleSymmetryTool()
{
  if (m_symmetryToolPanelSelected && !m_symmetryTool->sourceSelected())
  {
    m_symmetryToolPanelSelected = false;
    if (m_symmetryTool->active() && !m_symmetryTool->modifierActive())
    {
      m_symmetryTool->deactivate();
      toolDeactivatedNotifier(symmetryTool());
    }
  }
  else
  {
    m_symmetryToolPanelSelected = true;
    if (!m_symmetryTool->active() && m_symmetryTool->activate())
    {
      toolActivatedNotifier(symmetryTool());
    }
  }

  toolHandleSelectionChangedNotifier(symmetryTool());
  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleSymmetryTool() const
{
  if (m_symmetryTool->hasSource())
  {
    return m_symmetryTool->sourceSelected();
  }

  return m_symmetryTool->canAttachToSelection();
}

bool MapViewToolBox::symmetryToolActive() const
{
  return m_symmetryTool->active();
}

bool MapViewToolBox::symmetryToolPanelSelected() const
{
  return m_symmetryToolPanelSelected || m_symmetryTool->sourceSelected();
}

bool MapViewToolBox::symmetryModifierActive() const
{
  return m_symmetryTool->modifierActive();
}

bool MapViewToolBox::symmetryRelinkInProgress() const
{
  return m_symmetryTool->relinkInProgress();
}

bool MapViewToolBox::anyVertexToolActive() const
{
  return vertexToolActive() || edgeToolActive() || faceToolActive();
}

bool MapViewToolBox::anyNodeHandleToolActive() const
{
  return anyVertexToolActive() || controlPointToolActive();
}

void MapViewToolBox::toggleVertexTool()
{
  if (canToggleAnyVertexTool())
  {
    toggleTool(vertexTool());
  }
}

bool MapViewToolBox::vertexToolActive() const
{
  return m_vertexTool->active();
}

void MapViewToolBox::toggleEdgeTool()
{
  if (canToggleAnyVertexTool())
  {
    toggleTool(edgeTool());
  }
}

bool MapViewToolBox::edgeToolActive() const
{
  return m_edgeTool->active();
}

void MapViewToolBox::toggleBevelTool()
{
  if (m_edgeTool->bevelToolActive())
  {
    m_edgeTool->cancelBevelTool();
    if (m_edgeTool->active())
    {
      toggleTool(edgeTool());
    }
  }
  else
  {
    if (!m_edgeTool->active())
    {
      toggleTool(edgeTool());
    }
    m_edgeTool->activateBevelTool();
  }

  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleBevelTool() const
{
  return m_edgeTool->bevelToolActive() || m_document.map().selection().hasOnlyBrushes();
}

bool MapViewToolBox::bevelToolActive() const
{
  return m_edgeTool->bevelToolActive();
}

void MapViewToolBox::applyBevelTool()
{
  contract_pre(bevelToolActive());

  m_edgeTool->applyBevelTool();
  if (m_edgeTool->active())
  {
    toggleTool(edgeTool());
  }
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleFaceTool()
{
  if (canToggleAnyVertexTool())
  {
    toggleTool(faceTool());
  }
}

bool MapViewToolBox::faceToolActive() const
{
  return m_faceTool->active();
}

bool MapViewToolBox::canToggleControlPointTool() const
{
  const auto& map = m_document.map();
  return controlPointToolActive() || map.selection().hasOnlyPatches();
}

void MapViewToolBox::toggleInsetTool()
{
  if (m_faceTool->insetToolActive())
  {
    m_faceTool->cancelInsetTool();
    if (m_faceTool->active())
    {
      toggleTool(faceTool());
    }
  }
  else
  {
    if (!m_faceTool->active())
    {
      toggleTool(faceTool());
    }
    m_faceTool->activateInsetTool();
  }

  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleInsetTool() const
{
  return m_faceTool->insetToolActive() || m_document.map().selection().hasOnlyBrushes();
}

bool MapViewToolBox::insetToolActive() const
{
  return m_faceTool->insetToolActive();
}

void MapViewToolBox::applyInsetTool()
{
  contract_pre(insetToolActive());

  m_faceTool->applyInsetTool();
  if (m_faceTool->active())
  {
    toggleTool(faceTool());
  }
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleSpinTool()
{
  if (m_faceTool->spinToolActive())
  {
    m_faceTool->cancelSpinTool();
    if (m_faceTool->active())
    {
      toggleTool(faceTool());
    }
  }
  else
  {
    if (!m_faceTool->active())
    {
      toggleTool(faceTool());
    }
    m_faceTool->activateSpinTool();
  }

  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleSpinTool() const
{
  return m_faceTool->spinToolActive() || m_document.map().selection().hasOnlyBrushes();
}

bool MapViewToolBox::spinToolActive() const
{
  return m_faceTool->spinToolActive();
}

void MapViewToolBox::applySpinTool(const bool snapVerticesToInteger)
{
  contract_pre(spinToolActive());

  m_faceTool->applySpinTool(snapVerticesToInteger);
  if (m_faceTool->active())
  {
    toggleTool(faceTool());
  }
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleBezierBendTool()
{
  toggleTool(bezierBendTool());
  updateEditorContext();
  updateToolPage();
}

bool MapViewToolBox::canToggleBezierBendTool() const
{
  return m_bezierBendTool->active() || m_bezierBendTool->canAttachToSelection();
}

bool MapViewToolBox::bezierBendToolActive() const
{
  return m_bezierBendTool->active();
}

void MapViewToolBox::applyBezierBendTool()
{
  contract_pre(bezierBendToolActive());

  m_bezierBendTool->applyBendTool();
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toggleControlPointTool()
{
  if (canToggleControlPointTool())
  {
    toggleTool(controlPointTool());
  }
}

bool MapViewToolBox::controlPointToolActive() const
{
  return m_controlPointTool->active();
}

bool MapViewToolBox::anyModalToolActive() const
{
  return rotateToolActive() || sweepToolActive() || scaleToolActive() || shearToolActive()
         || anyNodeHandleToolActive() || parameterToolActive();
}

void MapViewToolBox::moveNodeHandles(const vm::vec3d& delta)
{
  contract_pre(anyNodeHandleToolActive());

  if (vertexToolActive())
  {
    vertexTool().moveSelection(delta);
  }
  else if (edgeToolActive())
  {
    edgeTool().moveSelection(delta);
  }
  else if (faceToolActive())
  {
    faceTool().moveSelection(delta);
  }
  else if (controlPointToolActive())
  {
    controlPointTool().moveSelection(delta);
  }
}

void MapViewToolBox::createTools(QStackedLayout* bookCtrl)
{
  m_clipTool = std::make_unique<ClipTool>(m_document);
  m_arrayTool = std::make_unique<ArrayTool>(m_document);
  m_bezierBendTool = std::make_unique<BezierBendTool>(m_document);
  m_polygonCutTool = std::make_unique<PolygonCutTool>(m_document);
  m_assembleBrushTool = std::make_unique<AssembleBrushTool>(m_document);
  m_createEntityTool = std::make_unique<CreateEntityTool>(m_document);
  m_drawShapeTool = std::make_unique<DrawShapeTool>(m_document);
  m_prefabTool = std::make_unique<PrefabTool>(m_document);
  m_moveObjectsTool = std::make_unique<MoveObjectsTool>(m_document);
  m_extrudeTool = std::make_unique<ExtrudeTool>(m_document);
  m_rotateTool = std::make_unique<RotateTool>(m_document);
  m_sweepTool = std::make_unique<SweepTool>(m_document);
  m_scaleTool = std::make_unique<ScaleTool>(m_document);
  m_sculptTool = std::make_unique<SculptTool>(m_document);
  m_shearTool = std::make_unique<ShearTool>(m_document);
  m_subdivisionTool = std::make_unique<SubdivisionTool>(m_document);
  m_symmetryTool = std::make_unique<SymmetryTool>(m_document);
  m_vertexTool = std::make_unique<VertexTool>(m_document);
  m_edgeTool = std::make_unique<EdgeTool>(m_document);
  m_faceTool = std::make_unique<FaceTool>(m_document);
  m_controlPointTool = std::make_unique<ControlPointTool>(m_document);
  for (size_t i = 0; i < m_generatorTools.size(); ++i)
  {
    m_generatorTools[i] =
      std::make_unique<GeneratorTool>(m_document, static_cast<GeneratorTool::Kind>(i));
  }

  addExclusiveToolGroup(
    assembleBrushTool(),
    *m_generatorTools[0],
    *m_generatorTools[1],
    *m_generatorTools[2],
    rotateTool(),
    sweepTool(),
    scaleTool(),
    sculptTool(),
    arrayTool(),
    subdivisionTool(),
    shearTool(),
    controlPointTool(),
    edgeTool(),
    faceTool(),
    bezierBendTool(),
    clipTool(),
    polygonCutTool());

  addExclusiveToolGroup(
    assembleBrushTool(),
    vertexTool(),
    *m_generatorTools[0],
    *m_generatorTools[1],
    *m_generatorTools[2],
    edgeTool(),
    faceTool(),
    controlPointTool(),
    sculptTool(),
    arrayTool(),
    subdivisionTool(),
    bezierBendTool(),
    clipTool(),
    polygonCutTool());

  suppressWhileActive(
    assembleBrushTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(rotateTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(sweepTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(scaleTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(sculptTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(arrayTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(
    subdivisionTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(shearTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(vertexTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(edgeTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(faceTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(
    controlPointTool(),
    bezierBendTool(),
    moveObjectsTool(),
    extrudeTool(),
    drawShapeTool());
  suppressWhileActive(clipTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());
  suppressWhileActive(
    polygonCutTool(), moveObjectsTool(), extrudeTool(), drawShapeTool());

  registerTool(moveObjectsTool(), bookCtrl);
  registerTool(rotateTool(), bookCtrl);
  registerTool(sweepTool(), bookCtrl);
  registerTool(scaleTool(), bookCtrl);
  registerTool(sculptTool(), bookCtrl);
  registerTool(arrayTool(), bookCtrl);
  registerTool(subdivisionTool(), bookCtrl);
  registerTool(shearTool(), bookCtrl);
  registerTool(symmetryTool(), bookCtrl);
  registerTool(bezierBendTool(), bookCtrl);
  registerTool(extrudeTool(), bookCtrl);
  registerTool(assembleBrushTool(), bookCtrl);
  registerTool(clipTool(), bookCtrl);
  registerTool(polygonCutTool(), bookCtrl);
  registerTool(vertexTool(), bookCtrl);
  registerTool(edgeTool(), bookCtrl);
  registerTool(faceTool(), bookCtrl);
  registerTool(controlPointTool(), bookCtrl);
  registerTool(createEntityTool(), bookCtrl);
  registerTool(prefabTool(), bookCtrl);
  registerTool(drawShapeTool(), bookCtrl);
  for (const auto& tool : m_generatorTools)
  {
    suppressWhileActive(*tool, moveObjectsTool(), extrudeTool(), drawShapeTool());
    registerTool(*tool, bookCtrl);
  }

  updateToolPage();
}

void MapViewToolBox::registerTool(Tool& tool, QStackedLayout* bookCtrl)
{
  tool.createPage(bookCtrl);
  addTool(tool);
}

void MapViewToolBox::connectObservers()
{
  for (const auto& tool : m_generatorTools)
  {
    m_notifierConnection +=
      tool->didRequestDeactivateNotifier.connect([this](auto& generator) {
        if (generator.active())
        {
          toggleTool(generator);
        }
      });
  }
  m_notifierConnection +=
    toolActivatedNotifier.connect(this, &MapViewToolBox::toolActivated);
  m_notifierConnection +=
    toolDeactivatedNotifier.connect(this, &MapViewToolBox::toolDeactivated);

  m_notifierConnection += m_document.documentWasLoadedNotifier.connect(
    this, &MapViewToolBox::documentWasLoaded);

  m_notifierConnection += m_document.selectionDidChangeNotifier.connect(
    this, &MapViewToolBox::selectionDidChange);

  m_notifierConnection +=
    m_symmetryTool->symmetryDidChangeNotifier.connect([this](auto&) {
      refreshViewsNotifier(symmetryTool());
      toolHandleSelectionChangedNotifier(symmetryTool());
      updateEditorContext();
      updateToolPage();
    });
  m_notifierConnection +=
    m_symmetryTool->symmetryDidRequestDeactivateNotifier.connect([this](auto&) {
      m_symmetryToolPanelSelected = false;
      if (m_symmetryTool->active())
      {
        m_symmetryTool->deactivate();
        toolDeactivatedNotifier(symmetryTool());
      }
      toolHandleSelectionChangedNotifier(symmetryTool());
      updateEditorContext();
      updateToolPage();
    });
  m_notifierConnection +=
    m_bezierBendTool->bendDidRequestDeactivateNotifier.connect([this](auto&) {
      if (m_bezierBendTool->active())
      {
        toggleTool(bezierBendTool());
      }
      toolHandleSelectionChangedNotifier(bezierBendTool());
      updateEditorContext();
      updateToolPage();
    });
  m_notifierConnection +=
    m_arrayTool->arrayDidRequestDeactivateNotifier.connect([this](auto&) {
      if (m_arrayTool->active())
      {
        toggleTool(arrayTool());
      }
      toolHandleSelectionChangedNotifier(arrayTool());
      updateEditorContext();
      updateToolPage();
    });
  m_notifierConnection +=
    m_subdivisionTool->subdivisionDidRequestDeactivateNotifier.connect([this](auto&) {
      if (m_subdivisionTool->active())
      {
        toggleTool(subdivisionTool());
      }
      toolHandleSelectionChangedNotifier(subdivisionTool());
      updateEditorContext();
      updateToolPage();
    });
}

void MapViewToolBox::toolActivated(Tool& tool)
{
  if (&tool != m_symmetryTool.get() && !m_symmetryTool->sourceSelected())
  {
    m_symmetryToolPanelSelected = false;
  }
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::toolDeactivated(Tool&)
{
  updateEditorContext();
  updateToolPage();
}

void MapViewToolBox::updateEditorContext()
{
  auto& editorContext = m_document.map().editorContext();
  editorContext.setBlockSelection(
    assembleBrushToolActive() || polygonCutToolActive() || polygonSpawnToolActive()
    || sculptToolActive() || arrayToolActive() || subdivisionToolActive()
    || anyGeneratorToolActive()
    || (bezierBendToolActive() && bezierBendTool().bendActive()));
}

void MapViewToolBox::documentWasLoaded()
{
  m_symmetryToolPanelSelected = false;
  deactivateAllTools();
}

void MapViewToolBox::selectionDidChange(const mdl::SelectionChange&)
{
  if (!m_symmetryTool->sourceSelected())
  {
    m_symmetryToolPanelSelected = false;
  }
  toolHandleSelectionChangedNotifier(symmetryTool());
  updateToolPage();
}

void MapViewToolBox::updateToolPage()
{
  for (const auto& tool : m_generatorTools)
  {
    if (tool->active())
    {
      tool->showPage();
      return;
    }
  }

  if (symmetryToolPanelSelected())
  {
    symmetryTool().showPage();
  }
  else if (rotateToolActive())
  {
    rotateTool().showPage();
  }
  else if (sweepToolActive())
  {
    sweepTool().showPage();
  }
  else if (scaleToolActive())
  {
    scaleTool().showPage();
  }
  else if (shearToolActive())
  {
    shearTool().showPage();
  }
  else if (bezierBendToolActive())
  {
    bezierBendTool().showPage();
  }
  else if (sculptToolActive())
  {
    sculptTool().showPage();
  }
  else if (arrayToolActive())
  {
    arrayTool().showPage();
  }
  else if (subdivisionToolActive())
  {
    subdivisionTool().showPage();
  }
  else if (vertexToolActive())
  {
    vertexTool().showPage();
  }
  else if (edgeToolActive())
  {
    edgeTool().showPage();
  }
  else if (faceToolActive())
  {
    faceTool().showPage();
  }
  else if (controlPointToolActive())
  {
    controlPointTool().showPage();
  }
  else if (clipToolActive())
  {
    clipTool().showPage();
  }
  else if (polygonCutToolActive() || polygonSpawnToolActive())
  {
    polygonCutTool().showPage();
  }
  else
  {
    drawShapeTool().showPage();
  }
}

} // namespace tb::ui
