/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include "mdl/BrushBuilder.h"
#include "mdl/BrushFracture.h"
#include "mdl/MapFormat.h"
#include "ui/FractureDialog.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
TEST_CASE("FractureDialog")
{
  const auto bounds = vm::bbox3d{8192.0};
  const auto format = mdl::MapFormat::Valve;
  const auto source =
    mdl::BrushBuilder{format, bounds}.createCube(128, "outside").value();
  auto dialog = FractureDialog{source, bounds, format, "inside", nullptr};
  auto* count = dialog.findChild<QSpinBox*>("fragmentCount");
  auto* seed = dialog.findChild<QSpinBox*>("fractureSeed");
  auto* material = dialog.findChild<QLineEdit*>("cutMaterial");
  auto* generate = dialog.findChild<QPushButton*>("generatePreview");
  auto* variant = dialog.findChild<QPushButton*>("anotherVariant");
  auto* buttons = dialog.findChild<QDialogButtonBox*>();
  REQUIRE(count);
  REQUIRE(seed);
  REQUIRE(material);
  REQUIRE(generate);
  REQUIRE(variant);
  REQUIRE(buttons);
  auto* apply = buttons->button(QDialogButtonBox::Ok);
  REQUIRE(apply->isEnabled());

  SECTION("Changed parameters cannot apply an outdated preview")
  {
    count->setValue(8);
    CHECK_FALSE(apply->isEnabled());
    generate->click();
    REQUIRE(apply->isEnabled());
    seed->setValue(17);
    CHECK_FALSE(apply->isEnabled());
    generate->click();
    REQUIRE(apply->isEnabled());
    const auto expected = mdl::fractureBrush(
      source, bounds, format, mdl::BrushFaceAttributes{"inside"}, 8, 17);
    REQUIRE(expected);
    apply->click();
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.takeFragments() == expected.value());
  }

  SECTION("Invalid cut material prevents applying")
  {
    material->setText("");
    CHECK_FALSE(apply->isEnabled());
    generate->click();
    CHECK_FALSE(apply->isEnabled());
    CHECK(dialog.takeFragments().empty());
    material->setText("inside");
    generate->click();
    REQUIRE(apply->isEnabled());
  }

  SECTION("Another variant changes seed and regenerates")
  {
    const auto oldSeed = seed->value();
    variant->click();
    CHECK(seed->value() == oldSeed + 1);
    REQUIRE(apply->isEnabled());
    const auto expected = mdl::fractureBrush(
      source,
      bounds,
      format,
      mdl::BrushFaceAttributes{"inside"},
      size_t(count->value()),
      uint32_t(seed->value()));
    REQUIRE(expected);
    CHECK(dialog.takeFragments() == expected.value());
  }

  SECTION("Cancel rejects the operation")
  {
    buttons->button(QDialogButtonBox::Cancel)->click();
    CHECK(dialog.result() == QDialog::Rejected);
  }
}
} // namespace tb::ui
