/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSpinBox>

#include "mdl/BrushBuilder.h"
#include "mdl/MapFormat.h"
#include "ui/PerforateDialog.h"

#include <catch2/catch_test_macros.hpp>
namespace tb::ui
{
TEST_CASE("PerforateDialog")
{
  const auto bounds = vm::bbox3d{8192.0};
  const auto format = mdl::MapFormat::Valve;
  const auto source = mdl::BrushBuilder{format, bounds}.createCube(256, "stone").value();
  auto dialog = PerforateDialog{
    source, source.findFace(vm::vec3d{0, 0, 1}).value(), bounds, format, nullptr};
  auto* apply = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
  auto* generate = dialog.findChild<QPushButton*>("generatePerforation");
  REQUIRE(apply->isEnabled());
  CHECK(dialog.keepPlugs());
  dialog.findChild<QSpinBox*>("holeCount")->setValue(5);
  CHECK_FALSE(apply->isEnabled());
  generate->click();
  REQUIRE(apply->isEnabled());
  dialog.findChild<QCheckBox*>("keepPlugs")->setChecked(false);
  CHECK_FALSE(dialog.keepPlugs());
  CHECK(dialog.takePerforation().plugs.size() == 5);
  dialog.findChild<QDoubleSpinBox*>("holeMinSize")->setValue(1024);
  generate->click();
  CHECK_FALSE(apply->isEnabled());
  CHECK(dialog.takePerforation().plugs.empty());
}
} // namespace tb::ui
