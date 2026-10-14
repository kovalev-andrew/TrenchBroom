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
#include "NotifierConnection.h"
#include "mdl/Node.h"
#include "mdl/NodeContents.h"
#include "ui/Tool.h"

#include "vm/mat.h"
#include "vm/vec.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tb
{
namespace mdl
{
class Node;
}

namespace render
{
class RenderBatch;
class RenderContext;
} // namespace render

namespace ui
{
class MapDocument;

class SymmetryTool : public Tool
{
public:
  enum class Mode
  {
    Off = 0,
    TwoSides = 2,
    FourSides = 4,
    EightSides = 8,
  };

private:
  struct GeneratedSide
  {
    std::string masterLinkId;
    size_t side;
  };

  struct StateSnapshot
  {
    Mode mode = Mode::Off;
    std::optional<mdl::NodePath> sourcePath;
    std::string sourceLinkId;
    std::map<std::string, std::vector<std::string>> masterToGeneratedLinkIds;
    std::unordered_set<std::string> knownLinkIds;
    bool rotateCopies = false;
  };

  struct HistoryEntry
  {
    std::string commandName;
    StateSnapshot before;
    StateSnapshot after;
  };

  MapDocument& m_document;
  Mode m_mode = Mode::Off;
  std::optional<mdl::NodePath> m_sourcePath;
  std::string m_sourceLinkId;
  std::map<std::string, std::vector<std::string>> m_masterToGeneratedLinkIds;
  std::unordered_set<std::string> m_knownLinkIds;
  std::unordered_set<std::string> m_touchedLinkIds;
  bool m_rotateCopies = false;
  size_t m_mirroringSuspended = 0u;
  bool m_ignoreNotifications = false;
  bool m_historyReplayActive = false;
  bool m_syncPending = false;
  bool m_relinkInProgress = false;
  std::vector<HistoryEntry> m_undoHistory;
  std::vector<HistoryEntry> m_redoHistory;
  std::shared_ptr<bool> m_alive;

  NotifierConnection m_notifierConnection;

public:
  Notifier<SymmetryTool&> symmetryDidChangeNotifier;
  Notifier<SymmetryTool&> symmetryDidRequestDeactivateNotifier;

public:
  explicit SymmetryTool(MapDocument& document);
  ~SymmetryTool() override;

  Mode mode() const;
  void setMode(Mode mode);
  bool rotateCopies() const;
  void setRotateCopies(bool rotateCopies);
  void suspendMirroring();
  void resumeMirroring();

  bool hasSource() const;
  bool modifierActive() const;
  bool relinkInProgress() const;
  bool sourceSelected() const;
  bool canAttachToSelection() const;
  void attachToSelection();

  void render(
    render::RenderContext& renderContext, render::RenderBatch& renderBatch) const;

private:
  size_t sideCount() const;
  mdl::Node* sourceNode() const;
  mdl::Node* findNodeByLinkId(const std::string& linkId) const;
  std::string linkIdForNode(const mdl::Node& node) const;
  void setSourceNode(mdl::Node& node);
  std::vector<mdl::Node*> collectMirrorNodes() const;
  std::vector<mdl::Node*> collectRootNodes(std::vector<mdl::Node*> nodes) const;
  std::unordered_set<std::string> collectLinkIds() const;
  bool isGeneratedLinkId(const std::string& linkId) const;
  std::optional<GeneratedSide> generatedSideForLinkId(const std::string& linkId) const;
  std::vector<GeneratedSide> removedGeneratedSides(
    const std::unordered_set<std::string>& currentLinkIds) const;
  mdl::Node* findMirroredNode(
    const mdl::Node& masterNode,
    size_t side,
    bool rotateCopies,
    const std::unordered_set<std::string>& claimedLinkIds,
    const std::vector<mdl::Node*>& candidates) const;
  bool nodesGeometricallyMatch(const mdl::Node& lhs, const mdl::Node& rhs) const;
  void rememberTouchedNodes(const std::vector<mdl::Node*>& nodes);
  void rememberTouchedNode(mdl::Node& node);
  void refreshKnownLinkIds();
  StateSnapshot captureState() const;
  void restoreState(const StateSnapshot& state);
  void pruneMissingHistoryLinks();
  void recordHistoryEntry(
    std::string commandName, StateSnapshot before, StateSnapshot after);
  bool restoreUndoHistoryEntry(const std::string& commandName);
  bool restoreRedoHistoryEntry(const std::string& commandName);
  void scheduleSyncMirroredNodes();
  void syncMirroredNodes();
  void syncMirroredNodesWithUndoBatch();
  void relinkExistingMirroredNodes();
  void setRelinkInProgress(bool relinkInProgress);
  void pumpRelinkProgressEvents();
  bool tryRelinkExistingMirroredNodes(bool rotateCopies);
  void rebuildMirroredMasters();
  void rebuildMirroredMaster(mdl::Node& masterNode);
  void rebuildMirroredMaster(mdl::Node& masterNode, std::optional<size_t> preservedSide);
  bool rebuildFromGeneratedSide(const GeneratedSide& generatedSide);
  bool replaceMasterFromGeneratedSide(
    const GeneratedSide& generatedSide, const std::vector<mdl::Node*>& replacementNodes);
  bool removeMasterFromGeneratedSide(const GeneratedSide& generatedSide);
  void pauseMirroring();
  void bakeGeneratedNodes();
  void removeGeneratedNodes();
  void removeGeneratedNodes(const std::vector<std::string>& linkIds);
  void clearSource();
  std::optional<mdl::NodeContents> transformedNodeContents(
    const mdl::Node& node, const vm::mat4x4d& transformation) const;
  double sourceAngle(const mdl::Node& sourceNode) const;
  vm::mat4x4d sideTransform(const mdl::Node& sourceNode, size_t side) const;
  vm::mat4x4d sideTransform(
    const mdl::Node& sourceNode, size_t side, bool rotateCopies) const;
  vm::mat4x4d inverseSideTransform(const mdl::Node& sourceNode, size_t side) const;
  vm::mat4x4d rotationTransform(const mdl::Node& sourceNode, size_t side) const;
  vm::mat4x4d mirrorTransform(const mdl::Node& sourceNode, size_t side) const;
  bool transformNode(mdl::Node& node, const vm::mat4x4d& transformation) const;
  void resetLinkIds(mdl::Node& node) const;

  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;

  void connectObservers();
  void documentWasLoaded();
  void documentDidChange();
  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  void notifyChange();
};

} // namespace ui
} // namespace tb
