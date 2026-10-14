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

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace tb::ui
{
class MapWindow;

class UnitySyncDialog : public QDialog
{
  Q_OBJECT
private:
  MapWindow* m_mapWindow;

  QLineEdit* m_targetFolderEdit = nullptr;
  QPushButton* m_browseTargetFolderButton = nullptr;
  QCheckBox* m_copyTexturesCheckBox = nullptr;
  QCheckBox* m_separateBrushesCheckBox = nullptr;
  QDoubleSpinBox* m_scaleFactorSpinBox = nullptr;
  QLabel* m_statusLabel = nullptr;
  QPushButton* m_syncButton = nullptr;
  QPushButton* m_closeButton = nullptr;

public:
  explicit UnitySyncDialog(MapWindow* mapWindow);

  void updateDefaultPath();

private:
  void createGui();
  void sync();
};

} // namespace tb::ui
