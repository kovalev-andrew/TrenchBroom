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

class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QSpinBox;

namespace tb::mdl
{
class BrushNode;
class Node;
} // namespace tb::mdl

namespace tb::ui
{
class MapDocument;

class TerrainGeneratorDialog : public QWidget
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

  QSpinBox* m_horizontalSegments = nullptr;
  QSpinBox* m_verticalSegments = nullptr;
  QDoubleSpinBox* m_height = nullptr;
  QDoubleSpinBox* m_intensity = nullptr;
  QDoubleSpinBox* m_frequency = nullptr;
  QDoubleSpinBox* m_roughness = nullptr;
  QSpinBox* m_seed = nullptr;
  QComboBox* m_side = nullptr;
  QCheckBox* m_triangularGrid = nullptr;
  QCheckBox* m_createGroup = nullptr;

  bool m_transactionActive = false;
  bool m_applied = false;
  bool m_updatingPreview = false;

public:
  TerrainGeneratorDialog(MapDocument& document, QWidget* parent = nullptr);
  ~TerrainGeneratorDialog() override;

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
