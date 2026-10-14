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

#include "ui/MapViewBar.h"

#include <QHBoxLayout>
#include <QResizeEvent>
#include <QStackedLayout>
#include <QtSystemDetection>
#include <QWidgetItem>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"
#include "ui/ViewEditor.h"

namespace tb::ui
{
namespace
{
// Hidden two-row tool pages must not reserve height for the active one-row page.
class ToolPageLayout : public QStackedLayout
{
public:
  QSize sizeHint() const override
  {
    auto result = QStackedLayout::sizeHint();
    result.setHeight(
      currentWidget() ? QWidgetItem{currentWidget()}.sizeHint().height() : 0);
    return result;
  }

  QSize minimumSize() const override
  {
    auto result = QStackedLayout::minimumSize();
    result.setHeight(
      currentWidget() ? QWidgetItem{currentWidget()}.minimumSize().height() : 0);
    return result;
  }

  bool hasHeightForWidth() const override
  {
    const auto* item = itemAt(currentIndex());
    return item && item->hasHeightForWidth();
  }

  int heightForWidth(const int width) const override
  {
    const auto* item = itemAt(currentIndex());
    return item ? item->heightForWidth(width) : -1;
  }
};
} // namespace

MapViewBar::MapViewBar(MapDocument& document, QWidget* parent)
  : ContainerBar(Sides::BottomSide, parent)
{
  createGui(document);
}

QStackedLayout* MapViewBar::toolBook()
{
  return m_toolBook;
}

void MapViewBar::createGui(MapDocument& document)
{
  setAttribute(Qt::WA_MacSmallSize);

  m_toolBook = new ToolPageLayout{};
  m_toolBook->setContentsMargins(0, 0, 0, 0);
  connect(m_toolBook, &QStackedLayout::currentChanged, this, [this](const int) {
    m_toolBook->invalidate();
    updateGeometry();
  });

  m_viewEditor = new ViewPopupEditor{document};

#if defined(Q_OS_MACOS)
  const auto vMargin = pref(Preferences::Theme) == Preferences::DarkTheme
                         ? LayoutConstants::MediumVMargin
                         : 0;
#else
  const auto vMargin = LayoutConstants::MediumVMargin;
#endif

  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(
    LayoutConstants::WideHMargin, vMargin, LayoutConstants::WideHMargin, vMargin);
  layout->setSpacing(LayoutConstants::WideHMargin);
  layout->addLayout(m_toolBook, 1);
  layout->addWidget(m_viewEditor, 0, Qt::AlignVCenter);

  setLayout(layout);
}

} // namespace tb::ui
