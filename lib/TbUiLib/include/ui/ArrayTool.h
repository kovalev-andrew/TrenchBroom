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

#include "vm/vec.h"

#include <vector>

namespace tb::mdl
{
class Node;
} // namespace tb::mdl

namespace tb::ui
{

class MapDocument;

class ArrayTool : public Tool
{
public:
  enum class Mode
  {
    RandomRadius,
    Line,
    Radial,
  };

  enum class Axis
  {
    X,
    Y,
    Z,
  };

  enum class LineOrigin
  {
    FromCenterForward,
    Centered,
  };

  enum class FacingSide
  {
    PosX,
    NegX,
    PosY,
    NegY,
    PosZ,
    NegZ
  };

private:
  struct Settings
  {
    Mode mode;
    Axis axis;
    LineOrigin lineOrigin;
    int count;
    int seed;
    double radius;
    double spacing;
    vm::vec3d offset;
    int lineCount;
    int ringCount;
    double ringSpacing;
    bool crossLines;
    bool mirrorOffsets;
    bool faceCenter;
    FacingSide facingSide;

    bool operator==(const Settings& other) const;
  };

  MapDocument& m_document;
  Mode m_mode = Mode::RandomRadius;
  Axis m_axis = Axis::Z;
  LineOrigin m_lineOrigin = LineOrigin::Centered;
  int m_count = 8;
  int m_seed = 1;
  double m_radius = 256.0;
  double m_spacing = 128.0;
  vm::vec3d m_offset = vm::vec3d{0.0, 0.0, 0.0};
  int m_lineCount = 1;
  int m_ringCount = 1;
  double m_ringSpacing = 256.0;
  bool m_crossLines = false;
  bool m_mirrorOffsets = true;
  bool m_faceCenter = false;
  FacingSide m_facingSide = FacingSide::PosX;
  bool m_previewTransactionOpen = false;
  std::vector<mdl::Node*> m_previewNodes;
  std::vector<Settings> m_undoStack;

public:
  Notifier<ArrayTool&> settingsDidChangeNotifier;
  Notifier<ArrayTool&> arrayDidRequestDeactivateNotifier;

  explicit ArrayTool(MapDocument& document);
  ~ArrayTool() override;

  bool canAttachToSelection() const;
  bool apply();
  bool undoStep();
  void resetSettings();

  Mode mode() const;
  void setMode(Mode mode);

  Axis axis() const;
  void setAxis(Axis axis);

  LineOrigin lineOrigin() const;
  void setLineOrigin(LineOrigin lineOrigin);

  bool crossLines() const;
  void setCrossLines(bool crossLines);

  bool mirrorOffsets() const;
  void setMirrorOffsets(bool mirrorOffsets);

  bool faceCenter() const;
  void setFaceCenter(bool faceCenter);
  FacingSide facingSide() const;
  void setFacingSide(FacingSide side);

  int count() const;
  void setCount(int count);

  int seed() const;
  void setSeed(int seed);

  double radius() const;
  void setRadius(double radius);

  double spacing() const;
  void setSpacing(double spacing);

  vm::vec3d offset() const;
  void setOffset(const vm::vec3d& offset);

  int lineCount() const;
  void setLineCount(int lineCount);

  int ringCount() const;
  void setRingCount(int ringCount);

  double ringSpacing() const;
  void setRingSpacing(double ringSpacing);

private:
  Settings currentSettings() const;
  void applySettings(const Settings& settings);
  void saveUndoState();
  void notifySettingsChanged();

  bool rebuildPreview();
  void updatePreview();
  void cancelPreview();

  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};

} // namespace tb::ui
