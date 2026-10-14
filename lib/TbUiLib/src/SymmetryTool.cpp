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

#include "ui/SymmetryTool.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

#include "Logger.h"
#include "Uuid.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"
#include "render/RenderService.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/overload.h"
#include "kd/set_temp.h"

#include "vm/mat_ext.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>

namespace tb::ui
{
namespace
{

constexpr auto DefaultMode = SymmetryTool::Mode::TwoSides;
constexpr auto SymmetryColor = RgbaF{1.0f, 0.05f, 0.72f, 1.0f};
constexpr auto RelinkEpsilon = 0.1;
constexpr auto MaxRelinkCandidateCount = size_t{8192};
constexpr auto MaxRelinkBoundsMatches = size_t{16};

int modeToIndex(const SymmetryTool::Mode mode)
{
  switch (mode)
  {
  case SymmetryTool::Mode::Off:
    return 0;
  case SymmetryTool::Mode::TwoSides:
    return 1;
  case SymmetryTool::Mode::FourSides:
    return 2;
  case SymmetryTool::Mode::EightSides:
    return 3;
  }

  return 0;
}

SymmetryTool::Mode indexToMode(const int index)
{
  switch (index)
  {
  case 1:
    return SymmetryTool::Mode::TwoSides;
  case 2:
    return SymmetryTool::Mode::FourSides;
  case 3:
    return SymmetryTool::Mode::EightSides;
  default:
    return SymmetryTool::Mode::Off;
  }
}

std::optional<vm::vec3d> horizontalDirection(vm::vec3d value)
{
  value = vm::vec3d{value.x(), value.y(), 0.0};
  if (vm::squared_length(value) <= vm::Cd::almost_zero())
  {
    return std::nullopt;
  }

  auto direction = vm::normalize(value);
  if (
    direction.x() < 0.0
    || (vm::is_zero(direction.x(), vm::Cd::almost_zero()) && direction.y() < 0.0))
  {
    direction = -direction;
  }
  return direction;
}

std::optional<vm::vec3d> dominantBrushDirection(const mdl::Brush& brush)
{
  auto result = std::optional<vm::vec3d>{};
  auto bestLength = 0.0;
  for (const auto* edge : brush.edges())
  {
    const auto edgeDirection = horizontalDirection(edge->vector());
    if (!edgeDirection)
    {
      continue;
    }

    const auto edgeVector = vm::vec3d{edge->vector().x(), edge->vector().y(), 0.0};
    const auto length = vm::squared_length(edgeVector);
    if (length > bestLength)
    {
      bestLength = length;
      result = edgeDirection;
    }
  }
  return result;
}

bool nearlyEqual(const double lhs, const double rhs)
{
  return std::abs(lhs - rhs) <= RelinkEpsilon;
}

bool nearlyEqual(const vm::vec3d& lhs, const vm::vec3d& rhs)
{
  return nearlyEqual(lhs.x(), rhs.x()) && nearlyEqual(lhs.y(), rhs.y())
         && nearlyEqual(lhs.z(), rhs.z());
}

bool nearlyEqual(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  return nearlyEqual(lhs.min, rhs.min) && nearlyEqual(lhs.max, rhs.max);
}

std::vector<vm::vec3d> sortedVertexPositions(const mdl::Brush& brush)
{
  auto result = brush.vertexPositions();
  std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
    if (!nearlyEqual(lhs.x(), rhs.x()))
    {
      return lhs.x() < rhs.x();
    }
    if (!nearlyEqual(lhs.y(), rhs.y()))
    {
      return lhs.y() < rhs.y();
    }
    return lhs.z() < rhs.z();
  });
  return result;
}

class SymmetryToolPage : public QWidget
{
private:
  SymmetryTool& m_tool;
  NotifierConnection m_notifierConnection;
  QComboBox* m_mode = nullptr;
  QCheckBox* m_rotateCopies = nullptr;

public:
  explicit SymmetryToolPage(SymmetryTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  void createGui()
  {
    auto* label = new QLabel{tr("Symmetry")};

    m_mode = new QComboBox{};
    m_mode->addItem(tr("Off"));
    m_mode->addItem(tr("2 sides"));
    m_mode->addItem(tr("4 sides"));
    m_mode->addItem(tr("8 sides"));
    m_mode->setCurrentIndex(modeToIndex(m_tool.mode()));

    m_rotateCopies = new QCheckBox{tr("Rotate copies")};
    m_rotateCopies->setChecked(m_tool.rotateCopies());

    connect(
      m_mode,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int index) { m_tool.setMode(indexToMode(index)); });
    connect(m_rotateCopies, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setRotateCopies(checked);
    });

    m_notifierConnection +=
      m_tool.symmetryDidChangeNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolActivatedNotifier.connect([&](auto&) { updateControls(); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addWidget(m_mode, 0, Qt::AlignVCenter);
    layout->addWidget(m_rotateCopies, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    setLayout(layout);
    updateControls();
  }

  void updateControls()
  {
    const auto index = modeToIndex(m_tool.mode());
    if (m_mode->currentIndex() != index)
    {
      m_mode->setCurrentIndex(index);
    }
    if (m_rotateCopies->isChecked() != m_tool.rotateCopies())
    {
      m_rotateCopies->setChecked(m_tool.rotateCopies());
    }
  }
};

} // namespace

SymmetryTool::SymmetryTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
  , m_alive{std::make_shared<bool>(true)}
{
}

SymmetryTool::~SymmetryTool()
{
  *m_alive = false;
}

SymmetryTool::Mode SymmetryTool::mode() const
{
  return m_mode;
}

void SymmetryTool::setMode(const Mode mode)
{
  const auto oldMode = m_mode;
  if (mode == Mode::Off)
  {
    auto* source = sourceNode();
    clearSource();
    if (source && source->parent())
    {
      mdl::deselectNodes(m_document.map(), {source});
    }
    notifyChange();
    symmetryDidRequestDeactivateNotifier(*this);
    return;
  }

  if (mode != Mode::Off && !sourceNode())
  {
    if (!canAttachToSelection())
    {
      return;
    }
    setSourceNode(*m_document.map().selection().nodes.front());
  }

  if (m_mode == mode)
  {
    return;
  }

  m_mode = mode;
  pruneMissingHistoryLinks();
  if (m_masterToGeneratedLinkIds.empty())
  {
    relinkExistingMirroredNodes();
  }
  refreshKnownLinkIds();

  auto linkedSideCountMatches = std::ranges::all_of(
    m_masterToGeneratedLinkIds,
    [&](const auto& entry) { return entry.second.size() == sideCount() - 1u; });
  if (
    oldMode != Mode::Off && oldMode != mode && !m_masterToGeneratedLinkIds.empty()
    && !linkedSideCountMatches)
  {
    auto previousLinks = std::move(m_masterToGeneratedLinkIds);
    m_masterToGeneratedLinkIds.clear();
    relinkExistingMirroredNodes();
    linkedSideCountMatches =
      !m_masterToGeneratedLinkIds.empty()
      && std::ranges::all_of(m_masterToGeneratedLinkIds, [&](const auto& entry) {
           return entry.second.size() == sideCount() - 1u;
         });

    if (!linkedSideCountMatches)
    {
      m_masterToGeneratedLinkIds = std::move(previousLinks);
      rebuildMirroredMasters();
    }
  }
  refreshKnownLinkIds();
  notifyChange();
}

bool SymmetryTool::rotateCopies() const
{
  return m_rotateCopies;
}

void SymmetryTool::setRotateCopies(const bool rotateCopies)
{
  if (m_rotateCopies == rotateCopies)
  {
    return;
  }

  m_rotateCopies = rotateCopies;
  if (m_mode != Mode::Off && sourceNode())
  {
    rebuildMirroredMasters();
    refreshKnownLinkIds();
  }
  notifyChange();
}

void SymmetryTool::suspendMirroring()
{
  ++m_mirroringSuspended;
  m_touchedLinkIds.clear();
}

void SymmetryTool::resumeMirroring()
{
  if (m_mirroringSuspended == 0u)
  {
    return;
  }

  --m_mirroringSuspended;
  if (m_mirroringSuspended == 0u)
  {
    m_syncPending = false;
    refreshKnownLinkIds();
    m_touchedLinkIds.clear();
  }
}

bool SymmetryTool::hasSource() const
{
  return sourceNode() != nullptr;
}

bool SymmetryTool::modifierActive() const
{
  return sourceNode() != nullptr && sideCount() > 1;
}

bool SymmetryTool::relinkInProgress() const
{
  return m_relinkInProgress;
}

bool SymmetryTool::sourceSelected() const
{
  const auto* source = sourceNode();
  if (!source)
  {
    return false;
  }

  const auto& selectedNodes = m_document.map().selection().nodes;
  return std::ranges::find(selectedNodes, source) != std::end(selectedNodes);
}

bool SymmetryTool::canAttachToSelection() const
{
  if (sourceNode())
  {
    return false;
  }

  const auto& selection = m_document.map().selection();
  return selection.nodes.size() == 1 && !selection.hasBrushFaces();
}

void SymmetryTool::attachToSelection()
{
  if (!canAttachToSelection())
  {
    return;
  }

  bakeGeneratedNodes();
  setSourceNode(*m_document.map().selection().nodes.front());
  m_mode = DefaultMode;
  relinkExistingMirroredNodes();
  refreshKnownLinkIds();
  notifyChange();
}

void SymmetryTool::render(
  render::RenderContext& renderContext, render::RenderBatch& renderBatch) const
{
  auto* source = sourceNode();
  if (!modifierActive() || !source || !source->parent())
  {
    return;
  }

  const auto bounds = source->logicalBounds();
  const auto center = bounds.center();
  const auto size = bounds.size();
  const auto radius = std::max({128.0, size.x() * 3.0, size.y() * 3.0}) * 30.0;
  const auto verticalMargin = std::max(32.0, size.z() * 0.25) * 30.0;

  auto renderService = render::RenderService{renderContext, renderBatch};
  renderService.setForegroundColor(SymmetryColor);
  renderService.setLineWidth(2.0f);
  renderService.setShowOccludedObjects();

  const auto sides = sideCount();
  const auto baseAngle = sourceAngle(*source);
  for (size_t i = 0; i < sides; ++i)
  {
    const auto angle =
      baseAngle + vm::Cd::two_pi() * static_cast<double>(i) / static_cast<double>(sides);
    const auto direction = vm::vec3d{std::cos(angle), std::sin(angle), 0.0};
    renderService.renderLine(vm::vec3f{center}, vm::vec3f{center + direction * radius});
  }

  renderService.renderLine(
    vm::vec3f{vm::vec3d{center.x(), center.y(), bounds.min.z() - verticalMargin}},
    vm::vec3f{vm::vec3d{center.x(), center.y(), bounds.max.z() + verticalMargin}});
}

size_t SymmetryTool::sideCount() const
{
  return static_cast<size_t>(m_mode);
}

mdl::Node* SymmetryTool::sourceNode() const
{
  if (m_sourceLinkId.empty())
  {
    return nullptr;
  }

  auto& worldNode = m_document.map().worldNode();
  if (m_sourcePath)
  {
    if (auto* node = worldNode.resolvePath(*m_sourcePath))
    {
      if (linkIdForNode(*node) == m_sourceLinkId)
      {
        return node;
      }
    }
  }

  return findNodeByLinkId(m_sourceLinkId);
}

mdl::Node* SymmetryTool::findNodeByLinkId(const std::string& linkId) const
{
  if (linkId.empty())
  {
    return nullptr;
  }

  auto* result = static_cast<mdl::Node*>(nullptr);
  m_document.map().worldNode().accept(
    kdl::overload(
      [&](auto&& thisLambda, mdl::WorldNode& worldNode) {
        if (!result)
        {
          worldNode.visitChildren(thisLambda);
        }
      },
      [&](auto&& thisLambda, mdl::LayerNode& layerNode) {
        if (!result)
        {
          layerNode.visitChildren(thisLambda);
        }
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        if (groupNode.linkId() == linkId)
        {
          result = &groupNode;
          return;
        }
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        if (entityNode.linkId() == linkId)
        {
          result = &entityNode;
          return;
        }
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) {
        if (brushNode.linkId() == linkId)
        {
          result = &brushNode;
        }
      },
      [&](mdl::PatchNode& patchNode) {
        if (patchNode.linkId() == linkId)
        {
          result = &patchNode;
        }
      }));

  return result;
}

std::string SymmetryTool::linkIdForNode(const mdl::Node& node) const
{
  return node.accept(
    kdl::overload(
      [](const mdl::WorldNode&) { return std::string{}; },
      [](const mdl::LayerNode&) { return std::string{}; },
      [](const mdl::GroupNode& groupNode) { return groupNode.linkId(); },
      [](const mdl::EntityNode& entityNode) { return entityNode.linkId(); },
      [](const mdl::BrushNode& brushNode) { return brushNode.linkId(); },
      [](const mdl::PatchNode& patchNode) { return patchNode.linkId(); }));
}

void SymmetryTool::setSourceNode(mdl::Node& node)
{
  m_sourcePath = node.pathFrom(m_document.map().worldNode());
  m_sourceLinkId = linkIdForNode(node);
}

std::vector<mdl::Node*> SymmetryTool::collectMirrorNodes() const
{
  auto result = std::vector<mdl::Node*>{};
  m_document.map().worldNode().accept(
    kdl::overload(
      [&](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        result.push_back(&groupNode);
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        result.push_back(&entityNode);
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) { result.push_back(&brushNode); },
      [&](mdl::PatchNode& patchNode) { result.push_back(&patchNode); }));

  return result;
}

std::vector<mdl::Node*> SymmetryTool::collectRootNodes(
  std::vector<mdl::Node*> nodes) const
{
  auto result = std::vector<mdl::Node*>{};
  for (auto* node : nodes)
  {
    const auto hasNewAncestor = std::ranges::any_of(nodes, [&](const auto* other) {
      return other != node && other->isAncestorOf(*node);
    });
    if (!hasNewAncestor)
    {
      result.push_back(node);
    }
  }

  return result;
}

std::unordered_set<std::string> SymmetryTool::collectLinkIds() const
{
  auto result = std::unordered_set<std::string>{};
  for (const auto* node : collectMirrorNodes())
  {
    if (const auto linkId = linkIdForNode(*node); !linkId.empty())
    {
      result.insert(linkId);
    }
  }
  return result;
}

bool SymmetryTool::isGeneratedLinkId(const std::string& linkId) const
{
  return std::ranges::any_of(m_masterToGeneratedLinkIds, [&](const auto& entry) {
    const auto& generatedLinkIds = entry.second;
    return std::ranges::find(generatedLinkIds, linkId) != std::end(generatedLinkIds);
  });
}

std::optional<SymmetryTool::GeneratedSide> SymmetryTool::generatedSideForLinkId(
  const std::string& linkId) const
{
  for (const auto& [masterLinkId, generatedLinkIds] : m_masterToGeneratedLinkIds)
  {
    const auto it = std::ranges::find(generatedLinkIds, linkId);
    if (it != std::end(generatedLinkIds))
    {
      return GeneratedSide{
        masterLinkId,
        static_cast<size_t>(std::distance(std::begin(generatedLinkIds), it) + 1)};
    }
  }
  return std::nullopt;
}

std::vector<SymmetryTool::GeneratedSide> SymmetryTool::removedGeneratedSides(
  const std::unordered_set<std::string>& currentLinkIds) const
{
  auto result = std::vector<GeneratedSide>{};
  for (const auto& [masterLinkId, generatedLinkIds] : m_masterToGeneratedLinkIds)
  {
    for (size_t i = 0; i < generatedLinkIds.size(); ++i)
    {
      const auto& generatedLinkId = generatedLinkIds[i];
      if (
        !generatedLinkId.empty() && !currentLinkIds.contains(generatedLinkId)
        && m_touchedLinkIds.contains(generatedLinkId))
      {
        result.push_back(GeneratedSide{masterLinkId, i + 1u});
      }
    }
  }
  return result;
}

mdl::Node* SymmetryTool::findMirroredNode(
  const mdl::Node& masterNode,
  const size_t side,
  const bool rotateCopies,
  const std::unordered_set<std::string>& claimedLinkIds,
  const std::vector<mdl::Node*>& candidates) const
{
  const auto* source = sourceNode();
  if (!source)
  {
    return nullptr;
  }

  auto clone = std::unique_ptr<mdl::Node>{
    masterNode.cloneRecursively(m_document.map().worldBounds())};
  if (!transformNode(*clone, sideTransform(*source, side, rotateCopies)))
  {
    return nullptr;
  }

  const auto expectedBounds = clone->logicalBounds();
  auto boundsMatchCount = size_t{0};
  for (auto* candidate : candidates)
  {
    if (!candidate || candidate == &masterNode)
    {
      continue;
    }

    const auto linkId = linkIdForNode(*candidate);
    if (linkId.empty() || linkId == m_sourceLinkId || claimedLinkIds.contains(linkId))
    {
      continue;
    }

    if (!nearlyEqual(expectedBounds, candidate->logicalBounds()))
    {
      continue;
    }

    ++boundsMatchCount;
    if (boundsMatchCount > MaxRelinkBoundsMatches)
    {
      return nullptr;
    }

    if (nodesGeometricallyMatch(*clone, *candidate))
    {
      return candidate;
    }
  }

  return nullptr;
}

bool SymmetryTool::nodesGeometricallyMatch(
  const mdl::Node& lhs, const mdl::Node& rhs) const
{
  if (!nearlyEqual(lhs.logicalBounds(), rhs.logicalBounds()))
  {
    return false;
  }

  const auto* lhsBrush = dynamic_cast<const mdl::BrushNode*>(&lhs);
  const auto* rhsBrush = dynamic_cast<const mdl::BrushNode*>(&rhs);
  if (lhsBrush || rhsBrush)
  {
    if (!lhsBrush || !rhsBrush)
    {
      return false;
    }

    const auto lhsVertices = sortedVertexPositions(lhsBrush->brush());
    const auto rhsVertices = sortedVertexPositions(rhsBrush->brush());
    if (lhsVertices.size() != rhsVertices.size())
    {
      return false;
    }

    for (size_t i = 0; i < lhsVertices.size(); ++i)
    {
      if (!nearlyEqual(lhsVertices[i], rhsVertices[i]))
      {
        return false;
      }
    }
    return true;
  }

  const auto lhsIsGroup = dynamic_cast<const mdl::GroupNode*>(&lhs) != nullptr;
  const auto rhsIsGroup = dynamic_cast<const mdl::GroupNode*>(&rhs) != nullptr;
  const auto lhsIsEntity = dynamic_cast<const mdl::EntityNode*>(&lhs) != nullptr;
  const auto rhsIsEntity = dynamic_cast<const mdl::EntityNode*>(&rhs) != nullptr;
  const auto lhsIsPatch = dynamic_cast<const mdl::PatchNode*>(&lhs) != nullptr;
  const auto rhsIsPatch = dynamic_cast<const mdl::PatchNode*>(&rhs) != nullptr;
  return (lhsIsGroup && rhsIsGroup) || (lhsIsEntity && rhsIsEntity)
         || (lhsIsPatch && rhsIsPatch);
}

void SymmetryTool::rememberTouchedNodes(const std::vector<mdl::Node*>& nodes)
{
  if (m_ignoreNotifications || m_historyReplayActive)
  {
    return;
  }

  for (auto* node : nodes)
  {
    if (node)
    {
      rememberTouchedNode(*node);
    }
  }
}

void SymmetryTool::rememberTouchedNode(mdl::Node& node)
{
  if (const auto linkId = linkIdForNode(node); !linkId.empty())
  {
    m_touchedLinkIds.insert(linkId);
  }

  for (auto* child : node.children())
  {
    rememberTouchedNode(*child);
  }

  auto* parent = node.parent();
  while (parent)
  {
    if (const auto linkId = linkIdForNode(*parent); !linkId.empty())
    {
      m_touchedLinkIds.insert(linkId);
    }
    parent = parent->parent();
  }
}

void SymmetryTool::refreshKnownLinkIds()
{
  m_knownLinkIds = collectLinkIds();
}

SymmetryTool::StateSnapshot SymmetryTool::captureState() const
{
  return StateSnapshot{
    m_mode,
    m_sourcePath,
    m_sourceLinkId,
    m_masterToGeneratedLinkIds,
    m_knownLinkIds,
    m_rotateCopies};
}

void SymmetryTool::restoreState(const StateSnapshot& state)
{
  m_mode = state.mode;
  m_sourcePath = state.sourcePath;
  m_sourceLinkId = state.sourceLinkId;
  m_masterToGeneratedLinkIds = state.masterToGeneratedLinkIds;
  m_knownLinkIds = state.knownLinkIds;
  m_rotateCopies = state.rotateCopies;
  m_touchedLinkIds.clear();
  m_syncPending = false;

  pruneMissingHistoryLinks();
  refreshKnownLinkIds();
}

void SymmetryTool::pruneMissingHistoryLinks()
{
  for (auto it = std::begin(m_masterToGeneratedLinkIds);
       it != std::end(m_masterToGeneratedLinkIds);)
  {
    if (!findNodeByLinkId(it->first))
    {
      it = m_masterToGeneratedLinkIds.erase(it);
      continue;
    }

    auto& generatedLinkIds = it->second;
    generatedLinkIds.erase(
      std::remove_if(
        std::begin(generatedLinkIds),
        std::end(generatedLinkIds),
        [&](const auto& linkId) { return linkId.empty() || !findNodeByLinkId(linkId); }),
      std::end(generatedLinkIds));
    ++it;
  }
}

void SymmetryTool::recordHistoryEntry(
  std::string commandName, StateSnapshot before, StateSnapshot after)
{
  m_undoHistory.push_back(
    HistoryEntry{std::move(commandName), std::move(before), std::move(after)});
  m_redoHistory.clear();
}

bool SymmetryTool::restoreUndoHistoryEntry(const std::string& commandName)
{
  if (m_undoHistory.empty() || m_undoHistory.back().commandName != commandName)
  {
    return false;
  }

  auto entry = std::move(m_undoHistory.back());
  m_undoHistory.pop_back();
  restoreState(entry.before);
  m_redoHistory.push_back(std::move(entry));
  return true;
}

bool SymmetryTool::restoreRedoHistoryEntry(const std::string& commandName)
{
  if (m_redoHistory.empty() || m_redoHistory.back().commandName != commandName)
  {
    return false;
  }

  auto entry = std::move(m_redoHistory.back());
  m_redoHistory.pop_back();
  restoreState(entry.after);
  m_undoHistory.push_back(std::move(entry));
  return true;
}

void SymmetryTool::scheduleSyncMirroredNodes()
{
  if (m_mirroringSuspended > 0u)
  {
    return;
  }

  if (m_syncPending)
  {
    return;
  }

  m_syncPending = true;
  const auto alive = std::weak_ptr<bool>{m_alive};
  QTimer::singleShot(0, [this, alive]() {
    const auto aliveFlag = alive.lock();
    if (!aliveFlag || !*aliveFlag)
    {
      return;
    }

    if (!m_syncPending)
    {
      return;
    }

    m_syncPending = false;
    if (
      m_ignoreNotifications || m_mirroringSuspended > 0u || m_sourceLinkId.empty()
      || m_mode == Mode::Off)
    {
      return;
    }

    const auto* source = sourceNode();
    if (!source || !source->parent())
    {
      clearSource();
      notifyChange();
      return;
    }

    syncMirroredNodesWithUndoBatch();
    notifyChange();
  });
}

void SymmetryTool::syncMirroredNodesWithUndoBatch()
{
  auto& map = m_document.map();
  const auto* undoCommandName = map.undoCommandName();
  const auto commandName = undoCommandName ? *undoCommandName : std::string{};
  auto beforeState = captureState();
  const auto transactionStarted =
    !commandName.empty() && map.startTransactionWithLastCommand(commandName);

  syncMirroredNodes();
  auto afterState = captureState();

  if (transactionStarted)
  {
    const auto wasCollationEnabled = map.isCommandCollationEnabled();
    map.setIsCommandCollationEnabled(false);
    const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
    const auto transactionCommitted = map.commitTransaction();
    map.setIsCommandCollationEnabled(wasCollationEnabled);

    if (transactionCommitted)
    {
      recordHistoryEntry(commandName, std::move(beforeState), std::move(afterState));
    }
    else
    {
      map.logger().error() << "Could not commit symmetry undo batch";
      restoreState(beforeState);
    }
  }
}

void SymmetryTool::syncMirroredNodes()
{
  const auto* source = sourceNode();
  if (!source || sideCount() <= 1)
  {
    return;
  }

  const auto currentLinkIds = collectLinkIds();
  for (auto it = std::begin(m_masterToGeneratedLinkIds);
       it != std::end(m_masterToGeneratedLinkIds);)
  {
    if (!currentLinkIds.contains(it->first))
    {
      removeGeneratedNodes(it->second);
      it = m_masterToGeneratedLinkIds.erase(it);
    }
    else
    {
      ++it;
    }
  }

  auto newNodes = std::vector<mdl::Node*>{};
  for (auto* node : collectMirrorNodes())
  {
    const auto linkId = linkIdForNode(*node);
    if (
      !linkId.empty() && linkId != m_sourceLinkId && !m_knownLinkIds.contains(linkId)
      && !isGeneratedLinkId(linkId))
    {
      newNodes.push_back(node);
    }
  }
  newNodes = collectRootNodes(std::move(newNodes));

  const auto removedSides = removedGeneratedSides(currentLinkIds);
  if (!removedSides.empty() && !newNodes.empty())
  {
    if (replaceMasterFromGeneratedSide(removedSides.front(), newNodes))
    {
      refreshKnownLinkIds();
      m_touchedLinkIds.clear();
      return;
    }
  }
  if (!removedSides.empty() && newNodes.empty())
  {
    if (removeMasterFromGeneratedSide(removedSides.front()))
    {
      refreshKnownLinkIds();
      m_touchedLinkIds.clear();
      return;
    }
  }

  auto rebuiltFromGeneratedSide = false;
  for (const auto& linkId : m_touchedLinkIds)
  {
    if (const auto generatedSide = generatedSideForLinkId(linkId))
    {
      if (rebuildFromGeneratedSide(*generatedSide))
      {
        rebuiltFromGeneratedSide = true;
        break;
      }
    }
  }

  auto rebuiltTouchedMaster = rebuiltFromGeneratedSide;
  const auto sourceTouched = m_touchedLinkIds.contains(m_sourceLinkId);
  if (sourceTouched)
  {
    rebuildMirroredMasters();
    rebuiltTouchedMaster = true;
  }
  else
  {
    for (const auto& linkId : m_touchedLinkIds)
    {
      if (m_masterToGeneratedLinkIds.contains(linkId))
      {
        if (auto* masterNode = findNodeByLinkId(linkId))
        {
          rebuildMirroredMaster(*masterNode);
          rebuiltTouchedMaster = true;
        }
      }
    }
  }

  if (!newNodes.empty())
  {
    for (auto* newNode : newNodes)
    {
      m_masterToGeneratedLinkIds.try_emplace(linkIdForNode(*newNode));
      rebuildMirroredMaster(*newNode);
    }
  }
  else if (!rebuiltTouchedMaster)
  {
    rebuildMirroredMasters();
  }

  refreshKnownLinkIds();
  m_touchedLinkIds.clear();
}

void SymmetryTool::relinkExistingMirroredNodes()
{
  if (!sourceNode() || sideCount() <= 1 || !m_masterToGeneratedLinkIds.empty())
  {
    return;
  }

  struct RelinkProgressScope
  {
    SymmetryTool& tool;

    explicit RelinkProgressScope(SymmetryTool& i_tool)
      : tool{i_tool}
    {
      tool.setRelinkInProgress(true);
    }

    ~RelinkProgressScope() { tool.setRelinkInProgress(false); }
  };

  const auto progressScope = RelinkProgressScope{*this};

  if (tryRelinkExistingMirroredNodes(m_rotateCopies))
  {
    return;
  }

  const auto fallbackRotateCopies = !m_rotateCopies;
  if (tryRelinkExistingMirroredNodes(fallbackRotateCopies))
  {
    m_rotateCopies = fallbackRotateCopies;
  }
}

void SymmetryTool::setRelinkInProgress(const bool relinkInProgress)
{
  if (m_relinkInProgress == relinkInProgress)
  {
    return;
  }

  m_relinkInProgress = relinkInProgress;
  notifyChange();
  pumpRelinkProgressEvents();
}

void SymmetryTool::pumpRelinkProgressEvents()
{
  if (!m_relinkInProgress)
  {
    return;
  }

  refreshViews();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

bool SymmetryTool::tryRelinkExistingMirroredNodes(const bool rotateCopies)
{
  const auto* source = sourceNode();
  if (!source || sideCount() <= 1 || !m_masterToGeneratedLinkIds.empty())
  {
    return false;
  }

  const auto candidateMasters = collectRootNodes(collectMirrorNodes());
  if (candidateMasters.size() > MaxRelinkCandidateCount)
  {
    m_document.map().logger().warn() << "Skipping symmetry relink: too many candidates";
    return false;
  }

  auto claimedLinkIds = std::unordered_set<std::string>{m_sourceLinkId};
  auto candidateIndex = size_t{0};
  for (auto* candidateMaster : candidateMasters)
  {
    if (candidateIndex++ % 64u == 0u)
    {
      pumpRelinkProgressEvents();
    }

    if (!candidateMaster)
    {
      continue;
    }

    const auto masterLinkId = linkIdForNode(*candidateMaster);
    if (masterLinkId.empty() || claimedLinkIds.contains(masterLinkId))
    {
      continue;
    }

    auto generatedLinkIds = std::vector<std::string>(sideCount() - 1u);
    auto localClaimedLinkIds = claimedLinkIds;
    localClaimedLinkIds.insert(masterLinkId);

    auto completeSet = true;
    for (size_t side = 1; side < sideCount(); ++side)
    {
      auto* generatedNode = findMirroredNode(
        *candidateMaster, side, rotateCopies, localClaimedLinkIds, candidateMasters);
      if (!generatedNode)
      {
        completeSet = false;
        break;
      }

      const auto generatedLinkId = linkIdForNode(*generatedNode);
      generatedLinkIds[side - 1u] = generatedLinkId;
      localClaimedLinkIds.insert(generatedLinkId);
    }

    if (!completeSet)
    {
      continue;
    }

    m_masterToGeneratedLinkIds.emplace(masterLinkId, std::move(generatedLinkIds));
    claimedLinkIds = std::move(localClaimedLinkIds);
  }

  return !m_masterToGeneratedLinkIds.empty();
}

void SymmetryTool::rebuildMirroredMasters()
{
  auto masterLinkIds = std::vector<std::string>{};
  masterLinkIds.reserve(m_masterToGeneratedLinkIds.size());
  for (const auto& [masterLinkId, _] : m_masterToGeneratedLinkIds)
  {
    masterLinkIds.push_back(masterLinkId);
  }

  for (const auto& masterLinkId : masterLinkIds)
  {
    if (auto* masterNode = findNodeByLinkId(masterLinkId))
    {
      rebuildMirroredMaster(*masterNode);
    }
  }
}

void SymmetryTool::rebuildMirroredMaster(mdl::Node& masterNode)
{
  rebuildMirroredMaster(masterNode, std::nullopt);
}

void SymmetryTool::rebuildMirroredMaster(
  mdl::Node& masterNode, const std::optional<size_t> preservedSide)
{
  const auto masterLinkId = linkIdForNode(masterNode);
  if (masterLinkId.empty() || masterLinkId == m_sourceLinkId || !masterNode.parent())
  {
    return;
  }

  auto& generatedLinkIds = m_masterToGeneratedLinkIds[masterLinkId];
  auto oldGeneratedLinkIds = generatedLinkIds;
  auto newGeneratedLinkIds =
    std::vector<std::string>(sideCount() > 0u ? sideCount() - 1u : 0u);

  auto linkIdsToRemove = std::vector<std::string>{};
  linkIdsToRemove.reserve(oldGeneratedLinkIds.size());
  for (size_t i = 0; i < oldGeneratedLinkIds.size(); ++i)
  {
    const auto side = i + 1u;
    if (preservedSide && side == *preservedSide)
    {
      if (auto* preservedNode = findNodeByLinkId(oldGeneratedLinkIds[i]))
      {
        if (preservedNode->parent() && i < newGeneratedLinkIds.size())
        {
          newGeneratedLinkIds[i] = oldGeneratedLinkIds[i];
          continue;
        }
      }
    }

    linkIdsToRemove.push_back(oldGeneratedLinkIds[i]);
  }
  removeGeneratedNodes(linkIdsToRemove);
  generatedLinkIds.clear();

  const auto* source = sourceNode();
  if (!source || sideCount() <= 1)
  {
    generatedLinkIds = std::move(newGeneratedLinkIds);
    return;
  }

  auto& map = m_document.map();
  auto toAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto& nodes = toAdd[masterNode.parent()];
  auto addedSides = std::vector<size_t>{};

  for (size_t side = 1; side < sideCount(); ++side)
  {
    if (!newGeneratedLinkIds[side - 1u].empty())
    {
      continue;
    }

    auto clone =
      std::unique_ptr<mdl::Node>{masterNode.cloneRecursively(map.worldBounds())};
    resetLinkIds(*clone);

    if (!transformNode(*clone, sideTransform(*source, side)))
    {
      map.logger().error() << "Could not create symmetry clone";
      continue;
    }

    nodes.push_back(clone.release());
    addedSides.push_back(side);
  }

  if (nodes.empty())
  {
    generatedLinkIds = std::move(newGeneratedLinkIds);
    return;
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  const auto generatedNodes = mdl::addNodes(map, toAdd);
  for (size_t i = 0; i < generatedNodes.size() && i < addedSides.size(); ++i)
  {
    const auto side = addedSides[i];
    newGeneratedLinkIds[side - 1u] = linkIdForNode(*generatedNodes[i]);
  }
  generatedLinkIds = std::move(newGeneratedLinkIds);
  refreshViews();
}

bool SymmetryTool::rebuildFromGeneratedSide(const GeneratedSide& generatedSide)
{
  auto* source = sourceNode();
  auto* masterNode = findNodeByLinkId(generatedSide.masterLinkId);
  if (
    !source || !masterNode || generatedSide.side == 0
    || generatedSide.side >= sideCount())
  {
    return false;
  }

  auto& generatedLinkIds = m_masterToGeneratedLinkIds[generatedSide.masterLinkId];
  if (generatedSide.side - 1 >= generatedLinkIds.size())
  {
    return false;
  }

  auto* generatedNode = findNodeByLinkId(generatedLinkIds[generatedSide.side - 1]);
  if (!generatedNode)
  {
    return false;
  }

  auto newContents = transformedNodeContents(
    *generatedNode, inverseSideTransform(*source, generatedSide.side));
  if (!newContents)
  {
    return false;
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  if (!mdl::updateNodeContents(
        m_document.map(),
        "Update Symmetry Master",
        {{masterNode, std::move(*newContents)}}))
  {
    return false;
  }

  rebuildMirroredMaster(*masterNode, generatedSide.side);
  return true;
}

bool SymmetryTool::replaceMasterFromGeneratedSide(
  const GeneratedSide& generatedSide, const std::vector<mdl::Node*>& replacementNodes)
{
  auto* source = sourceNode();
  auto* masterNode = findNodeByLinkId(generatedSide.masterLinkId);
  if (
    !source || !masterNode || !masterNode->parent() || replacementNodes.empty()
    || generatedSide.side == 0 || generatedSide.side >= sideCount())
  {
    return false;
  }

  auto& map = m_document.map();
  auto clonedMasters = std::vector<std::unique_ptr<mdl::Node>>{};
  clonedMasters.reserve(replacementNodes.size());

  const auto inverseTransform = inverseSideTransform(*source, generatedSide.side);
  for (const auto* replacementNode : replacementNodes)
  {
    if (!replacementNode || !replacementNode->parent())
    {
      continue;
    }

    auto clone =
      std::unique_ptr<mdl::Node>{replacementNode->cloneRecursively(map.worldBounds())};
    resetLinkIds(*clone);
    if (!transformNode(*clone, inverseTransform))
    {
      map.logger().error() << "Could not rebuild symmetry master from generated side";
      return false;
    }
    clonedMasters.push_back(std::move(clone));
  }

  if (clonedMasters.empty())
  {
    return false;
  }

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto& addedNodesForParent = nodesToAdd[masterNode->parent()];
  for (auto& clone : clonedMasters)
  {
    addedNodesForParent.push_back(clone.release());
  }

  auto nodesToRemove = std::vector<mdl::Node*>{};
  const auto addNodeToRemove = [&](mdl::Node* node) {
    if (
      node && node->parent()
      && std::ranges::find(nodesToRemove, node) == std::end(nodesToRemove))
    {
      nodesToRemove.push_back(node);
    }
  };

  for (auto* replacementNode : replacementNodes)
  {
    addNodeToRemove(replacementNode);
  }
  addNodeToRemove(masterNode);

  if (
    const auto it = m_masterToGeneratedLinkIds.find(generatedSide.masterLinkId);
    it != std::end(m_masterToGeneratedLinkIds))
  {
    for (const auto& generatedLinkId : it->second)
    {
      addNodeToRemove(findNodeByLinkId(generatedLinkId));
    }
    m_masterToGeneratedLinkIds.erase(it);
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  auto transaction = mdl::Transaction{map, "Update Symmetry"};
  mdl::deselectAll(map);

  const auto addedMasters = mdl::addNodes(map, nodesToAdd);
  if (addedMasters.empty())
  {
    transaction.cancel();
    return false;
  }

  if (!nodesToRemove.empty())
  {
    mdl::removeNodes(map, nodesToRemove);
  }

  mdl::selectNodes(map, addedMasters);
  for (auto* addedMaster : addedMasters)
  {
    if (addedMaster)
    {
      m_masterToGeneratedLinkIds.try_emplace(linkIdForNode(*addedMaster));
      rebuildMirroredMaster(*addedMaster);
    }
  }

  if (!transaction.commit())
  {
    return false;
  }

  refreshViews();
  return true;
}

bool SymmetryTool::removeMasterFromGeneratedSide(const GeneratedSide& generatedSide)
{
  auto* masterNode = findNodeByLinkId(generatedSide.masterLinkId);
  if (!masterNode || !masterNode->parent())
  {
    return false;
  }

  auto nodesToRemove = std::vector<mdl::Node*>{};
  const auto addNodeToRemove = [&](mdl::Node* node) {
    if (
      node && node->parent()
      && std::ranges::find(nodesToRemove, node) == std::end(nodesToRemove))
    {
      nodesToRemove.push_back(node);
    }
  };

  addNodeToRemove(masterNode);

  if (
    const auto it = m_masterToGeneratedLinkIds.find(generatedSide.masterLinkId);
    it != std::end(m_masterToGeneratedLinkIds))
  {
    for (const auto& generatedLinkId : it->second)
    {
      addNodeToRemove(findNodeByLinkId(generatedLinkId));
    }
    m_masterToGeneratedLinkIds.erase(it);
  }

  if (nodesToRemove.empty())
  {
    return false;
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  mdl::removeNodes(m_document.map(), nodesToRemove);
  refreshViews();
  return true;
}

void SymmetryTool::pauseMirroring()
{
  m_syncPending = false;
  m_touchedLinkIds.clear();
  m_undoHistory.clear();
  m_redoHistory.clear();
  pruneMissingHistoryLinks();
  refreshKnownLinkIds();
}

void SymmetryTool::bakeGeneratedNodes()
{
  m_masterToGeneratedLinkIds.clear();
  m_knownLinkIds.clear();
  m_touchedLinkIds.clear();
  m_undoHistory.clear();
  m_redoHistory.clear();
}

void SymmetryTool::removeGeneratedNodes()
{
  for (auto& [_, generatedLinkIds] : m_masterToGeneratedLinkIds)
  {
    removeGeneratedNodes(generatedLinkIds);
    generatedLinkIds.clear();
  }
}

void SymmetryTool::removeGeneratedNodes(const std::vector<std::string>& linkIds)
{
  auto nodesToRemove = std::vector<mdl::Node*>{};
  nodesToRemove.reserve(linkIds.size());
  for (const auto& linkId : linkIds)
  {
    if (auto* node = findNodeByLinkId(linkId))
    {
      if (node->parent())
      {
        nodesToRemove.push_back(node);
      }
    }
  }

  if (nodesToRemove.empty())
  {
    return;
  }

  const auto ignoreNotifications = kdl::set_temp{m_ignoreNotifications};
  mdl::removeNodes(m_document.map(), nodesToRemove);
  refreshViews();
}

void SymmetryTool::clearSource()
{
  bakeGeneratedNodes();
  m_sourcePath.reset();
  m_sourceLinkId.clear();
  m_mode = Mode::Off;
}

std::optional<mdl::NodeContents> SymmetryTool::transformedNodeContents(
  const mdl::Node& node, const vm::mat4x4d& transformation) const
{
  auto clone =
    std::unique_ptr<mdl::Node>{node.cloneRecursively(m_document.map().worldBounds())};
  if (!transformNode(*clone, transformation))
  {
    return std::nullopt;
  }

  return clone->accept(
    kdl::overload(
      [](const mdl::WorldNode&) -> std::optional<mdl::NodeContents> {
        return std::nullopt;
      },
      [](const mdl::LayerNode& layerNode) -> std::optional<mdl::NodeContents> {
        return mdl::NodeContents{layerNode.layer()};
      },
      [](const mdl::GroupNode& groupNode) -> std::optional<mdl::NodeContents> {
        return mdl::NodeContents{groupNode.group()};
      },
      [](const mdl::EntityNode& entityNode) -> std::optional<mdl::NodeContents> {
        return mdl::NodeContents{entityNode.entity()};
      },
      [](const mdl::BrushNode& brushNode) -> std::optional<mdl::NodeContents> {
        return mdl::NodeContents{brushNode.brush()};
      },
      [](const mdl::PatchNode& patchNode) -> std::optional<mdl::NodeContents> {
        return mdl::NodeContents{patchNode.patch()};
      }));
}

double SymmetryTool::sourceAngle(const mdl::Node& sourceNode) const
{
  const auto direction = sourceNode.accept(
    kdl::overload(
      [](const mdl::WorldNode&) -> std::optional<vm::vec3d> { return std::nullopt; },
      [](const mdl::LayerNode&) -> std::optional<vm::vec3d> { return std::nullopt; },
      [](const mdl::GroupNode& groupNode) -> std::optional<vm::vec3d> {
        return horizontalDirection(
          (groupNode.group().transformation() * vm::vec4d{1.0, 0.0, 0.0, 0.0}).xyz());
      },
      [](const mdl::EntityNode&) -> std::optional<vm::vec3d> { return std::nullopt; },
      [](const mdl::BrushNode& brushNode) -> std::optional<vm::vec3d> {
        return dominantBrushDirection(brushNode.brush());
      },
      [](const mdl::PatchNode&) -> std::optional<vm::vec3d> { return std::nullopt; }));

  return direction ? std::atan2(direction->y(), direction->x()) : 0.0;
}

vm::mat4x4d SymmetryTool::sideTransform(
  const mdl::Node& sourceNode, const size_t side) const
{
  return sideTransform(sourceNode, side, m_rotateCopies);
}

vm::mat4x4d SymmetryTool::sideTransform(
  const mdl::Node& sourceNode, const size_t side, const bool rotateCopies) const
{
  return rotateCopies ? rotationTransform(sourceNode, side)
                      : mirrorTransform(sourceNode, side);
}

vm::mat4x4d SymmetryTool::inverseSideTransform(
  const mdl::Node& sourceNode, const size_t side) const
{
  if (m_rotateCopies || side % 2 == 0)
  {
    return rotationTransform(sourceNode, (sideCount() - side) % sideCount());
  }

  return mirrorTransform(sourceNode, side);
}

vm::mat4x4d SymmetryTool::rotationTransform(
  const mdl::Node& sourceNode, const size_t side) const
{
  const auto center = sourceNode.logicalBounds().center();
  const auto angle =
    vm::Cd::two_pi() * static_cast<double>(side) / static_cast<double>(sideCount());
  return vm::translation_matrix(center)
         * vm::rotation_matrix(vm::vec3d{0.0, 0.0, 1.0}, angle)
         * vm::translation_matrix(-center);
}

vm::mat4x4d SymmetryTool::mirrorTransform(
  const mdl::Node& sourceNode, const size_t side) const
{
  if (side == 0)
  {
    return vm::mat4x4d::identity();
  }

  const auto center = sourceNode.logicalBounds().center();
  const auto step = vm::Cd::two_pi() / static_cast<double>(sideCount());
  if (side % 2 == 0)
  {
    const auto angle = step * static_cast<double>(side);
    return vm::translation_matrix(center)
           * vm::rotation_matrix(vm::vec3d{0.0, 0.0, 1.0}, angle)
           * vm::translation_matrix(-center);
  }

  const auto axisAngle =
    sourceAngle(sourceNode) + step * static_cast<double>((side + 1) / 2);
  return vm::translation_matrix(center)
         * vm::rotation_matrix(vm::vec3d{0.0, 0.0, 1.0}, axisAngle)
         * vm::mirror_matrix<double>(vm::axis::y)
         * vm::rotation_matrix(vm::vec3d{0.0, 0.0, 1.0}, -axisAngle)
         * vm::translation_matrix(-center);
}

bool SymmetryTool::transformNode(mdl::Node& node, const vm::mat4x4d& transformation) const
{
  auto& map = m_document.map();
  auto success = true;
  const auto alignmentLock = map.editorContext().alignmentLock();

  node.accept(
    kdl::overload(
      [&](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        auto group = groupNode.group();
        group.transform(transformation);
        groupNode.setGroup(std::move(group));
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        entityNode.visitChildren(thisLambda);

        auto entity = entityNode.entity();
        entity.transform(
          transformation,
          entityNode.entityPropertyConfig().updateAnglePropertyAfterTransform);
        entityNode.setEntity(std::move(entity));
      },
      [&](mdl::BrushNode& brushNode) {
        auto brush = brushNode.brush();
        if (brush.transform(map.worldBounds(), transformation, alignmentLock))
        {
          brushNode.setBrush(std::move(brush));
        }
        else
        {
          success = false;
        }
      },
      [&](mdl::PatchNode& patchNode) {
        auto patch = patchNode.patch();
        patch.transform(transformation);
        patchNode.setPatch(std::move(patch));
      }));

  return success;
}

void SymmetryTool::resetLinkIds(mdl::Node& node) const
{
  node.accept(
    kdl::overload(
      [&](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        groupNode.setLinkId(generateUuid());
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        entityNode.setLinkId(generateUuid());
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) { brushNode.setLinkId(generateUuid()); },
      [&](mdl::PatchNode& patchNode) { patchNode.setLinkId(generateUuid()); }));
}

bool SymmetryTool::doActivate()
{
  if (!hasSource() && !canAttachToSelection())
  {
    return false;
  }

  m_historyReplayActive = false;
  m_syncPending = false;
  m_touchedLinkIds.clear();
  connectObservers();
  if (hasSource() && m_mode != Mode::Off)
  {
    if (m_masterToGeneratedLinkIds.empty())
    {
      relinkExistingMirroredNodes();
    }
    refreshKnownLinkIds();
  }
  notifyChange();
  return true;
}

bool SymmetryTool::doDeactivate()
{
  m_notifierConnection.disconnect();
  m_historyReplayActive = false;
  m_syncPending = false;
  m_touchedLinkIds.clear();
  notifyChange();
  return true;
}

QWidget* SymmetryTool::doCreatePage(QWidget* parent)
{
  return new SymmetryToolPage{*this, parent};
}

void SymmetryTool::connectObservers()
{
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect(this, &SymmetryTool::documentWasLoaded);
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect(this, &SymmetryTool::documentDidChange);
  m_notifierConnection +=
    m_document.nodesWereAddedNotifier.connect(this, &SymmetryTool::nodesWereAdded);
  m_notifierConnection +=
    m_document.nodesWereRemovedNotifier.connect(this, &SymmetryTool::nodesWereRemoved);
  m_notifierConnection +=
    m_document.nodesDidChangeNotifier.connect(this, &SymmetryTool::nodesDidChange);
  m_notifierConnection +=
    m_document.transactionDoneNotifier.connect([&](const auto&, const auto, const auto) {
      if (!m_historyReplayActive)
      {
        m_redoHistory.clear();
      }
    });
  m_notifierConnection += m_document.commandUndoNotifier.connect([&](auto&) {
    if (m_document.map().hasOpenTransaction())
    {
      return;
    }

    m_historyReplayActive = true;
    m_syncPending = false;
    m_touchedLinkIds.clear();
  });
  m_notifierConnection += m_document.commandUndoneNotifier.connect([&](auto&) {
    if (!m_historyReplayActive)
    {
      return;
    }

    m_syncPending = false;
    m_touchedLinkIds.clear();
  });
  m_notifierConnection +=
    m_document.transactionRedoNotifier.connect([&](const auto&, const auto, const auto) {
      m_historyReplayActive = true;
      m_syncPending = false;
      m_touchedLinkIds.clear();
    });
  m_notifierConnection += m_document.transactionRedoneNotifier.connect(
    [&](const auto& name, const auto, const auto) {
      const auto commandName = std::string{name};
      const auto alive = std::weak_ptr<bool>{m_alive};
      QTimer::singleShot(0, [this, alive, commandName]() {
        const auto aliveFlag = alive.lock();
        if (!aliveFlag || !*aliveFlag)
        {
          return;
        }

        if (!restoreRedoHistoryEntry(commandName))
        {
          refreshKnownLinkIds();
        }
        m_historyReplayActive = false;
        m_syncPending = false;
        m_touchedLinkIds.clear();
        notifyChange();
      });
    });
  m_notifierConnection += m_document.commandUndoFailedNotifier.connect([&](auto&) {
    m_historyReplayActive = false;
    m_syncPending = false;
    m_touchedLinkIds.clear();
  });
  m_notifierConnection += m_document.transactionUndoneNotifier.connect(
    [&](const auto& name, const auto, const auto) {
      const auto commandName = std::string{name};
      const auto alive = std::weak_ptr<bool>{m_alive};
      QTimer::singleShot(0, [this, alive, commandName]() {
        const auto aliveFlag = alive.lock();
        if (!aliveFlag || !*aliveFlag)
        {
          return;
        }

        if (!restoreUndoHistoryEntry(commandName))
        {
          refreshKnownLinkIds();
        }
        m_historyReplayActive = false;
        m_syncPending = false;
        m_touchedLinkIds.clear();
        notifyChange();
      });
    });
}

void SymmetryTool::documentWasLoaded()
{
  m_historyReplayActive = false;
  m_syncPending = false;
  clearSource();
  notifyChange();
}

void SymmetryTool::documentDidChange()
{
  if (m_historyReplayActive)
  {
    m_syncPending = false;
    m_touchedLinkIds.clear();
    return;
  }

  if (
    m_ignoreNotifications || m_mirroringSuspended > 0u || m_sourceLinkId.empty()
    || m_mode == Mode::Off)
  {
    return;
  }
  if (m_document.map().hasOpenTransaction())
  {
    return;
  }

  scheduleSyncMirroredNodes();
}

void SymmetryTool::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  rememberTouchedNodes(nodes);
  documentDidChange();
}

void SymmetryTool::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  rememberTouchedNodes(nodes);
  documentDidChange();
}

void SymmetryTool::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  rememberTouchedNodes(nodes);
  documentDidChange();
}

void SymmetryTool::notifyChange()
{
  symmetryDidChangeNotifier(*this);
  refreshViews();
}

} // namespace tb::ui
