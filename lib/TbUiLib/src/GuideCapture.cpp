/*
 Copyright (C) 2026 Kristian Duske
 This file is part of TrenchBroom, licensed under the GNU General Public License,
 version 3 or later. See LICENSE.txt for details.
 */
#include "ui/GuideCapture.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QStatusBar>
#include <QTimer>
#include <QWheelEvent>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/Camera.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameManager.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Picking.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeWriter.h"
#include "mdl/PickResult.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/WorldNode.h"
#include "ui/AppController.h"
#include "ui/BezierBendTool.h"
#include "ui/EdgeTool.h"
#include "ui/EntityBrowser.h"
#include "ui/FaceTool.h"
#include "ui/Inspector.h"
#include "ui/MapDocument.h"
#include "ui/MapView3D.h"
#include "ui/MapViewBar.h"
#include "ui/MapViewLayout.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/MaterialBrowser.h"
#include "ui/PolygonCutTool.h"
#include "ui/PrefabTool.h"
#include "ui/QPathUtils.h"
#include "ui/SculptTool.h"
#include "ui/SwitchableMapViewContainer.h"
#include "ui/SwitchableTitledPanel.h"
#include "ui/SymmetryTool.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace tb::ui
{
namespace
{
vm::vec3d readVector(const QJsonValue& value)
{
  const auto values = value.toArray();
  if (values.size() != 3)
  {
    throw std::runtime_error{"Expected a three-component vector"};
  }
  auto result = vm::vec3d{};
  for (int i = 0; i < 3; ++i)
  {
    if (!values[i].isDouble() || !std::isfinite(values[i].toDouble()))
    {
      throw std::runtime_error{"Vector components must be finite numbers"};
    }
    result[size_t(i)] = values[i].toDouble();
  }
  return result;
}

void writeJson(const QString& path, const QJsonObject& object)
{
  auto file = QSaveFile{path};
  const auto data = QJsonDocument{object}.toJson();
  if (
    !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
  {
    throw std::runtime_error{"Could not save capture metadata"};
  }
}

bool containsLabel(QLayoutItem* item, const QString& name)
{
  if (!item)
  {
    return false;
  }
  if (auto* label = qobject_cast<QLabel*>(item->widget()))
  {
    return label->isVisible() && label->text() == name;
  }
  if (auto* layout = item->layout())
  {
    for (int i = 0; i < layout->count(); ++i)
    {
      if (containsLabel(layout->itemAt(i), name))
      {
        return true;
      }
    }
  }
  return false;
}

QWidget* firstField(QLayoutItem* item)
{
  if (!item)
  {
    return nullptr;
  }
  if (auto* widget = item->widget(); widget && widget->isVisible())
  {
    return widget;
  }
  if (auto* layout = item->layout())
  {
    for (int i = 0; i < layout->count(); ++i)
    {
      if (auto* field = firstField(layout->itemAt(i)))
      {
        return field;
      }
    }
  }
  return nullptr;
}

QWidget* labelledControl(QLayout* layout, const QString& name)
{
  if (!layout)
  {
    return nullptr;
  }
  if (auto* form = qobject_cast<QFormLayout*>(layout))
  {
    for (int row = 0; row < form->rowCount(); ++row)
    {
      if (containsLabel(form->itemAt(row, QFormLayout::LabelRole), name))
      {
        return firstField(form->itemAt(row, QFormLayout::FieldRole));
      }
    }
  }
  for (int i = 0; i < layout->count(); ++i)
  {
    auto* item = layout->itemAt(i);
    if (
      auto* label = qobject_cast<QLabel*>(item->widget());
      label && label->isVisible() && label->text() == name && i + 1 < layout->count())
    {
      auto* next = layout->itemAt(i + 1)->widget();
      if (next && next->isVisible())
      {
        return next;
      }
    }
    if (auto* nested = item->layout())
    {
      if (auto* control = labelledControl(nested, name))
      {
        return control;
      }
    }
    if (auto* child = item->widget(); child && child->isVisible())
    {
      if (auto* control = labelledControl(child->layout(), name))
      {
        return control;
      }
    }
  }
  return nullptr;
}

void setControl(QWidget& root, const QString& name, const QJsonValue& value)
{
  for (auto* checkbox : root.findChildren<QCheckBox*>())
  {
    if (checkbox->isVisible() && checkbox->text() == name)
    {
      checkbox->setChecked(value.toBool());
      return;
    }
  }
  auto* control = labelledControl(root.layout(), name);
  if (!control)
  {
    for (auto* widget : root.findChildren<QWidget*>())
    {
      if (
        widget->isVisible()
        && (widget->objectName() == name || widget->toolTip() == name))
      {
        control = widget;
        break;
      }
      if (
        auto* combo = qobject_cast<QComboBox*>(widget);
        combo && combo->isVisible() && combo->currentText() == name)
      {
        control = combo;
        break;
      }
    }
  }
  if (auto* doubleInput = qobject_cast<QDoubleSpinBox*>(control))
  {
    doubleInput->setValue(value.toDouble());
  }
  else if (auto* integerInput = qobject_cast<QSpinBox*>(control))
  {
    integerInput->setValue(value.toInt());
  }
  else if (auto* combo = qobject_cast<QComboBox*>(control))
  {
    const auto index =
      value.isString() ? combo->findText(value.toString()) : value.toInt(-1);
    if (index < 0 || index >= combo->count())
    {
      throw std::runtime_error{"Unknown combo box option"};
    }
    combo->setCurrentIndex(index);
    combo->activated(index);
  }
  else if (auto* textInput = qobject_cast<QLineEdit*>(control))
  {
    textInput->setText(value.toString());
  }
  else
  {
    throw std::runtime_error{("Control not found: " + name).toStdString()};
  }
}

bool containsAbsolutePath(const QString& text)
{
  static const auto pattern =
    QRegularExpression{R"((?:[A-Za-z]:[\\/]|/(?:Users|home|mnt)/|\\\\))"};
  return pattern.match(text).hasMatch();
}

QString examplePathLabel(const QString& text)
{
  if (text.contains(".wad", Qt::CaseInsensitive))
  {
    return "Textures/example.wad";
  }
  if (text.contains(".lmp", Qt::CaseInsensitive))
  {
    return "Palettes/example.lmp";
  }
  if (text.contains("Textures", Qt::CaseInsensitive))
  {
    return "ExampleProject/Textures";
  }
  return "ExampleProject/Assets";
}

int redactScreenshotPaths(QPainter& painter, QWidget& root)
{
  auto count = 0;
  const auto mask = [&](QRect rect, const QString& label, QWidget& source) {
    rect = rect.translated(source.mapTo(&root, QPoint{})).intersected(root.rect());
    if (rect.isEmpty())
    {
      return;
    }
    painter.save();
    painter.setClipRect(rect);
    painter.fillRect(rect, QColor{230, 232, 232});
    painter.setPen(QColor{70, 78, 80});
    painter.setFont(source.font());
    const auto text = QFontMetrics{source.font()}.elidedText(
      label, Qt::ElideRight, std::max(0, rect.width() - 8));
    painter.drawText(rect.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
    painter.restore();
    ++count;
  };
  for (auto* widget : root.findChildren<QWidget*>())
  {
    if (!widget->isVisibleTo(&root))
    {
      continue;
    }
    if (
      auto* label = qobject_cast<QLabel*>(widget);
      label && containsAbsolutePath(label->text()))
    {
      mask(label->rect(), examplePathLabel(label->text()), *label);
    }
    else if (
      auto* input = qobject_cast<QLineEdit*>(widget);
      input && containsAbsolutePath(input->text()))
    {
      mask(input->rect().adjusted(2, 2, -2, -2), examplePathLabel(input->text()), *input);
    }
    if (auto* view = qobject_cast<QAbstractItemView*>(widget); view && view->model())
    {
      const auto* model = view->model();
      for (int row = 0; row < model->rowCount(); ++row)
      {
        for (int column = 0; column < model->columnCount(); ++column)
        {
          const auto index = model->index(row, column);
          auto rect = view->visualRect(index).intersected(view->viewport()->rect());
          if (rect.isEmpty())
          {
            continue;
          }
          const auto text = model->data(index, Qt::DisplayRole).toString();
          if (containsAbsolutePath(text))
          {
            if (!model->data(index, Qt::DecorationRole).isNull())
            {
              rect.adjust(22, 0, 0, 0);
            }
            mask(rect, examplePathLabel(text), *view->viewport());
          }
        }
      }
    }
  }
  return count;
}

class GuideCaptureSession : public QObject
{
  AppController& m_controller;
  QPointer<MapWindow> m_window;
  QJsonObject m_script;
  QJsonArray m_steps;
  QJsonArray m_frames;
  std::map<std::string, mdl::Node*> m_nodes;
  std::map<std::string, std::string> m_prefabPayloads;
  QString m_output;
  int m_index = 0;

public:
  GuideCaptureSession(AppController& controller, QJsonObject script, QString output)
    : QObject{&controller}
    , m_controller{controller}
    , m_script{std::move(script)}
    , m_steps{m_script.value("steps").toArray()}
    , m_output{std::move(output)}
  {
  }

  void start()
  {
    qApp->setProperty("guidePathPrivacy", true);
    // Always use a new document in this dedicated process, never the user's open map.
    if (!m_controller.mapWindowManager().mapWindows().empty())
    {
      fail("Guide capture requires a separate editor process");
      return;
    }
    const auto gameName = m_script.value("game").toString("Quake");
    const auto* game = m_controller.gameManager().gameInfo(gameName.toStdString());
    if (!game)
    {
      fail("Unknown capture game: " + gameName);
      return;
    }
    setPref(Preferences::FaceRenderMode, std::string{Preferences::FaceRenderModeFlat});
    setPref(Preferences::ShadeFaces, true);
    setPref(Preferences::ShowEdges, true);
    setPref(Preferences::OccludedSelectedEdgeAlpha, 0.0f);
    setPref(Preferences::FaceColor, Color{RgbF{0.6f, 0.6f, 0.6f}});
    setPref(Preferences::PreviewQuakeLighting, false);
    setPref(Preferences::GridRotation, 0.0f);
    setPref(Preferences::GridUseUnified3D, true);
    const auto result = m_controller.mapWindowManager().createDocument(
      *game, mdl::MapFormat::Valve, MapDocument::DefaultWorldBounds);
    if (!result)
    {
      fail("Could not create the capture document");
      return;
    }
    m_window = m_controller.mapWindowManager().topMapWindow();
    if (!m_window)
    {
      fail("Capture window was not created");
      return;
    }
    container().switchToMapView(MapViewLayout::OnePane);
    m_window->resize(
      m_script.value("width").toInt(1536), m_script.value("height").toInt(960));
    m_window->show();
    qApp->setQuitOnLastWindowClosed(false);
    m_window->switchToInspectorPage(InspectorPage::Map);
    if (auto* splitter = m_window->findChild<QSplitter*>("MapWindow_HorizontalSplitter"))
    {
      splitter->setSizes({1180, 350});
    }
    if (m_window->infoPanelVisible())
    {
      m_window->toggleInfoPanel();
    }
    if (m_script.contains("gamePath"))
    {
      map().setGamePath(pathFromQString(m_script.value("gamePath").toString()));
    }
    QTimer::singleShot(1200, this, [this] { advance(); });
  }

private:
  mdl::Map& map() { return m_window->document().map(); }

  SwitchableMapViewContainer& container()
  {
    auto* result = m_window->findChild<SwitchableMapViewContainer*>();
    if (!result)
    {
      throw std::runtime_error{"Map view container was not found"};
    }
    return *result;
  }

  MapViewToolBox& tools() { return container().mapViewToolBox(); }

  MapViewBase& view()
  {
    for (auto* candidate : m_window->findChildren<MapView3D*>())
    {
      if (candidate->isVisible())
      {
        return *candidate;
      }
    }
    throw std::runtime_error{"A visible 3D viewport is required"};
  }

  QWidget& root(const QJsonObject& step)
  {
    const auto scope = step.value("scope").toString("page");
    if (scope == "popup")
    {
      for (auto* widget : QApplication::topLevelWidgets())
      {
        if (widget->isVisible() && widget->objectName() == "transformHudEditor")
        {
          return *widget;
        }
      }
      throw std::runtime_error{"No coordinate editor popup"};
    }
    if (scope == "material-browser")
    {
      for (auto* browser : m_window->findChildren<MaterialBrowser*>())
      {
        if (browser->isVisible())
        {
          return *browser;
        }
      }
      throw std::runtime_error{"No visible material browser"};
    }
    if (scope == "entity-browser")
    {
      for (auto* browser : m_window->findChildren<EntityBrowser*>())
      {
        if (browser->isVisible())
        {
          return *browser;
        }
      }
      throw std::runtime_error{"No visible entity browser"};
    }
    if (scope == "dialog")
    {
      if (auto* dialog = QApplication::activeModalWidget())
      {
        return *dialog;
      }
      for (auto* widget : QApplication::topLevelWidgets())
      {
        if (qobject_cast<QDialog*>(widget) && widget->isVisible())
        {
          return *widget;
        }
      }
      throw std::runtime_error{"No visible dialog"};
    }
    if (scope == "window")
    {
      return *m_window;
    }
    auto* bar = m_window->findChild<MapViewBar*>();
    if (!bar || !bar->toolBook()->currentWidget())
    {
      throw std::runtime_error{"No active tool settings page"};
    }
    return *bar->toolBook()->currentWidget();
  }

  mdl::Node* node(const QString& id)
  {
    const auto it = m_nodes.find(id.toStdString());
    if (it == m_nodes.end())
    {
      throw std::runtime_error{("Unknown scene node: " + id).toStdString()};
    }
    return it->second;
  }

  mdl::BrushNode& brush(const QJsonObject& step)
  {
    auto* result = dynamic_cast<mdl::BrushNode*>(node(step.value("id").toString()));
    if (!result)
    {
      throw std::runtime_error{"The scene node is not a brush"};
    }
    return *result;
  }

  void select(const QJsonArray& ids)
  {
    auto nodes = std::vector<mdl::Node*>{};
    for (const auto& id : ids)
    {
      nodes.push_back(node(id.toString()));
    }
    mdl::deselectAll(map());
    mdl::selectNodes(map(), nodes);
  }

  void resetScene()
  {
    tools().symmetryTool().setMode(SymmetryTool::Mode::Off);
    container().deactivateCurrentTool();
    mdl::deselectAll(map());
    m_nodes.clear();
    const auto children = map().worldNode().defaultLayer()->children();
    if (!children.empty())
    {
      mdl::removeNodes(map(), children);
    }
    map().grid().setSize(4);
    map().setCurrentMaterialName(
      m_script.value("material").toString("__TB_empty").toStdString());
  }

  void addBrush(const QJsonObject& step)
  {
    const auto builder =
      mdl::BrushBuilder{map().worldNode().mapFormat(), map().worldBounds()};
    const auto material = step.value("material")
                            .toString(m_script.value("material").toString("__TB_empty"))
                            .toStdString();
    auto result = [&] {
      if (step.contains("points"))
      {
        auto points = std::vector<vm::vec3d>{};
        for (const auto& point : step.value("points").toArray())
        {
          points.push_back(readVector(point));
        }
        return builder.createBrush(points, material);
      }
      return builder.createCuboid(
        vm::bbox3d{readVector(step.value("min")), readVector(step.value("max"))},
        material);
    }();
    if (!result)
    {
      throw std::runtime_error{"Invalid demonstration brush"};
    }
    auto* created = new mdl::BrushNode{std::move(result).value()};
    auto* parent = step.contains("parent") ? node(step.value("parent").toString())
                                           : map().worldNode().defaultLayer();
    mdl::addNodes(map(), {{parent, {created}}});
    m_nodes[step.value("id").toString().toStdString()] = created;
  }

  void addEntity(const QJsonObject& step)
  {
    auto properties = std::vector<mdl::EntityProperty>{};
    const auto json = step.value("properties").toObject();
    for (auto it = json.begin(); it != json.end(); ++it)
    {
      properties.emplace_back(
        it.key().toStdString(), it.value().toVariant().toString().toStdString());
    }
    auto* created = new mdl::EntityNode{mdl::Entity{std::move(properties)}};
    mdl::addNodes(map(), {{map().worldNode().defaultLayer(), {created}}});
    m_nodes[step.value("id").toString().toStdString()] = created;
    if (step.contains("brushes"))
    {
      auto children = std::vector<mdl::Node*>{};
      for (const auto& id : step.value("brushes").toArray())
      {
        children.push_back(node(id.toString()));
      }
      if (!mdl::reparentNodes(map(), {{created, children}}))
      {
        throw std::runtime_error{"Could not create the demonstration brush entity"};
      }
    }
  }

  void selectFace(const QJsonObject& step)
  {
    auto& source = brush(step);
    const auto index = source.brush().findFace(readVector(step.value("normal")));
    if (!index)
    {
      throw std::runtime_error{"Brush face was not found"};
    }
    const auto& face = source.brush().face(*index);
    if (tools().insetToolActive())
    {
      auto pick = mdl::PickResult::byDistance();
      pick.addHit(
        mdl::Hit{
          mdl::BrushNode::BrushHitType,
          1.0,
          face.polygon().center(),
          mdl::BrushFaceHandle{&source, *index}});
      if (!tools().faceTool().selectInsetFace(pick))
      {
        throw std::runtime_error{"Inset face selection failed"};
      }
    }
    else if (tools().faceToolActive())
    {
      map().nodeHandles().selectHandle(mdl::FaceHandle{face.polygon()});
    }
    else
    {
      mdl::selectBrushFaces(map(), {mdl::BrushFaceHandle{&source, *index}});
    }
  }

  void capture(const QJsonObject& step)
  {
    const auto name = step.value("file").toString();
    if (name.isEmpty() || QFileInfo{name}.fileName() != name || !name.endsWith(".png"))
    {
      throw std::runtime_error{"Capture file must be a plain PNG filename"};
    }
    const auto scoped =
      step.contains("scope") && step.value("scope").toString() != "window";
    auto& target = scoped ? root(step) : *m_window;
    if (&target == m_window.data() && !view().isValid())
    {
      throw std::runtime_error{"The OpenGL viewport is not ready"};
    }
    m_window->statusBar()->showMessage(step.value("caption").toString());
    target.repaint();
    const auto result =
      saveGuideScreenshot(target, pathFromQString(m_output + "/" + name), true);
    if (!result)
    {
      throw std::runtime_error{"Screenshot could not be saved"};
    }
    auto frame = QJsonObject{
      {"file", name},
      {"title", step.value("title")},
      {"chapter", step.value("chapter")},
      {"caption", step.value("caption")},
      {"source", "real-editor-capture"},
      {"pathsHidden", true},
      {"pathRedactions", target.property("guidePathRedactions").toInt()}};
    m_frames.append(frame);
    writeJson(
      m_output + "/manifest.json", {{"frames", m_frames}, {"source", "Qt/OpenGL"}});
  }

  // Returns true only when a nested dialog loop already has the next step scheduled.
  bool execute(const QJsonObject& step)
  {
    const auto op = step.value("op").toString();
    if (op == "reset")
    {
      resetScene();
    }
    else if (op == "materials")
    {
      auto paths = std::vector<std::filesystem::path>{};
      for (const auto& path : step.value("paths").toArray())
      {
        paths.push_back(pathFromQString(path.toString()));
      }
      mdl::deselectAll(map());
      auto wads = QStringList{};
      auto folders = std::vector<std::filesystem::path>{};
      for (const auto& path : paths)
      {
        if (path.extension() == ".wad")
        {
          wads.append(pathAsQString(path));
        }
        else
        {
          folders.push_back(path);
        }
      }
      mdl::setEntityProperty(map(), "wad", wads.join(';').toStdString());
      mdl::removeEntityProperty(map(), "_tb_textures");
      if (!folders.empty())
      {
        mdl::setEnabledMaterialCollections(map(), folders);
      }
    }
    else if (op == "brush")
    {
      addBrush(step);
    }
    else if (op == "entity")
    {
      addEntity(step);
    }
    else if (op == "save-map")
    {
      const auto file = step.value("file").toString("PrefabCollection.map");
      if (
        QFileInfo{file}.fileName() != file || !file.endsWith(".map")
        || !map().saveTo(pathFromQString(m_output + "/" + file)))
      {
        throw std::runtime_error{"Could not save demonstration collection"};
      }
    }
    else if (op == "stash-prefab")
    {
      auto stream = std::stringstream{};
      auto writer = mdl::NodeWriter{map().worldNode(), stream};
      writer.writeNodes({node(step.value("id").toString())}, map().taskManager());
      m_prefabPayloads[step.value("id").toString().toStdString()] =
        "prefab\n" + stream.str();
    }
    else if (op == "prefab-place")
    {
      const auto it = m_prefabPayloads.find(step.value("id").toString().toStdString());
      if (it == m_prefabPayloads.end() || !tools().prefabTool().createPrefab(it->second))
      {
        throw std::runtime_error{"Could not start prefab placement"};
      }
      const auto position = readVector(step.value("position"));
      const auto ray = vm::ray3d{position + vm::vec3d{0, 0, 1024}, {0, 0, -1}};
      auto pick = mdl::PickResult::byDistance();
      mdl::pick(map(), ray, pick);
      tools().prefabTool().updatePrefabPosition3D(ray, pick);
      tools().prefabTool().commitPrefab();
    }
    else if (op == "select")
    {
      select(step.value("ids").toArray());
    }
    else if (op == "group")
    {
      select(step.value("ids").toArray());
      auto* created =
        mdl::groupSelectedNodes(map(), step.value("name").toString().toStdString());
      if (!created)
      {
        throw std::runtime_error{"Could not group demonstration brushes"};
      }
      m_nodes[step.value("id").toString().toStdString()] = created;
    }
    else if (op == "render-prefs")
    {
      setPref(
        Preferences::FaceRenderMode, step.value("faces").toString("flat").toStdString());
      if (step.contains("brightness"))
      {
        setPref(Preferences::Brightness, float(step.value("brightness").toDouble()));
      }
    }
    else if (op == "camera")
    {
      view().camera().moveTo(vm::vec3f{readVector(step.value("position"))});
      view().camera().lookAt(
        vm::vec3f{readVector(step.value("target"))}, vm::vec3f{0, 0, 1});
      view().update();
    }
    else if (op == "action")
    {
      const auto name = step.value("path").toString();
      if (
        !(name.startsWith("Menu/Edit/") || name.startsWith("Menu/View/")
          || name == "Menu/Unity/Sync Map...")
        || name.contains("Import"))
      {
        throw std::runtime_error{"Action is not allowed in a guide capture"};
      }
      auto* action = m_window->findAction(pathFromQString(name));
      if (!action || !action->isEnabled())
      {
        throw std::runtime_error{("Unavailable action: " + name).toStdString()};
      }
      if (step.value("modal").toBool())
      {
        QTimer::singleShot(300, this, [this] { advance(); });
        action->trigger();
        return true;
      }
      action->trigger();
    }
    else if (op == "set")
    {
      setControl(root(step), step.value("field").toString(), step.value("value"));
    }
    else if (op == "button")
    {
      auto& widget = root(step);
      bool clicked = false;
      for (auto* button : widget.findChildren<QAbstractButton*>())
      {
        if (button->isVisible()
            && ((step.contains("text") && button->text() == step.value("text").toString())
              || (step.contains("toolTip") && button->toolTip() == step.value("toolTip").toString())
              || (step.contains("objectName")
                  && button->objectName() == step.value("objectName").toString())))
        {
          clicked = true;
          if (step.value("modal").toBool())
          {
            QTimer::singleShot(300, this, [this] { advance(); });
            button->click();
            return true;
          }
          button->click();
          break;
        }
      }
      if (!clicked)
      {
        throw std::runtime_error{"Capture button was not found"};
      }
    }
    else if (op == "face")
    {
      selectFace(step);
    }
    else if (op == "edge")
    {
      const auto handles =
        mdl::EdgeHandle::getPositions(map().nodeHandles().allHandles<mdl::EdgeHandle>());
      if (step.contains("center"))
      {
        const auto center = readVector(step.value("center"));
        const auto it = std::ranges::find_if(handles, [&](const auto& handle) {
          return vm::squared_length(handle.center() - center) < 0.001;
        });
        if (it == handles.end())
        {
          throw std::runtime_error{"Requested edge was not found"};
        }
        map().nodeHandles().selectHandle(mdl::EdgeHandle{*it});
        return false;
      }
      const auto index = step.value("index").toInt();
      if (index < 0 || size_t(index) >= handles.size())
      {
        throw std::runtime_error{"Unknown edge handle"};
      }
      map().nodeHandles().selectHandle(mdl::EdgeHandle{handles[size_t(index)]});
    }
    else if (op == "polygon-point")
    {
      tools().polygonCutTool().addPoint(
        readVector(step.value("position")),
        vm::plane3d{step.value("distance").toDouble(), readVector(step.value("normal"))});
    }
    else if (op == "polygon-perform")
    {
      if (step.value("spawn").toBool())
      {
        if (!tools().polygonCutTool().canSpawnBrush())
        {
          throw std::runtime_error{"Polygon outline is not ready for spawning"};
        }
        tools().performPolygonSpawn();
      }
      else
      {
        if (!tools().polygonCutTool().canCut())
        {
          throw std::runtime_error{"Polygon outline is not ready for cutting"};
        }
        tools().performPolygonCut();
      }
    }
    else if (op == "bevel-mouse")
    {
      auto& bevel = tools().edgeTool();
      if (step.contains("segments"))
      {
        bevel.adjustChamferSegments(step.value("segments").toInt());
      }
      if (step.contains("width"))
      {
        bevel.adjustChamferWidth(step.value("width").toInt());
      }
      if (step.contains("curve"))
      {
        bevel.adjustChamferCurve(step.value("curve").toDouble());
      }
      if (step.contains("proportion"))
      {
        bevel.adjustChamferProportion(step.value("proportion").toDouble());
      }
    }
    else if (op == "spin-end")
    {
      if (step.contains("position"))
      {
        tools().faceTool().moveSpinEndHandleTo(readVector(step.value("position")));
      }
      if (step.contains("rotation"))
      {
        tools().faceTool().setSpinEndRotation(readVector(step.value("rotation")));
      }
    }
    else if (op == "bend-handle")
    {
      const auto index = size_t(step.value("index").toInt());
      tools().bezierBendTool().moveHandleTo(index, readVector(step.value("position")));
    }
    else if (op == "sculpt")
    {
      auto& source = brush(step);
      const auto position = readVector(step.value("position"));
      auto pick = mdl::PickResult::byDistance();
      const auto index = source.brush().findFace(readVector(step.value("normal")));
      if (!index)
      {
        throw std::runtime_error{"Sculpt face was not found"};
      }
      pick.addHit(
        mdl::Hit{
          mdl::BrushNode::BrushHitType,
          1.0,
          position,
          mdl::BrushFaceHandle{&source, *index}});
      auto& sculpt = tools().sculptTool();
      sculpt.updateBrush(pick);
      sculpt.beginStroke();
      for (int i = 0; i < std::clamp(step.value("count").toInt(1), 1, 100); ++i)
      {
        sculpt.applyBrush();
      }
      sculpt.endStroke();
    }
    else if (op == "sculpt-ray")
    {
      auto& sculpt = tools().sculptTool();
      const auto position = readVector(step.value("position"));
      const auto count = std::clamp(step.value("count").toInt(1), 0, 100);
      const auto updateBrush = [&] {
        auto pick = mdl::PickResult::byDistance();
        mdl::pick(map(), vm::ray3d{position + vm::vec3d{0, 0, 1024}, {0, 0, -1}}, pick);
        sculpt.updateBrush(pick);
      };
      updateBrush();
      if (count > 0)
      {
        sculpt.beginStroke();
        for (auto i = 0; i < count; ++i)
        {
          updateBrush();
          sculpt.applyBrush();
        }
        sculpt.endStroke();
        updateBrush();
      }
    }
    else if (op == "translate")
    {
      mdl::translateSelection(map(), readVector(step.value("delta")));
    }
    else if (op == "property")
    {
      mdl::setEntityProperty(
        map(),
        step.value("key").toString().toStdString(),
        step.value("value").toVariant().toString().toStdString());
    }
    else if (op == "inspector")
    {
      m_window->switchToInspectorPage(
        static_cast<InspectorPage>(step.value("index").toInt()));
    }
    else if (op == "inspector-width")
    {
      auto* splitter = m_window->findChild<QSplitter*>("MapWindow_HorizontalSplitter");
      if (!splitter)
      {
        throw std::runtime_error{"Inspector splitter not found"};
      }
      const auto width = std::clamp(step.value("width").toInt(560), 300, 800);
      splitter->setSizes({m_window->width() - width, width});
    }
    else if (op == "splitter")
    {
      auto* splitter =
        m_window->findChild<QSplitter*>(step.value("objectName").toString());
      if (!splitter)
      {
        throw std::runtime_error{"Splitter not found"};
      }
      auto sizes = QList<int>{};
      for (const auto& value : step.value("sizes").toArray())
      {
        sizes.append(value.toInt());
      }
      splitter->setSizes(sizes);
    }
    else if (op == "material-panel")
    {
      bool found = false;
      for (auto* panel : m_window->findChildren<SwitchableTitledPanel*>())
      {
        if (panel->isVisible())
        {
          panel->setCurrentIndex(size_t(step.value("index").toInt()));
          found = true;
          break;
        }
      }
      if (!found)
      {
        throw std::runtime_error{"Material panel not found"};
      }
    }
    else if (op == "material-filter")
    {
      auto& browser =
        dynamic_cast<MaterialBrowser&>(root(QJsonObject{{"scope", "material-browser"}}));
      browser.setGroup(step.value("group").toBool(true));
      browser.setHideUnused(false);
      browser.setFilterText(step.value("text").toString().toStdString());
    }
    else if (op == "entity-definition")
    {
      auto& browser =
        dynamic_cast<EntityBrowser&>(root(QJsonObject{{"scope", "entity-browser"}}));
      browser.setSelectedEntityDefinition(
        step.value("classname").toString().toStdString());
      browser.entityDefinitionSelected(step.value("classname").toString());
    }
    else if (op == "list-item")
    {
      auto* list =
        root(step).findChild<QListWidget*>(step.value("objectName").toString());
      if (!list)
      {
        throw std::runtime_error{"List not found"};
      }
      const auto matches =
        list->findItems(step.value("text").toString(), Qt::MatchContains);
      if (matches.empty())
      {
        throw std::runtime_error{"List entry not found"};
      }
      list->setCurrentItem(matches.first());
      if (step.value("doubleClick").toBool())
      {
        list->itemDoubleClicked(matches.first());
      }
    }
    else if (op == "widget-click" || op == "widget-wheel")
    {
      auto& parent = root(step);
      QWidget* target = nullptr;
      for (auto* candidate : parent.findChildren<QWidget*>())
      {
        if (candidate->isVisible()
          && ((step.contains("objectName") && candidate->objectName() == step.value("objectName").toString())
            || (step.contains("class") && QString{candidate->metaObject()->className()}.endsWith(step.value("class").toString()))))
        {
          target = candidate;
          break;
        }
      }
      if (!target)
      {
        throw std::runtime_error{"Click target not found"};
      }
      const auto fractions = step.value("point").toArray();
      const auto local = fractions.size() == 2
        ? QPointF{fractions[0].toDouble() * target->width(), fractions[1].toDouble() * target->height()}
        : QPointF{target->rect().center()};
      const auto global = QPointF{target->mapToGlobal(local.toPoint())};
      if (op == "widget-wheel")
      {
        auto wheel = QWheelEvent{
          local,
          global,
          QPoint{},
          QPoint{0, step.value("steps").toInt(1) * 120},
          Qt::NoButton,
          Qt::ControlModifier,
          Qt::NoScrollPhase,
          false};
        QApplication::sendEvent(target, &wheel);
        return false;
      }
      auto press = QMouseEvent{
        QEvent::MouseButtonPress,
        local,
        global,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier};
      auto release = QMouseEvent{
        QEvent::MouseButtonRelease,
        local,
        global,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier};
      QApplication::sendEvent(target, &press);
      QApplication::sendEvent(target, &release);
    }
    else if (op == "line-edit")
    {
      bool found = false;
      for (auto* input : root(step).findChildren<QLineEdit*>())
      {
        if (input->isVisible()
            && (step.value("first").toBool() || (step.contains("placeholder")
                 && input->placeholderText() == step.value("placeholder").toString())
              || (step.contains("objectName")
                  && input->objectName() == step.value("objectName").toString())))
        {
          input->setText(step.value("text").toString());
          input->textEdited(input->text());
          found = true;
          break;
        }
      }
      if (!found)
      {
        throw std::runtime_error{"Text field was not found"};
      }
    }
    else if (op == "file-dialog")
    {
      auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
      if (!dialog)
      {
        throw std::runtime_error{"No active file chooser"};
      }
      const auto path = step.value("path").toString();
      dialog->setDirectory(QFileInfo{path}.absolutePath());
      QTimer::singleShot(150, dialog, [dialog, path] {
        dialog->selectFile(QFileInfo{path}.fileName());
        if (auto* input = dialog->findChild<QLineEdit*>("fileNameEdit"))
        {
          input->setText(QFileInfo{path}.fileName());
        }
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
      });
    }
    else if (op == "close-dialog")
    {
      auto& widget = root(QJsonObject{{"scope", "dialog"}});
      qobject_cast<QDialog*>(&widget)->reject();
    }
    else if (op == "capture")
    {
      capture(step);
    }
    else if (op == "wait")
    {
      // The delay is handled by advance().
    }
    else
    {
      throw std::runtime_error{("Unknown guide operation: " + op).toStdString()};
    }
    return false;
  }

  void advance()
  {
    if (!m_window)
    {
      fail("The capture window was closed");
      return;
    }
    if (m_index >= m_steps.size())
    {
      finish();
      return;
    }
    const auto step = m_steps[m_index++].toObject();
    try
    {
      if (!execute(step))
      {
        QTimer::singleShot(
          std::clamp(step.value("delay").toInt(220), 0, 10000), this, [this] {
            advance();
          });
      }
    }
    catch (const std::exception& e)
    {
      fail(QString("Step %1: %2").arg(m_index).arg(QString::fromUtf8(e.what())));
    }
  }

  void shutdown(const int code)
  {
    if (m_window)
    {
      if (auto* focused = QApplication::focusWidget())
      {
        focused->clearFocus();
      }
      for (auto* widget : QApplication::topLevelWidgets())
      {
        if (auto* dialog = qobject_cast<QDialog*>(widget))
        {
          dialog->reject();
        }
        else if (widget->windowType() == Qt::Popup)
        {
          widget->hide();
        }
      }
      container().deactivateCurrentTool();
      if (!map().saveTo(pathFromQString(m_output + "/capture-demo.map")))
      {
        qWarning("Could not save the demonstration map snapshot");
      }
      // This transient demo was never loaded from disk. Do not ask to save it on exit.
      map().decModificationCount(map().modificationCount());
      m_window->close();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QCoreApplication::exit(code);
  }

  void fail(const QString& message)
  {
    try
    {
      writeJson(
        m_output + "/capture-result.json",
        {{"success", false}, {"error", message}, {"completed", m_frames.size()}});
    }
    catch (...)
    {
    }
    qWarning("%s", qPrintable(message));
    shutdown(1);
  }

  void finish()
  {
    writeJson(
      m_output + "/capture-result.json",
      {{"success", true}, {"completed", m_frames.size()}});
    auto html = QString{
      "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>ReyondBroom - Real Screenshot Guide</title>"
      "<style>body{margin:0;background:#eee;color:#202427;font:18px Arial}"
      "main{max-width:1600px;margin:auto;padding:24px}"
      "h1{font-size:32px}h2{font-size:22px;margin:32px 0 10px}"
      "img{width:100%;height:auto;display:block;margin:0 0 28px}"
      "nav{display:flex;flex-wrap:wrap;gap:14px}a{color:#078834}</style>"
      "<main><h1>ReyondBroom</h1><nav>"};
    auto chapters = std::set<QString>{};
    for (const auto& value : m_frames)
    {
      const auto frame = value.toObject();
      const auto chapter = frame.value("chapter").toString();
      if (chapters.insert(chapter).second)
      {
        html += "<a href=\"#" + chapter.toHtmlEscaped() + "\">"
                + frame.value("title").toString().toHtmlEscaped() + "</a>";
      }
    }
    html += "</nav>";
    chapters.clear();
    for (const auto& value : m_frames)
    {
      const auto frame = value.toObject();
      const auto chapter = frame.value("chapter").toString();
      const auto anchor = chapters.insert(chapter).second
                            ? " id=\"" + chapter.toHtmlEscaped() + "\""
                            : QString{};
      html += "<h2" + anchor + ">" + frame.value("caption").toString().toHtmlEscaped()
              + "</h2><a href=\"" + frame.value("file").toString().toHtmlEscaped()
              + "\"><img loading=\"lazy\" src=\""
              + frame.value("file").toString().toHtmlEscaped() + "\" alt=\""
              + frame.value("caption").toString().toHtmlEscaped() + "\"></a>";
    }
    html += "</main></html>";
    auto file = QSaveFile{m_output + "/index.html"};
    if (
      !file.open(QIODevice::WriteOnly) || file.write(html.toUtf8()) < 0 || !file.commit())
    {
      fail("Could not save the guide page");
      return;
    }
    shutdown(0);
  }
};
} // namespace

Result<void> saveGuideScreenshot(
  QWidget& widget, const std::filesystem::path& path, const bool redactPaths)
{
  if (!widget.isVisible())
  {
    return Error{"Screenshot target is not visible"};
  }
  for (auto* view : widget.findChildren<QOpenGLWidget*>())
  {
    if (view->isVisible() && !view->isValid())
    {
      return Error{"An OpenGL view is not ready"};
    }
  }
  auto image = widget.grab();
  {
    // Qt widget grabs can omit parts of a composited OpenGL surface on Windows.
    // Copy each actual framebuffer, then render its real child HUD widgets over it.
    auto painter = QPainter{&image};
    for (auto* view : widget.findChildren<QOpenGLWidget*>())
    {
      if (!view->isVisible())
      {
        continue;
      }
      const auto offset = view->mapTo(&widget, QPoint{});
      const auto framebuffer = view->grabFramebuffer();
      if (framebuffer.isNull())
      {
        return Error{"Could not read an OpenGL framebuffer"};
      }
      painter.drawImage(QRect{offset, view->size()}, framebuffer);
      for (auto* overlay :
           view->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly))
      {
        if (overlay->isVisible())
        {
          overlay->render(&painter, offset + overlay->pos());
        }
      }
    }
    for (auto* popup : QApplication::topLevelWidgets())
    {
      if (
        popup->isVisible() && popup->windowType() == Qt::Popup && popup->parentWidget()
        && (popup->parentWidget() == &widget || widget.isAncestorOf(popup->parentWidget())))
      {
        const auto offset = popup->mapToGlobal(QPoint{}) - widget.mapToGlobal(QPoint{});
        painter.drawPixmap(offset, popup->grab());
      }
    }
    widget.setProperty(
      "guidePathRedactions", redactPaths ? redactScreenshotPaths(painter, widget) : 0);
  }
  if (image.isNull() || !image.save(pathAsQString(path), "PNG"))
  {
    return Error{"Could not save screenshot"};
  }
  return {};
}

void showGuideScreenshotDialog(MapWindow& window)
{
  const auto path = QFileDialog::getSaveFileName(
    &window,
    QObject::tr("Capture Guide Screenshot"),
    "editor-screenshot.png",
    QObject::tr("PNG Images (*.png)"));
  if (path.isEmpty())
  {
    return;
  }
  QTimer::singleShot(200, &window, [&window, path] {
    if (!saveGuideScreenshot(window, pathFromQString(path)))
    {
      QMessageBox::warning(
        &window,
        QObject::tr("Capture Guide Screenshot"),
        QObject::tr(
          "Could not capture the editor. Wait for the viewport to render and try "
          "again."));
    }
  });
}

bool startGuideCapture(AppController& controller, const QString& scriptPath)
{
  auto file = QFile{scriptPath};
  if (!file.open(QIODevice::ReadOnly))
  {
    qWarning("Could not open guide capture script");
    QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(1); });
    return true;
  }
  auto error = QJsonParseError{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
  {
    qWarning("Invalid guide capture JSON");
    QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(1); });
    return true;
  }
  const auto script = document.object();
  const auto output = QDir{QFileInfo{scriptPath}.absolutePath()}.absoluteFilePath(
    script.value("output").toString("screenshots"));
  if (!QDir{}.mkpath(output) || script.value("steps").toArray().isEmpty())
  {
    qWarning("Capture output directory or steps are invalid");
    QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(1); });
    return true;
  }
  auto* session = new GuideCaptureSession{controller, script, output};
  QTimer::singleShot(0, session, [session] { session->start(); });
  return true;
}
} // namespace tb::ui
