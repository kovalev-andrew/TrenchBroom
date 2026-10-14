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

#include "ui/EntityListInspector.h"

#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"

#include <QAbstractItemView>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <vector>

namespace tb::ui
{
namespace
{

QString makeEntityName(const mdl::EntityNode& entityNode, QString& detail)
{
  const auto& entity = entityNode.entity();
  const auto& classname = entity.classname();

  if (
    const auto* targetname = entity.property(mdl::EntityPropertyKeys::Targetname);
    targetname != nullptr && !targetname->empty())
  {
    detail = QString::fromStdString(classname);
    return QString::fromStdString(*targetname);
  }

  detail.clear();
  return classname.empty() ? QString{"<unnamed entity>"}
                           : QString::fromStdString(classname);
}

QString makeGroupName(const mdl::GroupNode& groupNode)
{
  const auto& name = groupNode.group().name();
  return name.empty() ? QString{"<unnamed group>"} : QString::fromStdString(name);
}

} // namespace

EntityListInspector::EntityListInspector(
  MapDocument& document, MapWindow& mapWindow, QWidget* parent)
  : TabBookPage{parent}
  , m_document{document}
  , m_mapWindow{mapWindow}
{
  createGui();
  connectObservers();
  rebuildEntries();
}

EntityListInspector::~EntityListInspector() = default;

void EntityListInspector::createGui()
{
  m_search = new QLineEdit{};
  m_search->setPlaceholderText(tr("Search entities and groups"));
  m_search->setClearButtonEnabled(true);

  m_list = new QListWidget{};
  m_list->setObjectName("EntityListInspector_List");
  m_list->setUniformItemSizes(true);
  m_list->setAlternatingRowColors(true);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);

  m_rebuildTimer = new QTimer{this};
  m_rebuildTimer->setInterval(100);
  m_rebuildTimer->setSingleShot(true);

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);
  layout->addWidget(m_search);
  layout->addWidget(m_list, 1);
  setLayout(layout);

  connect(m_search, &QLineEdit::textChanged, this, [this] { reloadList(); });
  connect(m_list, &QListWidget::itemDoubleClicked, this, &EntityListInspector::focusItem);
  connect(m_rebuildTimer, &QTimer::timeout, this, &EntityListInspector::rebuildEntries);
}

void EntityListInspector::connectObservers()
{
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect(
    this, &EntityListInspector::rebuildEntries);
  m_notifierConnection += m_document.documentDidChangeNotifier.connect(
    this, &EntityListInspector::scheduleRebuild);
  m_notifierConnection +=
    m_document.nodesWereAddedNotifier.connect([this](const auto&) { scheduleRebuild(); });
  m_notifierConnection += m_document.nodesWereRemovedNotifier.connect(
    [this](const auto&) { scheduleRebuild(); });
  m_notifierConnection +=
    m_document.nodesDidChangeNotifier.connect([this](const auto&) { scheduleRebuild(); });
}

void EntityListInspector::scheduleRebuild()
{
  m_rebuildTimer->start();
}

void EntityListInspector::rebuildEntries()
{
  m_entries.clear();
  collectEntries(m_document.map().worldNode());
  reloadList();
}

void EntityListInspector::collectEntries(mdl::Node& node)
{
  if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node))
  {
    auto entry = Entry{};
    entry.type = QString{"Group"};
    entry.name = makeGroupName(*groupNode);
    entry.detail = QString{"%1 children"}.arg(groupNode->childCount());
    entry.node = groupNode;
    entry.searchText =
      QString{"%1 %2 %3"}.arg(entry.type, entry.name, entry.detail).toLower();
    m_entries.push_back(entry);
  }
  else if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node))
  {
    auto entry = Entry{};
    entry.type = QString{"Entity"};
    entry.name = makeEntityName(*entityNode, entry.detail);
    entry.node = entityNode;
    entry.searchText =
      QString{"%1 %2 %3"}.arg(entry.type, entry.name, entry.detail).toLower();
    m_entries.push_back(entry);
  }

  for (auto* child : node.children())
  {
    collectEntries(*child);
  }
}

void EntityListInspector::reloadList()
{
  const auto query = m_search->text().trimmed().toLower();

  m_list->clear();
  for (size_t i = 0; i < m_entries.size(); ++i)
  {
    const auto& entry = m_entries[i];
    if (!query.isEmpty() && !entry.searchText.contains(query))
    {
      continue;
    }

    auto text = QString{"%1: %2"}.arg(entry.type, entry.name);
    if (!entry.detail.isEmpty())
    {
      text += QString{" (%1)"}.arg(entry.detail);
    }

    auto* item = new QListWidgetItem{text, m_list};
    item->setData(Qt::UserRole, static_cast<int>(i));
    item->setToolTip(text);
  }
}

void EntityListInspector::focusItem(QListWidgetItem* item)
{
  if (item == nullptr)
  {
    return;
  }

  auto ok = false;
  const auto index = item->data(Qt::UserRole).toInt(&ok);
  if (!ok || index < 0 || static_cast<size_t>(index) >= m_entries.size())
  {
    return;
  }

  auto* node = m_entries[static_cast<size_t>(index)].node;
  if (node == nullptr)
  {
    return;
  }

  auto nodes = node->nodesRequiredForViewSelection();
  if (nodes.empty())
  {
    nodes.push_back(node);
  }

  auto& map = m_document.map();
  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
  m_mapWindow.focusCameraOnSelection();
}

} // namespace tb::ui
