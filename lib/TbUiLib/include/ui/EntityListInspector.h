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

#include "NotifierConnection.h"
#include "ui/TabBook.h"

#include <QString>

#include <vector>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTimer;

namespace tb::mdl
{
class Node;
}

namespace tb::ui
{
class MapDocument;
class MapWindow;

class EntityListInspector : public TabBookPage
{
private:
  struct Entry
  {
    QString type;
    QString name;
    QString detail;
    QString searchText;
    mdl::Node* node = nullptr;
  };

  MapDocument& m_document;
  MapWindow& m_mapWindow;
  QLineEdit* m_search = nullptr;
  QListWidget* m_list = nullptr;
  QTimer* m_rebuildTimer = nullptr;
  std::vector<Entry> m_entries;
  NotifierConnection m_notifierConnection;

public:
  explicit EntityListInspector(
    MapDocument& document, MapWindow& mapWindow, QWidget* parent = nullptr);
  ~EntityListInspector() override;

private:
  void createGui();
  void connectObservers();
  void scheduleRebuild();
  void rebuildEntries();
  void collectEntries(mdl::Node& node);
  void reloadList();
  void focusItem(QListWidgetItem* item);
};

} // namespace tb::ui
