#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QStackedLayout>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Grid.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PickResult.h"
#include "mdl/NodeHandleManager.h"
#include "mdl/NodeHandles.h"
#include "mdl/WorldNode.h"
#include "ui/ArrayTool.h"
#include "ui/CatchConfig.h"
#include "ui/EdgeTool.h"
#include "ui/FaceTool.h"
#include "ui/GeneratorTool.h"
#include "ui/GuideCapture.h"
#include "ui/IssueBrowserView.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapViewBar.h"
#include "ui/MapViewToolBox.h"
#include "ui/SculptTool.h"
#include "ui/SubdivisionTool.h"

#include "vm/mat_ext.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{
void capturePanel(QWidget* page, const char* name)
{
  const auto directory = qEnvironmentVariable("TB_TOOL_SCREENSHOT_DIR");
  if (directory.isEmpty())
  {
    return;
  }
  page->resize(1280, 80);
  page->ensurePolished();
  page->layout()->activate();
  REQUIRE(page->grab().save(directory + "/" + name + ".png"));
  for (auto* child : page->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly))
  {
    if (child->isHidden())
    {
      continue;
    }
    CHECK(page->rect().contains(child->geometry()));
  }
}

double brushVolume(const mdl::Brush& brush)
{
  const auto center = brush.bounds().center();
  auto volume = 0.0;
  for (const auto& face : brush.faces())
  {
    const auto points = face.vertexPositions();
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
      volume += std::abs(
                  vm::dot(
                    points.front() - center,
                    vm::cross(points[i] - center, points[i + 1] - center)))
                / 6.0;
    }
  }
  return volume;
}

mdl::BrushNode* addBox(mdl::Map& map, const vm::bbox3d& bounds)
{
  const auto builder = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto* node = new mdl::BrushNode{builder.createCuboid(bounds, "test").value()};
  mdl::addNodes(map, {{map.worldNode().defaultLayer(), {node}}});
  return node;
}

mdl::PickResult topHit(mdl::BrushNode& node)
{
  const auto index = node.brush().findFace(vm::vec3d{0, 0, 1}).value();
  auto result = mdl::PickResult::byDistance();
  result.addHit(
    mdl::Hit{
      mdl::BrushNode::BrushHitType,
      1.0,
      node.brush().face(index).polygon().center(),
      mdl::BrushFaceHandle{&node, index}});
  return result;
}

void clickButton(QWidget* page, const QString& text)
{
  const auto buttons = page->findChildren<QPushButton*>();
  const auto it =
    std::ranges::find_if(buttons, [&](auto* button) { return button->text() == text; });
  REQUIRE(it != buttons.end());
  (*it)->click();
}
} // namespace

TEST_CASE("Guide screenshot captures visible Qt pixels")
{
  auto directory = QTemporaryDir{};
  REQUIRE(directory.isValid());
  auto widget = QWidget{};
  const auto path = std::filesystem::path{directory.path().toStdString()} / "capture.png";
  CHECK_FALSE(saveGuideScreenshot(widget, path));
  auto* layout = new QHBoxLayout{&widget};
  layout->addWidget(new QPushButton{"Real editor control"});
  widget.resize(360, 80);
  widget.show();
  QTest::qWait(1);
  REQUIRE(saveGuideScreenshot(widget, path));
  CHECK(std::filesystem::file_size(path) > 100u);
  CHECK_FALSE(saveGuideScreenshot(widget, path / "invalid.png"));
}

TEST_CASE("Guide screenshot hides private paths without changing editor values")
{
  auto directory = QTemporaryDir{};
  REQUIRE(directory.isValid());
  auto widget = QWidget{};
  auto* layout = new QVBoxLayout{&widget};
  auto* label = new QLabel{"C:/Users/PrivateUser/palette.lmp"};
  auto* input = new QLineEdit{"D:/PrivateProject/Assets"};
  auto* list = new QListWidget{};
  list->addItem("F:/PrivateProject/Textures");
  layout->addWidget(label);
  layout->addWidget(input);
  layout->addWidget(list);
  widget.resize(480, 220);
  widget.show();
  QTest::qWait(1);
  const auto path = std::filesystem::path{directory.path().toStdString()} / "private.png";
  REQUIRE(saveGuideScreenshot(widget, path, true));
  CHECK(widget.property("guidePathRedactions").toInt() == 3);
  CHECK(label->text() == "C:/Users/PrivateUser/palette.lmp");
  CHECK(input->text() == "D:/PrivateProject/Assets");
  CHECK(list->item(0)->text() == "F:/PrivateProject/Textures");
  const auto image = QImage{QString::fromStdString(path.string())};
  CHECK(image.pixelColor(label->mapTo(&widget, QPoint{1, 1})) == QColor(230, 232, 232));
}

TEST_CASE("Issue browser drops invalid issue pointers before deferred validation")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* source = addBox(map, vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
  auto view = IssueBrowserView{document};
  auto* model = view.findChild<IssueBrowserModel*>();
  REQUIRE(model);
  auto issue = std::make_unique<mdl::Issue>(mdl::IssueType{1}, *source, "Old issue");
  model->setIssues({issue.get()});
  REQUIRE(model->rowCount(QModelIndex{}) == 1);
  const auto oldIndex = model->index(0, 1);
  mdl::selectNodes(map, {source});
  view.reload();
  CHECK(model->rowCount(QModelIndex{}) == 0);
  CHECK(model->issues().empty());
  CHECK(map.selection().nodes == std::vector<mdl::Node*>{source});
  issue.reset();
  CHECK(model->data(oldIndex, Qt::DisplayRole).isNull());
  CHECK_FALSE(view.isEnabled());
  CHECK_FALSE(view.updatesEnabled());
  view.validate();
  CHECK(view.isEnabled());
  CHECK(view.updatesEnabled());
}

TEST_CASE("Map view bar follows the active tool page height")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto window = QWidget{};
  auto* layout = new QVBoxLayout{&window};
  auto* bar = new MapViewBar{document};
  layout->addWidget(bar);
  layout->addWidget(new QWidget{}, 1);
  auto* book = bar->toolBook();

  auto* singleRow = new QWidget{};
  auto* singleLayout = new QHBoxLayout{singleRow};
  singleLayout->setContentsMargins(0, 0, 0, 0);
  singleLayout->addWidget(new QPushButton{"One row"});
  book->addWidget(singleRow);

  auto* twoRows = new QWidget{};
  auto* doubleLayout = new QGridLayout{twoRows};
  doubleLayout->setContentsMargins(0, 0, 0, 0);
  doubleLayout->addWidget(new QPushButton{"First row"}, 0, 0);
  auto* secondRow = new QPushButton{"Second row"};
  doubleLayout->addWidget(secondRow, 1, 0);
  book->addWidget(twoRows);

  auto* hiddenTallPage = new QWidget{};
  hiddenTallPage->setMinimumHeight(300);
  book->addWidget(hiddenTallPage);

  window.resize(1280, 480);
  window.show();
  QTest::qWait(1);
  const auto singleHeight = bar->height();
  const auto singleHint = bar->sizeHint().height();
  const auto singleMinimum = bar->minimumSizeHint().height();
  CHECK(singleHeight < hiddenTallPage->minimumHeight());

  book->setCurrentWidget(twoRows);
  QTest::qWait(1);
  CHECK(bar->height() > singleHeight);
  CHECK(bar->sizeHint().height() > singleHint);
  CHECK(bar->minimumSizeHint().height() > singleMinimum);

  secondRow->hide();
  QTest::qWait(1);
  CHECK(bar->height() == singleHeight);
  secondRow->show();
  QTest::qWait(1);
  CHECK(bar->height() > singleHeight);

  book->setCurrentWidget(singleRow);
  QTest::qWait(1);
  CHECK(bar->height() == singleHeight);
  CHECK(bar->sizeHint().height() == singleHint);
  CHECK(bar->minimumSizeHint().height() == singleMinimum);
}

TEST_CASE("Geometry tool panels - numeric input and face selection")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* source = addBox(map, vm::bbox3d{{-128, -128, 0}, {128, 128, 128}});
  mdl::selectNodes(map, {source});
  auto pageContainer = QWidget{};
  auto* book = new QStackedLayout{&pageContainer};
  auto toolbox = MapViewToolBox{document, book};

  SECTION("Bevel width accepts a complete typed number")
  {
    toolbox.toggleBevelTool();
    auto& tool = toolbox.edgeTool();
    map.nodeHandles().selectHandle(
      *map.nodeHandles().allHandles<mdl::EdgeHandle>().begin());
    tool.setBevelSegments(1);
    auto* page = book->currentWidget();
    const auto inputs = page->findChildren<QDoubleSpinBox*>();
    const auto widthIt = std::ranges::find_if(
      inputs, [](auto* input) { return input->toolTip() == "Bevel width"; });
    REQUIRE(widthIt != inputs.end());
    auto* width = *widthIt;
    CHECK_FALSE(width->keyboardTracking());
    width->setFocus();
    width->selectAll();
    QTest::keyClicks(width, "32");
    QTest::keyClick(width, Qt::Key_Return);
    CHECK(width->value() == Catch::Approx(32.0));
    CHECK(tool.bevelWidth() == Catch::Approx(32.0));
    capturePanel(page, "bevel");
    toolbox.deactivateCurrentTool();
    CHECK_FALSE(tool.bevelToolActive());
    CHECK(map.worldNode().defaultLayer()->childCount() == 1u);
  }

  SECTION("Inset can pick the face surface and update its amount")
  {
    toolbox.toggleInsetTool();
    auto& tool = toolbox.faceTool();
    REQUIRE(tool.selectInsetFace(topHit(*source)));
    REQUIRE(tool.insetFaceSelected());
    tool.setInsetSnapToGrid(false);
    tool.setInsetAmount(32.0);
    CHECK(tool.insetAmount() == Catch::Approx(32.0));
    CHECK(map.worldNode().defaultLayer()->childCount() > 1u);
    capturePanel(book->currentWidget(), "inset");
    toolbox.deactivateCurrentTool();
    CHECK_FALSE(tool.insetToolActive());
    CHECK(map.worldNode().defaultLayer()->childCount() == 1u);
  }
}

TEST_CASE("Geometry tool panels - generators apply and cancel")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* source = addBox(map, vm::bbox3d{{-128, -128, 0}, {128, 128, 128}});
  const auto originalBrush = source->brush();
  mdl::selectNodes(map, {source});
  auto container = QWidget{};
  auto* book = new QStackedLayout{&container};
  auto toolbox = MapViewToolBox{document, book};

  SECTION("Terrain preview cancels to the original brush")
  {
    toolbox.toggleGeneratorTool(GeneratorTool::Kind::Terrain);
    REQUIRE(toolbox.generatorToolActive(GeneratorTool::Kind::Terrain));
    capturePanel(book->currentWidget(), "terrain");
    CHECK(map.worldNode().defaultLayer()->childCount() > 1u);
    clickButton(book->currentWidget(), "Cancel");
    CHECK_FALSE(toolbox.anyGeneratorToolActive());
    REQUIRE(map.worldNode().defaultLayer()->childCount() == 1u);
    CHECK(source->brush() == originalBrush);
  }

  SECTION("Rock applies as one undoable action")
  {
    toolbox.toggleGeneratorTool(GeneratorTool::Kind::Rock);
    REQUIRE(toolbox.anyGeneratorToolActive());
    capturePanel(book->currentWidget(), "rock");
    clickButton(book->currentWidget(), "Apply");
    CHECK_FALSE(toolbox.anyGeneratorToolActive());
    map.undoCommand();
    REQUIRE(map.worldNode().defaultLayer()->childCount() == 1u);
    CHECK(source->brush() == originalBrush);
  }

  SECTION("Grid preview restores preferences on cancel, saves them on apply")
  {
    const auto oldAngle = pref(Preferences::GridRotation);
    const auto oldUnified = pref(Preferences::GridUseUnified3D);
    toolbox.toggleGeneratorTool(GeneratorTool::Kind::GridRotation);
    auto* angle = book->currentWidget()->findChild<QDoubleSpinBox*>();
    REQUIRE(angle != nullptr);
    angle->setValue(30.0);
    capturePanel(book->currentWidget(), "grid-rotation");
    CHECK(pref(Preferences::GridRotation) == Catch::Approx(30.0f));
    toolbox.deactivateCurrentTool();
    CHECK(pref(Preferences::GridRotation) == oldAngle);
    CHECK(pref(Preferences::GridUseUnified3D) == oldUnified);
    toolbox.toggleGeneratorTool(GeneratorTool::Kind::GridRotation);
    angle->setValue(45.0);
    clickButton(book->currentWidget(), "Apply");
    CHECK_FALSE(toolbox.anyGeneratorToolActive());
    CHECK(pref(Preferences::GridRotation) == Catch::Approx(45.0f));
    setPref(Preferences::GridRotation, oldAngle);
    setPref(Preferences::GridUseUnified3D, oldUnified);
  }
}

TEST_CASE("Geometry tool panels - subdivision alignment")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* source = addBox(map, vm::bbox3d{{10, 10, 0}, {74, 74, 16}});
  const auto originalBrush = source->brush();
  auto container = QWidget{};
  auto* book = new QStackedLayout{&container};
  auto tool = SubdivisionTool{document};
  tool.createPage(book);
  tool.showPage();

  SECTION("World grid uses global cell boundaries, not brush bounds")
  {
    mdl::selectNodes(map, {source});
    REQUIRE(tool.activate());
    tool.setMode(SubdivisionTool::Mode::GridSize);
    tool.setCellSize(32.0);
    capturePanel(book->currentWidget(), "subdivision");
    const auto& nodes = map.worldNode().defaultLayer()->children();
    REQUIRE(nodes.size() == 9u);
    auto totalVolume = 0.0;
    for (auto* node : nodes)
    {
      const auto& bounds = static_cast<mdl::BrushNode*>(node)->brush().bounds();
      totalVolume += bounds.volume();
      CHECK(
        (bounds.min.x() == Catch::Approx(10.0) || bounds.min.x() == Catch::Approx(32.0)
         || bounds.min.x() == Catch::Approx(64.0)));
    }
    CHECK(totalVolume == Catch::Approx(originalBrush.bounds().volume()));
    REQUIRE(tool.undoStep());
    CHECK(tool.mode() == SubdivisionTool::Mode::Segments);
    REQUIRE(tool.deactivate());
    CHECK(source->brush() == originalBrush);
  }

  SECTION("World grid cuts follow the rotated editor grid")
  {
    map.grid().setRotation(vm::Cd::pi() / 4.0);
    mdl::selectNodes(map, {source});
    REQUIRE(tool.activate());
    tool.setMode(SubdivisionTool::Mode::GridSize);
    tool.setCellSize(32.0);
    const auto gridU = map.grid().fromGridSpace(vm::vec3d{1, 0, 0});
    auto alignedCutFound = false;
    auto totalVolume = 0.0;
    for (auto* node : map.worldNode().defaultLayer()->children())
    {
      totalVolume += brushVolume(static_cast<mdl::BrushNode*>(node)->brush());
      for (const auto& face : static_cast<mdl::BrushNode*>(node)->brush().faces())
      {
        if (std::abs(vm::dot(face.normal(), gridU)) > 0.999)
        {
          const auto line = face.boundary().distance / 32.0;
          CHECK(line == Catch::Approx(std::round(line)).margin(0.0001));
          alignedCutFound = true;
        }
      }
    }
    CHECK(alignedCutFound);
    CHECK(totalVolume == Catch::Approx(brushVolume(originalBrush)).margin(0.01));
    REQUIRE(tool.deactivate());
  }

  SECTION("Texture alignment respects offset and scale")
  {
    auto brush = source->brush();
    const auto topIndex = brush.findFace(vm::vec3d{0, 0, 1}).value();
    auto& top = brush.face(topIndex);
    auto attributes = top.attributes();
    attributes.setOffset(vm::vec2f{16, 16});
    attributes.setScale(vm::vec2f{2, 2});
    top.setAttributes(attributes);
    const auto textureMatrix =
      top.toUVCoordSystemMatrix(attributes.offset(), attributes.scale());
    source->setBrush(std::move(brush));
    mdl::selectBrushFaces(map, {mdl::BrushFaceHandle{source, topIndex}});
    REQUIRE(tool.activate());
    tool.setMode(SubdivisionTool::Mode::GridSize);
    tool.setAlignment(SubdivisionTool::Alignment::Texture);
    tool.setCellSize(32.0);
    auto interiorCutFound = false;
    auto totalVolume = 0.0;
    for (auto* node : map.worldNode().defaultLayer()->children())
    {
      totalVolume += brushVolume(static_cast<mdl::BrushNode*>(node)->brush());
      for (const auto& p : static_cast<mdl::BrushNode*>(node)->brush().vertexPositions())
      {
        const auto uv = textureMatrix * p;
        if (p.x() > 10.001 && p.x() < 73.999)
        {
          CHECK(uv.x() / 32.0 == Catch::Approx(std::round(uv.x() / 32.0)).margin(0.0001));
          interiorCutFound = true;
        }
      }
    }
    CHECK(interiorCutFound);
    CHECK(totalVolume == Catch::Approx(brushVolume(originalBrush)).margin(0.01));
    REQUIRE(tool.deactivate());
  }
}

TEST_CASE("Geometry tool panels - array faces center")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* source = addBox(map, vm::bbox3d{{-16, -8, 0}, {16, 8, 16}});
  auto brush = source->brush();
  auto& side = brush.face(brush.findFace(vm::vec3d{1, 0, 0}).value());
  auto attributes = side.attributes();
  attributes.setMaterialName("facing");
  side.setAttributes(attributes);
  source->setBrush(std::move(brush));
  mdl::selectNodes(map, {source});
  auto container = QWidget{};
  auto* book = new QStackedLayout{&container};
  auto tool = ArrayTool{document};
  tool.createPage(book);
  tool.showPage();
  tool.setMode(ArrayTool::Mode::Radial);
  tool.setCount(4);
  tool.setRingSpacing(128.0);
  tool.setFaceCenter(true);
  REQUIRE(tool.activate());
  capturePanel(book->currentWidget(), "array");
  const auto center = source->physicalBounds().center();
  const auto& nodes = map.worldNode().defaultLayer()->children();
  REQUIRE(nodes.size() == 5u);
  for (auto* node : nodes)
  {
    if (node == source)
    {
      continue;
    }
    const auto& copy = static_cast<mdl::BrushNode*>(node)->brush();
    const auto normal = copy.face(copy.findFace("facing").value()).normal();
    CHECK(
      vm::dot(normal, vm::normalize(center - copy.bounds().center()))
      == Catch::Approx(1.0));
  }
  REQUIRE(tool.deactivate());
  CHECK(map.worldNode().defaultLayer()->childCount() == 1u);
}

TEST_CASE("Geometry tool panels - sculpt prevents collapse")
{
  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  auto* source = addBox(map, vm::bbox3d{{-16, -16, 0}, {16, 16, 16}});
  const auto original = source->brush();
  auto tool = SculptTool{document};
  REQUIRE(tool.activate());
  tool.setMode(SculptTool::Mode::Lower);
  tool.setRadius(128.0);
  tool.setIntensity(1024.0);
  REQUIRE(tool.preventCollapse());
  tool.updateBrush(topHit(*source));
  tool.beginStroke();
  CHECK(tool.applyBrush());
  CHECK(source->brush().bounds().min.z() == Catch::Approx(0.0));
  CHECK(source->brush().bounds().max.z() == Catch::Approx(1.0));
  CHECK(source->brush().vertexCount() == original.vertexCount());
  tool.cancelStroke();
  CHECK(source->brush() == original);
  REQUIRE(tool.deactivate());

  SECTION("Connected brushes use the smallest shared thickness limit")
  {
    auto* neighbour = addBox(map, vm::bbox3d{{16, -16, 12}, {48, 16, 16}});
    auto* above = addBox(map, vm::bbox3d{{-16, -16, 64}, {16, 16, 80}});
    const auto aboveOriginal = above->brush();
    REQUIRE(tool.activate());
    tool.updateBrush(topHit(*source));
    tool.beginStroke();
    REQUIRE(tool.applyBrush());
    const auto neighbourPoints = neighbour->brush().vertexPositions();
    for (const auto& p : source->brush().vertexPositions())
    {
      if (std::abs(p.x() - 16.0) < 0.001 && p.z() > 0.1)
      {
        CHECK(p.z() == Catch::Approx(13.0));
        CHECK(std::ranges::any_of(neighbourPoints, [&](const auto& q) {
          return vm::squared_length(p - q) < 0.000001;
        }));
      }
    }
    CHECK(above->brush() == aboveOriginal);
    tool.cancelStroke();
    REQUIRE(tool.deactivate());
  }
}
} // namespace tb::ui
