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

#include "ui/MaterialBrowser.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QtGlobal>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"
#include "ui/MaterialBrowserView.h"
#include "ui/SearchBox.h"
#include "ui/ViewConstants.h"

#include <algorithm>
#include <array>
#include <cmath>

// for use in QVariant
Q_DECLARE_METATYPE(tb::ui::MaterialSortOrder)

namespace tb::ui
{
namespace
{
constexpr auto MaterialBrowserIconSizeSteps =
  std::array<float, 7>{0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f};

size_t nearestMaterialBrowserIconSizeStep(const float iconSize)
{
  auto bestIndex = size_t{0};
  auto bestDistance = std::abs(iconSize - MaterialBrowserIconSizeSteps[0]);
  for (size_t i = 1u; i < MaterialBrowserIconSizeSteps.size(); ++i)
  {
    const auto distance = std::abs(iconSize - MaterialBrowserIconSizeSteps[i]);
    if (distance < bestDistance)
    {
      bestIndex = i;
      bestDistance = distance;
    }
  }
  return bestIndex;
}
} // namespace

MaterialBrowser::MaterialBrowser(
  AppController& appController, MapDocument& document, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
{
  createGui(appController);
  bindEvents();
  connectObservers();
  reload();
}

const gl::Material* MaterialBrowser::selectedMaterial() const
{
  return m_view->selectedMaterial();
}

void MaterialBrowser::setSelectedMaterial(const gl::Material* selectedMaterial)
{
  m_view->setSelectedMaterial(selectedMaterial);
}

void MaterialBrowser::revealMaterial(const gl::Material* material)
{
  setFilterText("");
  m_view->revealMaterial(material);
}

void MaterialBrowser::setSortOrder(const MaterialSortOrder sortOrder)
{
  m_view->setSortOrder(sortOrder);
  switch (sortOrder)
  {
  case MaterialSortOrder::Name:
    m_sortOrderChoice->setCurrentIndex(0);
    break;
  case MaterialSortOrder::Usage:
    m_sortOrderChoice->setCurrentIndex(1);
    break;
    switchDefault();
  }
}

void MaterialBrowser::setGroup(const bool group)
{
  m_view->setGroup(group);
  m_groupButton->setChecked(group);
}

void MaterialBrowser::setHideUnused(const bool hideUnused)
{
  m_view->setHideUnused(hideUnused);
  m_usedButton->setChecked(hideUnused);
}

void MaterialBrowser::setFilterText(const std::string& filterText)
{
  m_view->setFilterText(filterText);
  m_filterBox->setText(QString::fromStdString(filterText));
}

/**
 * See EntityBrowser::createGui
 */
void MaterialBrowser::createGui(AppController& appController)
{
  auto* browserPanel = new QWidget{};
  m_scrollBar = new QScrollBar{Qt::Vertical};

  m_view = new MaterialBrowserView{appController, m_scrollBar, m_document};
  connect(
    m_view,
    &MaterialBrowserView::materialBrowserIconSizeAdjustmentRequested,
    this,
    &MaterialBrowser::adjustMaterialBrowserIconSize);

  auto* browserPanelSizer = new QHBoxLayout{};
  browserPanelSizer->setContentsMargins(0, 0, 0, 0);
  browserPanelSizer->setSpacing(0);
  browserPanelSizer->addWidget(m_view, 1);
  browserPanelSizer->addWidget(m_scrollBar, 0);
  browserPanel->setLayout(browserPanelSizer);

  m_sortOrderChoice = new QComboBox{};
  m_sortOrderChoice->addItem(tr("Name"), QVariant::fromValue(MaterialSortOrder::Name));
  m_sortOrderChoice->addItem(tr("Usage"), QVariant::fromValue(MaterialSortOrder::Usage));
  m_sortOrderChoice->setCurrentIndex(0);
  m_sortOrderChoice->setToolTip(tr("Select ordering criterion"));
  connect(
    m_sortOrderChoice, QOverload<int>::of(&QComboBox::activated), this, [&](int index) {
      auto sortOrder =
        static_cast<MaterialSortOrder>(m_sortOrderChoice->itemData(index).toInt());
      m_view->setSortOrder(sortOrder);
    });

  m_groupButton = new QPushButton{tr("Group")};
  m_groupButton->setToolTip(tr("Group materials by material collection"));
  m_groupButton->setCheckable(true);
  connect(m_groupButton, &QAbstractButton::clicked, this, [&]() {
    m_view->setGroup(m_groupButton->isChecked());
  });

  m_usedButton = new QPushButton{tr("Used")};
  m_usedButton->setToolTip(tr("Only show materials currently in use"));
  m_usedButton->setCheckable(true);
  connect(m_usedButton, &QAbstractButton::clicked, this, [&]() {
    m_view->setHideUnused(m_usedButton->isChecked());
  });

  m_decreaseIconSizeButton = new QPushButton{tr("-")};
  m_decreaseIconSizeButton->setFixedWidth(24);
  m_decreaseIconSizeButton->setToolTip(
    tr("Show more materials per row by making material previews smaller"));
  connect(m_decreaseIconSizeButton, &QAbstractButton::clicked, this, [&]() {
    adjustMaterialBrowserIconSize(-1);
  });

  m_increaseIconSizeButton = new QPushButton{tr("+")};
  m_increaseIconSizeButton->setFixedWidth(24);
  m_increaseIconSizeButton->setToolTip(
    tr("Show fewer materials per row by making material previews larger"));
  connect(m_increaseIconSizeButton, &QAbstractButton::clicked, this, [&]() {
    adjustMaterialBrowserIconSize(1);
  });

  m_filterBox = createSearchBox();
  connect(m_filterBox, &QLineEdit::textEdited, this, [&]() {
    m_view->setFilterText(m_filterBox->text().toStdString());
  });

  auto* controlLayout = new QHBoxLayout{};
  controlLayout->setContentsMargins(
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin,
    LayoutConstants::NarrowHMargin,
    LayoutConstants::NarrowVMargin);
  controlLayout->setSpacing(LayoutConstants::NarrowHMargin);
  controlLayout->addWidget(m_sortOrderChoice);
  controlLayout->addWidget(m_groupButton);
  controlLayout->addWidget(m_usedButton);
  controlLayout->addWidget(m_decreaseIconSizeButton);
  controlLayout->addWidget(m_increaseIconSizeButton);
  controlLayout->addWidget(m_filterBox, 1);

  auto* outerLayout = new QVBoxLayout{};
  outerLayout->setContentsMargins(0, 0, 0, 0);
  outerLayout->setSpacing(0);
  outerLayout->addWidget(browserPanel, 1);
  outerLayout->addLayout(controlLayout, 0);

  setLayout(outerLayout);
  updateIconSizeButtons();
}

void MaterialBrowser::bindEvents()
{
  connect(
    m_view,
    &MaterialBrowserView::materialSelected,
    this,
    &MaterialBrowser::materialSelected);
}

void MaterialBrowser::connectObservers()
{
  m_notifierConnection += m_document.documentWasLoadedNotifier.connect([&] { reload(); });
  m_notifierConnection += m_document.documentDidChangeNotifier.connect([&] { reload(); });
  m_notifierConnection += m_document.currentMaterialNameDidChangeNotifier.connect(
    [&] { updateSelectedMaterial(); });

  auto& prefs = PreferenceManager::instance();
  m_notifierConnection +=
    prefs.preferenceDidChangeNotifier.connect([&](const auto& path) {
      if (
        path == pref(m_document.map().gameInfo().gamePathPreference)
        || path == Preferences::MaterialBrowserIconSize.path)
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

void MaterialBrowser::reload()
{
  if (m_view)
  {
    updateSelectedMaterial();
    m_view->invalidate();
    m_view->update();
  }
}

void MaterialBrowser::updateSelectedMaterial()
{
  auto& map = m_document.map();

  const auto& materialName = map.currentMaterialName();
  const auto* material = map.materialManager().material(materialName);
  m_view->setSelectedMaterial(material);
}

void MaterialBrowser::adjustMaterialBrowserIconSize(const int direction)
{
  if (direction == 0)
  {
    return;
  }

  auto& prefs = PreferenceManager::instance();
  const auto currentSize = prefs.get(Preferences::MaterialBrowserIconSize);
  const auto currentIndex = nearestMaterialBrowserIconSizeStep(currentSize);
  const auto nextIndex = static_cast<size_t>(std::clamp<int>(
    static_cast<int>(currentIndex) + direction,
    0,
    static_cast<int>(MaterialBrowserIconSizeSteps.size()) - 1));

  if (nextIndex == currentIndex)
  {
    return;
  }

  setPref(Preferences::MaterialBrowserIconSize, MaterialBrowserIconSizeSteps[nextIndex]);
  reload();
  updateIconSizeButtons();
}

void MaterialBrowser::updateIconSizeButtons()
{
  if (!m_increaseIconSizeButton || !m_decreaseIconSizeButton)
  {
    return;
  }

  const auto currentSize =
    PreferenceManager::instance().get(Preferences::MaterialBrowserIconSize);
  const auto currentIndex = nearestMaterialBrowserIconSizeStep(currentSize);
  m_decreaseIconSizeButton->setEnabled(currentIndex > 0u);
  m_increaseIconSizeButton->setEnabled(
    currentIndex + 1u < MaterialBrowserIconSizeSteps.size());
}

} // namespace tb::ui
