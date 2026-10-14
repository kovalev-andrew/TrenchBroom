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

#include "ui/MapViewBase.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QRect>
#include <QResizeEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QString>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtGlobal>

#include "Logger.h"
#include "Notifier.h"
#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Camera.h"
#include "gl/FontDescriptor.h"
#include "gl/FontManager.h"
#include "gl/GlInterface.h"
#include "gl/GlManager.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionGroup.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Map_World.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PointTrace.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/WorldNode.h"
#include "render/Compass.h"
#include "render/MapRenderer.h"
#include "render/PrimitiveRenderer.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "render/RenderService.h"
#include "ui/ActionExecutionContext.h"
#include "ui/ActionManager.h"
#include "ui/Animation.h"
#include "ui/AppController.h"
#include "ui/EnableDisableTagCallback.h"
#include "ui/FlashSelectionAnimation.h"
#include "ui/MapDocument.h"
#include "ui/MapViewActivationTracker.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/SelectionTool.h"
#include "ui/SignalDelayer.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/vector_utils.h"

#include "vm/polygon.h"
#include "vm/segment.h"
#include "vm/util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

namespace tb::ui
{
namespace
{

enum class TransformHudTarget
{
  Vertex,
  Edge,
  FaceHandle,
  BrushFace,
  Brush,
};

constexpr auto MaxPreviewQuakeLights = size_t{32};

float renderTimeSeconds()
{
  using Clock = std::chrono::steady_clock;
  static const auto StartTime = Clock::now();
  return std::chrono::duration<float>{Clock::now() - StartTime}.count();
}

struct PreviewLightCandidate
{
  render::PreviewLight light;
  double distanceToCameraSquared = 0.0;
};

std::vector<float> parseFloatList(const std::string& value)
{
  auto result = std::vector<float>{};
  auto stream = std::istringstream{value};
  auto number = float{};
  while (stream >> number)
  {
    result.push_back(number);
  }
  return result;
}

float parseLightValue(const std::string* value, const float defaultValue)
{
  if (value == nullptr)
  {
    return defaultValue;
  }

  const auto values = parseFloatList(*value);
  return values.empty() ? defaultValue : values.front();
}

vm::vec3f clampLightColor(const vm::vec3f& color)
{
  return vm::vec3f{
    std::clamp(color.x(), 0.0f, 2.0f),
    std::clamp(color.y(), 0.0f, 2.0f),
    std::clamp(color.z(), 0.0f, 2.0f)};
}

vm::vec3f normalizeLightColor(vm::vec3f color)
{
  if (std::max({color.x(), color.y(), color.z()}) > 1.5f)
  {
    color = color / 255.0f;
  }
  return clampLightColor(color);
}

vm::vec3f parseLightColor(const std::string* value, const vm::vec3f& defaultValue)
{
  if (value == nullptr)
  {
    return defaultValue;
  }

  const auto values = parseFloatList(*value);
  if (values.size() < 3u)
  {
    return defaultValue;
  }

  return normalizeLightColor(vm::vec3f{values[0], values[1], values[2]});
}

bool isLightEntity(const mdl::Entity& entity)
{
  const auto& classname = entity.classname();
  return classname == "light" || classname.rfind("light_", 0) == 0;
}

bool isIntermissionPreviewEntity(const mdl::Entity& entity)
{
  return entity.classname() == "info_intermission";
}

vm::vec3f intermissionPreviewDirection(const mdl::Entity& entity)
{
  return vm::vec3f{vm::normalize(entity.rotation() * vm::vec3d{1, 0, 0})};
}

vm::vec3f intermissionPreviewUp(const mdl::Entity& entity)
{
  auto direction = intermissionPreviewDirection(entity);
  auto up = vm::vec3f{vm::normalize(entity.rotation() * vm::vec3d{0, 0, 1})};
  if (vm::squared_length(vm::cross(direction, up)) <= 0.000001f)
  {
    up = std::abs(direction.z()) < 0.95f ? vm::vec3f{0, 0, 1} : vm::vec3f{0, 1, 0};
  }
  return up;
}

bool isDoorButtonPreviewEntity(const mdl::Entity& entity)
{
  const auto& classname = entity.classname();
  return classname.find("door") != std::string::npos
         || classname.find("button") != std::string::npos
         || classname.find("plat") != std::string::npos
         || classname.find("lift") != std::string::npos
         || classname.find("elevator") != std::string::npos || classname == "func_train"
         || classname.find("train") != std::string::npos;
}

bool isTrainPreviewEntity(const mdl::Entity& entity)
{
  const auto& classname = entity.classname();
  return classname == "func_train" || classname.find("train") != std::string::npos;
}

bool isLiftPreviewEntity(const mdl::Entity& entity)
{
  const auto& classname = entity.classname();
  return classname.find("plat") != std::string::npos
         || classname.find("lift") != std::string::npos
         || classname.find("elevator") != std::string::npos;
}

double parseDoubleProperty(
  const mdl::Entity& entity, const std::string& propertyKey, const double defaultValue)
{
  const auto* value = entity.property(propertyKey);
  if (!value)
  {
    return defaultValue;
  }

  auto stream = std::istringstream{*value};
  auto result = double{};
  return stream >> result ? result : defaultValue;
}

vm::vec3d doorButtonPreviewDirection(const mdl::Entity& entity)
{
  auto angle =
    parseDoubleProperty(entity, "angle", isLiftPreviewEntity(entity) ? -2.0 : 0.0);
  if (const auto* mangle = entity.property("mangle"))
  {
    const auto values = parseFloatList(*mangle);
    if (values.size() >= 2u)
    {
      angle = values[1];
    }
  }

  if (angle == -1.0)
  {
    return vm::vec3d{0, 0, 1};
  }
  if (angle == -2.0)
  {
    return vm::vec3d{0, 0, -1};
  }

  constexpr auto Pi = 3.14159265358979323846;
  const auto radians = angle * Pi / 180.0;
  return vm::normalize(vm::vec3d{std::cos(radians), std::sin(radians), 0.0});
}

size_t dominantAxis(const vm::vec3d& direction)
{
  const auto absX = std::abs(direction.x());
  const auto absY = std::abs(direction.y());
  const auto absZ = std::abs(direction.z());
  if (absX >= absY && absX >= absZ)
  {
    return 0u;
  }
  if (absY >= absZ)
  {
    return 1u;
  }
  return 2u;
}

double doorButtonPreviewDistance(const mdl::EntityNodeBase& entityNode)
{
  const auto& entity = entityNode.entity();
  const auto& classname = entity.classname();
  if (isLiftPreviewEntity(entity))
  {
    const auto explicitHeight = std::max(
      {parseDoubleProperty(entity, "height", 0.0),
       parseDoubleProperty(entity, "_height", 0.0),
       parseDoubleProperty(entity, "distance", 0.0),
       parseDoubleProperty(entity, "move_distance", 0.0)});
    if (explicitHeight > 0.0)
    {
      return explicitHeight;
    }
  }

  const auto direction = doorButtonPreviewDirection(entity);
  const auto size = entityNode.logicalBounds().size();
  const auto axis = dominantAxis(direction);
  const auto extent = std::max(1.0, size[axis]);
  const auto defaultLip = classname.find("button") != std::string::npos ? 4.0 : 8.0;
  const auto lip = std::max(0.0, parseDoubleProperty(entity, "lip", defaultLip));
  return std::max(1.0, extent - lip);
}

double doorButtonPreviewDurationSeconds(
  const mdl::EntityNodeBase& entityNode, const double distance)
{
  const auto& entity = entityNode.entity();
  const auto& classname = entity.classname();
  const auto defaultSpeed = classname.find("button") != std::string::npos ? 40.0 : 100.0;
  const auto speed = std::max(1.0, parseDoubleProperty(entity, "speed", defaultSpeed));
  return std::clamp(distance / speed, 0.12, 8.0);
}

double doorButtonPreviewPingPongProgress(
  const double elapsedSeconds, const double durationSeconds)
{
  const auto duration = std::max(0.001, durationSeconds);
  const auto cycle = duration * 2.0;
  const auto phase = std::fmod(std::max(0.0, elapsedSeconds), cycle);
  const auto progress = phase <= duration ? phase / duration : (cycle - phase) / duration;
  return std::clamp(progress, 0.0, 1.0);
}

struct TrainPreviewPath
{
  std::vector<vm::vec3d> points;
  bool closed = false;
};

mdl::EntityNodeBase* findEntityNodeWithTargetname(
  mdl::Node& node, const std::string& targetname)
{
  if (auto* entityNode = dynamic_cast<mdl::EntityNodeBase*>(&node))
  {
    if (
      const auto* value =
        entityNode->entity().property(mdl::EntityPropertyKeys::Targetname))
    {
      if (*value == targetname)
      {
        return entityNode;
      }
    }
  }

  for (auto* child : node.children())
  {
    if (auto* result = findEntityNodeWithTargetname(*child, targetname))
    {
      return result;
    }
  }

  return nullptr;
}

vm::vec3d trainPreviewPathPoint(const mdl::EntityNodeBase& entityNode)
{
  if (const auto* origin = entityNode.entity().property(mdl::EntityPropertyKeys::Origin))
  {
    if (!origin->empty())
    {
      return entityNode.entity().origin();
    }
  }

  return entityNode.logicalBounds().center();
}

TrainPreviewPath collectTrainPreviewPath(
  mdl::Map& map, const mdl::EntityNodeBase& trainEntity)
{
  auto result = TrainPreviewPath{};
  const auto* target = trainEntity.entity().property(mdl::EntityPropertyKeys::Target);
  if (!target || target->empty())
  {
    return result;
  }

  auto targetname = *target;
  auto visitedNodes = std::vector<mdl::EntityNodeBase*>{};
  for (auto i = size_t{0}; i < 256u && !targetname.empty(); ++i)
  {
    auto* pathNode = findEntityNodeWithTargetname(map.worldNode(), targetname);
    if (!pathNode)
    {
      break;
    }

    const auto visitedIt = std::ranges::find(visitedNodes, pathNode);
    if (visitedIt != std::end(visitedNodes))
    {
      result.closed = pathNode == visitedNodes.front() && result.points.size() > 1u;
      break;
    }

    visitedNodes.push_back(pathNode);
    result.points.push_back(trainPreviewPathPoint(*pathNode));

    const auto* nextTarget = pathNode->entity().property(mdl::EntityPropertyKeys::Target);
    if (!nextTarget || nextTarget->empty())
    {
      break;
    }
    targetname = *nextTarget;
  }

  return result;
}

size_t trainPreviewSegmentCount(const TrainPreviewPath& path)
{
  if (path.points.size() < 2u)
  {
    return 0u;
  }
  return path.closed ? path.points.size() : path.points.size() - 1u;
}

double trainPreviewPathLength(const TrainPreviewPath& path)
{
  auto result = 0.0;
  const auto segmentCount = trainPreviewSegmentCount(path);
  for (auto i = size_t{0}; i < segmentCount; ++i)
  {
    const auto next = (i + 1u) % path.points.size();
    result += vm::length(path.points[next] - path.points[i]);
  }
  return result;
}

vm::vec3d trainPreviewPathPointAtDistance(
  const TrainPreviewPath& path, const double distance)
{
  if (path.points.empty())
  {
    return vm::vec3d{};
  }

  const auto totalLength = trainPreviewPathLength(path);
  if (totalLength <= 0.0001)
  {
    return path.points.front();
  }

  auto remaining = path.closed ? std::fmod(std::max(0.0, distance), totalLength)
                               : std::clamp(distance, 0.0, totalLength);

  const auto segmentCount = trainPreviewSegmentCount(path);
  for (auto i = size_t{0}; i < segmentCount; ++i)
  {
    const auto next = (i + 1u) % path.points.size();
    const auto from = path.points[i];
    const auto to = path.points[next];
    const auto segmentLength = vm::length(to - from);
    if (segmentLength <= 0.0001)
    {
      continue;
    }

    if (remaining <= segmentLength || i + 1u == segmentCount)
    {
      const auto progress = std::clamp(remaining / segmentLength, 0.0, 1.0);
      return from + (to - from) * progress;
    }

    remaining -= segmentLength;
  }

  return path.closed ? path.points.front() : path.points.back();
}

vm::vec3d trainPreviewDelta(
  mdl::Map& map,
  const mdl::EntityNodeBase& trainEntity,
  const vm::vec3d& trainMins,
  const double elapsedSeconds)
{
  const auto path = collectTrainPreviewPath(map, trainEntity);
  if (path.points.empty())
  {
    return vm::vec3d{};
  }

  const auto totalLength = trainPreviewPathLength(path);
  if (path.points.size() < 2u || totalLength <= 0.0001)
  {
    return path.points.front() - trainMins;
  }

  const auto speed =
    std::max(1.0, parseDoubleProperty(trainEntity.entity(), "speed", 100.0));
  auto distance = std::max(0.0, elapsedSeconds) * speed;
  if (!path.closed)
  {
    const auto cycle = totalLength * 2.0;
    const auto phase = std::fmod(distance, cycle);
    distance = phase <= totalLength ? phase : cycle - phase;
  }

  return trainPreviewPathPointAtDistance(path, distance) - trainMins;
}

void collectDoorButtonPreviewBrushes(
  mdl::Node& node, std::vector<mdl::BrushNode*>& brushes)
{
  if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
  {
    brushes.push_back(brushNode);
    return;
  }

  for (auto* child : node.children())
  {
    collectDoorButtonPreviewBrushes(*child, brushes);
  }
}

std::vector<mdl::BrushNode*> collectDoorButtonPreviewBrushes(
  mdl::EntityNodeBase& entityNode)
{
  auto brushes = std::vector<mdl::BrushNode*>{};
  collectDoorButtonPreviewBrushes(entityNode, brushes);
  return kdl::vec_sort_and_remove_duplicates(std::move(brushes));
}

std::optional<render::PreviewLight> makePreviewLight(const mdl::EntityNode& entityNode)
{
  const auto& entity = entityNode.entity();
  if (!isLightEntity(entity))
  {
    return std::nullopt;
  }

  auto color = parseLightColor(entity.property("_color"), vm::vec3f{1.0f, 0.92f, 0.78f});
  color = parseLightColor(entity.property("color"), color);

  auto intensity = parseLightValue(entity.property("light"), 300.0f);
  if (const auto* light = entity.property("_light"))
  {
    const auto values = parseFloatList(*light);
    if (values.size() >= 4u)
    {
      color = normalizeLightColor(vm::vec3f{values[0], values[1], values[2]});
      intensity = values[3];
    }
    else if (!values.empty())
    {
      intensity = values.front();
    }
  }

  intensity = std::clamp(intensity, 1.0f, 2500.0f);

  return render::PreviewLight{
    vm::vec3f{entity.origin()},
    color,
    std::clamp(intensity * 1.265f, 53.0f, 2253.0f),
    std::clamp(intensity / 169.23f, 0.13f, 6.5f)};
}

void collectPreviewLights(
  mdl::Node& node,
  const vm::vec3f& cameraPosition,
  std::vector<PreviewLightCandidate>& lights)
{
  if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node))
  {
    if (const auto light = makePreviewLight(*entityNode))
    {
      const auto delta = vm::vec3d{light->position} - vm::vec3d{cameraPosition};
      lights.push_back({*light, vm::squared_length(delta)});
    }
  }

  for (auto* child : node.children())
  {
    collectPreviewLights(*child, cameraPosition, lights);
  }
}

std::vector<render::PreviewLight> collectPreviewQuakeLights(
  mdl::Map& map, const vm::vec3f& cameraPosition)
{
  auto candidates = std::vector<PreviewLightCandidate>{};
  collectPreviewLights(map.worldNode(), cameraPosition, candidates);

  std::sort(
    std::begin(candidates), std::end(candidates), [](const auto& lhs, const auto& rhs) {
      return lhs.distanceToCameraSquared < rhs.distanceToCameraSquared;
    });

  auto result = std::vector<render::PreviewLight>{};
  result.reserve(std::min(candidates.size(), MaxPreviewQuakeLights));
  for (size_t i = 0; i < candidates.size() && i < MaxPreviewQuakeLights; ++i)
  {
    result.push_back(candidates[i].light);
  }
  return result;
}

struct TransformHudInfo
{
  TransformHudTarget target;
  QString label;
  vm::vec3d position;
};

QString formatHudNumber(const double value)
{
  return QString::number(value, 'f', 1);
}

QString formatTransformHudText(const TransformHudInfo& info)
{
  return QString{"%1\nX %2  Y %3  Z %4"}
    .arg(info.label)
    .arg(formatHudNumber(info.position.x()))
    .arg(formatHudNumber(info.position.y()))
    .arg(formatHudNumber(info.position.z()));
}

bool transformSelectedBrushFaces(mdl::Map& map, const vm::mat4x4d& transform)
{
  const auto brushFaces = map.selection().brushFaces;
  if (brushFaces.empty())
  {
    return false;
  }

  auto processedBrushes = std::vector<mdl::BrushNode*>{};
  auto nodesToSwap = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};

  for (const auto& brushFace : brushFaces)
  {
    auto* brushNode = brushFace.node();
    if (std::ranges::find(processedBrushes, brushNode) != std::end(processedBrushes))
    {
      continue;
    }
    processedBrushes.push_back(brushNode);

    auto brush = brushNode->brush();
    auto facesToMove = std::vector<vm::polygon3d>{};
    for (const auto& otherFace : brushFaces)
    {
      if (otherFace.node() == brushNode)
      {
        facesToMove.push_back(brush.face(otherFace.faceIndex()).polygon());
      }
    }

    if (!brush.canTransformFaces(map.worldBounds(), facesToMove, transform))
    {
      return false;
    }

    const auto result = brush.transformFaces(
      map.worldBounds(), facesToMove, transform, map.editorContext().uvLock());
    if (!result)
    {
      return false;
    }

    nodesToSwap.emplace_back(brushNode, mdl::NodeContents{std::move(brush)});
  }

  return updateNodeContents(map, "Move Brush Face", std::move(nodesToSwap));
}

std::optional<TransformHudInfo> transformHudInfo(const mdl::Map& map)
{
  const auto& selection = map.selection();
  const auto selectedVertices =
    map.nodeHandles().selectedHandleCount<mdl::VertexHandle>();
  const auto selectedEdges = map.nodeHandles().selectedHandleCount<mdl::EdgeHandle>();
  const auto selectedFaceHandles =
    map.nodeHandles().selectedHandleCount<mdl::FaceHandle>();
  const auto selectedBrushFaces = selection.brushFaces.size();
  const auto noHandles =
    selectedVertices == 0u && selectedEdges == 0u && selectedFaceHandles == 0u;

  if (
    selectedVertices == 1u && selectedEdges == 0u && selectedFaceHandles == 0u
    && selectedBrushFaces == 0u)
  {
    return TransformHudInfo{
      TransformHudTarget::Vertex,
      QString{"Vertex"},
      mdl::VertexHandle::getPositions(
        map.nodeHandles().selectedHandles<mdl::VertexHandle>())
        .front()};
  }

  if (
    selectedEdges == 1u && selectedVertices == 0u && selectedFaceHandles == 0u
    && selectedBrushFaces == 0u)
  {
    return TransformHudInfo{
      TransformHudTarget::Edge,
      QString{"Edge"},
      mdl::EdgeHandle::getPositions(map.nodeHandles().selectedHandles<mdl::EdgeHandle>())
        .front()
        .center()};
  }

  if (selectedFaceHandles == 1u && selectedVertices == 0u && selectedEdges == 0u)
  {
    return TransformHudInfo{
      TransformHudTarget::FaceHandle,
      QString{"Face"},
      mdl::FaceHandle::getPositions(map.nodeHandles().selectedHandles<mdl::FaceHandle>())
        .front()
        .center()};
  }

  if (selectedBrushFaces == 1u && noHandles)
  {
    return TransformHudInfo{
      TransformHudTarget::BrushFace,
      QString{"Face"},
      selection.brushFaces.front().face().center()};
  }

  if (selection.brushes.size() == 1u && selectedBrushFaces == 0u && noHandles)
  {
    return TransformHudInfo{
      TransformHudTarget::Brush,
      QString{"Brush"},
      selection.brushes.front()->logicalBounds().center()};
  }

  return std::nullopt;
}

} // namespace

const int MapViewBase::DefaultCameraAnimationDuration = 250;

MapViewBase::MapViewBase(
  AppController& appController, MapDocument& document, MapViewToolBox& toolBox)
  : RenderView{appController}
  , m_appController{appController}
  , m_document{document}
  , m_toolBox{toolBox}
  , m_animationManager{std::make_unique<AnimationManager>(this)}
  , m_updateActionStatesSignalDelayer{new SignalDelayer{this}}
{
  setToolBox(toolBox);
  bindEvents();
  connectObservers();

  setAcceptDrops(true);
  createTransformHud();
  createIntermissionPreviewFrame();
  createDoorButtonPreviewButton();
  createPreviewSkyTimer();
}

void MapViewBase::setCompass(std::unique_ptr<render::Compass> compass)
{
  m_compass = std::move(compass);
}

void MapViewBase::mapViewBaseVirtualInit()
{
  createActionsAndUpdatePicking();
}

MapViewBase::~MapViewBase()
{
  stopDoorButtonPreview(true);

  // Deleting m_compass will access the VBO so we need to be current
  // see: http://doc.qt.io/qt-5/qopenglwidget.html#resource-initialization-and-cleanup
  makeCurrent();
}

void MapViewBase::setIsCurrent(const bool isCurrent)
{
  m_isCurrent = isCurrent;
}

void MapViewBase::bindEvents()
{
  connect(
    m_updateActionStatesSignalDelayer,
    &SignalDelayer::processSignal,
    this,
    &MapViewBase::updateActionStates);
}

void MapViewBase::connectObservers()
{
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect(this, &MapViewBase::documentWasLoaded);
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect(this, &MapViewBase::documentDidChange);
  m_notifierConnection +=
    m_document.selectionDidChangeNotifier.connect(this, &MapViewBase::selectionDidChange);

  m_notifierConnection += m_document.materialCollectionsDidChangeNotifier.connect(
    this, &MapViewBase::materialCollectionsDidChange);
  m_notifierConnection += m_document.entityDefinitionsDidChangeNotifier.connect(
    this, &MapViewBase::entityDefinitionsDidChange);
  m_notifierConnection +=
    m_document.modsDidChangeNotifier.connect(this, &MapViewBase::modsDidChange);
  m_notifierConnection += m_document.editorContextDidChangeNotifier.connect(
    this, &MapViewBase::editorContextDidChange);
  m_notifierConnection += m_document.nodeVisibilityDidChangeNotifier.connect(
    this, &MapViewBase::nodeVisibilityDidChange);
  m_notifierConnection += m_document.nodeLockingDidChangeNotifier.connect(
    this, &MapViewBase::nodeLockingDidChange);
  m_notifierConnection +=
    m_document.pointFileWasLoadedNotifier.connect(this, &MapViewBase::pointFileDidChange);
  m_notifierConnection += m_document.pointFileWasUnloadedNotifier.connect(
    this, &MapViewBase::pointFileDidChange);
  m_notifierConnection += m_document.portalFileWasLoadedNotifier.connect(
    this, &MapViewBase::portalFileDidChange);
  m_notifierConnection += m_document.portalFileWasUnloadedNotifier.connect(
    this, &MapViewBase::portalFileDidChange);

  m_notifierConnection +=
    m_document.gridDidChangeNotifier.connect(this, &MapViewBase::gridDidChange);

  m_notifierConnection +=
    m_toolBox.toolActivatedNotifier.connect(this, &MapViewBase::toolChanged);
  m_notifierConnection +=
    m_toolBox.toolDeactivatedNotifier.connect(this, &MapViewBase::toolChanged);

  auto& prefs = PreferenceManager::instance();
  m_notifierConnection +=
    prefs.preferenceDidChangeNotifier.connect(this, &MapViewBase::preferenceDidChange);
}

/**
 * Full re-initialization of QActions and picking state.
 */
void MapViewBase::createActionsAndUpdatePicking()
{
  createActions();
  updateActionStates();
  updatePickResult();
}

void MapViewBase::createTransformHud()
{
  m_transformHud = new QFrame{this};
  m_transformHud->setObjectName("transformHud");
  m_transformHud->setAttribute(Qt::WA_StyledBackground);
  m_transformHud->setAutoFillBackground(false);
  m_transformHud->setCursor(Qt::PointingHandCursor);
  m_transformHud->setStyleSheet(
    "QFrame#transformHud {"
    "  background: rgba(18, 22, 26, 165);"
    "  border: 1px solid rgba(255, 255, 255, 70);"
    "  border-radius: 6px;"
    "}"
    "QLabel {"
    "  color: rgba(255, 255, 255, 230);"
    "  font-size: 11px;"
    "}");

  auto* layout = new QVBoxLayout{m_transformHud};
  layout->setContentsMargins(10, 7, 10, 7);
  layout->setSpacing(0);

  m_transformHudText = new QLabel{m_transformHud};
  m_transformHudText->setTextFormat(Qt::PlainText);
  m_transformHudText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  layout->addWidget(m_transformHudText);

  m_transformHud->installEventFilter(this);
  m_transformHudText->installEventFilter(this);

  m_transformHud->hide();
}

void MapViewBase::createIntermissionPreviewFrame()
{
  m_intermissionPreviewFrame = new QFrame{this};
  m_intermissionPreviewFrame->setObjectName("intermissionPreviewFrame");
  m_intermissionPreviewFrame->setAttribute(Qt::WA_StyledBackground);
  m_intermissionPreviewFrame->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_intermissionPreviewFrame->setAutoFillBackground(false);
  m_intermissionPreviewFrame->setStyleSheet(
    "QFrame#intermissionPreviewFrame {"
    "  background: transparent;"
    "  border: 1px solid rgba(255, 255, 255, 115);"
    "  border-radius: 4px;"
    "}");
  m_intermissionPreviewFrame->hide();
}

void MapViewBase::createTransformHudEditor()
{
  m_transformHudEditor = new QFrame{this, Qt::Popup | Qt::FramelessWindowHint};
  m_transformHudEditor->setObjectName("transformHudEditor");
  m_transformHudEditor->setAttribute(Qt::WA_StyledBackground);
  m_transformHudEditor->setAutoFillBackground(false);
  m_transformHudEditor->setStyleSheet(
    "QFrame#transformHudEditor {"
    "  background: rgba(18, 22, 26, 225);"
    "  border: 1px solid rgba(255, 255, 255, 90);"
    "  border-radius: 6px;"
    "}"
    "QLabel {"
    "  color: rgba(255, 255, 255, 230);"
    "  font-size: 11px;"
    "}"
    "QDoubleSpinBox {"
    "  color: rgba(255, 255, 255, 235);"
    "  background: rgba(255, 255, 255, 22);"
    "  border: 1px solid rgba(255, 255, 255, 70);"
    "  border-radius: 4px;"
    "  padding: 3px 5px;"
    "  selection-background-color: rgba(80, 155, 255, 180);"
    "}");

  auto* layout = new QHBoxLayout{m_transformHudEditor};
  layout->setContentsMargins(8, 6, 8, 6);
  layout->setSpacing(6);

  const auto makeSpinBox = [&](const QString& label) {
    auto* axisLabel = new QLabel{label, m_transformHudEditor};
    auto* spinBox = new QDoubleSpinBox{m_transformHudEditor};
    spinBox->setDecimals(3);
    spinBox->setRange(-1048576.0, 1048576.0);
    spinBox->setSingleStep(1.0);
    spinBox->setAccelerated(true);
    spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spinBox->setFixedWidth(92);

    layout->addWidget(axisLabel);
    layout->addWidget(spinBox);

    connect(
      spinBox,
      qOverload<double>(&QDoubleSpinBox::valueChanged),
      this,
      [this](const double) { applyTransformHudEditorValues(); });

    return spinBox;
  };

  m_transformHudXSpin = makeSpinBox("X");
  m_transformHudYSpin = makeSpinBox("Y");
  m_transformHudZSpin = makeSpinBox("Z");

  m_transformHudEditor->hide();
}

void MapViewBase::updateTransformHud()
{
  if (!m_transformHud || !m_transformHudText)
  {
    return;
  }

  if (m_toolBox.symmetryRelinkInProgress())
  {
    hideTransformHudEditor();
    if (m_intermissionPreviewFrame)
    {
      m_intermissionPreviewFrame->hide();
    }
    if (m_doorButtonPreviewButton)
    {
      m_doorButtonPreviewButton->hide();
    }

    const auto text = QString{"LOADING..."};
    if (m_transformHudText->text() != text)
    {
      m_transformHudText->setText(text);
      m_transformHud->adjustSize();
    }
    if (m_transformHudText->alignment() != (Qt::AlignCenter | Qt::AlignVCenter))
    {
      m_transformHudText->setAlignment(Qt::AlignCenter | Qt::AlignVCenter);
      m_transformHud->adjustSize();
    }

    positionTransformHud();
    m_transformHud->show();
    m_transformHud->raise();
    return;
  }

  if (m_transformHudText->alignment() != (Qt::AlignLeft | Qt::AlignVCenter))
  {
    m_transformHudText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  }

  const auto info = transformHudInfo(m_document.map());
  if (!info)
  {
    hideTransformHudEditor();
    m_transformHud->hide();
    updateIntermissionPreviewFrame();
    updateDoorButtonPreviewButton();
    return;
  }

  const auto text = formatTransformHudText(*info);
  if (m_transformHudText->text() != text)
  {
    m_transformHudText->setText(text);
    m_transformHud->adjustSize();
  }

  positionTransformHud();
  m_transformHud->show();
  m_transformHud->raise();
  updateTransformHudEditor();
  updateIntermissionPreviewFrame();
  updateDoorButtonPreviewButton();
}

void MapViewBase::positionTransformHud()
{
  if (!m_transformHud || !m_transformHudText)
  {
    return;
  }

  m_transformHud->adjustSize();

  const auto margin = 12;
  const auto x = m_toolBox.symmetryRelinkInProgress()
                   ? std::max(0, (width() - m_transformHud->width()) / 2)
                   : std::max(margin, width() - m_transformHud->width() - margin);
  const auto y = m_toolBox.symmetryRelinkInProgress()
                   ? std::max(0, (height() - m_transformHud->height()) / 2)
                   : std::max(margin, height() - m_transformHud->height() - margin);
  m_transformHud->move(x, y);

  positionTransformHudEditor();
  positionIntermissionPreviewFrame();
  positionDoorButtonPreviewButton();
}

void MapViewBase::updateIntermissionPreviewFrame()
{
  if (!m_intermissionPreviewFrame)
  {
    return;
  }

  if (!selectedIntermissionEntity())
  {
    m_intermissionPreviewFrame->hide();
    return;
  }

  positionIntermissionPreviewFrame();
  m_intermissionPreviewFrame->show();
  m_intermissionPreviewFrame->raise();
}

void MapViewBase::positionIntermissionPreviewFrame()
{
  if (!m_intermissionPreviewFrame)
  {
    return;
  }

  m_intermissionPreviewFrame->setGeometry(intermissionPreviewRect());
}

QRect MapViewBase::intermissionPreviewRect() const
{
  constexpr auto margin = 12;
  constexpr auto spacing = 6;

  const auto previewWidth = std::clamp(width() / 4, 240, 360);
  const auto previewHeight = std::max(135, previewWidth * 9 / 16);
  const auto x = std::max(margin, width() - previewWidth - margin);

  auto y = std::max(margin, height() - previewHeight - margin);
  if (
    m_transformHud && m_transformHud->isVisible()
    && !m_toolBox.symmetryRelinkInProgress())
  {
    y = m_transformHud->y() - previewHeight - spacing;
    if (y < margin)
    {
      y = m_transformHud->y() + m_transformHud->height() + spacing;
    }
  }

  return QRect{x, std::max(margin, y), previewWidth, previewHeight};
}

mdl::EntityNode* MapViewBase::selectedIntermissionEntity() const
{
  const auto& selection = m_document.map().selection();
  if (
    selection.entities.size() != 1u || selection.hasBrushes() || selection.hasGroups()
    || selection.hasPatches() || selection.hasAnyBrushFaces())
  {
    return nullptr;
  }

  auto* entityNode = selection.entities.front();
  return entityNode && isIntermissionPreviewEntity(entityNode->entity()) ? entityNode
                                                                         : nullptr;
}

void MapViewBase::positionTransformHudEditor()
{
  if (!m_transformHud || !m_transformHudEditor || !m_transformHudEditor->isVisible())
  {
    return;
  }

  m_transformHudEditor->adjustSize();

  const auto editorWidth = m_transformHudEditor->width();
  const auto editorHeight = m_transformHudEditor->height();
  const auto localX = std::max(0, m_transformHud->width() - editorWidth);
  auto globalTopLeft = m_transformHud->mapToGlobal(QPoint{localX, -editorHeight - 6});
  if (globalTopLeft.y() < 0)
  {
    globalTopLeft =
      m_transformHud->mapToGlobal(QPoint{localX, m_transformHud->height() + 6});
  }
  m_transformHudEditor->move(globalTopLeft);
}

void MapViewBase::showTransformHudEditor()
{
  if (m_toolBox.symmetryRelinkInProgress() || !transformHudInfo(m_document.map()))
  {
    return;
  }

  if (!m_transformHudEditor)
  {
    createTransformHudEditor();
  }

  updateTransformHudEditor();
  m_transformHudEditor->show();
  m_transformHudEditor->raise();
  positionTransformHudEditor();

  if (m_transformHudXSpin)
  {
    m_transformHudXSpin->setFocus(Qt::MouseFocusReason);
    m_transformHudXSpin->selectAll();
  }
}

void MapViewBase::hideTransformHudEditor()
{
  if (m_transformHudEditor)
  {
    m_transformHudEditor->hide();
  }
}

void MapViewBase::updateTransformHudEditor()
{
  if (!m_transformHudEditor)
  {
    return;
  }

  const auto info = transformHudInfo(m_document.map());
  if (!info)
  {
    hideTransformHudEditor();
    return;
  }

  const auto step = std::max(1.0, double(m_document.map().grid().actualSize()));
  m_updatingTransformHudEditor = true;
  const auto blockX = QSignalBlocker{m_transformHudXSpin};
  const auto blockY = QSignalBlocker{m_transformHudYSpin};
  const auto blockZ = QSignalBlocker{m_transformHudZSpin};

  m_transformHudXSpin->setSingleStep(step);
  m_transformHudYSpin->setSingleStep(step);
  m_transformHudZSpin->setSingleStep(step);
  m_transformHudXSpin->setValue(info->position.x());
  m_transformHudYSpin->setValue(info->position.y());
  m_transformHudZSpin->setValue(info->position.z());
  m_updatingTransformHudEditor = false;
}

void MapViewBase::applyTransformHudEditorValues()
{
  if (m_updatingTransformHudEditor || !m_transformHudEditor)
  {
    return;
  }

  const auto info = transformHudInfo(m_document.map());
  if (!info)
  {
    hideTransformHudEditor();
    return;
  }

  const auto target = vm::vec3d{
    m_transformHudXSpin->value(),
    m_transformHudYSpin->value(),
    m_transformHudZSpin->value()};
  const auto delta = target - info->position;
  if (vm::is_zero(delta, 0.001))
  {
    return;
  }

  if (!moveTransformHudSelection(delta))
  {
    QApplication::beep();
    updateTransformHudEditor();
    return;
  }

  updateTransformHud();
  update();
}

bool MapViewBase::moveTransformHudSelection(const vm::vec3d& delta)
{
  auto& map = m_document.map();
  const auto info = transformHudInfo(map);
  if (!info)
  {
    return false;
  }

  const auto transform = vm::translation_matrix(delta);
  switch (info->target)
  {
  case TransformHudTarget::Vertex:
    if (m_toolBox.vertexToolActive())
    {
      m_toolBox.moveNodeHandles(delta);
      return true;
    }
    return transformVertices(
             map,
             mdl::VertexHandle::getPositions(
               map.nodeHandles().selectedHandles<mdl::VertexHandle>()),
             transform)
      .success;
  case TransformHudTarget::Edge:
    if (m_toolBox.edgeToolActive())
    {
      m_toolBox.moveNodeHandles(delta);
      return true;
    }
    return transformEdges(
      map,
      mdl::EdgeHandle::getPositions(map.nodeHandles().selectedHandles<mdl::EdgeHandle>()),
      transform);
  case TransformHudTarget::FaceHandle:
    if (m_toolBox.faceToolActive())
    {
      m_toolBox.moveNodeHandles(delta);
      return true;
    }
    return transformFaces(
      map,
      mdl::FaceHandle::getPositions(map.nodeHandles().selectedHandles<mdl::FaceHandle>()),
      transform);
  case TransformHudTarget::BrushFace:
    return transformSelectedBrushFaces(map, transform);
  case TransformHudTarget::Brush:
    return translateSelection(map, delta);
  }

  return false;
}

void MapViewBase::createDoorButtonPreviewButton()
{
  m_doorButtonPreviewButton = new QToolButton{this};
  m_doorButtonPreviewButton->setObjectName("doorButtonPreviewButton");
  m_doorButtonPreviewButton->setCheckable(true);
  m_doorButtonPreviewButton->setCursor(Qt::PointingHandCursor);
  m_doorButtonPreviewButton->setFixedSize(34, 28);
  m_doorButtonPreviewButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
  m_doorButtonPreviewButton->setToolTip(
    tr("Play door/button/lift/train animation preview"));
  m_doorButtonPreviewButton->setStyleSheet(
    "QToolButton#doorButtonPreviewButton {"
    "  background: rgba(18, 22, 26, 190);"
    "  border: 1px solid rgba(255, 255, 255, 85);"
    "  border-radius: 6px;"
    "  color: rgba(255, 255, 255, 235);"
    "}"
    "QToolButton#doorButtonPreviewButton:hover {"
    "  background: rgba(35, 42, 48, 220);"
    "}"
    "QToolButton#doorButtonPreviewButton:checked {"
    "  background: rgba(38, 176, 74, 230);"
    "  border: 1px solid rgba(90, 240, 125, 230);"
    "}");
  connect(
    m_doorButtonPreviewButton,
    &QToolButton::clicked,
    this,
    &MapViewBase::toggleDoorButtonPreview);
  m_doorButtonPreviewButton->hide();

  m_doorButtonPreviewTimer = new QTimer{this};
  m_doorButtonPreviewTimer->setInterval(16);
  connect(
    m_doorButtonPreviewTimer,
    &QTimer::timeout,
    this,
    &MapViewBase::tickDoorButtonPreview);
}

void MapViewBase::updateDoorButtonPreviewButton()
{
  if (!m_doorButtonPreviewButton)
  {
    return;
  }

  const auto canPreview =
    !m_toolBox.symmetryRelinkInProgress() && !selectedDoorButtonPreviewEntities().empty();
  if (!canPreview)
  {
    if (m_doorButtonPreviewActive)
    {
      stopDoorButtonPreview(true);
    }
    m_doorButtonPreviewButton->hide();
    return;
  }

  setDoorButtonPreviewButtonActive(m_doorButtonPreviewActive);
  positionDoorButtonPreviewButton();
  m_doorButtonPreviewButton->show();
  m_doorButtonPreviewButton->raise();
}

void MapViewBase::positionDoorButtonPreviewButton()
{
  if (!m_doorButtonPreviewButton || !m_doorButtonPreviewButton->isVisible())
  {
    return;
  }

  constexpr auto margin = 12;
  constexpr auto spacing = 6;
  auto x = std::max(margin, width() - m_doorButtonPreviewButton->width() - margin);
  auto y = std::max(margin, height() - m_doorButtonPreviewButton->height() - margin);

  if (
    m_transformHud && m_transformHud->isVisible()
    && !m_toolBox.symmetryRelinkInProgress())
  {
    x = std::max(
      margin,
      m_transformHud->x() + m_transformHud->width() - m_doorButtonPreviewButton->width());
    y = m_transformHud->y() - m_doorButtonPreviewButton->height() - spacing;
    if (y < margin)
    {
      y = m_transformHud->y() + m_transformHud->height() + spacing;
    }
  }

  m_doorButtonPreviewButton->move(x, y);
}

void MapViewBase::setDoorButtonPreviewButtonActive(const bool active)
{
  if (!m_doorButtonPreviewButton)
  {
    return;
  }

  const auto blocker = QSignalBlocker{m_doorButtonPreviewButton};
  m_doorButtonPreviewButton->setChecked(active);
  m_doorButtonPreviewButton->setIcon(
    style()->standardIcon(active ? QStyle::SP_MediaStop : QStyle::SP_MediaPlay));
  m_doorButtonPreviewButton->setToolTip(
    active ? tr("Stop door/button/lift/train animation preview")
           : tr("Play door/button/lift/train animation preview"));
}

void MapViewBase::toggleDoorButtonPreview()
{
  if (m_doorButtonPreviewActive)
  {
    stopDoorButtonPreview(true);
  }
  else
  {
    startDoorButtonPreview();
  }
}

void MapViewBase::startDoorButtonPreview()
{
  stopDoorButtonPreview(true);

  const auto entities = selectedDoorButtonPreviewEntities();
  if (entities.empty())
  {
    updateDoorButtonPreviewButton();
    return;
  }

  m_doorButtonPreviewEntries.clear();
  for (auto* entityNode : entities)
  {
    auto brushes = collectDoorButtonPreviewBrushes(*entityNode);
    if (brushes.empty())
    {
      continue;
    }

    auto entry = DoorButtonPreviewEntry{};
    entry.entity = entityNode;
    entry.direction = doorButtonPreviewDirection(entityNode->entity());
    entry.distance = doorButtonPreviewDistance(*entityNode);
    entry.durationSeconds = doorButtonPreviewDurationSeconds(*entityNode, entry.distance);
    entry.brushes.reserve(brushes.size());
    for (auto* brushNode : brushes)
    {
      entry.brushes.push_back(DoorButtonPreviewBrushState{brushNode, brushNode->brush()});
    }
    m_doorButtonPreviewEntries.push_back(std::move(entry));
  }

  if (m_doorButtonPreviewEntries.empty())
  {
    updateDoorButtonPreviewButton();
    return;
  }

  m_doorButtonPreviewActive = true;
  m_doorButtonPreviewStartTime = std::chrono::steady_clock::now();
  setDoorButtonPreviewButtonActive(true);
  applyDoorButtonPreview(0.0);
  m_doorButtonPreviewTimer->start();
  update();
}

void MapViewBase::stopDoorButtonPreview(const bool restore)
{
  const auto hadPreview =
    m_doorButtonPreviewActive || !m_doorButtonPreviewEntries.empty();
  m_doorButtonPreviewActive = false;
  if (m_doorButtonPreviewTimer)
  {
    m_doorButtonPreviewTimer->stop();
  }

  if (restore && !m_doorButtonPreviewEntries.empty())
  {
    restoreDoorButtonPreview();
  }
  m_doorButtonPreviewEntries.clear();
  setDoorButtonPreviewButtonActive(false);

  if (hadPreview)
  {
    updateTransformHud();
    update();
  }
}

void MapViewBase::tickDoorButtonPreview()
{
  if (!m_doorButtonPreviewActive)
  {
    return;
  }
  if (!doorButtonPreviewSelectionMatches())
  {
    stopDoorButtonPreview(true);
    return;
  }

  const auto elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - m_doorButtonPreviewStartTime)
                         .count();
  applyDoorButtonPreview(elapsed);
  update();
}

void MapViewBase::applyDoorButtonPreview(const double elapsedSeconds)
{
  if (m_doorButtonPreviewEntries.empty())
  {
    return;
  }

  auto& map = m_document.map();
  auto changedNodes = std::vector<mdl::Node*>{};
  for (const auto& entry : m_doorButtonPreviewEntries)
  {
    for (const auto& brushState : entry.brushes)
    {
      changedNodes.push_back(brushState.node);
    }
  }
  changedNodes = kdl::vec_sort_and_remove_duplicates(std::move(changedNodes));

  auto notifyNodes = NotifyBeforeAndAfter{
    map.nodesWillChangeNotifier, map.nodesDidChangeNotifier, changedNodes};
  const auto lockMaterials = map.editorContext().alignmentLock();
  for (auto& entry : m_doorButtonPreviewEntries)
  {
    auto delta = vm::vec3d{};
    if (isTrainPreviewEntity(entry.entity->entity()))
    {
      auto sourceBounds = vm::bbox3d::builder{};
      for (const auto& brushState : entry.brushes)
      {
        sourceBounds.add(brushState.brush.bounds());
      }
      delta =
        trainPreviewDelta(map, *entry.entity, sourceBounds.bounds().min, elapsedSeconds);
    }
    else
    {
      entry.direction = doorButtonPreviewDirection(entry.entity->entity());
      entry.distance = doorButtonPreviewDistance(*entry.entity);
      entry.durationSeconds =
        doorButtonPreviewDurationSeconds(*entry.entity, entry.distance);

      const auto progress =
        doorButtonPreviewPingPongProgress(elapsedSeconds, entry.durationSeconds);
      delta = entry.direction * (entry.distance * progress);
    }
    const auto transformation = vm::translation_matrix(delta);

    for (const auto& brushState : entry.brushes)
    {
      auto brush = brushState.brush;
      if (brush.transform(map.worldBounds(), transformation, lockMaterials))
      {
        brushState.node->setBrushForPreview(std::move(brush));
      }
      else
      {
        brushState.node->setBrushForPreview(brushState.brush);
      }
    }
  }
}

void MapViewBase::restoreDoorButtonPreview()
{
  auto& map = m_document.map();
  auto changedNodes = std::vector<mdl::Node*>{};
  for (const auto& entry : m_doorButtonPreviewEntries)
  {
    for (const auto& brushState : entry.brushes)
    {
      changedNodes.push_back(brushState.node);
    }
  }
  changedNodes = kdl::vec_sort_and_remove_duplicates(std::move(changedNodes));
  if (changedNodes.empty())
  {
    return;
  }

  auto notifyNodes = NotifyBeforeAndAfter{
    map.nodesWillChangeNotifier, map.nodesDidChangeNotifier, changedNodes};
  for (const auto& entry : m_doorButtonPreviewEntries)
  {
    for (const auto& brushState : entry.brushes)
    {
      brushState.node->setBrushForPreview(brushState.brush);
    }
  }
}

std::vector<mdl::EntityNodeBase*> MapViewBase::selectedDoorButtonPreviewEntities() const
{
  auto& map = m_document.map();
  const auto& selection = map.selection();
  if (!selection.hasAny())
  {
    return {};
  }

  auto result = std::vector<mdl::EntityNodeBase*>{};
  for (auto* entityNode : selection.allEntities())
  {
    if (!entityNode || entityNode == &map.worldNode())
    {
      return {};
    }
    if (!isDoorButtonPreviewEntity(entityNode->entity()))
    {
      return {};
    }
    if (collectDoorButtonPreviewBrushes(*entityNode).empty())
    {
      return {};
    }
    result.push_back(entityNode);
  }

  return kdl::vec_sort_and_remove_duplicates(std::move(result));
}

bool MapViewBase::doorButtonPreviewSelectionMatches() const
{
  auto currentEntities = selectedDoorButtonPreviewEntities();
  auto previewEntities = std::vector<mdl::EntityNodeBase*>{};
  previewEntities.reserve(m_doorButtonPreviewEntries.size());
  for (const auto& entry : m_doorButtonPreviewEntries)
  {
    previewEntities.push_back(entry.entity);
  }
  previewEntities = kdl::vec_sort_and_remove_duplicates(std::move(previewEntities));
  return currentEntities == previewEntities;
}

void MapViewBase::createPreviewSkyTimer()
{
  m_previewSkyTimer = new QTimer{this};
  m_previewSkyTimer->setInterval(33);
  connect(m_previewSkyTimer, &QTimer::timeout, this, [this]() { update(); });
  updatePreviewSkyTimer();
}

void MapViewBase::updatePreviewSkyTimer()
{
  if (!m_previewSkyTimer)
  {
    return;
  }

  if (pref(Preferences::PreviewQuakeLighting))
  {
    if (!m_previewSkyTimer->isActive())
    {
      m_previewSkyTimer->start();
    }
  }
  else
  {
    m_previewSkyTimer->stop();
  }
}

void MapViewBase::documentWasLoaded()
{
  stopDoorButtonPreview(false);
  createActionsAndUpdatePicking();
  updateTransformHud();
  update();
}

void MapViewBase::documentDidChange()
{
  updatePickResult();
  updateActionStates();
  updateTransformHud();
  update();
}

void MapViewBase::selectionDidChange(const mdl::SelectionChange&)
{
  if (m_doorButtonPreviewActive && !doorButtonPreviewSelectionMatches())
  {
    stopDoorButtonPreview(true);
  }
  updatePickResult();
  updateActionStates();
  updateTransformHud();
  update();
}

void MapViewBase::toolChanged(Tool&)
{
  updatePickResult();
  updateActionStates();
  updateTransformHud();
  update();
}

void MapViewBase::materialCollectionsDidChange()
{
  update();
}

void MapViewBase::entityDefinitionsDidChange()
{
  createActions();
}

void MapViewBase::modsDidChange()
{
  update();
}

void MapViewBase::editorContextDidChange()
{
  update();
}

void MapViewBase::nodeVisibilityDidChange(const std::vector<mdl::Node*>&)
{
  update();
}

void MapViewBase::nodeLockingDidChange(const std::vector<mdl::Node*>&)
{
  update();
}

void MapViewBase::gridDidChange()
{
  update();
}

void MapViewBase::pointFileDidChange()
{
  update();
}

void MapViewBase::portalFileDidChange()
{
  invalidatePortalFileRenderer();
  update();
}

void MapViewBase::preferenceDidChange(const std::filesystem::path& path)
{
  if (path == Preferences::RendererFontSize.path)
  {
    fontManager().clearCache();
  }
  if (path == Preferences::PreviewQuakeLighting.path)
  {
    updatePreviewSkyTimer();
  }

  updateActionBindings();
  update();
}

void MapViewBase::createActions()
{
  // Destroy existing QShortcuts via the weak references in m_shortcuts
  for (auto& [shortcut, action] : m_shortcuts)
  {
    unused(action);
    delete shortcut;
  }
  m_shortcuts.clear();

  auto visitor = [this](const Action& action) {
    const auto& keySequences = pref(action.preference());

    auto* shortcut = new QShortcut{this};
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    shortcut->setKeys(keySequences | kdl::ranges::to<QList>());
    connect(
      shortcut, &QShortcut::activated, this, [this, &action] { triggerAction(action); });
    connect(shortcut, &QShortcut::activatedAmbiguously, this, [this, &action] {
      triggerAmbiguousAction(action.label());
    });
    m_shortcuts.emplace_back(shortcut, &action);
  };

  const auto& actionManager = m_appController.actionManager();
  // We don't create a QShortcut for actions whose key binding is handled
  // by the menu or toolbar since they would conflict.
  actionManager.visitMapViewActions(visitor);

  m_document.visitTagActions(actionManager, visitor);
  m_document.visitEntityDefinitionActions(actionManager, visitor);
}

void MapViewBase::updateActionBindings()
{
  for (auto& [shortcut, action] : m_shortcuts)
  {
    shortcut->setKeys(pref(action->preference()) | kdl::ranges::to<QList>());
  }
}

void MapViewBase::updateActionStates()
{
  auto* mapWindow = dynamic_cast<MapWindow*>(window());
  auto context = ActionExecutionContext{m_appController, mapWindow, this};
  for (auto& [shortcut, action] : m_shortcuts)
  {
    shortcut->setEnabled(hasFocus() && action->enabled(context));
  }
}

void MapViewBase::updateActionStatesDelayed()
{
  m_updateActionStatesSignalDelayer->queueSignal();
}

void MapViewBase::triggerAction(const Action& action)
{
  auto* mapWindow = dynamic_cast<MapWindow*>(window());
  auto context = ActionExecutionContext{m_appController, mapWindow, this};
  action.execute(context);
}

void MapViewBase::triggerAmbiguousAction(const QString& label)
{
  qDebug() << "Ambiguous action triggered: " << label;
}

void MapViewBase::move(const vm::direction direction)
{
  if ((actionContext() & ActionContext::RotateTool) != 0)
  {
    moveRotationCenter(direction);
  }
  else if ((actionContext() & ActionContext::AnyNodeHandleTool) != 0)
  {
    moveNodeHandles(direction);
  }
  else if ((actionContext() & ActionContext::SweepTool) != 0)
  {
    moveSweepCenter(direction);
  }
  else if ((actionContext() & ActionContext::NodeSelection) != 0)
  {
    moveObjects(direction);
  }
}

void MapViewBase::moveRotationCenter(const vm::direction direction)
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto delta = moveDirection(direction) * double(grid.actualSize());
  m_toolBox.moveRotationCenter(delta);
  update();
}

void MapViewBase::moveSweepCenter(const vm::direction direction)
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto delta = moveDirection(direction) * double(grid.actualSize());
  m_toolBox.moveSweepCenter(delta);
  update();
}

void MapViewBase::moveNodeHandles(const vm::direction direction)
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto delta = moveDirection(direction) * double(grid.actualSize());
  m_toolBox.moveNodeHandles(delta);
}

void MapViewBase::moveObjects(const vm::direction direction)
{
  auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto delta = moveDirection(direction) * double(grid.actualSize());
  translateSelection(map, delta);
}

void MapViewBase::duplicateObjects()
{
  auto& map = m_document.map();
  if (map.selection().hasNodes())
  {
    duplicateSelectedNodes(map);
  }
}

void MapViewBase::duplicateAndMoveObjects(const vm::direction direction)
{
  auto& map = m_document.map();
  auto transaction = mdl::Transaction{map};
  duplicateObjects();
  moveObjects(direction);
  transaction.commit();
}

void MapViewBase::rotate(const vm::rotation_axis axisSpec, const bool clockwise)
{
  auto& map = m_document.map();
  if (m_toolBox.sweepToolActive())
  {
    const auto axis = rotationAxis(axisSpec, clockwise);
    m_toolBox.rotateSweepCap(axis, map.grid().angle());
    update();
  }
  else if (const auto& bounds = map.selectionBounds())
  {
    const auto axis = rotationAxis(axisSpec, clockwise);
    const auto angle = m_toolBox.rotateToolActive() ? vm::abs(m_toolBox.rotateToolAngle())
                                                    : vm::Cd::half_pi();

    const auto& grid = map.grid();
    const auto center = m_toolBox.rotateToolActive() ? m_toolBox.rotateToolCenter()
                                                     : grid.referencePoint(*bounds);

    rotateSelection(map, center, axis, angle);
  }
}

vm::vec3d MapViewBase::rotationAxis(
  const vm::rotation_axis axisSpec, const bool clockwise) const
{
  vm::vec3d axis;
  switch (axisSpec)
  {
  case vm::rotation_axis::roll:
    axis = -moveDirection(vm::direction::forward);
    break;
  case vm::rotation_axis::pitch:
    axis = moveDirection(vm::direction::right);
    break;
  case vm::rotation_axis::yaw:
    axis = moveDirection(vm::direction::up);
    break;
    switchDefault();
  }

  return clockwise ? -axis : axis;
}

void MapViewBase::increaseSweepScale()
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  m_toolBox.scaleSweepCap(double(grid.actualSize()));
  update();
}

void MapViewBase::decreaseSweepScale()
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  m_toolBox.scaleSweepCap(-double(grid.actualSize()));
  update();
}

void MapViewBase::flip(const vm::direction direction)
{
  if (canFlip())
  {
    auto& map = m_document.map();

    // If we snap the selection bounds' center to the grid size, then
    // selections that are an odd number of grid units wide get translated.
    // Instead, snap to 1/2 the grid size.
    // (see: https://github.com/TrenchBroom/TrenchBroom/issues/1495 )
    auto halfGrid = mdl::Grid{map.grid().size()};
    halfGrid.setRotation(map.grid().rotation());
    halfGrid.decSize();

    const auto center = halfGrid.referencePoint(*map.selectionBounds());
    const auto axis = flipAxis(direction);

    flipSelection(map, center, axis);
  }
}

bool MapViewBase::canFlip() const
{
  const auto& map = m_document.map();
  return !m_toolBox.anyModalToolActive() && map.selection().hasNodes();
}

void MapViewBase::moveUV(const vm::direction direction, const UVActionMode mode)
{
  auto& map = m_document.map();
  if (map.selection().hasBrushFaces())
  {
    const auto offset = moveUVOffset(direction, mode);
    translateUV(map, camera().up(), camera().right(), offset);
  }
}

vm::vec2f MapViewBase::moveUVOffset(
  const vm::direction direction, const UVActionMode mode) const
{
  switch (direction)
  {
  case vm::direction::up:
    return vm::vec2f{0.0f, moveUVDistance(mode)};
  case vm::direction::down:
    return vm::vec2f{0.0f, -moveUVDistance(mode)};
  case vm::direction::left:
    return vm::vec2f{-moveUVDistance(mode), 0.0f};
  case vm::direction::right:
    return vm::vec2f{moveUVDistance(mode), 0.0f};
  case vm::direction::forward:
  case vm::direction::backward:
    return vm::vec2f{};
    switchDefault();
  }
}

float MapViewBase::moveUVDistance(const UVActionMode mode) const
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto gridSize = static_cast<float>(grid.actualSize());

  switch (mode)
  {
  case UVActionMode::Fine:
    return 1.0f;
  case UVActionMode::Coarse:
    return 2.0f * gridSize;
  case UVActionMode::Normal:
    return gridSize;
    switchDefault();
  }
}

void MapViewBase::rotateUV(const bool clockwise, const UVActionMode mode)
{
  auto& map = m_document.map();
  if (map.selection().hasBrushFaces())
  {
    const auto angle = rotateUVAngle(clockwise, mode);
    mdl::rotateUV(map, angle);
  }
}

float MapViewBase::rotateUVAngle(const bool clockwise, const UVActionMode mode) const
{
  const auto& map = m_document.map();
  const auto& grid = map.grid();
  const auto gridAngle = static_cast<float>(vm::to_degrees(grid.angle()));
  auto angle = 0.0f;

  switch (mode)
  {
  case UVActionMode::Fine:
    angle = 1.0f;
    break;
  case UVActionMode::Coarse:
    angle = 90.0f;
    break;
  case UVActionMode::Normal:
    angle = gridAngle;
    break;
  }
  return clockwise ? angle : -angle;
}

void MapViewBase::flipUV(const vm::direction direction)
{
  auto& map = m_document.map();
  if (map.selection().hasBrushFaces())
  {
    mdl::flipUV(map, camera().up(), camera().right(), direction);
  }
}

void MapViewBase::resetUV()
{
  auto& map = m_document.map();
  setBrushFaceAttributes(
    map, mdl::resetAll(map.gameInfo().gameConfig.faceAttribsConfig.defaults));
}

void MapViewBase::resetUVToWorld()
{
  auto& map = m_document.map();
  setBrushFaceAttributes(
    map, mdl::resetAllToParaxial(map.gameInfo().gameConfig.faceAttribsConfig.defaults));
}

void MapViewBase::assembleBrush()
{
  if (m_toolBox.assembleBrushToolActive())
  {
    m_toolBox.performAssembleBrush();
  }
}

void MapViewBase::toggleClipSide()
{
  m_toolBox.toggleClipSide();
}

void MapViewBase::performClip()
{
  m_toolBox.performClip();
}

void MapViewBase::performSweep()
{
  m_toolBox.performSweep();
}

void MapViewBase::performPolygonCut()
{
  m_toolBox.performPolygonCut();
}

void MapViewBase::performPolygonSpawn()
{
  m_toolBox.performPolygonSpawn();
}

void MapViewBase::resetCameraZoom()
{
  camera().setZoom(1.0f);
}

void MapViewBase::cancel()
{
  if (m_toolBox.parameterToolActive())
  {
    m_toolBox.deactivateCurrentTool();
    return;
  }

  if (!ToolBoxConnector::cancel())
  {
    auto& map = m_document.map();
    if (map.selection().hasAny())
    {
      deselectAll(map);
    }
    else if (map.editorContext().currentGroup())
    {
      closeGroup(map);
    }
  }
}

void MapViewBase::deactivateCurrentTool()
{
  m_toolBox.deactivateCurrentTool();
}

void MapViewBase::createPointEntity()
{
  auto* action = qobject_cast<const QAction*>(sender());
  auto& map = m_document.map();

  const auto classname = action->data().toString().toStdString();
  if (const auto* definition = map.entityDefinitionManager().definition(classname))
  {
    contract_assert(getType(*definition) == mdl::EntityDefinitionType::Point);
    createPointEntity(*definition);
  }
  else
  {
    map.logger().error() << "Unknown entity classname: " << classname;
  }
}

void MapViewBase::createBrushEntity()
{
  auto* action = qobject_cast<const QAction*>(sender());
  auto& map = m_document.map();

  const auto classname = action->data().toString().toStdString();
  if (const auto* definition = map.entityDefinitionManager().definition(classname))
  {
    createBrushEntity(*definition);
  }
  else
  {
    map.logger().error() << "Unknown entity classname: " << classname;
  }
}

void MapViewBase::createPointEntity(const mdl::EntityDefinition& definition)
{
  contract_pre(definition.pointEntityDefinition);

  auto& map = m_document.map();
  const auto delta = computePointEntityPosition(definition.pointEntityDefinition->bounds);
  mdl::createPointEntity(map, definition, delta);
}

void MapViewBase::createBrushEntity(const mdl::EntityDefinition& definition)
{
  auto& map = m_document.map();
  mdl::createBrushEntity(map, definition);
}

bool MapViewBase::canCreateBrushEntity()
{
  const auto& map = m_document.map();
  return map.selection().hasOnlyBrushes();
}

void MapViewBase::toggleTagVisible(const mdl::SmartTag& tag)
{
  const auto tagIndex = tag.index();

  auto& map = m_document.map();
  auto& editorContext = map.editorContext();
  auto hiddenTags = editorContext.hiddenTags();
  hiddenTags ^= mdl::TagType::Type{1} << tagIndex;
  editorContext.setHiddenTags(hiddenTags);
}

void MapViewBase::enableTag(const mdl::SmartTag& tag)
{
  contract_pre(tag.canEnable());

  auto& map = m_document.map();
  auto transaction = mdl::Transaction{map, "Turn Selection into " + tag.name()};
  auto callback = EnableDisableTagCallback{};
  tag.enable(callback, map);
  transaction.commit();
}

void MapViewBase::disableTag(const mdl::SmartTag& tag)
{
  contract_pre(tag.canDisable());

  auto& map = m_document.map();
  auto transaction = mdl::Transaction{map, "Turn Selection into non-" + tag.name()};
  auto callback = EnableDisableTagCallback{};
  tag.disable(callback, map);
  transaction.commit();
}

void MapViewBase::makeStructural()
{
  auto& map = m_document.map();
  if (!map.selection().hasBrushes())
  {
    return;
  }

  auto toReparent = std::vector<mdl::Node*>{};
  const auto& selectedBrushes = map.selection().brushes;
  std::ranges::copy_if(
    selectedBrushes, std::back_inserter(toReparent), [&](const auto* brushNode) {
      return brushNode->entity() != &map.worldNode();
    });

  auto transaction = mdl::Transaction{map, "Make Structural"};

  if (!toReparent.empty())
  {
    reparentNodes(toReparent, parentForNodes(map, toReparent), false);
  }

  auto anyTagDisabled = false;
  auto callback = EnableDisableTagCallback{};
  for (auto* brush : map.selection().brushes)
  {
    for (const auto& tag : map.smartTags())
    {
      if (brush->hasTag(tag) || brush->anyFacesHaveAnyTagInMask(tag.type()))
      {
        anyTagDisabled = true;
        tag.disable(callback, map);
      }
    }
  }

  if (!anyTagDisabled && toReparent.empty())
  {
    transaction.cancel();
  }
  else
  {
    transaction.commit();
  }
}

void MapViewBase::toggleEntityDefinitionVisible(const mdl::EntityDefinition& definition)
{
  auto& map = m_document.map();

  auto& editorContext = map.editorContext();
  editorContext.setEntityDefinitionHidden(
    definition, !editorContext.entityDefinitionHidden(definition));
}

void MapViewBase::createEntity(const mdl::EntityDefinition& definition)
{
  switch (getType(definition))
  {
  case mdl::EntityDefinitionType::Point:
    createPointEntity(definition);
    break;
  case mdl::EntityDefinitionType::Brush:
    if (canCreateBrushEntity())
    {
      createBrushEntity(definition);
    }
    break;
    switchDefault();
  }
}

void MapViewBase::toggleShowEntityClassnames()
{
  togglePref(Preferences::ShowEntityClassnames);
}

void MapViewBase::toggleShowGroupBounds()
{
  togglePref(Preferences::ShowGroupBounds);
}

void MapViewBase::toggleShowBrushEntityBounds()
{
  togglePref(Preferences::ShowBrushEntityBounds);
}

void MapViewBase::toggleShowPointEntityBounds()
{
  togglePref(Preferences::ShowPointEntityBounds);
}

void MapViewBase::toggleShowPointEntities()
{
  togglePref(Preferences::ShowPointEntities);
}

void MapViewBase::toggleShowPointEntityModels()
{
  togglePref(Preferences::ShowPointEntityModels);
}

void MapViewBase::toggleShowBrushes()
{
  togglePref(Preferences::ShowBrushes);
}

void MapViewBase::showMaterials()
{
  setPref(Preferences::FaceRenderMode, Preferences::FaceRenderModeTextured);
}

void MapViewBase::hideMaterials()
{
  setPref(Preferences::FaceRenderMode, Preferences::FaceRenderModeFlat);
}

void MapViewBase::hideFaces()
{
  setPref(Preferences::FaceRenderMode, Preferences::FaceRenderModeSkip);
}

void MapViewBase::toggleShadeFaces()
{
  togglePref(Preferences::ShadeFaces);
}

void MapViewBase::togglePreviewQuakeLighting()
{
  togglePref(Preferences::PreviewQuakeLighting);
}

void MapViewBase::toggleShowFog()
{
  togglePref(Preferences::ShowFog);
}

void MapViewBase::toggleShowEdges()
{
  togglePref(Preferences::ShowEdges);
}

void MapViewBase::showAllEntityLinks()
{
  setPref(Preferences::FaceRenderMode, Preferences::EntityLinkModeAll);
}

void MapViewBase::showTransitivelySelectedEntityLinks()
{
  setPref(Preferences::FaceRenderMode, Preferences::EntityLinkModeTransitive);
}

void MapViewBase::showDirectlySelectedEntityLinks()
{
  setPref(Preferences::FaceRenderMode, Preferences::EntityLinkModeDirect);
}

void MapViewBase::hideAllEntityLinks()
{
  setPref(Preferences::FaceRenderMode, Preferences::EntityLinkModeNone);
}

bool MapViewBase::eventFilter(QObject* watched, QEvent* event)
{
  if (
    (watched == m_transformHud || watched == m_transformHudText)
    && event->type() == QEvent::MouseButtonPress)
  {
    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (
      mouseEvent->button() == Qt::LeftButton && m_transformHud
      && m_transformHud->isVisible() && !m_toolBox.symmetryRelinkInProgress())
    {
      showTransformHudEditor();
      mouseEvent->accept();
      return true;
    }
  }

  return RenderView::eventFilter(watched, event);
}

bool MapViewBase::event(QEvent* event)
{
  if (event->type() == QEvent::WindowDeactivate)
  {
    cancelMouseDrag();
  }

  return RenderView::event(event);
}

void MapViewBase::focusInEvent(QFocusEvent* event)
{
  updateActionStates(); // enable/disable QShortcut's to reflect whether we have focus
                        // (needed because of QOpenGLWindow; see comment in
                        // createAndRegisterShortcut)
  updateModifierKeys();
  update();
  RenderView::focusInEvent(event);
}

void MapViewBase::focusOutEvent(QFocusEvent* event)
{
  clearModifierKeys();
  update();
  RenderView::focusOutEvent(event);
}

void MapViewBase::keyPressEvent(QKeyEvent* event)
{
  if (event->matches(QKeySequence::Undo) && m_toolBox.undoActiveToolStep())
  {
    event->accept();
    updateTransformHud();
    update();
    return;
  }

  RenderView::keyPressEvent(event);
}

void MapViewBase::resizeEvent(QResizeEvent* event)
{
  RenderView::resizeEvent(event);
  positionTransformHud();
}

ActionContext::Type MapViewBase::actionContext() const
{
  const auto& map = m_document.map();

  const auto viewContext = viewActionContext();
  auto toolContext =
    m_toolBox.assembleBrushToolActive() ? ActionContext::AssembleBrushTool
    : m_toolBox.clipToolActive()        ? ActionContext::ClipTool
    : m_toolBox.polygonCutToolActive() || m_toolBox.polygonSpawnToolActive()
      ? ActionContext::PolygonCutTool
    : m_toolBox.bezierBendToolActive()   ? ActionContext::BezierBendTool
    : m_toolBox.anyVertexToolActive()    ? ActionContext::AnyVertexTool
    : m_toolBox.controlPointToolActive() ? ActionContext::ControlPointTool
    : m_toolBox.rotateToolActive()       ? ActionContext::RotateTool
    : m_toolBox.sweepToolActive()        ? ActionContext::SweepTool
    : m_toolBox.scaleToolActive()        ? ActionContext::ScaleTool
    : m_toolBox.shearToolActive()        ? ActionContext::ShearTool
                                         : ActionContext::NoTool;
  const auto selectionContext =
    m_toolBox.selectionOwnedByTool()  ? ActionContext::SelectionOwnedByTool
    : map.selection().hasNodes()      ? ActionContext::NodeSelection
    : map.selection().hasBrushFaces() ? ActionContext::FaceSelection
                                      : ActionContext::NoSelection;
  if (m_toolBox.polygonCutToolActive())
  {
    toolContext |= ActionContext::PolygonCutMode;
  }
  if (m_toolBox.polygonSpawnToolActive())
  {
    toolContext |= ActionContext::PolygonSpawnMode;
  }
  if (m_toolBox.bevelToolActive())
  {
    toolContext |= ActionContext::BevelMode;
  }
  if (m_toolBox.insetToolActive())
  {
    toolContext |= ActionContext::InsetMode;
  }
  if (m_toolBox.spinToolActive())
  {
    toolContext |= ActionContext::SpinMode;
  }
  return viewContext | toolContext | selectionContext;
}

void MapViewBase::installActivationTracker(MapViewActivationTracker& activationTracker)
{
  activationTracker.addWindow(this);
}

bool MapViewBase::isCurrent() const
{
  return m_isCurrent;
}

MapViewBase* MapViewBase::firstMapViewBase()
{
  return this;
}

bool MapViewBase::cancelMouseDrag()
{
  return ToolBoxConnector::cancelDrag();
}

void MapViewBase::flashSelection()
{
  auto animation =
    std::make_unique<FlashSelectionAnimation>(m_document.mapRenderer(), this, 300);
  m_animationManager->runAnimation(std::move(animation), true);
}

void MapViewBase::refreshViews()
{
  update();
}

void MapViewBase::initializeGL()
{
  if (doInitializeGL())
  {
    const auto& glInfo = m_appController.glManager().glInfo();

    auto& logger = m_document.logger();
    logger.info() << "Renderer info: " << glInfo.renderer << " version " << glInfo.version
                  << " from " << glInfo.vendor;
    logger.info() << "Depth buffer bits: " << depthBits();
    logger.info() << "Multisampling "
                  << kdl::str_select(multisample(), "enabled", "disabled");
  }
}

bool MapViewBase::shouldRenderFocusIndicator() const
{
  return true;
}

void MapViewBase::renderContents(gl::Gl& gl)
{
  preRender();
  updateTransformHud();

  const auto& fontPath = pref(Preferences::RendererFontPath);
  const auto fontSize = static_cast<size_t>(pref(Preferences::RendererFontSize));
  const auto fontDescriptor = gl::FontDescriptor{fontPath, fontSize};

  const auto& map = m_document.map();
  const auto& grid = map.grid();

  auto renderContext =
    render::RenderContext{gl, renderMode(), camera(), fontManager(), shaderManager()};
  renderContext.setTimeSeconds(renderTimeSeconds());
  renderContext.setFilterMode(
    pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter));
  renderContext.setShowMaterials(
    pref(Preferences::FaceRenderMode) == Preferences::FaceRenderModeTextured);
  renderContext.setShowFaces(
    pref(Preferences::FaceRenderMode) != Preferences::FaceRenderModeSkip);
  renderContext.setShowEdges(pref(Preferences::ShowEdges));
  renderContext.setShadeFaces(pref(Preferences::ShadeFaces));
  renderContext.setPreviewLighting(
    pref(Preferences::PreviewQuakeLighting),
    pref(Preferences::PreviewQuakeLighting)
      ? collectPreviewQuakeLights(m_document.map(), camera().position())
      : std::vector<render::PreviewLight>{});
  renderContext.setShowPointEntities(pref(Preferences::ShowPointEntities));
  renderContext.setShowPointEntityModels(pref(Preferences::ShowPointEntityModels));
  renderContext.setShowEntityClassnames(pref(Preferences::ShowEntityClassnames));
  renderContext.setShowGroupBounds(pref(Preferences::ShowGroupBounds));
  renderContext.setShowBrushEntityBounds(pref(Preferences::ShowBrushEntityBounds));
  renderContext.setShowPointEntityBounds(pref(Preferences::ShowPointEntityBounds));
  renderContext.setShowFog(pref(Preferences::ShowFog));
  renderContext.setShowGrid(grid.visible());
  renderContext.setGridSize(grid.actualSize());
  renderContext.setDpiScale(static_cast<float>(window()->devicePixelRatioF()));
  renderContext.setSoftMapBounds(
    pref(Preferences::ShowSoftMapBounds)
      ? vm::bbox3f{softMapBounds(map).bounds.value_or(vm::bbox3d{})}
      : vm::bbox3f{});

  setupGL(renderContext);
  setRenderOptions(renderContext);

  auto renderBatch = render::RenderBatch{vboManager()};

  renderGrid(renderContext, renderBatch);
  renderMap(m_document.mapRenderer(), renderContext, renderBatch);
  renderTools(m_toolBox, renderContext, renderBatch);

  renderCoordinateSystem(renderContext, renderBatch);
  renderSoftWorldBounds(renderContext, renderBatch);
  renderPointFile(renderContext, renderBatch);
  renderPortalFile(renderContext, renderBatch);
  renderCompass(renderBatch);
  renderFPS(renderContext, renderBatch);

  renderBatch.render(renderContext);
  renderIntermissionPreview(gl);
}

void MapViewBase::renderIntermissionPreview(gl::Gl& gl)
{
  auto* entityNode = selectedIntermissionEntity();
  if (
    !entityNode || !m_intermissionPreviewFrame
    || !m_intermissionPreviewFrame->isVisible())
  {
    return;
  }

  const auto rect = intermissionPreviewRect();
  if (rect.width() <= 0 || rect.height() <= 0)
  {
    return;
  }

  const auto r = devicePixelRatioF();
  const auto viewport = gl::Camera::Viewport{
    rect.x(), height() - rect.y() - rect.height(), rect.width(), rect.height()};

  auto previewCamera = gl::PerspectiveCamera{
    pref(Preferences::CameraFov),
    camera().nearPlane(),
    camera().farPlane(),
    viewport,
    vm::vec3f{entityNode->entity().origin()},
    intermissionPreviewDirection(entityNode->entity()),
    intermissionPreviewUp(entityNode->entity())};

  auto renderContext = render::RenderContext{
    gl, render::RenderMode::Render3D, previewCamera, fontManager(), shaderManager()};
  renderContext.setTimeSeconds(renderTimeSeconds());
  renderContext.setFilterMode(
    pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter));
  renderContext.setShowMaterials(
    pref(Preferences::FaceRenderMode) == Preferences::FaceRenderModeTextured);
  renderContext.setShowFaces(
    pref(Preferences::FaceRenderMode) != Preferences::FaceRenderModeSkip);
  renderContext.setShowEdges(pref(Preferences::ShowEdges));
  renderContext.setShadeFaces(pref(Preferences::ShadeFaces));
  renderContext.setPreviewLighting(
    pref(Preferences::PreviewQuakeLighting),
    pref(Preferences::PreviewQuakeLighting)
      ? collectPreviewQuakeLights(m_document.map(), previewCamera.position())
      : std::vector<render::PreviewLight>{});
  renderContext.setShowPointEntities(false);
  renderContext.setShowPointEntityModels(false);
  renderContext.setShowEntityClassnames(false);
  renderContext.setShowGroupBounds(false);
  renderContext.setShowBrushEntityBounds(false);
  renderContext.setShowPointEntityBounds(false);
  renderContext.setShowFog(pref(Preferences::ShowFog));
  renderContext.setShowGrid(false);
  renderContext.setGridSize(m_document.map().grid().actualSize());
  renderContext.setDpiScale(static_cast<float>(window()->devicePixelRatioF()));
  renderContext.setHideSelection();

  gl.pushAttrib(
    GL_VIEWPORT_BIT | GL_ENABLE_BIT | GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT
    | GL_DEPTH_BUFFER_BIT);
  setupGL(renderContext);
  gl.enable(GL_SCISSOR_TEST);
  gl.scissor(
    static_cast<GLint>(std::round(double(viewport.x) * r)),
    static_cast<GLint>(std::round(double(viewport.y) * r)),
    static_cast<GLsizei>(std::round(double(viewport.width) * r)),
    static_cast<GLsizei>(std::round(double(viewport.height) * r)));
  gl.clearColor(0.02f, 0.023f, 0.026f, 1.0f);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  auto renderBatch = render::RenderBatch{vboManager()};
  m_document.mapRenderer().render(renderContext, renderBatch);
  renderBatch.render(renderContext);
  gl.popAttrib();
}

void MapViewBase::preRender() {}

void MapViewBase::renderGrid(render::RenderContext&, render::RenderBatch&) {}

void MapViewBase::setupGL(render::RenderContext& context)
{
  auto& gl = context.gl();

  const auto& viewport = context.camera().viewport();
  const auto r = devicePixelRatioF();
  const auto x = static_cast<int>(viewport.x * r);
  const auto y = static_cast<int>(viewport.y * r);
  const auto width = static_cast<int>(viewport.width * r);
  const auto height = static_cast<int>(viewport.height * r);
  gl.viewport(x, y, width, height);

  if (pref(Preferences::EnableMSAA))
  {
    gl.enable(GL_MULTISAMPLE);
  }
  else
  {
    gl.disable(GL_MULTISAMPLE);
  }
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl.shadeModel(GL_SMOOTH);
}

void MapViewBase::renderCoordinateSystem(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch)
{
  if (pref(Preferences::ShowAxes))
  {
    const auto& map = m_document.map();
    const auto& worldBounds = map.worldBounds();

    auto renderService = render::RenderService{renderContext, renderBatch};
    renderService.renderCoordinateSystem(vm::bbox3f{worldBounds});
  }
}

void MapViewBase::renderSoftWorldBounds(render::RenderContext&, render::RenderBatch&) {}

void MapViewBase::renderPointFile(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch)
{
  if (const auto* pointFile = m_document.pointTrace())
  {
    auto renderService = render::RenderService{renderContext, renderBatch};
    renderService.setForegroundColor(pref(Preferences::PointFileColor));
    renderService.renderLineStrip(pointFile->points());
  }
}

void MapViewBase::renderPortalFile(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch)
{
  if (!m_portalFileRenderer)
  {
    validatePortalFileRenderer(renderContext);
    contract_assert(m_portalFileRenderer);
  }
  renderBatch.add(m_portalFileRenderer.get());
}

void MapViewBase::invalidatePortalFileRenderer()
{
  m_portalFileRenderer = nullptr;
}

void MapViewBase::validatePortalFileRenderer(render::RenderContext&)
{
  contract_pre(m_portalFileRenderer == nullptr);

  m_portalFileRenderer = std::make_unique<render::PrimitiveRenderer>();

  if (const auto* portals = m_document.portals())
  {
    for (const auto& portal : *portals)
    {
      m_portalFileRenderer->renderFilledPolygon(
        pref(Preferences::PortalFileFillColor),
        render::PrimitiveRendererOcclusionPolicy::Hide,
        render::PrimitiveRendererCullingPolicy::ShowBackfaces,
        portal.vertices());

      const auto lineWidth = 4.0f;
      m_portalFileRenderer->renderPolygon(
        pref(Preferences::PortalFileBorderColor),
        lineWidth,
        render::PrimitiveRendererOcclusionPolicy::Hide,
        portal.vertices());
    }
  }
}

void MapViewBase::renderCompass(render::RenderBatch& renderBatch)
{
  if (m_compass)
  {
    m_compass->render(renderBatch);
  }
}

void MapViewBase::renderFPS(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch)
{
  if (pref(Preferences::ShowFPS))
  {
    auto renderService = render::RenderService{renderContext, renderBatch};
    renderService.renderHeadsUp(m_currentFPS);
  }
}

void MapViewBase::processEvent(const KeyEvent& event)
{
  ToolBoxConnector::processEvent(event);
}

void MapViewBase::processEvent(const MouseEvent& event)
{
  ToolBoxConnector::processEvent(event);
}

void MapViewBase::processEvent(const ScrollEvent& event)
{
  ToolBoxConnector::processEvent(event);
}

void MapViewBase::processEvent(const GestureEvent& event)
{
  ToolBoxConnector::processEvent(event);
}

void MapViewBase::processEvent(const CancelEvent& event)
{
  ToolBoxConnector::processEvent(event);
}

void MapViewBase::doShowPopupMenu()
{
  // We process input events during paint event processing, but we cannot show a popup
  // menu during paint processing, so we enqueue an event for later.
  QMetaObject::invokeMethod(this, "showPopupMenuLater", Qt::QueuedConnection);
}

void MapViewBase::showPopupMenuLater()
{
  beforePopupMenu();

  auto& map = m_document.map();
  const auto& nodes = map.selection().nodes;
  auto* newBrushParent = findNewParentEntityForBrushes(nodes);
  auto* currentGroup = map.editorContext().currentGroup();
  auto* newGroup = findNewGroupForObjects(nodes);
  auto* mergeGroup = findGroupToMergeGroupsInto(map.selection());

  auto* mapWindow = dynamic_cast<MapWindow*>(window());

  auto menu = QMenu{};
  const auto addMainMenuAction = [&](const auto& path) -> QAction* {
    auto* groupAction = mapWindow->findAction(path);
    contract_assert(groupAction);

    menu.addAction(groupAction);
    return groupAction;
  };

  addMainMenuAction("Menu/Edit/Group");
  addMainMenuAction("Menu/Edit/Ungroup");

  auto* mergeGroupAction = menu.addAction(
    mergeGroup
      ? tr("Merge Groups into %1").arg(QString::fromStdString(mergeGroup->name()))
      : tr("Merge Groups"),
    this,
    &MapViewBase::mergeSelectedGroups);
  mergeGroupAction->setEnabled(canMergeGroups());

  addMainMenuAction("Menu/Edit/Rename Groups");

  if (newGroup && canReparentNodes(nodes, newGroup))
  {
    menu.addAction(
      tr("Add Objects to Group %1").arg(QString::fromStdString(newGroup->name())),
      this,
      &MapViewBase::addSelectedObjectsToGroup);
  }
  if (currentGroup && map.selection().hasNodes())
  {
    menu.addAction(
      tr("Remove Objects from Group %1")
        .arg(QString::fromStdString(currentGroup->name())),
      this,
      &MapViewBase::removeSelectedObjectsFromGroup);
  }
  menu.addSeparator();

  // Linked group operations

  addMainMenuAction("Menu/Edit/Create Linked Duplicate");
  addMainMenuAction("Menu/Edit/Select Linked Groups");
  addMainMenuAction("Menu/Edit/Separate Linked Groups");
  addMainMenuAction("Menu/Edit/Extract Linked Groups");
  menu.addSeparator();

  // Layer operations

  const auto selectedObjectLayers = mdl::collectContainingLayersUserSorted(nodes);

  auto* moveSelectionTo = menu.addMenu(tr("Move to Layer"));
  for (auto* layerNode : map.worldNode().allLayersUserSorted())
  {
    auto* action = moveSelectionTo->addAction(
      QString::fromStdString(layerNode->name()), this, [&map, layerNode] {
        moveSelectedNodesToLayer(map, layerNode);
      });
    action->setEnabled(canMoveSelectedNodesToLayer(map, layerNode));
  }

  const auto moveSelectionToItems = moveSelectionTo->actions();
  moveSelectionTo->setEnabled(
    std::ranges::any_of(
      moveSelectionToItems, [](QAction* action) { return action->isEnabled(); }));

  if (selectedObjectLayers.size() == 1u)
  {
    auto* layerNode = selectedObjectLayers[0];
    auto* action = menu.addAction(
      tr("Make Layer %1 Active").arg(QString::fromStdString(layerNode->name())),
      this,
      [&map, layerNode]() { setCurrentLayer(map, layerNode); });
    action->setEnabled(canSetCurrentLayer(map, layerNode));
  }
  else
  {
    auto* makeLayerActive = menu.addMenu(tr("Make Layer Active"));
    for (auto* layerNode : selectedObjectLayers)
    {
      auto* action = makeLayerActive->addAction(
        QString::fromStdString(layerNode->name()), this, [&map, layerNode]() {
          setCurrentLayer(map, layerNode);
        });
      action->setEnabled(canSetCurrentLayer(map, layerNode));
    }
    if (makeLayerActive->isEmpty())
    {
      makeLayerActive->setDisabled(true);
    }
  }

  auto* hideLayersAction =
    menu.addAction(tr("Hide Layers"), this, [&map, selectedObjectLayers]() {
      hideLayers(map, selectedObjectLayers);
    });
  hideLayersAction->setEnabled(canHideLayers(selectedObjectLayers));
  auto* isolateLayersAction =
    menu.addAction(tr("Isolate Layers"), this, [&map, selectedObjectLayers]() {
      isolateLayers(map, selectedObjectLayers);
    });
  isolateLayersAction->setEnabled(canIsolateLayers(map, selectedObjectLayers));
  auto* selectAllInLayersAction =
    menu.addAction(tr("Select All in Layers"), this, [&map, selectedObjectLayers]() {
      selectAllInLayers(map, selectedObjectLayers);
    });
  selectAllInLayersAction->setEnabled(canSelectAllInLayers(map, selectedObjectLayers));

  menu.addSeparator();

  if (map.selection().hasOnlyBrushes())
  {
    auto* moveToWorldAction =
      menu.addAction(tr("Make Structural"), this, &MapViewBase::makeStructural);
    moveToWorldAction->setEnabled(canMakeStructural());

    const auto isEntity = newBrushParent->accept(
      kdl::overload(
        [](const mdl::WorldNode&) { return false; },
        [](const mdl::LayerNode&) { return false; },
        [](const mdl::GroupNode&) { return false; },
        [](const mdl::EntityNode&) { return true; },
        [](const mdl::BrushNode&) { return false; },
        [](const mdl::PatchNode&) { return false; }));

    if (isEntity)
    {
      menu.addAction(
        tr("Move Brushes to Entity %1")
          .arg(QString::fromStdString(newBrushParent->name())),
        this,
        &MapViewBase::moveSelectedBrushesToEntity);
    }
  }

  menu.addSeparator();

  using namespace mdl::HitFilters;
  const auto& hit = pickResult().first(type(mdl::BrushNode::BrushHitType));
  const auto faceHandle = mdl::hitToFaceHandle(hit);
  if (faceHandle)
  {
    const auto* material = faceHandle->face().material();
    menu.addAction(
      tr("Reveal %1 in Material Browser")
        .arg(QString::fromStdString(faceHandle->face().attributes().materialName())),
      mapWindow,
      [=] { mapWindow->revealMaterial(material); });

    menu.addAction(tr("Copy Material Name"), mapWindow, [=] {
      auto* clipboard = QApplication::clipboard();
      clipboard->setText(
        QString::fromStdString(faceHandle->face().attributes().materialName()));
    });

    menu.addSeparator();
  }

  menu.addMenu(makeEntityGroupsMenu(mdl::EntityDefinitionType::Point));
  menu.addMenu(makeEntityGroupsMenu(mdl::EntityDefinitionType::Brush));

  menu.exec(QCursor::pos());

  // Generate a synthetic mouse move event to update the mouse position after the popup
  // menu closes.
  const auto screenPos = QCursor::pos();
  const auto windowPos = window()->mapFromGlobal(screenPos);
  const auto localPos = mapFromGlobal(screenPos);
  auto mouseEvent = QMouseEvent(
    QEvent::MouseMove,
    localPos,
    windowPos,
    screenPos,
    Qt::NoButton,
    Qt::NoButton,
    Qt::NoModifier,
    Qt::MouseEventSynthesizedByApplication);
  mouseMoveEvent(&mouseEvent);
}

void MapViewBase::beforePopupMenu() {}

/**
 * Forward drag and drop events from QWidget to ToolBoxConnector
 */
void MapViewBase::dragEnterEvent(QDragEnterEvent* dragEnterEvent)
{
  if (dragEnter(
        static_cast<float>(dragEnterEvent->position().x()),
        static_cast<float>(dragEnterEvent->position().y()),
        dragEnterEvent->mimeData()->text().toStdString()))
  {
    dragEnterEvent->acceptProposedAction();
  }
}

void MapViewBase::dragLeaveEvent(QDragLeaveEvent*)
{
  dragLeave();
}

void MapViewBase::dragMoveEvent(QDragMoveEvent* dragMoveEvent)
{
  dragMove(
    static_cast<float>(dragMoveEvent->position().x()),
    static_cast<float>(dragMoveEvent->position().y()),
    dragMoveEvent->mimeData()->text().toStdString());
  dragMoveEvent->acceptProposedAction();
}

void MapViewBase::dropEvent(QDropEvent* dropEvent)
{
  dragDrop(
    static_cast<float>(dropEvent->position().x()),
    static_cast<float>(dropEvent->position().y()),
    dropEvent->mimeData()->text().toStdString());
  dropEvent->acceptProposedAction();
}

QMenu* MapViewBase::makeEntityGroupsMenu(const mdl::EntityDefinitionType type)
{
  auto* menu = new QMenu{};

  switch (type)
  {
  case mdl::EntityDefinitionType::Point:
    menu->setTitle(tr("Create Point Entity"));
    break;
  case mdl::EntityDefinitionType::Brush:
    menu->setTitle(tr("Create Brush Entity"));
    break;
  }

  const auto enableMakeBrushEntity = canCreateBrushEntity();

  const auto& map = m_document.map();
  for (const auto& group : map.entityDefinitionManager().groups())
  {
    auto creatableDefinitions =
      filterAndSort(group.definitions, type, mdl::EntityDefinitionSortOrder::Name)
      | std::views::filter([](const auto* d) {
          return !kdl::cs::str_is_equal(
            d->name, mdl::EntityPropertyValues::WorldspawnClassname);
        });

    if (!std::ranges::empty(creatableDefinitions))
    {
      const auto groupName = QString::fromStdString(displayName(group));
      auto* groupMenu = new QMenu{groupName};

      for (const auto* definition : creatableDefinitions)
      {
        const auto label =
          QString::fromStdString(std::string{mdl::getShortName(*definition)});
        QAction* action = nullptr;

        switch (type)
        {
        case mdl::EntityDefinitionType::Point:
          action = groupMenu->addAction(
            label, this, qOverload<>(&MapViewBase::createPointEntity));
          break;
        case mdl::EntityDefinitionType::Brush:
          action = groupMenu->addAction(
            label, this, qOverload<>(&MapViewBase::createBrushEntity));
          action->setEnabled(enableMakeBrushEntity);
          break;
        }

        action->setData(QVariant::fromValue(QString::fromStdString(definition->name)));
      }

      menu->addMenu(groupMenu);
    }
  }

  return menu;
}

void MapViewBase::addSelectedObjectsToGroup()
{
  auto& map = m_document.map();
  const auto nodes = map.selection().nodes;

  auto* newGroup = findNewGroupForObjects(nodes);
  contract_assert(newGroup != nullptr);

  auto transaction = mdl::Transaction{map, "Add Objects to Group"};
  reparentNodes(nodes, newGroup, true);
  deselectAll(map);
  selectNodes(map, {newGroup});
  transaction.commit();
}

void MapViewBase::removeSelectedObjectsFromGroup()
{
  auto& map = m_document.map();
  const auto& editorContext = map.editorContext();

  const auto nodes = map.selection().nodes;
  auto* currentGroup = editorContext.currentGroup();
  contract_assert(currentGroup);

  auto transaction = mdl::Transaction{map, "Remove Objects from Group"};
  reparentNodes(nodes, editorContext.currentLayer(), true);

  while (editorContext.currentGroup())
  {
    closeGroup(map);
  }
  selectNodes(map, nodes);
  transaction.commit();
}

mdl::Node* MapViewBase::findNewGroupForObjects(const std::vector<mdl::Node*>& nodes) const
{
  using namespace mdl::HitFilters;

  const auto hits = pickResult().all(type(mdl::nodeHitType()));
  if (!hits.empty())
  {
    auto* newGroup = mdl::findOutermostClosedGroup(mdl::hitToNode(hits.front()));
    if (newGroup && canReparentNodes(nodes, newGroup))
    {
      return newGroup;
    }
  }
  return nullptr;
}

void MapViewBase::mergeSelectedGroups()
{
  auto& map = m_document.map();
  auto* newGroup = findGroupToMergeGroupsInto(map.selection());
  contract_assert(newGroup != nullptr);

  auto transaction = mdl::Transaction{map, "Merge Groups"};
  mergeSelectedGroupsWithGroup(map, newGroup);
  transaction.commit();
}

mdl::GroupNode* MapViewBase::findGroupToMergeGroupsInto(
  const mdl::Selection& selection) const
{
  using namespace mdl::HitFilters;

  if (!(selection.hasOnlyGroups() && selection.groups.size() >= 2))
  {
    return nullptr;
  }

  const auto hits = pickResult().all(type(mdl::nodeHitType()));
  if (!hits.empty())
  {
    if (auto* mergeTarget = findOutermostClosedGroup(mdl::hitToNode(hits.front())))
    {
      if (std::ranges::all_of(selection.nodes, [&](const auto* node) {
            return node == mergeTarget || canReparentNode(node, mergeTarget);
          }))
      {
        return mergeTarget;
      }
    }
  }

  return nullptr;
}

bool MapViewBase::canReparentNode(const mdl::Node* node, const mdl::Node* newParent) const
{
  return newParent != node && newParent != node->parent()
         && newParent->canAddChild(*node);
}

void MapViewBase::moveSelectedBrushesToEntity()
{
  auto& map = m_document.map();
  const auto nodes = map.selection().nodes;
  auto* newParent = findNewParentEntityForBrushes(nodes);
  contract_assert(newParent);

  auto transaction =
    mdl::Transaction{map, "Move " + kdl::str_plural(nodes.size(), "Brush", "Brushes")};
  reparentNodes(nodes, newParent, false);

  deselectAll(map);
  selectNodes(map, nodes);
  transaction.commit();
}

mdl::Node* MapViewBase::findNewParentEntityForBrushes(
  const std::vector<mdl::Node*>& nodes) const
{
  using namespace mdl::HitFilters;

  const auto& map = m_document.map();
  const auto& hit = pickResult().first(type(mdl::BrushNode::BrushHitType));
  if (const auto faceHandle = mdl::hitToFaceHandle(hit))
  {
    auto* brush = faceHandle->node();
    auto* newParent = brush->entity();

    if (newParent && newParent != &map.worldNode() && canReparentNodes(nodes, newParent))
    {
      return newParent;
    }
  }

  if (!nodes.empty())
  {
    auto* lastNode = nodes.back();

    if (auto* group = mdl::findContainingGroup(lastNode))
    {
      return group;
    }

    if (auto* layer = mdl::findContainingLayer(lastNode))
    {
      return layer;
    }
  }

  return map.editorContext().currentLayer();
}

bool MapViewBase::canReparentNodes(
  const std::vector<mdl::Node*>& nodes, const mdl::Node* newParent) const
{
  return std::ranges::any_of(
    nodes, [&](const auto* node) { return canReparentNode(node, newParent); });
}

/**
 * Return the given nodes, but replace all entity brushes with the parent entity (with
 * duplicates removed).
 */
static std::vector<mdl::Node*> collectEntitiesForNodes(
  const std::vector<mdl::Node*>& selectedNodes, const mdl::WorldNode& worldNode)
{
  auto result = std::vector<mdl::Node*>{};
  const auto addNode = [&](auto&& thisLambda, auto& node) {
    if (node.entity() == &worldNode)
    {
      result.push_back(&node);
    }
    else
    {
      node.visitParent(thisLambda);
    }
  };

  mdl::Node::visitAll(
    selectedNodes,
    kdl::overload(
      [](mdl::WorldNode&) {},
      [](mdl::LayerNode&) {},
      [&](mdl::GroupNode& groupNode) { result.push_back(&groupNode); },
      [&](mdl::EntityNode& entityNode) { result.push_back(&entityNode); },
      [&](
        auto&& thisLambda, mdl::BrushNode& brushNode) { addNode(thisLambda, brushNode); },
      [&](auto&& thisLambda, mdl::PatchNode& patchNode) {
        addNode(thisLambda, patchNode);
      }));
  return kdl::vec_sort_and_remove_duplicates(std::move(result));
}

void MapViewBase::reparentNodes(
  const std::vector<mdl::Node*>& nodes, mdl::Node* newParent, const bool preserveEntities)
{
  contract_pre(newParent != nullptr);

  auto& map = m_document.map();
  const auto inputNodes =
    preserveEntities ? collectEntitiesForNodes(nodes, map.worldNode()) : nodes;

  const auto reparentableNodes = collectReparentableNodes(inputNodes, newParent);
  contract_assert(!reparentableNodes.empty());

  const auto name = "Move "
                    + kdl::str_plural(reparentableNodes.size(), "Object", "Objects")
                    + " to " + newParent->name();

  auto transaction = mdl::Transaction{map, name};
  deselectAll(map);
  if (!mdl::reparentNodes(map, {{newParent, reparentableNodes}}))
  {
    transaction.cancel();
    return;
  }
  selectNodes(map, reparentableNodes);
  transaction.commit();
}

std::vector<mdl::Node*> MapViewBase::collectReparentableNodes(
  const std::vector<mdl::Node*>& nodes, const mdl::Node* newParent) const
{
  return nodes | std::views::filter([&](const auto* node) {
           return newParent != node && newParent != node->parent()
                  && !newParent->isDescendantOf(*node);
         })
         | kdl::ranges::to<std::vector>();
}

bool MapViewBase::canMergeGroups() const
{
  const auto& map = m_document.map();
  auto* mergeGroup = findGroupToMergeGroupsInto(map.selection());
  return mergeGroup;
}

bool MapViewBase::canMakeStructural() const
{
  const auto& map = m_document.map();
  if (map.selection().hasOnlyBrushes())
  {
    const auto& brushes = map.selection().brushes;
    return std::ranges::any_of(brushes, [&](const auto* brush) {
      return brush->hasAnyTag() || brush->entity() != &map.worldNode()
             || brush->anyFaceHasAnyTag();
    });
  }
  return false;
}

} // namespace tb::ui
