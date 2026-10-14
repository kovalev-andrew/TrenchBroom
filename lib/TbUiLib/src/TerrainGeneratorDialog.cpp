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

#include "ui/TerrainGeneratorDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
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
#include <array>
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

namespace tb::ui
{
namespace
{

enum class TerrainSide
{
  PosZ,
  NegZ,
  PosX,
  NegX,
  PosY,
  NegY,
};

struct TerrainSideInfo
{
  size_t normalAxis;
  double normalSign;
  size_t uAxis;
  size_t vAxis;
};

TerrainSideInfo terrainSideInfo(const TerrainSide side)
{
  switch (side)
  {
  case TerrainSide::PosZ:
    return {2u, 1.0, 0u, 1u};
  case TerrainSide::NegZ:
    return {2u, -1.0, 0u, 1u};
  case TerrainSide::PosX:
    return {0u, 1.0, 1u, 2u};
  case TerrainSide::NegX:
    return {0u, -1.0, 1u, 2u};
  case TerrainSide::PosY:
    return {1u, 1.0, 0u, 2u};
  case TerrainSide::NegY:
    return {1u, -1.0, 0u, 2u};
  }

  return {2u, 1.0, 0u, 1u};
}

struct TerrainSettings
{
  int segmentsX = 10;
  int segmentsY = 10;
  double height = 128.0;
  double intensity = 1.0;
  double frequency = 4.0;
  double roughness = 1.0;
  int seed = 1337;
  TerrainSide side = TerrainSide::PosZ;
  bool triangularGrid = true;
  bool createGroup = false;
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

std::uint32_t hash32(const int x, const int y, const int seed, const std::uint32_t salt)
{
  auto h = std::uint32_t(seed) ^ salt;
  h ^= std::uint32_t(x) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
  h ^= std::uint32_t(y) + 0x85ebca6bu + (h << 6u) + (h >> 2u);
  h ^= h >> 16u;
  h *= 0x7feb352du;
  h ^= h >> 15u;
  h *= 0x846ca68bu;
  h ^= h >> 16u;
  return h;
}

double hash01(const int x, const int y, const int seed, const std::uint32_t salt)
{
  return double(hash32(x, y, seed, salt) & 0x00FFFFFFu) / double(0x01000000u);
}

double voronoiNoise(const double u, const double v, const TerrainSettings& settings)
{
  const auto x = u * settings.frequency;
  const auto y = v * settings.frequency;
  const auto cellX = int(std::floor(x));
  const auto cellY = int(std::floor(y));

  auto nearest = std::numeric_limits<double>::max();
  auto secondNearest = std::numeric_limits<double>::max();

  for (auto dy = -1; dy <= 1; ++dy)
  {
    for (auto dx = -1; dx <= 1; ++dx)
    {
      const auto cx = cellX + dx;
      const auto cy = cellY + dy;
      const auto featureX = double(cx) + hash01(cx, cy, settings.seed, 0x1234u);
      const auto featureY = double(cy) + hash01(cx, cy, settings.seed, 0x9abcu);
      const auto distance = std::hypot(x - featureX, y - featureY);

      if (distance < nearest)
      {
        secondNearest = nearest;
        nearest = distance;
      }
      else if (distance < secondNearest)
      {
        secondNearest = distance;
      }
    }
  }

  const auto cellPeak = 1.0 - std::clamp(nearest / std::sqrt(2.0), 0.0, 1.0);
  const auto cellRidge = std::clamp((secondNearest - nearest) * 1.35, 0.0, 1.0);
  const auto roughness = std::clamp(settings.roughness, 0.0, 2.0) / 2.0;
  return std::clamp(cellPeak * (1.0 - roughness) + cellRidge * roughness, 0.0, 1.0);
}

std::optional<mdl::Brush> createTrianglePrismBrush(
  const mdl::BrushBuilder& builder,
  const mdl::Brush& sourceBrush,
  const std::string& materialName,
  const std::array<vm::vec3d, 3>& bottom,
  const std::array<vm::vec3d, 3>& top)
{
  auto points = std::vector<vm::vec3d>{};
  points.reserve(6);
  points.insert(std::end(points), std::begin(bottom), std::end(bottom));
  points.insert(std::end(points), std::begin(top), std::end(top));

  auto result = builder.createBrush(points, materialName);
  if (!result)
  {
    return std::nullopt;
  }

  auto brush = std::move(result).value();
  brush.cloneFaceAttributesFrom(sourceBrush);
  return brush;
}

std::optional<mdl::Brush> createQuadPrismBrush(
  const mdl::BrushBuilder& builder,
  const mdl::Brush& sourceBrush,
  const std::string& materialName,
  const std::array<vm::vec3d, 4>& bottom,
  const std::array<vm::vec3d, 4>& top)
{
  auto points = std::vector<vm::vec3d>{};
  points.reserve(8);
  points.insert(std::end(points), std::begin(bottom), std::end(bottom));
  points.insert(std::end(points), std::begin(top), std::end(top));

  auto result = builder.createBrush(points, materialName);
  if (!result)
  {
    return std::nullopt;
  }

  auto brush = std::move(result).value();
  brush.cloneFaceAttributesFrom(sourceBrush);
  return brush;
}

std::vector<mdl::BrushNode*> createTerrainNodes(
  mdl::Map& map, const mdl::Brush& sourceBrush, const TerrainSettings& settings)
{
  const auto& bounds = sourceBrush.bounds();
  const auto side = terrainSideInfo(settings.side);
  const auto uMin = bounds.min[side.uAxis];
  const auto uMax = bounds.max[side.uAxis];
  const auto vMin = bounds.min[side.vAxis];
  const auto vMax = bounds.max[side.vAxis];
  auto normalMin = bounds.min[side.normalAxis];
  auto normalMax = bounds.max[side.normalAxis];
  if (normalMax <= normalMin)
  {
    normalMax = normalMin + 1.0;
  }

  if (uMax <= uMin || vMax <= vMin)
  {
    return {};
  }

  const auto segmentsX = std::max(1, settings.segmentsX);
  const auto segmentsY = std::max(1, settings.segmentsY);
  const auto stepU = (uMax - uMin) / double(segmentsX);
  const auto stepV = (vMax - vMin) / double(segmentsY);
  const auto heightScale = std::max(0.0, settings.height) * settings.intensity;
  const auto baseNormal = side.normalSign > 0.0 ? normalMin : normalMax;
  const auto terrainSurfaceNormal = side.normalSign > 0.0 ? normalMax : normalMin;

  auto heights = std::vector<double>(
    static_cast<size_t>(segmentsX + 1) * static_cast<size_t>(segmentsY + 1),
    terrainSurfaceNormal);
  const auto heightIndex = [&](const int x, const int y) {
    return static_cast<size_t>(y) * static_cast<size_t>(segmentsX + 1)
           + static_cast<size_t>(x);
  };
  const auto terrainNormal = [&](const double noise) {
    return terrainSurfaceNormal + side.normalSign * noise * heightScale;
  };

  for (auto y = 0; y <= segmentsY; ++y)
  {
    for (auto x = 0; x <= segmentsX; ++x)
    {
      const auto u = double(x) / double(segmentsX);
      const auto v = double(y) / double(segmentsY);
      const auto noise = voronoiNoise(u, v, settings);
      heights[heightIndex(x, y)] = terrainNormal(noise);
    }
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  auto result = std::vector<mdl::BrushNode*>{};
  result.reserve(
    static_cast<size_t>(segmentsX) * static_cast<size_t>(segmentsY)
    * (settings.triangularGrid ? 2u : 1u));

  const auto materialName = map.currentMaterialName();
  const auto makeVertex = [&](const int x, const int y, const double normal) {
    auto result = vm::vec3d{};
    result[side.uAxis] = uMin + stepU * double(x);
    result[side.vAxis] = vMin + stepV * double(y);
    result[side.normalAxis] = normal;
    return result;
  };

  for (auto y = 0; y < segmentsY; ++y)
  {
    for (auto x = 0; x < segmentsX; ++x)
    {
      const auto bottom00 = makeVertex(x, y, baseNormal);
      const auto bottom10 = makeVertex(x + 1, y, baseNormal);
      const auto bottom01 = makeVertex(x, y + 1, baseNormal);
      const auto bottom11 = makeVertex(x + 1, y + 1, baseNormal);

      if (!settings.triangularGrid)
      {
        // Square mode keeps brush faces planar by using one flat-topped brush per cell.
        const auto u = (double(x) + 0.5) / double(segmentsX);
        const auto v = (double(y) + 0.5) / double(segmentsY);
        const auto cellTopNormal = terrainNormal(voronoiNoise(u, v, settings));
        const auto squareBrush = createQuadPrismBrush(
          builder,
          sourceBrush,
          materialName,
          {bottom00, bottom10, bottom11, bottom01},
          {
            makeVertex(x, y, cellTopNormal),
            makeVertex(x + 1, y, cellTopNormal),
            makeVertex(x + 1, y + 1, cellTopNormal),
            makeVertex(x, y + 1, cellTopNormal),
          });
        if (squareBrush)
        {
          result.push_back(new mdl::BrushNode{std::move(*squareBrush)});
        }
        continue;
      }

      const auto top00 = makeVertex(x, y, heights[heightIndex(x, y)]);
      const auto top10 = makeVertex(x + 1, y, heights[heightIndex(x + 1, y)]);
      const auto top01 = makeVertex(x, y + 1, heights[heightIndex(x, y + 1)]);
      const auto top11 = makeVertex(x + 1, y + 1, heights[heightIndex(x + 1, y + 1)]);

      const auto diagonalForward = ((x + y) % 2) == 0;
      const auto firstBrush = diagonalForward ? createTrianglePrismBrush(
                                                  builder,
                                                  sourceBrush,
                                                  materialName,
                                                  {bottom00, bottom10, bottom11},
                                                  {top00, top10, top11})
                                              : createTrianglePrismBrush(
                                                  builder,
                                                  sourceBrush,
                                                  materialName,
                                                  {bottom00, bottom10, bottom01},
                                                  {top00, top10, top01});
      const auto secondBrush = diagonalForward ? createTrianglePrismBrush(
                                                   builder,
                                                   sourceBrush,
                                                   materialName,
                                                   {bottom00, bottom11, bottom01},
                                                   {top00, top11, top01})
                                               : createTrianglePrismBrush(
                                                   builder,
                                                   sourceBrush,
                                                   materialName,
                                                   {bottom10, bottom11, bottom01},
                                                   {top10, top11, top01});

      if (firstBrush)
      {
        result.push_back(new mdl::BrushNode{std::move(*firstBrush)});
      }
      if (secondBrush)
      {
        result.push_back(new mdl::BrushNode{std::move(*secondBrush)});
      }
    }
  }

  return result;
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

TerrainGeneratorDialog::TerrainGeneratorDialog(MapDocument& document, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
{
  createGui();
}

TerrainGeneratorDialog::~TerrainGeneratorDialog()
{
  cancelPreview();
}

void TerrainGeneratorDialog::startPreview()
{
  cancelPreview();
  m_applied = false;
  captureSelection();
  resetSettings();
  beginPreview();
}

void TerrainGeneratorDialog::createGui()
{
  const auto firstBrushHeight = m_sourceBrushes.empty()
                                  ? 128.0
                                  : std::max(
                                      32.0,
                                      m_sourceBrushes.front().brush.bounds().max.z()
                                        - m_sourceBrushes.front().brush.bounds().min.z());

  m_horizontalSegments = new QSpinBox{};
  m_horizontalSegments->setRange(1, 64);
  m_horizontalSegments->setValue(10);

  m_verticalSegments = new QSpinBox{};
  m_verticalSegments->setRange(1, 64);
  m_verticalSegments->setValue(10);

  m_height = new QDoubleSpinBox{};
  m_height->setRange(0.0, 4096.0);
  m_height->setDecimals(1);
  m_height->setSingleStep(16.0);
  m_height->setValue(firstBrushHeight);

  m_intensity = new QDoubleSpinBox{};
  m_intensity->setRange(0.0, 4.0);
  m_intensity->setDecimals(2);
  m_intensity->setSingleStep(0.1);
  m_intensity->setValue(1.0);

  m_frequency = new QDoubleSpinBox{};
  m_frequency->setRange(0.1, 32.0);
  m_frequency->setDecimals(2);
  m_frequency->setSingleStep(0.25);
  m_frequency->setValue(4.0);

  m_roughness = new QDoubleSpinBox{};
  m_roughness->setRange(0.0, 2.0);
  m_roughness->setDecimals(2);
  m_roughness->setSingleStep(0.1);
  m_roughness->setValue(1.0);

  m_seed = new QSpinBox{};
  m_seed->setRange(0, 999999);
  m_seed->setValue(1337);

  m_side = new QComboBox{};
  m_side->addItem(tr("Top (+Z)"), static_cast<int>(TerrainSide::PosZ));
  m_side->addItem(tr("Bottom (-Z)"), static_cast<int>(TerrainSide::NegZ));
  m_side->addItem(tr("Right (+X)"), static_cast<int>(TerrainSide::PosX));
  m_side->addItem(tr("Left (-X)"), static_cast<int>(TerrainSide::NegX));
  m_side->addItem(tr("Front (+Y)"), static_cast<int>(TerrainSide::PosY));
  m_side->addItem(tr("Back (-Y)"), static_cast<int>(TerrainSide::NegY));

  m_triangularGrid = new QCheckBox{tr("Triangular grid")};
  m_triangularGrid->setChecked(true);
  m_createGroup = new QCheckBox{tr("Create group")};

  auto* layout = new QGridLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  const auto addControl =
    [&](const int row, const int column, const QString& name, QWidget* control) {
      layout->addWidget(new QLabel{name}, row, column * 2);
      layout->addWidget(control, row, column * 2 + 1);
    };
  addControl(0, 0, tr("Terrain U"), m_horizontalSegments);
  addControl(0, 1, tr("V"), m_verticalSegments);
  addControl(0, 2, tr("Height"), m_height);
  addControl(0, 3, tr("Intensity"), m_intensity);
  addControl(0, 4, tr("Side"), m_side);
  addControl(1, 0, tr("Frequency"), m_frequency);
  addControl(1, 1, tr("Roughness"), m_roughness);
  addControl(1, 2, tr("Seed"), m_seed);
  layout->addWidget(m_triangularGrid, 1, 6, 1, 2);
  layout->addWidget(m_createGroup, 1, 8, 1, 2);
  auto* defaults = new QPushButton{tr("Default")};
  auto* applyButton = new QPushButton{tr("Apply")};
  auto* cancel = new QPushButton{tr("Cancel")};
  layout->addWidget(defaults, 0, 10);
  layout->addWidget(applyButton, 0, 11);
  layout->addWidget(cancel, 1, 11);
  layout->setColumnStretch(12, 1);
  setLayout(layout);
  for (auto* spin : findChildren<QAbstractSpinBox*>())
  {
    spin->setKeyboardTracking(false);
  }

  connect(
    m_horizontalSegments, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
      updatePreview();
    });
  connect(m_verticalSegments, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
    updatePreview();
  });
  connect(m_height, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
    updatePreview();
  });
  connect(
    m_intensity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
      updatePreview();
    });
  connect(
    m_frequency, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
      updatePreview();
    });
  connect(
    m_roughness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
      updatePreview();
    });
  connect(m_seed, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
    updatePreview();
  });
  connect(m_side, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    updatePreview();
  });
  connect(m_triangularGrid, &QCheckBox::toggled, this, [this] { updatePreview(); });
  connect(m_createGroup, &QCheckBox::toggled, this, [this] { updatePreview(); });
  connect(applyButton, &QPushButton::clicked, this, [this] { apply(); });
  connect(defaults, &QPushButton::clicked, this, &TerrainGeneratorDialog::resetSettings);
  connect(cancel, &QPushButton::clicked, this, [this] { emit finished(); });
}

void TerrainGeneratorDialog::resetSettings()
{
  m_updatingPreview = true;
  m_horizontalSegments->setValue(10);
  m_verticalSegments->setValue(10);
  m_height->setValue(
    m_sourceBrushes.empty()
      ? 128.0
      : std::max(32.0, m_sourceBrushes.front().brush.bounds().size().z()));
  m_intensity->setValue(1.0);
  m_frequency->setValue(4.0);
  m_roughness->setValue(1.0);
  m_seed->setValue(1337);
  m_side->setCurrentIndex(0);
  m_triangularGrid->setChecked(true);
  m_createGroup->setChecked(false);
  m_updatingPreview = false;
  updatePreview();
}

void TerrainGeneratorDialog::captureSelection()
{
  m_sourceBrushes.clear();
  const auto& selection = m_document.map().selection();
  m_sourceBrushes.reserve(selection.brushes.size());
  for (auto* brushNode : selection.brushes)
  {
    m_sourceBrushes.push_back({brushNode, brushNode->parent(), brushNode->brush()});
  }
}

void TerrainGeneratorDialog::beginPreview()
{
  if (m_sourceBrushes.empty())
  {
    return;
  }

  m_document.map().startTransaction(
    "Generate Terrain", mdl::TransactionScope::LongRunning);
  m_transactionActive = true;
  updatePreview();
}

void TerrainGeneratorDialog::updatePreview()
{
  if (!m_transactionActive || m_updatingPreview)
  {
    return;
  }

  m_updatingPreview = true;

  auto& map = m_document.map();
  map.rollbackTransaction();

  const auto settings = TerrainSettings{
    m_horizontalSegments->value(),
    m_verticalSegments->value(),
    m_height->value(),
    m_intensity->value(),
    m_frequency->value(),
    m_roughness->value(),
    m_seed->value(),
    static_cast<TerrainSide>(m_side->currentData().toInt()),
    m_triangularGrid->isChecked(),
    m_createGroup->isChecked()};

  auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto originalNodes = std::vector<mdl::Node*>{};
  originalNodes.reserve(m_sourceBrushes.size());
  auto usedGroupNames = std::set<std::string>{};
  collectGroupNames(map.worldNode(), usedGroupNames);

  for (const auto& sourceBrush : m_sourceBrushes)
  {
    auto terrainNodes = createTerrainNodes(map, sourceBrush.brush, settings);
    if (terrainNodes.empty())
    {
      deleteNodes(nodesToAdd);
      m_updatingPreview = false;
      return;
    }

    if (settings.createGroup)
    {
      auto* groupNode =
        new mdl::GroupNode{mdl::Group{nextGeneratedGroupName(usedGroupNames, "Terrain")}};
      for (auto* terrainNode : terrainNodes)
      {
        groupNode->addChild(terrainNode);
      }
      nodesToAdd[sourceBrush.parent].push_back(groupNode);
    }
    else
    {
      auto& parentNodes = nodesToAdd[sourceBrush.parent];
      for (auto* terrainNode : terrainNodes)
      {
        parentNodes.push_back(terrainNode);
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

void TerrainGeneratorDialog::apply()
{
  if (m_transactionActive)
  {
    m_applied = true;
    m_document.map().commitTransaction();
    m_transactionActive = false;
  }
  emit finished();
}

void TerrainGeneratorDialog::cancelPreview()
{
  if (m_transactionActive && !m_applied)
  {
    m_document.map().cancelTransaction();
    m_transactionActive = false;
  }
}

} // namespace tb::ui
