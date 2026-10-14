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
#include "ui/Tool.h"

#include <optional>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Node;
} // namespace tb::mdl

namespace tb::ui
{

class MapDocument;

class SubdivisionTool : public Tool
{
public:
  enum class Plane
  {
    XY,
    XZ,
    YZ,
  };
  enum class Mode
  {
    Segments,
    GridSize
  };
  enum class Alignment
  {
    WorldGrid,
    Texture
  };

private:
  struct SourceTarget
  {
    mdl::BrushNode* node = nullptr;
    std::vector<size_t> faceIndices;
    std::optional<size_t> pieceFaceIndex;
  };

  struct Settings
  {
    int uSegments = 2;
    int vSegments = 2;
    Plane plane = Plane::XY;
    Mode mode = Mode::Segments;
    Alignment alignment = Alignment::WorldGrid;
    double cellSize = 32.0;

    bool operator==(const Settings& other) const;
  };

  MapDocument& m_document;
  int m_uSegments = 2;
  int m_vSegments = 2;
  Plane m_plane = Plane::XY;
  Mode m_mode = Mode::Segments;
  Alignment m_alignment = Alignment::WorldGrid;
  double m_cellSize = 32.0;
  bool m_previewTransactionOpen = false;
  bool m_previewHasChanges = false;
  std::vector<SourceTarget> m_sourceTargets;
  std::vector<mdl::Node*> m_previewNodes;
  std::vector<Settings> m_undoStack;

public:
  Notifier<SubdivisionTool&> settingsDidChangeNotifier;
  Notifier<SubdivisionTool&> subdivisionDidRequestDeactivateNotifier;

  explicit SubdivisionTool(MapDocument& document);
  ~SubdivisionTool() override;

  bool canAttachToSelection() const;
  bool apply();
  bool undoStep();
  void resetSettings();

  int uSegments() const;
  void setUSegments(int segments);

  int vSegments() const;
  void setVSegments(int segments);

  Plane plane() const;
  void setPlane(Plane plane);

  Mode mode() const;
  void setMode(Mode mode);
  Alignment alignment() const;
  void setAlignment(Alignment alignment);
  double cellSize() const;
  void setCellSize(double cellSize);

private:
  Settings currentSettings() const;
  void applySettings(const Settings& settings);
  void saveUndoState();
  void notifySettingsChanged();
  void collectSourceTargets();
  bool rebuildPreview();
  void updatePreview();
  void cancelPreview();

  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};

} // namespace tb::ui
