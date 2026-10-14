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

#include "ui/ArrayTool.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "Logger.h"
#include "NotifierConnection.h"
#include "Uuid.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/overload.h"

#include "vm/mat_ext.h"
#include "vm/scalar.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto MinCount = 1;
constexpr auto MaxCount = 1024;
constexpr auto MinRadius = 1.0;
constexpr auto MaxRadius = 65536.0;
constexpr auto MinSpacing = -65536.0;
constexpr auto MaxSpacing = 65536.0;
constexpr auto MinLines = 1;
constexpr auto MaxLines = 128;
constexpr auto MinRings = 1;
constexpr auto MaxRings = 128;
constexpr auto MaxUndoSteps = 64u;

constexpr auto DefaultAxis = ArrayTool::Axis::Z;
constexpr auto DefaultLineOrigin = ArrayTool::LineOrigin::Centered;
constexpr auto DefaultCount = 8;
constexpr auto DefaultSeed = 1;
constexpr auto DefaultRadius = 256.0;
constexpr auto DefaultSpacing = 128.0;
constexpr auto DefaultLineCount = 1;
constexpr auto DefaultRingCount = 1;
constexpr auto DefaultRingSpacing = 256.0;
constexpr auto DefaultCrossLines = false;
constexpr auto DefaultMirrorOffsets = true;

vm::vec3d axisVector(const ArrayTool::Axis axis)
{
  switch (axis)
  {
  case ArrayTool::Axis::X:
    return vm::vec3d{1.0, 0.0, 0.0};
  case ArrayTool::Axis::Y:
    return vm::vec3d{0.0, 1.0, 0.0};
  case ArrayTool::Axis::Z:
    return vm::vec3d{0.0, 0.0, 1.0};
    switchDefault();
  }
}

std::tuple<vm::vec3d, vm::vec3d> planeBasis(const ArrayTool::Axis axis)
{
  switch (axis)
  {
  case ArrayTool::Axis::X:
    return {vm::vec3d{0.0, 1.0, 0.0}, vm::vec3d{0.0, 0.0, 1.0}};
  case ArrayTool::Axis::Y:
    return {vm::vec3d{1.0, 0.0, 0.0}, vm::vec3d{0.0, 0.0, 1.0}};
  case ArrayTool::Axis::Z:
    return {vm::vec3d{1.0, 0.0, 0.0}, vm::vec3d{0.0, 1.0, 0.0}};
    switchDefault();
  }
}

vm::vec3d lateralAxis(const ArrayTool::Axis axis)
{
  const auto [u, v] = planeBasis(axis);
  return u;
}

double centeredOrdinal(const int index)
{
  if (index == 0)
  {
    return 0.0;
  }

  const auto ordinal = (index + 1) / 2;
  return index % 2 == 1 ? double(ordinal) : -double(ordinal);
}

double lineStep(const int index, const ArrayTool::LineOrigin lineOrigin)
{
  switch (lineOrigin)
  {
  case ArrayTool::LineOrigin::FromCenterForward:
    return double(index + 1);
  case ArrayTool::LineOrigin::Centered:
    return centeredOrdinal(index + 1);
    switchDefault();
  }
}

double lineOffsetStep(const int index, const ArrayTool::LineOrigin lineOrigin)
{
  switch (lineOrigin)
  {
  case ArrayTool::LineOrigin::FromCenterForward:
    return double(index);
  case ArrayTool::LineOrigin::Centered:
    return centeredOrdinal(index);
    switchDefault();
  }
}

bool transformNode(mdl::Node& node, mdl::Map& map, const vm::mat4x4d& transformation)
{
  auto success = true;
  const auto alignmentLock = map.editorContext().alignmentLock();

  node.accept(
    kdl::overload(
      [&](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        auto group = groupNode.group();
        group.transform(transformation);
        groupNode.setGroup(std::move(group));
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        entityNode.visitChildren(thisLambda);

        auto entity = entityNode.entity();
        entity.transform(
          transformation,
          entityNode.entityPropertyConfig().updateAnglePropertyAfterTransform);
        entityNode.setEntity(std::move(entity));
      },
      [&](mdl::BrushNode& brushNode) {
        auto brush = brushNode.brush();
        if (brush.transform(map.worldBounds(), transformation, alignmentLock))
        {
          brushNode.setBrush(std::move(brush));
        }
        else
        {
          success = false;
        }
      },
      [&](mdl::PatchNode& patchNode) {
        auto patch = patchNode.patch();
        patch.transform(transformation);
        patchNode.setPatch(std::move(patch));
      }));

  return success;
}

void resetLinkIds(mdl::Node& node)
{
  node.accept(
    kdl::overload(
      [&](auto&& thisLambda, mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::GroupNode& groupNode) {
        groupNode.setLinkId(generateUuid());
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, mdl::EntityNode& entityNode) {
        entityNode.setLinkId(generateUuid());
        entityNode.visitChildren(thisLambda);
      },
      [&](mdl::BrushNode& brushNode) { brushNode.setLinkId(generateUuid()); },
      [&](mdl::PatchNode& patchNode) { patchNode.setLinkId(generateUuid()); }));
}

bool shouldCloneParentWhenCloningNode(const mdl::Node* node)
{
  if (!node || !node->parent())
  {
    return false;
  }

  return node->parent()->accept(
    kdl::overload(
      [](const mdl::WorldNode&) { return false; },
      [](const mdl::LayerNode&) { return false; },
      [](const mdl::GroupNode&) { return false; },
      [](const mdl::EntityNode&) { return true; },
      [](const mdl::BrushNode&) { return false; },
      [](const mdl::PatchNode&) { return false; }));
}

mdl::Node* makeCloneForArray(mdl::Node& sourceNode, mdl::Map& map)
{
  auto* clone = sourceNode.cloneRecursively(map.worldBounds());
  resetLinkIds(*clone);

  if (shouldCloneParentWhenCloningNode(&sourceNode))
  {
    auto* parentClone = sourceNode.parent()->clone(map.worldBounds());
    resetLinkIds(*parentClone);
    parentClone->addChild(clone);
    return parentClone;
  }

  return clone;
}

std::vector<vm::vec3d> randomRadiusDeltas(
  const int count, const int seed, const double radius, const ArrayTool::Axis axis)
{
  const auto [u, v] = planeBasis(axis);
  auto rng = std::mt19937{static_cast<std::mt19937::result_type>(seed)};
  auto angleDistribution = std::uniform_real_distribution<double>{0.0, vm::Cd::two_pi()};
  auto radiusDistribution = std::uniform_real_distribution<double>{0.0, 1.0};

  auto result = std::vector<vm::vec3d>{};
  result.reserve(size_t(count));
  for (int i = 0; i < count; ++i)
  {
    const auto angle = angleDistribution(rng);
    const auto distance = std::sqrt(radiusDistribution(rng)) * radius;
    result.push_back((u * std::cos(angle) + v * std::sin(angle)) * distance);
  }
  return result;
}

std::vector<vm::vec3d> lineDeltas(
  const int count,
  const double spacing,
  const vm::vec3d& offset,
  const ArrayTool::Axis axis,
  const int lineCount,
  const ArrayTool::LineOrigin lineOrigin,
  const bool crossLines,
  const bool mirrorOffsets)
{
  const auto offsetsAreCentered =
    lineOrigin == ArrayTool::LineOrigin::Centered || crossLines;
  const auto offsetForStep = [&](const double step) {
    if (offsetsAreCentered && !mirrorOffsets)
    {
      return offset * std::abs(step);
    }
    return offset * step;
  };

  if (crossLines && lineCount > 1)
  {
    const auto [u, v] = planeBasis(axis);

    auto result = std::vector<vm::vec3d>{};
    result.reserve(size_t(count * lineCount));
    for (int line = 0; line < lineCount; ++line)
    {
      const auto angle = vm::Cd::pi() * double(line) / double(std::max(1, lineCount));
      const auto direction = u * std::cos(angle) + v * std::sin(angle);
      for (int i = 0; i < count; ++i)
      {
        const auto step = lineStep(i, ArrayTool::LineOrigin::Centered);
        result.push_back(direction * (step * spacing) + offsetForStep(step));
      }
    }
    return result;
  }

  const auto direction = axisVector(axis);
  const auto side = lateralAxis(axis);

  auto result = std::vector<vm::vec3d>{};
  result.reserve(size_t(count * lineCount));
  for (int line = 0; line < lineCount; ++line)
  {
    const auto sideOffset = side * (lineOffsetStep(line, lineOrigin) * spacing);
    for (int i = 0; i < count; ++i)
    {
      const auto step = lineStep(i, lineOrigin);
      result.push_back(direction * (step * spacing) + offsetForStep(step) + sideOffset);
    }
  }
  return result;
}

std::vector<vm::vec3d> radialDeltas(
  const int count,
  const int ringCount,
  const double ringSpacing,
  const vm::vec3d& offset,
  const ArrayTool::Axis axis)
{
  const auto [u, v] = planeBasis(axis);

  auto result = std::vector<vm::vec3d>{};
  result.reserve(size_t(count * ringCount));
  for (int ring = 1; ring <= ringCount; ++ring)
  {
    const auto radius = ringSpacing * double(ring);
    for (int i = 0; i < count; ++i)
    {
      const auto angle = (double(i) / double(std::max(1, count))) * vm::Cd::two_pi();
      result.push_back(
        (u * std::cos(angle) + v * std::sin(angle)) * radius + offset * double(ring));
    }
  }
  return result;
}

std::vector<vm::vec3d> deltasForSettings(const ArrayTool& tool)
{
  switch (tool.mode())
  {
  case ArrayTool::Mode::RandomRadius:
    return randomRadiusDeltas(tool.count(), tool.seed(), tool.radius(), tool.axis());
  case ArrayTool::Mode::Line:
    return lineDeltas(
      tool.count(),
      tool.spacing(),
      tool.offset(),
      tool.axis(),
      tool.lineCount(),
      tool.lineOrigin(),
      tool.crossLines(),
      tool.mirrorOffsets());
  case ArrayTool::Mode::Radial:
    return radialDeltas(
      tool.count(), tool.ringCount(), tool.ringSpacing(), tool.offset(), tool.axis());
    switchDefault();
  }
}

QLabel* label(const QString& text)
{
  return new QLabel{text};
}

class ArrayToolPage : public QWidget
{
private:
  ArrayTool& m_tool;
  NotifierConnection m_notifierConnection;
  QComboBox* m_mode = nullptr;
  QComboBox* m_axis = nullptr;
  QStackedWidget* m_modePages = nullptr;

  QSpinBox* m_randomCount = nullptr;
  QSpinBox* m_randomSeed = nullptr;
  QDoubleSpinBox* m_randomRadius = nullptr;

  QSpinBox* m_lineCountPerLine = nullptr;
  QDoubleSpinBox* m_lineSpacing = nullptr;
  QDoubleSpinBox* m_lineOffsetX = nullptr;
  QDoubleSpinBox* m_lineOffsetY = nullptr;
  QDoubleSpinBox* m_lineOffsetZ = nullptr;
  QSpinBox* m_lineLines = nullptr;
  QComboBox* m_lineOrigin = nullptr;
  QCheckBox* m_lineCrossCenter = nullptr;
  QCheckBox* m_lineMirrorOffsets = nullptr;

  QSpinBox* m_radialCount = nullptr;
  QSpinBox* m_radialRings = nullptr;
  QDoubleSpinBox* m_radialRingSpacing = nullptr;
  QDoubleSpinBox* m_radialOffsetX = nullptr;
  QDoubleSpinBox* m_radialOffsetY = nullptr;
  QDoubleSpinBox* m_radialOffsetZ = nullptr;
  QCheckBox* m_faceCenter = nullptr;
  QComboBox* m_facingSide = nullptr;
  QPushButton* m_default = nullptr;
  QPushButton* m_apply = nullptr;

public:
  explicit ArrayToolPage(ArrayTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    createGui();
  }

private:
  QSpinBox* makeIntSpinBox(const int min, const int max)
  {
    auto* spinBox = new QSpinBox{};
    spinBox->setRange(min, max);
    spinBox->setSingleStep(1);
    return spinBox;
  }

  QDoubleSpinBox* makeDoubleSpinBox(const double min, const double max, const double step)
  {
    auto* spinBox = new QDoubleSpinBox{};
    spinBox->setRange(min, max);
    spinBox->setDecimals(2);
    spinBox->setSingleStep(step);
    return spinBox;
  }

  QWidget* createRandomPage()
  {
    m_randomCount = makeIntSpinBox(MinCount, MaxCount);
    m_randomSeed = makeIntSpinBox(-999999, 999999);
    m_randomRadius = makeDoubleSpinBox(MinRadius, MaxRadius, 16.0);

    connect(
      m_randomCount,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setCount(value); });
    connect(
      m_randomSeed,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setSeed(value); });
    connect(
      m_randomRadius,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setRadius(value); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label(tr("Count")), 0, Qt::AlignVCenter);
    layout->addWidget(m_randomCount, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Seed")), 0, Qt::AlignVCenter);
    layout->addWidget(m_randomSeed, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Radius")), 0, Qt::AlignVCenter);
    layout->addWidget(m_randomRadius, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    auto* page = new QWidget{};
    page->setLayout(layout);
    return page;
  }

  QWidget* createLinePage()
  {
    m_lineCountPerLine = makeIntSpinBox(MinCount, MaxCount);
    m_lineSpacing = makeDoubleSpinBox(MinSpacing, MaxSpacing, 16.0);
    m_lineOffsetX = makeDoubleSpinBox(MinSpacing, MaxSpacing, 8.0);
    m_lineOffsetY = makeDoubleSpinBox(MinSpacing, MaxSpacing, 8.0);
    m_lineOffsetZ = makeDoubleSpinBox(MinSpacing, MaxSpacing, 8.0);
    m_lineLines = makeIntSpinBox(MinLines, MaxLines);
    m_lineOrigin = new QComboBox{};
    m_lineOrigin->addItem(tr("From center"));
    m_lineOrigin->addItem(tr("Left / right"));
    m_lineCrossCenter = new QCheckBox{tr("Cross center")};
    m_lineCrossCenter->setToolTip(
      tr("Place multiple lines through the source object, forming a star"));
    m_lineMirrorOffsets = new QCheckBox{tr("Mirror offset")};
    m_lineMirrorOffsets->setToolTip(
      tr("Mirror the offset on the opposite side when spawning left / right"));

    connect(
      m_lineCountPerLine,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setCount(value); });
    connect(
      m_lineSpacing,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setSpacing(value); });
    const auto updateOffset = [this]() {
      m_tool.setOffset(
        vm::vec3d{
          m_lineOffsetX->value(), m_lineOffsetY->value(), m_lineOffsetZ->value()});
    };
    connect(
      m_lineOffsetX,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      updateOffset);
    connect(
      m_lineOffsetY,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      updateOffset);
    connect(
      m_lineOffsetZ,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      updateOffset);
    connect(
      m_lineLines,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setLineCount(value); });
    connect(
      m_lineOrigin,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int value) {
        m_tool.setLineOrigin(
          value == 0 ? ArrayTool::LineOrigin::FromCenterForward
                     : ArrayTool::LineOrigin::Centered);
      });
    connect(m_lineCrossCenter, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setCrossLines(checked);
    });
    connect(m_lineMirrorOffsets, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setMirrorOffsets(!checked);
    });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label(tr("Count")), 0, Qt::AlignVCenter);
    layout->addWidget(m_lineCountPerLine, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Spacing")), 0, Qt::AlignVCenter);
    layout->addWidget(m_lineSpacing, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Offset")), 0, Qt::AlignVCenter);
    layout->addWidget(m_lineOffsetX, 0, Qt::AlignVCenter);
    layout->addWidget(m_lineOffsetY, 0, Qt::AlignVCenter);
    layout->addWidget(m_lineOffsetZ, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Lines")), 0, Qt::AlignVCenter);
    layout->addWidget(m_lineLines, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Spawn")), 0, Qt::AlignVCenter);
    layout->addWidget(m_lineOrigin, 0, Qt::AlignVCenter);
    layout->addWidget(m_lineCrossCenter, 0, Qt::AlignVCenter);
    layout->addWidget(m_lineMirrorOffsets, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    auto* page = new QWidget{};
    page->setLayout(layout);
    return page;
  }

  QWidget* createRadialPage()
  {
    m_radialCount = makeIntSpinBox(MinCount, MaxCount);
    m_radialRings = makeIntSpinBox(MinRings, MaxRings);
    m_radialRingSpacing = makeDoubleSpinBox(MinRadius, MaxRadius, 16.0);
    m_radialOffsetX = makeDoubleSpinBox(MinSpacing, MaxSpacing, 8.0);
    m_radialOffsetY = makeDoubleSpinBox(MinSpacing, MaxSpacing, 8.0);
    m_radialOffsetZ = makeDoubleSpinBox(MinSpacing, MaxSpacing, 8.0);

    connect(
      m_radialCount,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setCount(value); });
    connect(
      m_radialRings,
      QOverload<int>::of(&QSpinBox::valueChanged),
      this,
      [&](const int value) { m_tool.setRingCount(value); });
    connect(
      m_radialRingSpacing,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](const double value) { m_tool.setRingSpacing(value); });
    const auto updateOffset = [this]() {
      m_tool.setOffset(
        vm::vec3d{
          m_radialOffsetX->value(), m_radialOffsetY->value(), m_radialOffsetZ->value()});
    };
    connect(
      m_radialOffsetX,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      updateOffset);
    connect(
      m_radialOffsetY,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      updateOffset);
    connect(
      m_radialOffsetZ,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      updateOffset);

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label(tr("Count")), 0, Qt::AlignVCenter);
    layout->addWidget(m_radialCount, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Rings")), 0, Qt::AlignVCenter);
    layout->addWidget(m_radialRings, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Ring spacing")), 0, Qt::AlignVCenter);
    layout->addWidget(m_radialRingSpacing, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Offset")), 0, Qt::AlignVCenter);
    layout->addWidget(m_radialOffsetX, 0, Qt::AlignVCenter);
    layout->addWidget(m_radialOffsetY, 0, Qt::AlignVCenter);
    layout->addWidget(m_radialOffsetZ, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    auto* page = new QWidget{};
    page->setLayout(layout);
    return page;
  }

  void createGui()
  {
    m_mode = new QComboBox{};
    m_mode->addItem(tr("RandomRadius"));
    m_mode->addItem(tr("Line"));
    m_mode->addItem(tr("Radial"));

    m_axis = new QComboBox{};
    m_axis->addItem(tr("X"));
    m_axis->addItem(tr("Y"));
    m_axis->addItem(tr("Z"));

    m_faceCenter = new QCheckBox{tr("Face center")};
    m_facingSide = new QComboBox{};
    m_facingSide->addItems({"+X", "-X", "+Y", "-Y", "+Z", "-Z"});
    m_facingSide->setToolTip(
      tr("Side of the original object that points toward the center"));
    connect(m_faceCenter, &QCheckBox::toggled, this, [&](const bool checked) {
      m_tool.setFaceCenter(checked);
    });
    connect(
      m_facingSide, qOverload<int>(&QComboBox::activated), this, [&](const int index) {
        m_tool.setFacingSide(static_cast<ArrayTool::FacingSide>(index));
      });

    m_modePages = new QStackedWidget{};
    m_modePages->addWidget(createRandomPage());
    m_modePages->addWidget(createLinePage());
    m_modePages->addWidget(createRadialPage());

    m_default = new QPushButton{tr("Default")};
    m_apply = new QPushButton{tr("Apply")};

    connect(
      m_mode,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int index) {
        m_tool.setMode(
          index == 0   ? ArrayTool::Mode::RandomRadius
          : index == 1 ? ArrayTool::Mode::Line
                       : ArrayTool::Mode::Radial);
        m_modePages->setCurrentIndex(index);
        updateControls();
      });
    connect(
      m_axis,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
      this,
      [&](const int index) {
        m_tool.setAxis(
          index == 0   ? ArrayTool::Axis::X
          : index == 1 ? ArrayTool::Axis::Y
                       : ArrayTool::Axis::Z);
      });
    connect(m_default, &QPushButton::clicked, this, [&]() { m_tool.resetSettings(); });
    connect(m_apply, &QPushButton::clicked, this, [&]() { m_tool.apply(); });

    m_notifierConnection +=
      m_tool.settingsDidChangeNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolActivatedNotifier.connect([&](auto&) { updateControls(); });
    m_notifierConnection +=
      m_tool.toolDeactivatedNotifier.connect([&](auto&) { updateControls(); });

    auto* layout = new QHBoxLayout{};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(LayoutConstants::MediumHMargin);
    layout->addWidget(label(tr("Array")), 0, Qt::AlignVCenter);
    layout->addWidget(m_mode, 0, Qt::AlignVCenter);
    layout->addWidget(label(tr("Axis")), 0, Qt::AlignVCenter);
    layout->addWidget(m_axis, 0, Qt::AlignVCenter);
    layout->addWidget(m_modePages, 1, Qt::AlignVCenter);
    layout->addWidget(m_default, 0, Qt::AlignVCenter);
    layout->addWidget(m_apply, 0, Qt::AlignVCenter);
    auto* orientation = new QHBoxLayout{};
    orientation->setContentsMargins(0, 0, 0, 0);
    orientation->addWidget(m_faceCenter);
    orientation->addWidget(m_facingSide);
    orientation->addStretch(1);
    auto* outer = new QVBoxLayout{};
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addLayout(layout);
    outer->addLayout(orientation);
    setLayout(outer);

    updateControls();
  }

  void updateControls()
  {
    {
      const auto centerBlocker = QSignalBlocker{m_faceCenter};
      const auto sideBlocker = QSignalBlocker{m_facingSide};
      const auto radial = m_tool.mode() != ArrayTool::Mode::Line;
      m_faceCenter->setVisible(radial);
      m_facingSide->setVisible(radial);
      m_faceCenter->setChecked(m_tool.faceCenter());
      m_facingSide->setCurrentIndex(static_cast<int>(m_tool.facingSide()));
      m_facingSide->setEnabled(m_tool.faceCenter());
    }
    {
      const auto blocker = QSignalBlocker{m_mode};
      m_mode->setCurrentIndex(
        m_tool.mode() == ArrayTool::Mode::RandomRadius ? 0
        : m_tool.mode() == ArrayTool::Mode::Line       ? 1
                                                       : 2);
      m_modePages->setCurrentIndex(m_mode->currentIndex());
    }
    {
      const auto blocker = QSignalBlocker{m_axis};
      m_axis->setCurrentIndex(
        m_tool.axis() == ArrayTool::Axis::X   ? 0
        : m_tool.axis() == ArrayTool::Axis::Y ? 1
                                              : 2);
    }
    {
      const auto blocker = QSignalBlocker{m_randomCount};
      m_randomCount->setValue(m_tool.count());
    }
    {
      const auto blocker = QSignalBlocker{m_randomSeed};
      m_randomSeed->setValue(m_tool.seed());
    }
    {
      const auto blocker = QSignalBlocker{m_randomRadius};
      m_randomRadius->setValue(m_tool.radius());
    }
    {
      const auto blocker = QSignalBlocker{m_lineCountPerLine};
      m_lineCountPerLine->setValue(m_tool.count());
    }
    {
      const auto blocker = QSignalBlocker{m_lineSpacing};
      m_lineSpacing->setValue(m_tool.spacing());
    }
    {
      const auto blockerX = QSignalBlocker{m_lineOffsetX};
      const auto blockerY = QSignalBlocker{m_lineOffsetY};
      const auto blockerZ = QSignalBlocker{m_lineOffsetZ};
      m_lineOffsetX->setValue(m_tool.offset().x());
      m_lineOffsetY->setValue(m_tool.offset().y());
      m_lineOffsetZ->setValue(m_tool.offset().z());
    }
    {
      const auto blocker = QSignalBlocker{m_lineLines};
      m_lineLines->setValue(m_tool.lineCount());
    }
    {
      const auto blocker = QSignalBlocker{m_lineOrigin};
      m_lineOrigin->setCurrentIndex(
        m_tool.lineOrigin() == ArrayTool::LineOrigin::FromCenterForward ? 0 : 1);
    }
    {
      const auto blocker = QSignalBlocker{m_lineCrossCenter};
      m_lineCrossCenter->setChecked(m_tool.crossLines());
      m_lineOrigin->setEnabled(!m_tool.crossLines());
    }
    {
      const auto blocker = QSignalBlocker{m_lineMirrorOffsets};
      m_lineMirrorOffsets->setChecked(!m_tool.mirrorOffsets());
      m_lineMirrorOffsets->setEnabled(
        m_tool.lineOrigin() == ArrayTool::LineOrigin::Centered || m_tool.crossLines());
    }
    {
      const auto blocker = QSignalBlocker{m_radialCount};
      m_radialCount->setValue(m_tool.count());
    }
    {
      const auto blocker = QSignalBlocker{m_radialRings};
      m_radialRings->setValue(m_tool.ringCount());
    }
    {
      const auto blocker = QSignalBlocker{m_radialRingSpacing};
      m_radialRingSpacing->setValue(m_tool.ringSpacing());
    }
    {
      const auto blockerX = QSignalBlocker{m_radialOffsetX};
      const auto blockerY = QSignalBlocker{m_radialOffsetY};
      const auto blockerZ = QSignalBlocker{m_radialOffsetZ};
      m_radialOffsetX->setValue(m_tool.offset().x());
      m_radialOffsetY->setValue(m_tool.offset().y());
      m_radialOffsetZ->setValue(m_tool.offset().z());
    }
  }
};

} // namespace

ArrayTool::ArrayTool(MapDocument& document)
  : Tool{false}
  , m_document{document}
{
}

ArrayTool::~ArrayTool() = default;

bool ArrayTool::Settings::operator==(const Settings& other) const
{
  return mode == other.mode && axis == other.axis && lineOrigin == other.lineOrigin
         && count == other.count && seed == other.seed && radius == other.radius
         && spacing == other.spacing && offset == other.offset
         && lineCount == other.lineCount && ringCount == other.ringCount
         && ringSpacing == other.ringSpacing && crossLines == other.crossLines
         && mirrorOffsets == other.mirrorOffsets && faceCenter == other.faceCenter
         && facingSide == other.facingSide;
}

bool ArrayTool::canAttachToSelection() const
{
  const auto& selection = m_document.map().selection();
  return selection.nodes.size() == 1u && !selection.hasBrushFaces();
}

bool ArrayTool::apply()
{
  auto& map = m_document.map();
  if (!m_previewTransactionOpen)
  {
    return false;
  }

  if (m_previewNodes.empty())
  {
    map.cancelTransaction();
    m_previewTransactionOpen = false;
    return false;
  }

  const auto nodesToSelect = m_previewNodes;
  mdl::selectNodes(map, nodesToSelect);
  m_previewNodes.clear();
  m_previewTransactionOpen = false;

  if (!map.commitTransaction())
  {
    return false;
  }

  m_undoStack.clear();
  refreshViews();
  arrayDidRequestDeactivateNotifier(*this);
  return true;
}

bool ArrayTool::undoStep()
{
  if (!active() || m_undoStack.empty())
  {
    return false;
  }

  const auto settings = m_undoStack.back();
  m_undoStack.pop_back();
  applySettings(settings);
  updatePreview();
  notifySettingsChanged();
  return true;
}

void ArrayTool::resetSettings()
{
  const auto defaults = Settings{
    m_mode,
    DefaultAxis,
    DefaultLineOrigin,
    DefaultCount,
    DefaultSeed,
    DefaultRadius,
    DefaultSpacing,
    vm::vec3d{0.0, 0.0, 0.0},
    DefaultLineCount,
    DefaultRingCount,
    DefaultRingSpacing,
    DefaultCrossLines,
    DefaultMirrorOffsets,
    false,
    FacingSide::PosX};

  if (currentSettings() == defaults)
  {
    return;
  }

  saveUndoState();
  applySettings(defaults);
  updatePreview();
  notifySettingsChanged();
}

ArrayTool::Settings ArrayTool::currentSettings() const
{
  return Settings{
    m_mode,
    m_axis,
    m_lineOrigin,
    m_count,
    m_seed,
    m_radius,
    m_spacing,
    m_offset,
    m_lineCount,
    m_ringCount,
    m_ringSpacing,
    m_crossLines,
    m_mirrorOffsets,
    m_faceCenter,
    m_facingSide};
}

void ArrayTool::applySettings(const Settings& settings)
{
  m_mode = settings.mode;
  m_axis = settings.axis;
  m_lineOrigin = settings.lineOrigin;
  m_count = settings.count;
  m_seed = settings.seed;
  m_radius = settings.radius;
  m_spacing = settings.spacing;
  m_offset = settings.offset;
  m_lineCount = settings.lineCount;
  m_ringCount = settings.ringCount;
  m_ringSpacing = settings.ringSpacing;
  m_crossLines = settings.crossLines;
  m_mirrorOffsets = settings.mirrorOffsets;
  m_faceCenter = settings.faceCenter;
  m_facingSide = settings.facingSide;
}

void ArrayTool::saveUndoState()
{
  if (!active())
  {
    return;
  }

  const auto settings = currentSettings();
  if (!m_undoStack.empty() && m_undoStack.back() == settings)
  {
    return;
  }

  if (m_undoStack.size() >= MaxUndoSteps)
  {
    m_undoStack.erase(m_undoStack.begin());
  }
  m_undoStack.push_back(settings);
}

void ArrayTool::notifySettingsChanged()
{
  settingsDidChangeNotifier(*this);
}

bool ArrayTool::rebuildPreview()
{
  if (!canAttachToSelection())
  {
    m_document.logger().error()
      << "Array Tool needs exactly one selected object or group";
    return false;
  }

  auto& map = m_document.map();
  auto* sourceNode = map.selection().nodes.front();
  auto* parent = mdl::parentForNodes(map, {sourceNode});
  if (!parent)
  {
    return false;
  }

  auto deltas = deltasForSettings(*this);
  if (deltas.empty())
  {
    return false;
  }

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto& children = nodesToAdd[parent];
  children.reserve(deltas.size());

  for (const auto& delta : deltas)
  {
    auto clone = std::unique_ptr<mdl::Node>{makeCloneForArray(*sourceNode, map)};
    auto transformation = vm::translation_matrix(delta);
    if (
      m_faceCenter && m_mode != Mode::Line
      && vm::squared_length(delta) > vm::Cd::almost_zero())
    {
      auto side = vm::vec3d{0, 0, 0};
      const auto sideIndex = static_cast<size_t>(m_facingSide);
      side[sideIndex / 2] = sideIndex % 2 == 0 ? 1.0 : -1.0;
      const auto center = sourceNode->physicalBounds().center();
      transformation = vm::translation_matrix(center + delta)
                       * vm::rotation_matrix(side, vm::normalize(-delta))
                       * vm::translation_matrix(-center);
    }
    if (!transformNode(*clone, map, transformation))
    {
      map.logger().error() << "Could not transform array clone";
      continue;
    }

    children.push_back(clone.release());
  }

  if (children.empty())
  {
    return false;
  }

  m_previewNodes = mdl::addNodes(map, nodesToAdd);
  return !m_previewNodes.empty();
}

void ArrayTool::updatePreview()
{
  if (!m_previewTransactionOpen)
  {
    return;
  }

  m_document.map().rollbackTransaction();
  m_previewNodes.clear();
  rebuildPreview();
  refreshViews();
}

void ArrayTool::cancelPreview()
{
  if (!m_previewTransactionOpen)
  {
    m_undoStack.clear();
    return;
  }

  m_document.map().cancelTransaction();
  m_previewTransactionOpen = false;
  m_previewNodes.clear();
  m_undoStack.clear();
  refreshViews();
}

ArrayTool::Mode ArrayTool::mode() const
{
  return m_mode;
}

void ArrayTool::setMode(const Mode mode)
{
  if (m_mode == mode)
  {
    return;
  }
  saveUndoState();
  m_mode = mode;
  updatePreview();
  notifySettingsChanged();
}

ArrayTool::Axis ArrayTool::axis() const
{
  return m_axis;
}

void ArrayTool::setAxis(const Axis axis)
{
  if (m_axis == axis)
  {
    return;
  }
  saveUndoState();
  m_axis = axis;
  updatePreview();
  notifySettingsChanged();
}

ArrayTool::LineOrigin ArrayTool::lineOrigin() const
{
  return m_lineOrigin;
}

void ArrayTool::setLineOrigin(const LineOrigin lineOrigin)
{
  if (m_lineOrigin == lineOrigin)
  {
    return;
  }
  saveUndoState();
  m_lineOrigin = lineOrigin;
  updatePreview();
  notifySettingsChanged();
}

bool ArrayTool::crossLines() const
{
  return m_crossLines;
}

void ArrayTool::setCrossLines(const bool crossLines)
{
  if (m_crossLines == crossLines)
  {
    return;
  }
  saveUndoState();
  m_crossLines = crossLines;
  updatePreview();
  notifySettingsChanged();
}

bool ArrayTool::mirrorOffsets() const
{
  return m_mirrorOffsets;
}

bool ArrayTool::faceCenter() const
{
  return m_faceCenter;
}

void ArrayTool::setFaceCenter(const bool faceCenter)
{
  if (m_faceCenter == faceCenter)
  {
    return;
  }
  saveUndoState();
  m_faceCenter = faceCenter;
  updatePreview();
  notifySettingsChanged();
}

ArrayTool::FacingSide ArrayTool::facingSide() const
{
  return m_facingSide;
}

void ArrayTool::setFacingSide(const FacingSide side)
{
  if (m_facingSide == side)
  {
    return;
  }
  saveUndoState();
  m_facingSide = side;
  updatePreview();
  notifySettingsChanged();
}

void ArrayTool::setMirrorOffsets(const bool mirrorOffsets)
{
  if (m_mirrorOffsets == mirrorOffsets)
  {
    return;
  }
  saveUndoState();
  m_mirrorOffsets = mirrorOffsets;
  updatePreview();
  notifySettingsChanged();
}

int ArrayTool::count() const
{
  return m_count;
}

void ArrayTool::setCount(const int count)
{
  const auto clampedCount = std::clamp(count, MinCount, MaxCount);
  if (m_count == clampedCount)
  {
    return;
  }
  saveUndoState();
  m_count = clampedCount;
  updatePreview();
  notifySettingsChanged();
}

int ArrayTool::seed() const
{
  return m_seed;
}

void ArrayTool::setSeed(const int seed)
{
  if (m_seed == seed)
  {
    return;
  }
  saveUndoState();
  m_seed = seed;
  updatePreview();
  notifySettingsChanged();
}

double ArrayTool::radius() const
{
  return m_radius;
}

void ArrayTool::setRadius(const double radius)
{
  const auto clampedRadius = std::clamp(radius, MinRadius, MaxRadius);
  if (m_radius == clampedRadius)
  {
    return;
  }
  saveUndoState();
  m_radius = clampedRadius;
  updatePreview();
  notifySettingsChanged();
}

double ArrayTool::spacing() const
{
  return m_spacing;
}

void ArrayTool::setSpacing(const double spacing)
{
  const auto clampedSpacing = std::clamp(spacing, MinSpacing, MaxSpacing);
  if (m_spacing == clampedSpacing)
  {
    return;
  }
  saveUndoState();
  m_spacing = clampedSpacing;
  updatePreview();
  notifySettingsChanged();
}

vm::vec3d ArrayTool::offset() const
{
  return m_offset;
}

void ArrayTool::setOffset(const vm::vec3d& offset)
{
  const auto clampedOffset = vm::vec3d{
    std::clamp(offset.x(), MinSpacing, MaxSpacing),
    std::clamp(offset.y(), MinSpacing, MaxSpacing),
    std::clamp(offset.z(), MinSpacing, MaxSpacing)};
  if (m_offset == clampedOffset)
  {
    return;
  }
  saveUndoState();
  m_offset = clampedOffset;
  updatePreview();
  notifySettingsChanged();
}

int ArrayTool::lineCount() const
{
  return m_lineCount;
}

void ArrayTool::setLineCount(const int lineCount)
{
  const auto clampedLineCount = std::clamp(lineCount, MinLines, MaxLines);
  if (m_lineCount == clampedLineCount)
  {
    return;
  }
  saveUndoState();
  m_lineCount = clampedLineCount;
  updatePreview();
  notifySettingsChanged();
}

int ArrayTool::ringCount() const
{
  return m_ringCount;
}

void ArrayTool::setRingCount(const int ringCount)
{
  const auto clampedRingCount = std::clamp(ringCount, MinRings, MaxRings);
  if (m_ringCount == clampedRingCount)
  {
    return;
  }
  saveUndoState();
  m_ringCount = clampedRingCount;
  updatePreview();
  notifySettingsChanged();
}

double ArrayTool::ringSpacing() const
{
  return m_ringSpacing;
}

void ArrayTool::setRingSpacing(const double ringSpacing)
{
  const auto clampedRingSpacing = std::clamp(ringSpacing, MinRadius, MaxRadius);
  if (m_ringSpacing == clampedRingSpacing)
  {
    return;
  }
  saveUndoState();
  m_ringSpacing = clampedRingSpacing;
  updatePreview();
  notifySettingsChanged();
}

bool ArrayTool::doActivate()
{
  if (!canAttachToSelection())
  {
    return false;
  }

  m_undoStack.clear();
  auto& map = m_document.map();
  map.startTransaction("Array Copies", mdl::TransactionScope::LongRunning);
  m_previewTransactionOpen = true;
  if (!rebuildPreview())
  {
    cancelPreview();
    return false;
  }

  refreshViews();
  return true;
}

bool ArrayTool::doDeactivate()
{
  cancelPreview();
  return true;
}

QWidget* ArrayTool::doCreatePage(QWidget* parent)
{
  return new ArrayToolPage{*this, parent};
}

} // namespace tb::ui
