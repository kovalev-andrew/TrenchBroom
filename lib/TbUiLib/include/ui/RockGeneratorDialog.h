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

#include <QWidget>

#include "mdl/Brush.h"

#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace tb::mdl
{
class BrushNode;
class Node;
} // namespace tb::mdl

namespace tb::ui
{
class MapDocument;

class RockGeneratorDialog : public QWidget
{
  Q_OBJECT

private:
  struct SourceBrush
  {
    mdl::BrushNode* node;
    mdl::Node* parent;
    mdl::Brush brush;
  };

  MapDocument& m_document;
  std::vector<SourceBrush> m_sourceBrushes;

  QSpinBox* m_segments = nullptr;
  QSpinBox* m_seed = nullptr;
  QCheckBox* m_hollow = nullptr;
  QLabel* m_thicknessLabel = nullptr;
  QDoubleSpinBox* m_thickness = nullptr;
  QCheckBox* m_createGroup = nullptr;

  bool m_transactionActive = false;
  bool m_applied = false;
  bool m_updatingPreview = false;

public:
  RockGeneratorDialog(MapDocument& document, QWidget* parent = nullptr);
  ~RockGeneratorDialog() override;

  void startPreview();
  void cancelPreview();

signals:
  void finished();

private:
  void createGui();
  void captureSelection();
  void beginPreview();
  void updatePreview();
  void apply();
  void resetSettings();
};

} // namespace tb::ui
