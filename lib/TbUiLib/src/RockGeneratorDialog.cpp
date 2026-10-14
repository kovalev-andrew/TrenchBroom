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

#include "ui/RockGeneratorDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tb::ui
{
namespace
{

struct RockSettings
{
  int segments = 10;
  int seed = 1337;
  bool hollow = false;
  double thickness = 1.0;
  bool createGroup = false;
};

struct RockGeometry
{
  vm::vec3d center;
  std::vector<vm::vec3d> points;
  int sideSegments = 0;
  int ringCount = 0;
};

void collectGroupNames(const mdl::Node& node, std::set<std::string>& names)
{
  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    names.insert(groupNode->group().name());
  }

  for (const auto* child : node.children())
  {
    collectGroupNames(*child, names);
  }
}

std::string nextGeneratedGroupName(
  std::set<std::string>& usedNames, const std::string& prefix)
{
  for (auto index = 1; index < 1000000; ++index)
  {
    auto stream = std::ostringstream{};
    stream << prefix << "_" << std::setw(3) << std::setfill('0') << index;
    auto name = stream.str();
    if (!usedNames.contains(name))
    {
      usedNames.insert(name);
      return name;
    }
  }

  return prefix;
}

std::uint32_t hash32(
  const int x, const int y, const int z, const int seed, const std::uint32_t salt)
{
  auto h = std::uint32_t(seed) ^ salt;
  h ^= std::uint32_t(x) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
  h ^= std::uint32_t(y) + 0x85ebca6bu + (h << 6u) + (h >> 2u);
  h ^= std::uint32_t(z) + 0xc2b2ae35u + (h << 6u) + (h >> 2u);
  h ^= h >> 16u;
  h *= 0x7feb352du;
  h ^= h >> 15u;
  h *= 0x846ca68bu;
  h ^= h >> 16u;
  return h;
}

double hash01(
  const int x, const int y, const int z, const int seed, const std::uint32_t salt)
{
  return double(hash32(x, y, z, seed, salt) & 0x00FFFFFFu) / double(0x01000000u);
}

double rockRadiusFactor(
  const int ring, const int segment, const int seed, const int brushIndex)
{
  const auto coarse = hash01(ring / 2, segment / 2, brushIndex, seed, 0x6c8e9cf5u);
  const auto fine = hash01(ring, segment, brushIndex, seed, 0x4b1d5a3bu);
  const auto chip = hash01(ring * 5, segment * 7, brushIndex, seed, 0x91e10da5u);
  return std::clamp(0.64 + 0.25 * coarse + 0.17 * fine - 0.10 * chip, 0.55, 1.05);
}

RockGeometry createRockGeometry(
  const mdl::Brush& sourceBrush, const RockSettings& settings, const int brushIndex)
{
  const auto& bounds = sourceBrush.bounds();
  const auto center = bounds.center();
  const auto halfSize = bounds.size() / 2.0;
  const auto radius = vm::vec3d{
    std::max(0.5, halfSize.x()),
    std::max(0.5, halfSize.y()),
    std::max(0.5, halfSize.z())};

  const auto sideSegments = std::clamp(settings.segments, 4, 48);
  const auto ringCount = std::clamp(settings.segments / 2, 2, 24);
  const auto seed = settings.seed + brushIndex * 7919;

  auto geometry = RockGeometry{};
  geometry.center = center;
  geometry.sideSegments = sideSegments;
  geometry.ringCount = ringCount;
  geometry.points = std::vector<vm::vec3d>{};
  auto& points = geometry.points;
  points.reserve(static_cast<size_t>(sideSegments * ringCount + 2));

  const auto makePoint = [&](const double x, const double y, const double z) {
    return center + vm::vec3d{x * radius.x(), y * radius.y(), z * radius.z()};
  };

  const auto topFactor = 0.82 + 0.18 * hash01(0, 0, brushIndex, seed, 0xf00du);
  const auto bottomFactor = 0.82 + 0.18 * hash01(0, 1, brushIndex, seed, 0xbeefu);
  points.push_back(makePoint(0.0, 0.0, topFactor));

  for (auto ring = 1; ring <= ringCount; ++ring)
  {
    const auto v = double(ring) / double(ringCount + 1);
    const auto theta = v * vm::Cd::pi();
    const auto sinTheta = std::sin(theta);
    const auto cosTheta = std::cos(theta);
    const auto ringTwist = (hash01(ring, 0, brushIndex, seed, 0xabc123u) - 0.5)
                           * (vm::Cd::pi() / double(sideSegments));

    for (auto segment = 0; segment < sideSegments; ++segment)
    {
      const auto u = double(segment) / double(sideSegments);
      const auto phi = u * vm::Cd::two_pi() + ringTwist;
      const auto factor = rockRadiusFactor(ring, segment, seed, brushIndex);

      points.push_back(makePoint(
        std::cos(phi) * sinTheta * factor,
        std::sin(phi) * sinTheta * factor,
        cosTheta * factor));
    }
  }

  points.push_back(makePoint(0.0, 0.0, -bottomFactor));

  return geometry;
}

std::optional<mdl::Brush> createBrushFromPoints(
  mdl::Map& map, const mdl::Brush& sourceBrush, const std::vector<vm::vec3d>& points)
{
  if (points.size() < 4u)
  {
    return std::nullopt;
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  auto result = builder.createBrush(points, map.currentMaterialName());
  if (!result)
  {
    return std::nullopt;
  }

  auto brush = std::move(result).value();
  brush.cloneFaceAttributesFrom(sourceBrush);
  return brush;
}

std::vector<mdl::Brush> createHollowRockBrushes(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const RockGeometry& geometry,
  const RockSettings& settings)
{
  auto minRadius = std::numeric_limits<double>::max();
  for (const auto& point : geometry.points)
  {
    minRadius = std::min(minRadius, vm::length(point - geometry.center));
  }

  if (minRadius <= vm::Cd::almost_zero())
  {
    return {};
  }

  const auto thickness = std::clamp(settings.thickness, 0.001, minRadius * 0.8);
  auto innerPoints = std::vector<vm::vec3d>{};
  innerPoints.reserve(geometry.points.size());
  for (const auto& point : geometry.points)
  {
    const auto fromCenter = point - geometry.center;
    const auto distance = vm::length(fromCenter);
    const auto scale = std::max(0.05, (distance - thickness) / distance);
    innerPoints.push_back(geometry.center + fromCenter * scale);
  }

  const auto ringIndex = [&](const int ring, const int segment) {
    const auto wrappedSegment = (segment + geometry.sideSegments) % geometry.sideSegments;
    return size_t{1}
           + static_cast<size_t>(ring - 1) * static_cast<size_t>(geometry.sideSegments)
           + static_cast<size_t>(wrappedSegment);
  };
  const auto bottomIndex = size_t{1}
                           + static_cast<size_t>(geometry.ringCount)
                               * static_cast<size_t>(geometry.sideSegments);

  auto brushes = std::vector<mdl::Brush>{};
  brushes.reserve(
    static_cast<size_t>(geometry.sideSegments)
    * static_cast<size_t>(geometry.ringCount + 1));

  const auto addShellBrush = [&](const std::vector<size_t>& indices) {
    auto points = std::vector<vm::vec3d>{};
    points.reserve(indices.size() * 2u);
    for (const auto index : indices)
    {
      points.push_back(geometry.points[index]);
    }
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
      points.push_back(innerPoints[*it]);
    }

    if (auto brush = createBrushFromPoints(map, sourceBrush, points))
    {
      brushes.push_back(std::move(*brush));
    }
  };

  for (auto segment = 0; segment < geometry.sideSegments; ++segment)
  {
    const auto nextSegment = (segment + 1) % geometry.sideSegments;
    addShellBrush({0u, ringIndex(1, segment), ringIndex(1, nextSegment)});
  }

  for (auto ring = 1; ring < geometry.ringCount; ++ring)
  {
    for (auto segment = 0; segment < geometry.sideSegments; ++segment)
    {
      const auto nextSegment = (segment + 1) % geometry.sideSegments;
      addShellBrush(
        {ringIndex(ring, segment),
         ringIndex(ring, nextSegment),
         ringIndex(ring + 1, nextSegment),
         ringIndex(ring + 1, segment)});
    }
  }

  for (auto segment = 0; segment < geometry.sideSegments; ++segment)
  {
    const auto nextSegment = (segment + 1) % geometry.sideSegments;
    addShellBrush(
      {bottomIndex,
       ringIndex(geometry.ringCount, nextSegment),
       ringIndex(geometry.ringCount, segment)});
  }

  return brushes;
}

std::vector<mdl::Brush> createRockBrushes(
  mdl::Map& map,
  const mdl::Brush& sourceBrush,
  const RockSettings& settings,
  const int brushIndex)
{
  const auto geometry = createRockGeometry(sourceBrush, settings, brushIndex);
  if (settings.hollow)
  {
    return createHollowRockBrushes(map, sourceBrush, geometry, settings);
  }

  auto brushes = std::vector<mdl::Brush>{};
  if (auto brush = createBrushFromPoints(map, sourceBrush, geometry.points))
  {
    brushes.push_back(std::move(*brush));
  }
  return brushes;
}

void deleteNodes(const std::map<mdl::Node*, std::vector<mdl::Node*>>& nodes)
{
  for (const auto& [parent, children] : nodes)
  {
    static_cast<void>(parent);
    for (auto* child : children)
    {
      delete child;
    }
  }
}

} // namespace

RockGeneratorDialog::RockGeneratorDialog(MapDocument& document, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
{
  createGui();
}

RockGeneratorDialog::~RockGeneratorDialog()
{
  cancelPreview();
}

void RockGeneratorDialog::startPreview()
{
  cancelPreview();
  m_applied = false;
  captureSelection();
  resetSettings();
  beginPreview();
}

void RockGeneratorDialog::createGui()
{
  m_segments = new QSpinBox{};
  m_segments->setRange(4, 48);
  m_segments->setValue(10);

  m_seed = new QSpinBox{};
  m_seed->setRange(0, 999999);
  m_seed->setValue(1337);

  m_hollow = new QCheckBox{tr("Hollow")};

  const auto gridSize = std::max(0.001, m_document.map().grid().actualSize());
  m_thicknessLabel = new QLabel{tr("Thickness")};
  m_thickness = new QDoubleSpinBox{};
  m_thickness->setRange(0.001, 4096.0);
  m_thickness->setDecimals(3);
  m_thickness->setSingleStep(gridSize);
  m_thickness->setValue(gridSize);
  m_thicknessLabel->setVisible(false);
  m_thickness->setVisible(false);

  m_createGroup = new QCheckBox{tr("Create group")};

  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(new QLabel{tr("Rock - Segments")});
  layout->addWidget(m_segments);
  layout->addWidget(new QLabel{tr("Seed")});
  layout->addWidget(m_seed);
  layout->addWidget(m_hollow);
  layout->addWidget(m_thicknessLabel);
  layout->addWidget(m_thickness);
  layout->addWidget(m_createGroup);
  auto* defaults = new QPushButton{tr("Default")};
  auto* applyButton = new QPushButton{tr("Apply")};
  auto* cancel = new QPushButton{tr("Cancel")};
  layout->addWidget(defaults);
  layout->addWidget(applyButton);
  layout->addWidget(cancel);
  layout->addStretch(1);
  setLayout(layout);
  for (auto* spin : findChildren<QAbstractSpinBox*>())
  {
    spin->setKeyboardTracking(false);
  }

  connect(m_segments, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
    updatePreview();
  });
  connect(m_seed, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
    updatePreview();
  });
  connect(m_hollow, &QCheckBox::toggled, this, [this](const bool hollow) {
    m_thicknessLabel->setVisible(hollow);
    m_thickness->setVisible(hollow);
    updatePreview();
  });
  connect(
    m_thickness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
      updatePreview();
    });
  connect(m_createGroup, &QCheckBox::toggled, this, [this] { updatePreview(); });
  connect(applyButton, &QPushButton::clicked, this, [this] { apply(); });
  connect(defaults, &QPushButton::clicked, this, &RockGeneratorDialog::resetSettings);
  connect(cancel, &QPushButton::clicked, this, [this] { emit finished(); });
}

void RockGeneratorDialog::resetSettings()
{
  m_updatingPreview = true;
  m_segments->setValue(10);
  m_seed->setValue(1337);
  m_hollow->setChecked(false);
  const auto gridSize = std::max(0.001, m_document.map().grid().actualSize());
  m_thickness->setSingleStep(gridSize);
  m_thickness->setValue(gridSize);
  m_createGroup->setChecked(false);
  m_updatingPreview = false;
  updatePreview();
}

void RockGeneratorDialog::captureSelection()
{
  m_sourceBrushes.clear();
  const auto& selection = m_document.map().selection();
  m_sourceBrushes.reserve(selection.brushes.size());
  for (auto* brushNode : selection.brushes)
  {
    m_sourceBrushes.push_back({brushNode, brushNode->parent(), brushNode->brush()});
  }
}

void RockGeneratorDialog::beginPreview()
{
  if (m_sourceBrushes.empty())
  {
    return;
  }

  m_document.map().startTransaction("Generate Rock", mdl::TransactionScope::LongRunning);
  m_transactionActive = true;
  updatePreview();
}

void RockGeneratorDialog::updatePreview()
{
  if (!m_transactionActive || m_updatingPreview)
  {
    return;
  }

  m_updatingPreview = true;

  auto& map = m_document.map();
  map.rollbackTransaction();

  const auto settings = RockSettings{
    m_segments->value(),
    m_seed->value(),
    m_hollow->isChecked(),
    m_thickness->value(),
    m_createGroup->isChecked()};

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto originalNodes = std::vector<mdl::Node*>{};
  originalNodes.reserve(m_sourceBrushes.size());
  auto usedGroupNames = std::set<std::string>{};
  collectGroupNames(map.worldNode(), usedGroupNames);

  auto brushIndex = 0;
  for (const auto& sourceBrush : m_sourceBrushes)
  {
    auto rockBrushes = createRockBrushes(map, sourceBrush.brush, settings, brushIndex++);
    if (rockBrushes.empty())
    {
      deleteNodes(nodesToAdd);
      m_updatingPreview = false;
      return;
    }

    if (settings.createGroup)
    {
      auto* groupNode =
        new mdl::GroupNode{mdl::Group{nextGeneratedGroupName(usedGroupNames, "Rock")}};
      for (auto& rockBrush : rockBrushes)
      {
        groupNode->addChild(new mdl::BrushNode{std::move(rockBrush)});
      }
      nodesToAdd[sourceBrush.parent].push_back(groupNode);
    }
    else
    {
      for (auto& rockBrush : rockBrushes)
      {
        nodesToAdd[sourceBrush.parent].push_back(
          new mdl::BrushNode{std::move(rockBrush)});
      }
    }
    originalNodes.push_back(sourceBrush.node);
  }

  mdl::deselectAll(map);
  const auto addedNodes = mdl::addNodes(map, nodesToAdd);
  mdl::removeNodes(map, originalNodes);
  mdl::selectNodes(map, addedNodes);

  m_updatingPreview = false;
}

void RockGeneratorDialog::apply()
{
  if (m_transactionActive)
  {
    m_applied = true;
    m_document.map().commitTransaction();
    m_transactionActive = false;
  }
  emit finished();
}

void RockGeneratorDialog::cancelPreview()
{
  if (m_transactionActive && !m_applied)
  {
    m_document.map().cancelTransaction();
    m_transactionActive = false;
  }
}

} // namespace tb::ui
