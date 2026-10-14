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

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>

#include "Result.h"
#include "TestEnvironment.h"
#include "gl/GlManager.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/MapWindow.h"

#include <functional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

// Simple resource type for testing resource processing
struct TestResource
{
  void upload(gl::Gl&) const {}
  void drop(gl::Gl&) const {}
};

TEST_CASE("MapWindow")
{
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  auto document = MapDocument::createDocument(
                    appController.environmentConfig(),
                    mdl::QuakeGameInfo,
                    mdl::MapFormat::Valve,
                    vm::bbox3d{8192.0},
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();

  auto window = MapWindow{appController, std::move(document)};

  SECTION("Esc disables parameter tools, including from a numeric field")
  {
    auto& map = window.document().map();
    auto* source = mdl::createBrushNode(map);
    mdl::addNodes(map, {{mdl::parentForNodes(map), {source}}});
    auto* group = new mdl::GroupNode{mdl::Group{"bend-test"}};
    group->addChild(mdl::createBrushNode(map));
    mdl::addNodes(map, {{mdl::parentForNodes(map), {group}}});
    struct ToolCase
    {
      const char* name;
      std::function<void()> activate;
      std::function<bool()> active;
    };
    const auto cases = std::vector<ToolCase>{
      {"Bezier",
       [&] { window.toggleBezierBendTool(); },
       [&] { return window.bezierBendToolActive(); }},
      {"Sculpt",
       [&] { window.toggleSculptTool(); },
       [&] { return window.sculptToolActive(); }},
      {"Array",
       [&] { window.toggleArrayTool(); },
       [&] { return window.arrayToolActive(); }},
      {"Subdivision",
       [&] { window.toggleSubdivisionTool(); },
       [&] { return window.subdivisionToolActive(); }},
      {"Bevel",
       [&] { window.toggleBevelTool(); },
       [&] { return window.bevelToolActive(); }},
      {"Inset",
       [&] { window.toggleInsetTool(); },
       [&] { return window.insetToolActive(); }},
      {"Spin", [&] { window.toggleSpinTool(); }, [&] { return window.spinToolActive(); }},
      {"Polygon Cut",
       [&] { window.togglePolygonCutTool(); },
       [&] { return window.polygonCutToolActive(); }},
      {"Polygon Spawn",
       [&] { window.togglePolygonSpawnTool(); },
       [&] { return window.polygonSpawnToolActive(); }},
      {"Terrain",
       [&] { window.showTerrainGeneratorDialog(); },
       [&] { return window.terrainGeneratorActive(); }},
      {"Rock",
       [&] { window.showRockGeneratorDialog(); },
       [&] { return window.rockGeneratorActive(); }},
      {"Grid Rotation",
       [&] { window.showGridRotationDialog(); },
       [&] { return window.gridRotationActive(); }},
    };
    for (const auto& test : cases)
    {
      DYNAMIC_SECTION(test.name)
      {
        mdl::selectNodes(
          map,
          {test.name == std::string{"Bezier"} ? static_cast<mdl::Node*>(group) : source});
        test.activate();
        REQUIRE(test.active());
        auto* input = window.findChild<QDoubleSpinBox*>();
        REQUIRE(input != nullptr);
        auto escape = QKeyEvent{QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
        QApplication::sendEvent(input, &escape);
        CHECK_FALSE(test.active());
        QApplication::sendEvent(input, &escape);
        QApplication::sendEvent(input, &escape);
        CHECK_FALSE(test.active());
      }
    }
  }

  SECTION("load resets grid size dropdown")
  {
    auto* gridChoice = window.findChild<QComboBox*>("MapWindow_GridChoice");
    REQUIRE(gridChoice != nullptr);

    const auto changedGridSize = 5;
    const auto changedGridIndex = changedGridSize - mdl::Grid::MinSize;
    const auto path = getFixtureRoot() / "test/ui/MapDocument/emptyValveMap.map";

    window.setGridSize(changedGridSize);
    QApplication::processEvents();

    REQUIRE(window.document().map().grid().size() == changedGridSize);
    REQUIRE(gridChoice->currentIndex() == changedGridIndex);

    REQUIRE(window.document().load(
      appController.environmentConfig(),
      mdl::QuakeGameInfo,
      mdl::MapFormat::Unknown,
      vm::bbox3d{8192.0},
      path));

    QApplication::processEvents();

    const auto loadedGridSize = window.document().map().grid().size();
    const auto loadedGridIndex = loadedGridSize - mdl::Grid::MinSize;

    CHECK(loadedGridSize != changedGridSize);
    CHECK(gridChoice->currentIndex() == loadedGridIndex);
    CHECK(gridChoice->currentData().toInt() == loadedGridSize);
  }

  SECTION("canReloadMaterialCollections returns false when resources need processing")
  {
    using TestResourceT = gl::Resource<TestResource>;

    // Verify initial state: no resources pending processing
    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadMaterialCollections());

    // Add a resource that needs processing
    auto testResource = std::make_shared<TestResourceT>(
      []() { return Result<TestResource>{TestResource{}}; });
    appController.glManager().resourceManager().addResource(testResource);

    REQUIRE(appController.glManager().resourceManager().needsProcessing());
    CHECK(!window.canReloadMaterialCollections());

    // Process all resources synchronously
    auto testGl = gl::TestGl{};
    const auto processContext = gl::ProcessContext{testGl, [](auto, auto) {}};
    gl::processResourcesSync(appController.glManager().resourceManager(), processContext);

    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadMaterialCollections());
  }

  SECTION("canReloadEntityDefinitions returns false when resources need processing")
  {
    using TestResourceT = gl::Resource<TestResource>;

    // Verify initial state: no resources pending processing
    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadEntityDefinitions());

    // Add a resource that needs processing
    auto testResource = std::make_shared<TestResourceT>(
      []() { return Result<TestResource>{TestResource{}}; });
    appController.glManager().resourceManager().addResource(testResource);

    REQUIRE(appController.glManager().resourceManager().needsProcessing());
    CHECK(!window.canReloadEntityDefinitions());

    // Process all resources synchronously
    auto testGl = gl::TestGl{};
    const auto processContext = gl::ProcessContext{testGl, [](auto, auto) {}};
    gl::processResourcesSync(appController.glManager().resourceManager(), processContext);

    REQUIRE(!appController.glManager().resourceManager().needsProcessing());
    CHECK(window.canReloadEntityDefinitions());
  }
}

} // namespace tb::ui
