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

#include "ui/PrefabInspector.h"

#include "Logger.h"
#include "SimpleParserStatus.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Node.h"
#include "mdl/NodeWriter.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"
#include "ui/MapDocument.h"
#include "ui/QPathUtils.h"

#include "kd/result.h"

#include "vm/bbox.h"

#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QStringList>
#include <QTextOption>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto PrefabPayloadPrefix = "prefab\n";
constexpr auto PrefabCollectionsSetting = "PrefabInspector/Collections";
constexpr auto PrefabColumnsSetting = "PrefabInspector/Columns";

struct PreviewSegment
{
  vm::vec3d start;
  vm::vec3d end;
  double depth = 0.0;
};

struct PrefabItem
{
  QString name;
  QString serialized;
  vm::bbox3d bounds;
  std::vector<PreviewSegment> previewSegments;
  size_t nodeCount = 0;
};

struct Collection
{
  std::filesystem::path path;
  QString title;
  std::vector<PrefabItem> prefabs;
  bool expanded = true;

  QWidget* widget = nullptr;
  QToolButton* toggleButton = nullptr;
  QWidget* prefabArea = nullptr;
  QGridLayout* gridLayout = nullptr;
};

std::string readTextFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    throw std::runtime_error{"Could not open file"};
  }

  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

void collectGroups(mdl::Node& node, std::vector<mdl::GroupNode*>& groups)
{
  if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node))
  {
    groups.push_back(groupNode);
  }

  for (auto* child : node.children())
  {
    collectGroups(*child, groups);
  }
}

size_t descendantNodeCount(const mdl::Node& node)
{
  auto result = size_t{1};
  for (const auto* child : node.children())
  {
    result += descendantNodeCount(*child);
  }
  return result;
}

void collectPreviewSegments(const mdl::Node& node, std::vector<PreviewSegment>& segments)
{
  constexpr auto MaxPreviewSegments = size_t{1200};
  if (segments.size() >= MaxPreviewSegments)
  {
    return;
  }

  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    for (const auto* edge : brushNode->brush().edges())
    {
      if (segments.size() >= MaxPreviewSegments)
      {
        break;
      }

      const auto start = edge->firstVertex()->position();
      const auto end = edge->secondVertex()->position();
      segments.push_back(
        PreviewSegment{
          start,
          end,
          (start.x() + start.y() + start.z() + end.x() + end.y() + end.z()) * 0.5});
    }
  }

  for (const auto* child : node.children())
  {
    collectPreviewSegments(*child, segments);
    if (segments.size() >= MaxPreviewSegments)
    {
      return;
    }
  }
}

std::vector<mdl::MapFormat> mapFormatsToTry(const mdl::Map& map)
{
  auto result = mdl::compatibleFormats(map.worldNode().mapFormat());
  for (const auto& formatConfig : map.gameInfo().gameConfig.fileFormats)
  {
    const auto format = mdl::formatFromName(formatConfig.format);
    const auto compatibleFormats = mdl::compatibleFormats(format);
    for (const auto compatibleFormat : compatibleFormats)
    {
      if (std::ranges::find(result, compatibleFormat) == std::end(result))
      {
        result.push_back(compatibleFormat);
      }
    }
  }
  return result;
}

QString bboxSizeText(const vm::bbox3d& bounds)
{
  const auto size = bounds.size();
  return QString{"%1 x %2 x %3"}
    .arg(size.x(), 0, 'f', 0)
    .arg(size.y(), 0, 'f', 0)
    .arg(size.z(), 0, 'f', 0);
}

std::array<vm::vec3d, 8> bboxCorners(const vm::bbox3d& bounds)
{
  return {{
    {bounds.min.x(), bounds.min.y(), bounds.min.z()},
    {bounds.max.x(), bounds.min.y(), bounds.min.z()},
    {bounds.max.x(), bounds.max.y(), bounds.min.z()},
    {bounds.min.x(), bounds.max.y(), bounds.min.z()},
    {bounds.min.x(), bounds.min.y(), bounds.max.z()},
    {bounds.max.x(), bounds.min.y(), bounds.max.z()},
    {bounds.max.x(), bounds.max.y(), bounds.max.z()},
    {bounds.min.x(), bounds.max.y(), bounds.max.z()},
  }};
}

QPointF rawPreviewPoint(const vm::vec3d& point, const vm::bbox3d& referenceBounds)
{
  const auto center = referenceBounds.center();
  const auto relative = point - center;
  return {
    (relative.x() - relative.y()) * 0.78,
    (relative.x() + relative.y()) * 0.34 - relative.z() * 0.90};
}

struct PreviewProjection
{
  vm::bbox3d referenceBounds;
  QPointF rawCenter;
  QPointF targetCenter;
  double scale = 1.0;

  QPointF project(const vm::vec3d& point) const
  {
    const auto raw = rawPreviewPoint(point, referenceBounds);
    return {
      targetCenter.x() + (raw.x() - rawCenter.x()) * scale,
      targetCenter.y() + (raw.y() - rawCenter.y()) * scale};
  }
};

PreviewProjection makePreviewProjection(
  const vm::bbox3d& referenceBounds, const QRectF& preview)
{
  const auto corners = bboxCorners(referenceBounds);
  auto minX = std::numeric_limits<double>::max();
  auto minY = std::numeric_limits<double>::max();
  auto maxX = std::numeric_limits<double>::lowest();
  auto maxY = std::numeric_limits<double>::lowest();
  for (const auto& corner : corners)
  {
    const auto raw = rawPreviewPoint(corner, referenceBounds);
    minX = std::min(minX, raw.x());
    minY = std::min(minY, raw.y());
    maxX = std::max(maxX, raw.x());
    maxY = std::max(maxY, raw.y());
  }

  const auto rawWidth = std::max(1.0, maxX - minX);
  const auto rawHeight = std::max(1.0, maxY - minY);
  const auto scale =
    std::min(preview.width() * 0.90 / rawWidth, preview.height() * 0.86 / rawHeight);

  return {
    referenceBounds,
    QPointF{(minX + maxX) * 0.5, (minY + maxY) * 0.5},
    preview.center(),
    scale};
}

void drawPreviewBox(
  QPainter& painter,
  const vm::bbox3d& bounds,
  const PreviewProjection& projection,
  const size_t brushCount)
{
  const auto corners = bboxCorners(bounds);
  const auto point = [&](const size_t index) {
    return projection.project(corners[index]);
  };

  const auto face = [&](std::initializer_list<size_t> indices) {
    auto polygon = QPolygonF{};
    for (const auto index : indices)
    {
      polygon << point(index);
    }
    return polygon;
  };

  const auto bottomFace = face({0, 1, 2, 3});
  const auto topFace = face({4, 5, 6, 7});
  const auto xFace = face({1, 2, 6, 5});
  const auto yFace = face({2, 3, 7, 6});
  const auto edgeAlpha = brushCount > 48 ? 105 : 170;
  const auto faceAlpha = brushCount > 48 ? 165 : 220;

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor{31, 47, 35, 95});
  painter.drawPolygon(bottomFace);
  painter.setBrush(QColor{45, 78, 55, faceAlpha - 30});
  painter.drawPolygon(yFace);
  painter.setBrush(QColor{57, 103, 69, faceAlpha - 15});
  painter.drawPolygon(xFace);
  painter.setBrush(QColor{89, 151, 94, faceAlpha});
  painter.drawPolygon(topFace);

  painter.setPen(QPen{QColor{28, 43, 31, 145}, 2});
  painter.setBrush(Qt::NoBrush);
  painter.drawPolygon(topFace);
  painter.drawPolygon(xFace);
  painter.drawPolygon(yFace);

  painter.setPen(QPen{QColor{185, 255, 190, edgeAlpha}, 1});
  static constexpr auto Edges = std::array<std::pair<size_t, size_t>, 12>{{
    {0, 1},
    {1, 2},
    {2, 3},
    {3, 0},
    {4, 5},
    {5, 6},
    {6, 7},
    {7, 4},
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7},
  }};
  for (const auto& [start, end] : Edges)
  {
    painter.drawLine(point(start), point(end));
  }
}

void drawPreviewSegments(
  QPainter& painter,
  const std::vector<PreviewSegment>& segments,
  const PreviewProjection& projection)
{
  const auto segmentCount = segments.size();
  const auto lineAlpha = segmentCount > 800 ? 135 : segmentCount > 400 ? 165 : 210;
  const auto shadowAlpha = segmentCount > 800 ? 120 : 165;

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen{QColor{6, 10, 7, shadowAlpha}, 3.0, Qt::SolidLine, Qt::RoundCap});
  for (const auto& segment : segments)
  {
    painter.drawLine(projection.project(segment.start), projection.project(segment.end));
  }

  painter.setPen(
    QPen{QColor{181, 255, 188, lineAlpha}, 1.15, Qt::SolidLine, Qt::RoundCap});
  for (const auto& segment : segments)
  {
    painter.drawLine(projection.project(segment.start), projection.project(segment.end));
  }
}

class PrefabCard : public QFrame
{
private:
  PrefabItem m_prefab;
  std::vector<PreviewSegment> m_previewSegments;
  QPoint m_dragStartPosition;
  mutable QPixmap m_previewPixmap;
  mutable QSize m_previewPixmapSize;
  mutable qreal m_previewPixmapDevicePixelRatio = 0.0;

public:
  explicit PrefabCard(PrefabItem prefab, QWidget* parent = nullptr)
    : QFrame{parent}
    , m_prefab{std::move(prefab)}
  {
    m_previewSegments = m_prefab.previewSegments;
    std::stable_sort(
      m_previewSegments.begin(),
      m_previewSegments.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.depth < rhs.depth; });

    setFrameStyle(QFrame::StyledPanel);
    setCursor(Qt::OpenHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(158);
    setToolTip(
      QObject::tr("%1\n%2 nodes\n%3")
        .arg(m_prefab.name)
        .arg(m_prefab.nodeCount)
        .arg(bboxSizeText(m_prefab.bounds)));
  }

  QSize sizeHint() const override { return {136, 158}; }

private:
  const QPixmap& previewPixmap(const QSize& logicalSize) const
  {
    const auto devicePixelRatio = devicePixelRatioF();
    if (
      !m_previewPixmap.isNull() && m_previewPixmapSize == logicalSize
      && m_previewPixmapDevicePixelRatio == devicePixelRatio)
    {
      return m_previewPixmap;
    }

    const auto pixelSize = QSize{
      std::max(1, int(std::ceil(double(logicalSize.width()) * devicePixelRatio))),
      std::max(1, int(std::ceil(double(logicalSize.height()) * devicePixelRatio)))};
    m_previewPixmap = QPixmap{pixelSize};
    m_previewPixmap.setDevicePixelRatio(devicePixelRatio);
    m_previewPixmap.fill(Qt::transparent);
    m_previewPixmapSize = logicalSize;
    m_previewPixmapDevicePixelRatio = devicePixelRatio;

    auto painter = QPainter{&m_previewPixmap};
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto preview =
      QRectF{0.0, 0.0, double(logicalSize.width()), double(logicalSize.height())};
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{36, 40, 41});
    painter.drawRoundedRect(preview, 4, 4);

    const auto innerPreview = preview.adjusted(7, 7, -7, -7);
    const auto projection = makePreviewProjection(m_prefab.bounds, innerPreview);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{17, 19, 19, 70});
    painter.drawEllipse(
      QRectF{
        innerPreview.left() + innerPreview.width() * 0.16,
        innerPreview.bottom() - innerPreview.height() * 0.20,
        innerPreview.width() * 0.68,
        innerPreview.height() * 0.14});

    if (m_previewSegments.empty())
    {
      drawPreviewBox(painter, m_prefab.bounds, projection, 1);
    }
    else
    {
      drawPreviewSegments(painter, m_previewSegments, projection);
    }

    painter.setPen(QPen{QColor{205, 255, 210, 175}, 1});
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(preview.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    return m_previewPixmap;
  }

protected:
  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton)
    {
      m_dragStartPosition = event->pos();
      setCursor(Qt::ClosedHandCursor);
    }
    QFrame::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    setCursor(Qt::OpenHandCursor);
    QFrame::mouseReleaseEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    if (!(event->buttons() & Qt::LeftButton))
    {
      QFrame::mouseMoveEvent(event);
      return;
    }

    if (
      (event->pos() - m_dragStartPosition).manhattanLength()
      < QApplication::startDragDistance())
    {
      return;
    }

    auto* mimeData = new QMimeData{};
    mimeData->setText(QString{PrefabPayloadPrefix} + m_prefab.serialized);

    auto* drag = new QDrag{this};
    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);
    setCursor(Qt::OpenHandCursor);
  }

  void paintEvent(QPaintEvent* event) override
  {
    QFrame::paintEvent(event);

    auto painter = QPainter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto content = rect().adjusted(8, 8, -8, -8);
    const auto preview = QRectF{
      double(content.left()), double(content.top()), double(content.width()), 88.0};
    const auto title = QRectF{
      double(content.left()),
      preview.bottom() + 8.0,
      double(content.width()),
      double(content.bottom()) - preview.bottom() - 8.0};

    painter.drawPixmap(preview.topLeft(), previewPixmap(preview.size().toSize()));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{34, 37, 38, 210});
    painter.drawRoundedRect(title, 4, 4);

    auto font = painter.font();
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
    painter.setFont(font);
    const auto text = painter.fontMetrics().elidedText(
      m_prefab.name, Qt::ElideRight, int(title.width()) - 8);
    auto textOption = QTextOption{Qt::AlignCenter};
    textOption.setWrapMode(QTextOption::NoWrap);
    painter.setPen(QColor{225, 230, 225});
    painter.drawText(title.adjusted(4, 0, -4, 0), text, textOption);
  }
};

} // namespace

class PrefabInspector::Impl
{
private:
  PrefabInspector& m_owner;
  MapDocument& m_document;
  QComboBox* m_columnsComboBox = nullptr;
  QWidget* m_collectionsWidget = nullptr;
  QVBoxLayout* m_collectionsLayout = nullptr;
  std::vector<std::unique_ptr<Collection>> m_collections;

public:
  explicit Impl(PrefabInspector& owner, MapDocument& document)
    : m_owner{owner}
    , m_document{document}
  {
    createGui();
  }

  ~Impl() { saveSettings(); }

private:
  void createGui()
  {
    auto* addButton = new QPushButton{QObject::tr("Add Collection")};
    m_columnsComboBox = new QComboBox{};
    m_columnsComboBox->addItem(QObject::tr("1 / row"), 1);
    m_columnsComboBox->addItem(QObject::tr("2 / row"), 2);
    m_columnsComboBox->addItem(QObject::tr("3 / row"), 3);
    m_columnsComboBox->addItem(QObject::tr("4 / row"), 4);
    const auto savedColumns =
      std::clamp(QSettings{}.value(PrefabColumnsSetting, 2).toInt(), 1, 4);
    m_columnsComboBox->setCurrentIndex(savedColumns - 1);

    auto* headerLayout = new QHBoxLayout{};
    headerLayout->setContentsMargins(8, 8, 8, 8);
    headerLayout->addWidget(addButton, 1);
    headerLayout->addWidget(m_columnsComboBox, 0);

    m_collectionsWidget = new QWidget{};
    m_collectionsLayout = new QVBoxLayout{};
    m_collectionsLayout->setContentsMargins(8, 0, 8, 8);
    m_collectionsLayout->setSpacing(8);
    m_collectionsLayout->addStretch(1);
    m_collectionsWidget->setLayout(m_collectionsLayout);

    auto* scrollArea = new QScrollArea{};
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(m_collectionsWidget);

    auto* layout = new QVBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerLayout, 0);
    layout->addWidget(scrollArea, 1);
    m_owner.setLayout(layout);

    QObject::connect(
      addButton, &QPushButton::clicked, &m_owner, [this]() { addCollection(); });
    QObject::connect(
      m_columnsComboBox,
      QOverload<int>::of(&QComboBox::currentIndexChanged),
      &m_owner,
      [this](int) {
        rebuildAllGrids();
        QSettings{}.setValue(PrefabColumnsSetting, columns());
      });

    loadSettings();
  }

  int columns() const
  {
    return std::clamp(m_columnsComboBox->currentData().toInt(), 1, 4);
  }

  void addCollection()
  {
    const auto fileName = QFileDialog::getOpenFileName(
      &m_owner,
      QObject::tr("Add Prefab Collection"),
      QString{},
      QObject::tr("Map Files (*.map);;All Files (*.*)"));
    if (fileName.isEmpty())
    {
      return;
    }

    const auto path = pathFromQString(fileName);
    if (hasCollection(path))
    {
      return;
    }

    auto collection = loadCollection(path, true);
    if (!collection)
    {
      return;
    }

    auto* collectionPtr = collection.get();
    createCollectionWidgets(*collectionPtr);
    m_collectionsLayout->insertWidget(
      std::max(0, m_collectionsLayout->count() - 1), collectionPtr->widget, 0);
    m_collections.push_back(std::move(collection));
    saveSettings();
  }

  std::unique_ptr<Collection> loadCollection(
    const std::filesystem::path& path, const bool showWarnings)
  {
    auto fileContents = std::string{};
    try
    {
      fileContents = readTextFile(path);
    }
    catch (const std::exception& e)
    {
      if (showWarnings)
      {
        QMessageBox::warning(
          &m_owner,
          QObject::tr("Add Prefab Collection"),
          QObject::tr("Could not read '%1':\n%2").arg(pathAsQString(path)).arg(e.what()));
      }
      return nullptr;
    }

    if (fileContents.empty())
    {
      if (showWarnings)
      {
        QMessageBox::warning(
          &m_owner,
          QObject::tr("Add Prefab Collection"),
          QObject::tr("The selected map is empty."));
      }
      return nullptr;
    }

    auto& map = m_document.map();
    auto parserStatus = SimpleParserStatus{map.logger()};
    auto worldResult = mdl::WorldReader::tryRead(
      fileContents,
      mapFormatsToTry(map),
      map.worldBounds(),
      map.worldNode().entityPropertyConfig(),
      parserStatus,
      map.taskManager());

    if (!worldResult)
    {
      if (showWarnings)
      {
        auto message =
          QString{QObject::tr("Could not parse '%1'.").arg(pathAsQString(path))};
        std::visit(
          [&](const auto& error) {
            message += QString{"\n%1"}.arg(QString::fromStdString(error.msg));
          },
          worldResult.error());
        QMessageBox::warning(&m_owner, QObject::tr("Add Prefab Collection"), message);
      }
      return nullptr;
    }

    auto world = std::move(worldResult).value();
    auto groups = std::vector<mdl::GroupNode*>{};
    collectGroups(*world, groups);

    if (groups.empty())
    {
      if (showWarnings)
      {
        QMessageBox::information(
          &m_owner,
          QObject::tr("Add Prefab Collection"),
          QObject::tr("The selected map does not contain any groups."));
      }
      return nullptr;
    }

    auto collection = std::make_unique<Collection>();
    collection->path = path;
    collection->title = pathAsQString(path.filename());

    for (auto* group : groups)
    {
      auto stream = std::stringstream{};
      auto writer = mdl::NodeWriter{*world, stream};
      auto nodes = std::vector<mdl::Node*>{group};
      writer.writeNodes(nodes, map.taskManager());
      auto previewSegments = std::vector<PreviewSegment>{};
      collectPreviewSegments(*group, previewSegments);

      collection->prefabs.push_back(
        PrefabItem{
          QString::fromStdString(group->group().name()),
          QString::fromStdString(stream.str()),
          group->logicalBounds(),
          std::move(previewSegments),
          descendantNodeCount(*group)});
    }

    return collection;
  }

  void createCollectionWidgets(Collection& collection)
  {
    collection.widget = new QFrame{};
    collection.widget->setObjectName("PrefabCollection");
    collection.widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    collection.widget->setStyleSheet(
      "#PrefabCollection { border: 1px solid palette(mid); border-radius: 4px; }");

    collection.toggleButton = new QToolButton{};
    collection.toggleButton->setText("v");
    collection.toggleButton->setAutoRaise(true);

    auto* titleLabel = new QLabel{
      QObject::tr("%1 (%2)").arg(collection.title).arg(collection.prefabs.size())};
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* removeButton = new QToolButton{};
    removeButton->setText("X");
    removeButton->setAutoRaise(true);
    removeButton->setToolTip(QObject::tr("Remove collection"));

    auto* headerLayout = new QHBoxLayout{};
    headerLayout->setContentsMargins(6, 4, 6, 4);
    headerLayout->addWidget(collection.toggleButton, 0);
    headerLayout->addWidget(titleLabel, 1);
    headerLayout->addWidget(removeButton, 0);

    collection.prefabArea = new QWidget{};
    collection.prefabArea->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    collection.gridLayout = new QGridLayout{};
    collection.gridLayout->setContentsMargins(6, 4, 6, 6);
    collection.gridLayout->setSpacing(6);
    collection.gridLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    collection.prefabArea->setLayout(collection.gridLayout);

    auto* layout = new QVBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerLayout, 0);
    layout->addWidget(collection.prefabArea, 0);
    collection.widget->setLayout(layout);

    QObject::connect(
      collection.toggleButton, &QToolButton::clicked, &m_owner, [&collection]() {
        collection.expanded = !collection.expanded;
        collection.prefabArea->setVisible(collection.expanded);
        collection.toggleButton->setText(collection.expanded ? "v" : ">");
        collection.gridLayout->invalidate();
        collection.prefabArea->adjustSize();
        collection.prefabArea->updateGeometry();
        collection.widget->adjustSize();
        collection.widget->updateGeometry();
      });

    QObject::connect(
      removeButton, &QToolButton::clicked, &m_owner, [this, &collection]() {
        const auto result = QMessageBox::question(
          &m_owner,
          QObject::tr("Remove Prefab Collection"),
          QObject::tr("Remove '%1' from the prefab list?").arg(collection.title));
        if (result == QMessageBox::Yes)
        {
          removeCollection(&collection);
        }
      });

    rebuildGrid(collection);
  }

  void removeCollection(Collection* collection)
  {
    const auto it = std::ranges::find_if(
      m_collections, [&](const auto& item) { return item.get() == collection; });
    if (it == std::end(m_collections))
    {
      return;
    }

    delete (*it)->widget;
    m_collections.erase(it);
    saveSettings();
  }

  void rebuildAllGrids()
  {
    for (auto& collection : m_collections)
    {
      rebuildGrid(*collection);
    }
  }

  void rebuildGrid(Collection& collection)
  {
    while (auto* item = collection.gridLayout->takeAt(0))
    {
      delete item->widget();
      delete item;
    }

    const auto columnCount = columns();
    const auto minWidth = std::max(72, 220 / columnCount);
    for (size_t i = 0; i < collection.prefabs.size(); ++i)
    {
      auto* card = new PrefabCard{collection.prefabs[i]};
      card->setMinimumWidth(minWidth);
      collection.gridLayout->addWidget(
        card, int(i / size_t(columnCount)), int(i % size_t(columnCount)));
    }
    collection.gridLayout->setRowStretch(
      int((collection.prefabs.size() + size_t(columnCount) - 1) / size_t(columnCount)),
      1);
    collection.gridLayout->activate();
    collection.prefabArea->adjustSize();
    collection.prefabArea->updateGeometry();
    collection.widget->adjustSize();
    collection.widget->updateGeometry();
  }

  bool hasCollection(const std::filesystem::path& path) const
  {
    const auto normalizedPath = path.lexically_normal();
    return std::ranges::any_of(m_collections, [&](const auto& collection) {
      return collection->path.lexically_normal() == normalizedPath;
    });
  }

  void loadSettings()
  {
    const auto paths = QSettings{}.value(PrefabCollectionsSetting).toStringList();
    for (const auto& pathString : paths)
    {
      const auto path = pathFromQString(pathString);
      if (path.empty() || hasCollection(path))
      {
        continue;
      }

      auto collection = loadCollection(path, false);
      if (!collection)
      {
        continue;
      }

      auto* collectionPtr = collection.get();
      createCollectionWidgets(*collectionPtr);
      m_collectionsLayout->insertWidget(
        std::max(0, m_collectionsLayout->count() - 1), collectionPtr->widget, 0);
      m_collections.push_back(std::move(collection));
    }
  }

  void saveSettings() const
  {
    auto paths = QStringList{};
    for (const auto& collection : m_collections)
    {
      paths.push_back(pathAsQString(collection->path));
    }

    auto settings = QSettings{};
    settings.setValue(PrefabCollectionsSetting, paths);
    settings.setValue(PrefabColumnsSetting, columns());
  }
};

PrefabInspector::PrefabInspector(MapDocument& document, QWidget* parent)
  : TabBookPage{parent}
  , m_impl{std::make_unique<Impl>(*this, document)}
{
}

PrefabInspector::~PrefabInspector() = default;

} // namespace tb::ui
