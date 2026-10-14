/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTimer>

#include "mdl/BrushBuilder.h"
#include "mdl/MapFormat.h"
#include "ui/SettleDialog.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
TEST_CASE("SettleDialog")
{
  const auto bounds = vm::bbox3d{8192.0};
  const auto builder = mdl::BrushBuilder{mdl::MapFormat::Valve, bounds};
  const auto source =
    builder.createCuboid(vm::bbox3d{{-16, -16, 96}, {16, 16, 128}}, "stone").value();
  const auto floor =
    builder.createCuboid(vm::bbox3d{{-512, -512, -32}, {512, 512, 0}}, "floor").value();
  auto dialog = SettleDialog{{source}, {floor}, bounds, nullptr};
  auto* start = dialog.findChild<QPushButton*>("settleStart");
  auto* pause = dialog.findChild<QPushButton*>("settlePause");
  auto* kick = dialog.findChild<QDoubleSpinBox*>("settleKick");
  auto* timer = dialog.findChild<QTimer*>("settleTimer");
  auto* buttons = dialog.findChild<QDialogButtonBox*>();
  REQUIRE(start);
  REQUIRE(pause);
  REQUIRE(kick);
  REQUIRE(timer);
  REQUIRE(buttons);
  auto* apply = buttons->button(QDialogButtonBox::Ok);
  CHECK_FALSE(apply->isEnabled());
  kick->setValue(0);
  start->click();
  REQUIRE(timer->isActive());
  REQUIRE(pause->isEnabled());
  const auto tick = [&] {
    REQUIRE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
  };

  SECTION("Pause validates, resume disables Apply, changed settings discard result")
  {
    for (int i = 0; i < 10; ++i)
    {
      tick();
    }
    pause->click();
    CHECK_FALSE(timer->isActive());
    CHECK(apply->isEnabled());
    pause->click();
    CHECK(timer->isActive());
    CHECK_FALSE(apply->isEnabled());
    kick->setValue(32);
    CHECK_FALSE(timer->isActive());
    CHECK_FALSE(apply->isEnabled());
    CHECK(dialog.takeBrushes().empty());
  }
  SECTION("Completion returns settled brushes and stops timer")
  {
    for (int i = 0; timer->isActive() && i < 601; ++i)
    {
      tick();
    }
    CHECK_FALSE(timer->isActive());
    REQUIRE(apply->isEnabled());
    apply->click();
    CHECK(dialog.result() == QDialog::Accepted);
    const auto result = dialog.takeBrushes();
    REQUIRE(result.size() == 1);
    CHECK(result[0].bounds().min.z() >= -0.25);
    CHECK(result[0].bounds().min.z() < 0.2);
  }
  SECTION("Cancel stops a running simulation")
  {
    buttons->button(QDialogButtonBox::Cancel)->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(timer->isActive());
    CHECK(dialog.takeBrushes().empty());
  }
}
} // namespace tb::ui
