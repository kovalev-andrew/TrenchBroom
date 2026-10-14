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

#include "ui/EntityBrowser.h"

#include <algorithm>
#include <array>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QtGlobal>

#include "PreferenceManager.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"
#include "ui/EntityBrowserView.h"
#include "ui/MapDocument.h"
#include "ui/SearchBox.h"
#include "ui/ViewConstants.h"

// for use in QVariant
Q_DECLARE_METATYPE(tb::mdl::EntityDefinitionSortOrder)

namespace tb::ui
{

namespace
{

constexpr auto EntityBrowserIconSizeSteps =
  std::array<float, 6>{0.65f, 0.8f, 1.0f, 1.25f, 1.55f, 1.9f};

} // namespace

EntityBrowser::EntityBrowser(
  AppController& appController, MapDocument& document, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
{
  createGui(appController);
  connectObservers();
}

void EntityBrowser::reload()
{
  if (m_view)
  {
    m_view->setDefaultModelScaleExpression(
      m_document.map().worldNode().entityPropertyConfig().defaultModelScaleExpression);

    m_view->invalidate();
  }
}

void EntityBrowser::setSelectedEntityDefinition(const std::string& classname)
{
  if (m_view)
  {
    m_view->setSelectedEntityDefinition(classname);
  }
}

void EntityBrowser::createGui(AppController& appController)
{
  m_scrollBar = new QScrollBar{Qt::Vertical};
  m_view = new EntityBrowserView{appController, m_scrollBar, m_document};
  m_view->setIconScaleFactor(EntityBrowserIconSizeSteps[m_iconSizeStep]);
  connect(
    m_view,
    &EntityBrowserView::entityDefinitionSelected,
    this,
    &EntityBrowser::entityDefinitionSelected);
  connect(
    m_view,
    &EntityBrowserView::entityBrowserIconSizeAdjustmentRequested,
    this,
    &EntityBrowser::adjustEntityBrowserIconSize);

  auto* browserPanelSizer = new QHBoxLayout{};
  browserPanelSizer->setContentsMargins(0, 0, 0, 0);
  browserPanelSizer->setSpacing(0);
  browserPanelSizer->addWidget(m_view, 1);
  browserPanelSizer->addWidget(m_scrollBar, 0);

  auto* browserPanel = new QWidget{};
  browserPanel->setLayout(browserPanelSizer);

  m_sortOrderChoice = new QComboBox{};
  m_sortOrderChoice->addItem(
    tr("Name"), QVariant::fromValue(mdl::EntityDefinitionSortOrder::Name));
  m_sortOrderChoice->addItem(
    tr("Usage"), QVariant::fromValue(mdl::EntityDefinitionSortOrder::Usage));
  m_sortOrderChoice->setCurrentIndex(0);
  m_sortOrderChoice->setToolTip(tr("Select ordering criterion"));
  connect(
    m_sortOrderChoice, QOverload<int>::of(&QComboBox::activated), this, [&](int index) {
      auto sortOrder = static_cast<mdl::EntityDefinitionSortOrder>(
        m_sortOrderChoice->itemData(index).toInt());
      m_view->setSortOrder(sortOrder);
    });

  m_groupButton = new QPushButton{tr("Group")};
  m_groupButton->setToolTip(tr("Group entity definitions by category"));
  m_groupButton->setCheckable(true);
  connect(m_groupButton, &QAbstractButton::clicked, this, [&]() {
    m_view->setGroup(m_groupButton->isChecked());
  });

  m_usedButton = new QPushButton{tr("Used")};
  m_usedButton->setToolTip(tr("Only show entity definitions currently in use"));
  m_usedButton->setCheckable(true);
  connect(m_usedButton, &QAbstractButton::clicked, this, [&]() {
    m_view->setHideUnused(m_usedButton->isChecked());
  });

  m_decreaseIconSizeButton = new QPushButton{tr("-")};
  m_decreaseIconSizeButton->setFixedWidth(24);
  m_decreaseIconSizeButton->setToolTip(
    tr("Show more entity definitions per row by making previews smaller"));
  connect(m_decreaseIconSizeButton, &QAbstractButton::clicked, this, [&]() {
    adjustEntityBrowserIconSize(-1);
  });

  m_increaseIconSizeButton = new QPushButton{tr("+")};
  m_increaseIconSizeButton->setFixedWidth(24);
  m_increaseIconSizeButton->setToolTip(
    tr("Show fewer entity definitions per row by making previews larger"));
  connect(m_increaseIconSizeButton, &QAbstractButton::clicked, this, [&]() {
    adjustEntityBrowserIconSize(1);
  });

  m_filterBox = createSearchBox();
  connect(m_filterBox, &QLineEdit::textEdited, this, [&]() {
    m_view->setFilterText(m_filterBox->text().toStdString());
  });

  auto* controlSizer = new QHBoxLayout{};
  controlSizer->setContentsMargins(
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin,
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin);
  controlSizer->setSpacing(LayoutConstants::NarrowHMargin);
  controlSizer->addWidget(m_sortOrderChoice, 0);
  controlSizer->addWidget(m_groupButton, 0);
  controlSizer->addWidget(m_usedButton, 0);
  controlSizer->addWidget(m_filterBox, 1);
  controlSizer->addWidget(m_decreaseIconSizeButton, 0);
  controlSizer->addWidget(m_increaseIconSizeButton, 0);

  auto* outerSizer = new QVBoxLayout{};
  outerSizer->setContentsMargins(0, 0, 0, 0);
  outerSizer->setSpacing(0);
  outerSizer->addWidget(browserPanel, 1);
  outerSizer->addLayout(controlSizer, 0);

  setLayout(outerSizer);
  updateIconSizeButtons();
}

void EntityBrowser::connectObservers()
{
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect([&]() { reload(); });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&]() { reload(); });
  m_notifierConnection +=
    m_document.resourcesWereProcessedNotifier.connect([&](const auto&) { reload(); });

  auto& prefs = PreferenceManager::instance();
  m_notifierConnection +=
    prefs.preferenceDidChangeNotifier.connect([&](const auto& path) {
      if (path == pref(m_document.map().gameInfo().gamePathPreference))
      {
        reload();
        updateIconSizeButtons();
      }
      else
      {
        m_view->update();
      }
    });
}

void EntityBrowser::adjustEntityBrowserIconSize(const int direction)
{
  if (direction == 0)
  {
    return;
  }

  const auto nextStep = static_cast<size_t>(std::clamp<int>(
    static_cast<int>(m_iconSizeStep) + direction,
    0,
    static_cast<int>(EntityBrowserIconSizeSteps.size()) - 1));
  if (nextStep == m_iconSizeStep)
  {
    return;
  }

  m_iconSizeStep = nextStep;
  m_view->setIconScaleFactor(EntityBrowserIconSizeSteps[m_iconSizeStep]);
  updateIconSizeButtons();
}

void EntityBrowser::updateIconSizeButtons()
{
  if (!m_decreaseIconSizeButton || !m_increaseIconSizeButton)
  {
    return;
  }

  m_decreaseIconSizeButton->setEnabled(m_iconSizeStep > 0u);
  m_increaseIconSizeButton->setEnabled(
    m_iconSizeStep + 1u < EntityBrowserIconSizeSteps.size());
}

} // namespace tb::ui
