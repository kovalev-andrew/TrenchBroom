/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCheckBox>
#include <QSlider>
#include "mdl/BrushFace.h"
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

  SECTION("Separation defaults to zero and moves the applied fragments")
  {
    auto* separation = dialog.findChild<QSlider*>("fractureSeparation");
    REQUIRE(separation);
    CHECK(separation->value() == 0);
    const auto base = dialog.takeFragments();
    separation->setValue(50);
    REQUIRE(apply->isEnabled());
    const auto moved = dialog.takeFragments();
    REQUIRE(moved.size() == base.size());
    for (size_t i = 0; i < base.size(); ++i)
    {
      const auto expected =
        base[i].bounds().center()
        + (base[i].bounds().center() - source.bounds().center()) * 0.5;
      CHECK(vm::length(moved[i].bounds().center() - expected) < 0.001);
    }
    separation->setValue(0);
    CHECK(dialog.takeFragments() == base);
    CHECK_FALSE(dialog.createFuncWalls());
    dialog.findChild<QCheckBox*>("fractureNoCollide")->setChecked(true);
    CHECK(dialog.createFuncWalls());
  }

  SECTION("Nonunit texture scales survive fracture and separation")
  {
    auto scaled = source;
    for (auto& face : scaled.faces())
    {
      auto attributes = face.attributes();
      attributes.setScale(vm::vec2f{0.25f, -0.5f});
      face.setAttributes(attributes);
    }
    auto scaledDialog = FractureDialog{scaled, bounds, format, "inside", nullptr};
    for (const auto separation : {0, 50})
    {
      scaledDialog.findChild<QSlider*>("fractureSeparation")->setValue(separation);
      const auto fragments = scaledDialog.takeFragments();
      REQUIRE_FALSE(fragments.empty());
      for (const auto& brush : fragments)
        for (const auto& face : brush.faces())
          CHECK(face.attributes().scale() == vm::vec2f{0.25f, -0.5f});
    }
  }

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
