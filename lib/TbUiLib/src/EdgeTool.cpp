/*
 Copyright (C) 2010 Kristian Duske

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

#include "ui/EdgeTool.h"

#include <QCheckBox>
#include <QComboBox>
#include <QVBoxLayout>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include "Logger.h"
#include "NotifierConnection.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushGeometry.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/NodeHandles.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeContents.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"
#include "ui/ViewConstants.h"

#include "kd/contracts.h"
#include "kd/map_utils.h"
#include "kd/result.h"
#include "kd/string_format.h"

#include "vm/scalar.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace tb::ui
{
namespace
{
constexpr auto MaxChamferSegments = size_t{16};
constexpr auto GeometryEpsilon = 0.001;
constexpr auto AreaEpsilon = 0.0001;
constexpr auto MaxOrthogonalInwardChamferFragments = size_t{12};
constexpr auto DefaultInwardChamferCurve = -0.55;
constexpr auto MinInwardChamferCurve = -1.0;
constexpr auto MaxInwardChamferCurve = -0.05;
constexpr auto MinOutwardChamferCurve = 0.05;
constexpr auto MaxOutwardChamferCurve = 1.0;
constexpr auto MaxChamferProportion = 0.95;

class EdgeBevelToolPage : public QWidget
{
  EdgeTool& m_tool;
  NotifierConnection m_connection;
  QComboBox* m_mode;
  QComboBox* m_sizeType;
  QDoubleSpinBox* m_size;
  QDoubleSpinBox* m_profile;
  QSpinBox* m_segments;
  QCheckBox* m_clamp;
  QLabel* m_status;
  QPushButton* m_apply;

public:
  EdgeBevelToolPage(EdgeTool& tool, QWidget* parent)
    : QWidget{parent}
    , m_tool{tool}
  {
    auto* layout = new QVBoxLayout{this};
    auto* row = new QHBoxLayout{};
    layout->addLayout(row);
    row->addWidget(new QLabel{tr("Bevel")});
    m_mode = new QComboBox{};
    m_mode->setObjectName("BevelMode");
    m_mode->addItems({tr("Edges"), tr("Face boundary"), tr("Vertices")});
    m_mode->setToolTip(tr(
      "Select faces or vertices before opening Bevel. Edges can also be selected before "
      "preview."));
    row->addWidget(m_mode);
    m_sizeType = new QComboBox{};
    m_sizeType->addItems({tr("Radius"), tr("Offset")});
    row->addWidget(m_sizeType);
    m_size = new QDoubleSpinBox{};
    m_size->setObjectName("BevelSize");
    m_size->setRange(0, 131072);
    m_size->setDecimals(3);
    m_size->setKeyboardTracking(false);
    m_size->setToolTip(tr("Bevel width"));
    row->addWidget(m_size);
    row->addWidget(new QLabel{tr("Segments")});
    m_segments = new QSpinBox{};
    m_segments->setRange(1, 16);
    m_segments->setKeyboardTracking(false);
    row->addWidget(m_segments);
    row->addWidget(new QLabel{tr("Profile")});
    m_profile = new QDoubleSpinBox{};
    m_profile->setObjectName("BevelProfile");
    m_profile->setRange(0, 0.99);
    m_profile->setDecimals(3);
    m_profile->setSingleStep(0.05);
    m_profile->setKeyboardTracking(false);
    m_profile->setToolTip(tr(
      "Convex profile: 0 = flat chamfer, 0.5 = round, toward 1 = square. Radius refers "
      "to the round reference profile."));
    row->addWidget(m_profile);
    m_clamp = new QCheckBox{tr("Clamp per brush")};
    row->addWidget(m_clamp);
    row->addStretch();
    auto* second = new QHBoxLayout{};
    layout->addLayout(second);
    m_status = new QLabel{};
    m_status->setWordWrap(true);
    second->addWidget(m_status, 1);
    m_apply = new QPushButton{tr("Apply")};
    second->addWidget(m_apply);
    auto* cancel = new QPushButton{tr("Cancel")};
    second->addWidget(cancel);
    connect(
      m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [&](int i) {
        m_tool.setBevelMode(i);
      });
    connect(
      m_sizeType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [&](int i) {
        m_tool.setBevelRadius(i == 0);
      });
    connect(
      m_size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [&](double v) {
        m_tool.setBevelWidth(v);
      });
    connect(
      m_profile,
      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      this,
      [&](double v) { m_tool.setBevelCurve(v); });
    connect(m_segments, QOverload<int>::of(&QSpinBox::valueChanged), this, [&](int v) {
      m_tool.setBevelSegments(size_t(v));
    });
    connect(m_clamp, &QCheckBox::toggled, this, [&](bool v) { m_tool.setBevelClamp(v); });
    connect(m_apply, &QPushButton::clicked, this, [&] { m_tool.applyBevelTool(); });
    connect(cancel, &QPushButton::clicked, this, [&] { m_tool.cancelBevelTool(); });
    m_connection +=
      tool.bevelSettingsDidChangeNotifier.connect([&](auto&) { updateControls(); });
    updateControls();
  }
  void updateControls()
  {
    const auto a = QSignalBlocker{m_mode};
    const auto b = QSignalBlocker{m_sizeType};
    const auto c = QSignalBlocker{m_size};
    const auto d = QSignalBlocker{m_profile};
    const auto e = QSignalBlocker{m_segments};
    const auto f = QSignalBlocker{m_clamp};
    m_mode->setCurrentIndex(m_tool.bevelMode());
    m_sizeType->setCurrentIndex(m_tool.bevelRadius() ? 0 : 1);
    m_size->setValue(m_tool.bevelWidth());
    m_profile->setValue(m_tool.bevelCurve());
    m_segments->setValue(int(m_tool.bevelSegments()));
    m_clamp->setChecked(m_tool.bevelClamp());
    m_status->setText(QString::fromStdString(m_tool.bevelStatus()));
    m_apply->setEnabled(m_tool.bevelPreviewValid());
    setVisible(m_tool.bevelToolActive());
  }
};

std::optional<vm::vec3d> normalized(const vm::vec3d& value)
{
  if (vm::squared_length(value) <= vm::Cd::almost_zero())
  {
    return std::nullopt;
  }
  return vm::normalize(value);
}

vm::vec2d projectToCutPlane(const vm::vec3d& point, const vm::vec3d& normal)
{
  switch (vm::find_abs_max_component(normal))
  {
  case vm::axis::x:
    return {point.y(), point.z()};
  case vm::axis::y:
    return {point.x(), point.z()};
  case vm::axis::z:
    return {point.x(), point.y()};
  }

  return {point.x(), point.y()};
}

vm::vec3d unprojectFromCutPlane(const vm::vec2d& point, const vm::plane3d& plane)
{
  const auto& normal = plane.normal;
  switch (vm::find_abs_max_component(normal))
  {
  case vm::axis::x: {
    const auto x =
      (plane.distance - normal.y() * point.x() - normal.z() * point.y()) / normal.x();
    return {x, point.x(), point.y()};
  }
  case vm::axis::y: {
    const auto y =
      (plane.distance - normal.x() * point.x() - normal.z() * point.y()) / normal.y();
    return {point.x(), y, point.y()};
  }
  case vm::axis::z: {
    const auto z =
      (plane.distance - normal.x() * point.x() - normal.y() * point.y()) / normal.z();
    return {point.x(), point.y(), z};
  }
  }

  return {point.x(), point.y(), 0.0};
}

double cross2D(const vm::vec2d& a, const vm::vec2d& b, const vm::vec2d& c)
{
  const auto ab = b - a;
  const auto ac = c - a;
  return ab.x() * ac.y() - ab.y() * ac.x();
}

double signedArea2D(const std::vector<vm::vec2d>& points)
{
  auto area = 0.0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    const auto& a = points[i];
    const auto& b = points[(i + 1u) % points.size()];
    area += a.x() * b.y() - b.x() * a.y();
  }
  return area / 2.0;
}

bool nearEqual(const double lhs, const double rhs)
{
  return std::abs(lhs - rhs) <= GeometryEpsilon;
}

bool nearEqual(const vm::vec2d& lhs, const vm::vec2d& rhs)
{
  return nearEqual(lhs.x(), rhs.x()) && nearEqual(lhs.y(), rhs.y());
}

std::vector<vm::vec2d> cleanupPolygon(std::vector<vm::vec2d> polygon)
{
  auto result = std::vector<vm::vec2d>{};
  result.reserve(polygon.size());
  for (const auto& point : polygon)
  {
    if (result.empty() || !nearEqual(result.back(), point))
    {
      result.push_back(point);
    }
  }

  if (result.size() > 1u && nearEqual(result.front(), result.back()))
  {
    result.pop_back();
  }

  auto removedColinearPoint = true;
  while (removedColinearPoint && result.size() > 3u)
  {
    removedColinearPoint = false;
    for (size_t i = 0; i < result.size(); ++i)
    {
      const auto& prev = result[(i + result.size() - 1u) % result.size()];
      const auto& cur = result[i];
      const auto& next = result[(i + 1u) % result.size()];
      if (std::abs(cross2D(prev, cur, next)) <= AreaEpsilon)
      {
        result.erase(std::next(std::begin(result), static_cast<std::ptrdiff_t>(i)));
        removedColinearPoint = true;
        break;
      }
    }
  }

  return result;
}

bool isConvexPolygon2D(const std::vector<vm::vec2d>& polygon)
{
  if (polygon.size() < 3u || std::abs(signedArea2D(polygon)) <= AreaEpsilon)
  {
    return false;
  }

  const auto positive = signedArea2D(polygon) > 0.0;
  for (size_t i = 0u; i < polygon.size(); ++i)
  {
    const auto cross = cross2D(
      polygon[i], polygon[(i + 1u) % polygon.size()], polygon[(i + 2u) % polygon.size()]);
    if (positive && cross < -AreaEpsilon)
    {
      return false;
    }
    if (!positive && cross > AreaEpsilon)
    {
      return false;
    }
  }

  return true;
}

void addUniqueCoordinate(std::vector<double>& coordinates, const double coordinate)
{
  if (std::ranges::none_of(coordinates, [&](const auto existing) {
        return nearEqual(existing, coordinate);
      }))
  {
    coordinates.push_back(coordinate);
  }
}

std::optional<std::array<double, 2>> convexPolygonYRangeAtX(
  const std::vector<vm::vec2d>& polygon, const double x)
{
  auto intersections = std::vector<double>{};
  for (size_t i = 0u; i < polygon.size(); ++i)
  {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1u) % polygon.size()];
    const auto minX = std::min(a.x(), b.x());
    const auto maxX = std::max(a.x(), b.x());

    if (nearEqual(a.x(), b.x()))
    {
      if (nearEqual(x, a.x()))
      {
        intersections.push_back(a.y());
        intersections.push_back(b.y());
      }
      continue;
    }

    if (x < minX - GeometryEpsilon || x > maxX + GeometryEpsilon)
    {
      continue;
    }

    const auto t = (x - a.x()) / (b.x() - a.x());
    if (t >= -GeometryEpsilon && t <= 1.0 + GeometryEpsilon)
    {
      intersections.push_back(a.y() + (b.y() - a.y()) * std::clamp(t, 0.0, 1.0));
    }
  }

  if (intersections.empty())
  {
    return std::nullopt;
  }

  std::ranges::sort(intersections);
  auto unique = std::vector<double>{};
  for (const auto y : intersections)
  {
    if (unique.empty() || !nearEqual(unique.back(), y))
    {
      unique.push_back(y);
    }
  }

  return std::array<double, 2>{unique.front(), unique.back()};
}

std::optional<mdl::BrushFace> createFaceForPlane(
  const vm::plane3d& plane,
  const mdl::BrushFaceAttributes& attributes,
  const mdl::MapFormat mapFormat)
{
  const auto normal = vm::normalize(plane.normal);
  const auto anchor = normal * plane.distance;
  const auto helper =
    std::abs(normal.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{0, 1, 0};
  const auto u = normalized(vm::cross(helper, normal));
  if (!u)
  {
    return std::nullopt;
  }
  const auto v = vm::cross(normal, *u);
  constexpr auto Size = 64.0;

  auto result = mdl::BrushFace::create(
    anchor, anchor + v * Size, anchor + *u * Size, attributes, mapFormat);
  if (!result)
  {
    return std::nullopt;
  }
  return std::move(result).value();
}

const mdl::BrushEdge* findEdge(const mdl::Brush& brush, const vm::segment3d& edgePosition)
{
  for (const auto* edge : brush.edges())
  {
    if (edge->hasPositions(edgePosition.start(), edgePosition.end(), GeometryEpsilon))
    {
      return edge;
    }
  }
  return nullptr;
}

std::optional<vm::vec3d> faceNormalInEdgePlane(
  const mdl::BrushFace& face, const vm::vec3d& edgeDir)
{
  auto normal = face.normal();
  normal = normal - edgeDir * vm::dot(normal, edgeDir);
  return normalized(normal);
}

std::optional<vm::vec3d> inwardFaceTangent(
  const vm::vec3d& edgeDir, const vm::vec3d& faceNormal, const vm::vec3d& otherFaceNormal)
{
  auto tangent = normalized(vm::cross(edgeDir, faceNormal));
  if (!tangent)
  {
    return std::nullopt;
  }

  if (vm::dot(*tangent, otherFaceNormal) > 0.0)
  {
    tangent = -*tangent;
  }
  return tangent;
}

double faceExtentAlong(
  const mdl::BrushFace& face, const vm::segment3d& edge, const vm::vec3d& direction)
{
  auto result = 0.0;
  for (const auto* vertex : face.vertices())
  {
    result = std::max(
      result,
      std::max(
        vm::dot(vertex->position() - edge.start(), direction),
        vm::dot(vertex->position() - edge.end(), direction)));
  }
  return result;
}

struct ChamferWidthRange
{
  double initial = 0.0;
  double min = 0.0;
  double max = 0.0;
  double step = 1.0;
};

std::optional<ChamferWidthRange> chamferWidthRange(
  const mdl::BrushFace& faceA,
  const mdl::BrushFace& faceB,
  const vm::segment3d& edge,
  const vm::vec3d& tangentA,
  const vm::vec3d& tangentB,
  const double gridSize)
{
  const auto nextFaceDistance = std::min(
    faceExtentAlong(faceA, edge, tangentA), faceExtentAlong(faceB, edge, tangentB));
  const auto maxWidth = nextFaceDistance * 0.98;

  if (maxWidth <= GeometryEpsilon)
  {
    return std::nullopt;
  }

  const auto step = std::max(1.0, gridSize);
  const auto minWidth = std::min(maxWidth, std::max(GeometryEpsilon * 2.0, step * 0.25));
  const auto initial = std::clamp(step, minWidth, maxWidth);
  return ChamferWidthRange{initial, minWidth, maxWidth, step};
}

std::optional<std::pair<vm::vec3d, double>> arcCenterAndRadius(
  const vm::vec3d& endpointA,
  const vm::vec3d& endpointB,
  const vm::vec3d& normalA,
  const vm::vec3d& normalB)
{
  const auto c = std::clamp(vm::dot(normalA, normalB), -0.999999, 0.999999);
  const auto denominator = c * c - 1.0;
  if (std::abs(denominator) <= vm::Cd::almost_zero())
  {
    return std::nullopt;
  }

  const auto delta = endpointA - endpointB;
  const auto rhsA = vm::dot(delta, normalA);
  const auto rhsB = vm::dot(delta, normalB);
  const auto radiusB = (rhsB - c * rhsA) / denominator;
  const auto radiusA = rhsA + c * radiusB;

  if (radiusA <= GeometryEpsilon || radiusB <= GeometryEpsilon)
  {
    return std::nullopt;
  }

  const auto centerA = endpointA - normalA * radiusA;
  const auto centerB = endpointB - normalB * radiusB;
  return std::pair{(centerA + centerB) / 2.0, (radiusA + radiusB) / 2.0};
}

vm::vec3d interpolateNormal(
  const vm::vec3d& normalA, const vm::vec3d& normalB, const double t)
{
  const auto c = std::clamp(vm::dot(normalA, normalB), -0.999999, 0.999999);
  const auto angle = std::acos(c);
  if (angle <= vm::Cd::almost_zero())
  {
    return vm::normalize(normalA * (1.0 - t) + normalB * t);
  }

  const auto sinAngle = std::sin(angle);
  return vm::normalize(
    normalA * (std::sin((1.0 - t) * angle) / sinAngle)
    + normalB * (std::sin(t * angle) / sinAngle));
}

std::vector<vm::vec3d> chamferProfile(
  const vm::vec3d& edgePoint,
  const vm::vec3d& normalA,
  const vm::vec3d& normalB,
  const vm::vec3d& tangentA,
  const vm::vec3d& tangentB,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion)
{
  const auto clampedProportion =
    std::clamp(proportion, -MaxChamferProportion, MaxChamferProportion);
  const auto widthA = width * (1.0 - clampedProportion);
  const auto widthB = width * (1.0 + clampedProportion);
  const auto endpointA = edgePoint + tangentA * widthA;
  const auto endpointB = edgePoint + tangentB * widthB;
  auto result = std::vector<vm::vec3d>{};
  result.reserve(segments + 1u);

  if (curve >= 0.999)
  {
    const auto centerAndRadius =
      arcCenterAndRadius(endpointA, endpointB, normalA, normalB);

    for (size_t i = 0u; i <= segments; ++i)
    {
      const auto t = double(i) / double(segments);
      if (centerAndRadius)
      {
        const auto& [center, radius] = *centerAndRadius;
        result.push_back(center + interpolateNormal(normalA, normalB, t) * radius);
      }
      else
      {
        const auto point = endpointA + (endpointB - endpointA) * t;
        auto bulge = normalized(normalA + normalB);
        if (!bulge)
        {
          bulge = normalized(edgePoint - point);
        }
        if (!bulge)
        {
          bulge = normalA;
        }
        result.push_back(point + *bulge * (std::sin(vm::Cd::pi() * t) * width * 0.25));
      }
    }
    return result;
  }

  const auto midpoint = (endpointA + endpointB) / 2.0;
  auto bulge = normalized(edgePoint - midpoint);
  if (!bulge)
  {
    bulge = normalized(normalA + normalB);
  }
  if (!bulge)
  {
    bulge = normalA;
  }

  const auto clampedCurve = std::clamp(curve, -1.0, 1.0);
  for (size_t i = 0u; i <= segments; ++i)
  {
    const auto t = double(i) / double(segments);
    const auto point = endpointA + (endpointB - endpointA) * t;
    result.push_back(
      point + *bulge * (std::sin(vm::Cd::pi() * t) * width * 0.5 * clampedCurve));
  }
  return result;
}

std::optional<std::vector<mdl::BrushFace>> createChamferFacesForEdge(
  const mdl::Brush& originalBrush,
  const vm::segment3d& edgePosition,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  const mdl::Map& map)
{
  if (segments == 0u)
  {
    return std::vector<mdl::BrushFace>{};
  }

  const auto* edge = findEdge(originalBrush, edgePosition);
  if (!edge || !edge->fullySpecified())
  {
    return std::nullopt;
  }

  const auto faceIndexA = edge->firstFace()->payload();
  const auto faceIndexB = edge->secondFace()->payload();
  if (!faceIndexA || !faceIndexB)
  {
    return std::nullopt;
  }

  const auto& faceA = originalBrush.face(*faceIndexA);
  const auto& faceB = originalBrush.face(*faceIndexB);
  const auto edgeDir = normalized(edgePosition.end() - edgePosition.start());
  if (!edgeDir)
  {
    return std::nullopt;
  }

  const auto normalA = faceNormalInEdgePlane(faceA, *edgeDir);
  const auto normalB = faceNormalInEdgePlane(faceB, *edgeDir);
  if (!normalA || !normalB)
  {
    return std::nullopt;
  }

  const auto tangentA = inwardFaceTangent(*edgeDir, *normalA, *normalB);
  const auto tangentB = inwardFaceTangent(*edgeDir, *normalB, *normalA);
  if (!tangentA || !tangentB)
  {
    return std::nullopt;
  }

  auto result = std::vector<mdl::BrushFace>{};
  result.reserve(segments);

  const auto profile = chamferProfile(
    edgePosition.start(),
    *normalA,
    *normalB,
    *tangentA,
    *tangentB,
    width,
    segments,
    curve,
    proportion);

  for (size_t i = 0u; i < segments; ++i)
  {
    const auto& a = profile[i];
    const auto& b = profile[i + 1u];
    const auto chord = b - a;
    auto normal = normalized(vm::cross(chord, *edgeDir));
    if (!normal)
    {
      return std::nullopt;
    }

    const auto midpoint = (a + b) / 2.0;
    if (vm::dot(*normal, edgePosition.start() - midpoint) < 0.0)
    {
      normal = -*normal;
    }

    const auto plane = vm::plane3d{vm::dot(*normal, a), *normal};
    auto face =
      createFaceForPlane(plane, faceA.attributes(), map.worldNode().mapFormat());
    if (!face)
    {
      return std::nullopt;
    }

    result.push_back(std::move(*face));
  }

  return result;
}

std::optional<mdl::Brush> createChamferedBrush(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  const mdl::Map& map)
{
  if (segments == 0u)
  {
    return originalBrush;
  }

  auto brush = originalBrush;
  for (const auto& edgePosition : edgePositions)
  {
    auto faces = createChamferFacesForEdge(
      originalBrush, edgePosition, width, segments, curve, proportion, map);
    if (!faces)
    {
      return std::nullopt;
    }

    for (auto& face : *faces)
    {
      const auto result = brush.clip(map.worldBounds(), std::move(face));
      if (!result)
      {
        return std::nullopt;
      }
    }
  }

  brush.cloneFaceAttributesFrom(originalBrush);
  return brush;
}

struct ChamferCutterGeometry
{
  vm::vec3d edgeDir;
  std::vector<vm::vec3d> section;
};

std::optional<ChamferCutterGeometry> createChamferCutterGeometry(
  const mdl::Brush& originalBrush,
  const vm::segment3d& edgePosition,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion)
{
  if (segments == 0u)
  {
    return std::nullopt;
  }

  const auto* edge = findEdge(originalBrush, edgePosition);
  if (!edge || !edge->fullySpecified())
  {
    return std::nullopt;
  }

  const auto faceIndexA = edge->firstFace()->payload();
  const auto faceIndexB = edge->secondFace()->payload();
  if (!faceIndexA || !faceIndexB)
  {
    return std::nullopt;
  }

  const auto& faceA = originalBrush.face(*faceIndexA);
  const auto& faceB = originalBrush.face(*faceIndexB);
  const auto edgeVector = edgePosition.end() - edgePosition.start();
  const auto edgeDir = normalized(edgeVector);
  if (!edgeDir)
  {
    return std::nullopt;
  }

  const auto normalA = faceNormalInEdgePlane(faceA, *edgeDir);
  const auto normalB = faceNormalInEdgePlane(faceB, *edgeDir);
  if (!normalA || !normalB)
  {
    return std::nullopt;
  }

  const auto tangentA = inwardFaceTangent(*edgeDir, *normalA, *normalB);
  const auto tangentB = inwardFaceTangent(*edgeDir, *normalB, *normalA);
  if (!tangentA || !tangentB)
  {
    return std::nullopt;
  }

  auto section = std::vector<vm::vec3d>{};
  section.reserve(segments + 2u);
  section.push_back(edgePosition.start());
  const auto profile = chamferProfile(
    edgePosition.start(),
    *normalA,
    *normalB,
    *tangentA,
    *tangentB,
    width,
    segments,
    curve,
    proportion);
  section.insert(std::end(section), std::begin(profile), std::end(profile));

  return ChamferCutterGeometry{*edgeDir, std::move(section)};
}

std::optional<mdl::Brush> createExtrudedSectionBrush(
  const std::vector<vm::vec3d>& section,
  const vm::segment3d& edgePosition,
  const vm::vec3d& edgeDir,
  const double width,
  const mdl::Map& map)
{
  if (section.size() < 3u)
  {
    return std::nullopt;
  }

  const auto margin = std::max(64.0, width * 4.0);
  const auto start = edgePosition.start() - edgeDir * margin;
  const auto end = edgePosition.end() + edgeDir * margin;

  auto points = std::vector<vm::vec3d>{};
  points.reserve(section.size() * 2u);
  for (const auto& point : section)
  {
    const auto offset = point - edgePosition.start();
    points.push_back(start + offset);
    points.push_back(end + offset);
  }

  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};
  auto brush = builder.createBrush(points, map.currentMaterialName());
  if (!brush)
  {
    return std::nullopt;
  }

  return std::move(brush).value();
}

std::optional<mdl::Brush> createChamferCutterBrush(
  const mdl::Brush& originalBrush,
  const vm::segment3d& edgePosition,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  const mdl::Map& map)
{
  const auto cutterGeometry = createChamferCutterGeometry(
    originalBrush, edgePosition, width, segments, curve, proportion);
  if (!cutterGeometry)
  {
    return std::nullopt;
  }

  return createExtrudedSectionBrush(
    cutterGeometry->section, edgePosition, cutterGeometry->edgeDir, width, map);
}

std::optional<std::vector<mdl::Brush>> createExactKeptChamferCutBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<const mdl::Brush*>& remainingBrushes,
  mdl::Map& map)
{
  if (remainingBrushes.empty())
  {
    return std::vector<mdl::Brush>{originalBrush};
  }

  auto result = std::vector<mdl::Brush>{};
  auto failed = false;
  for (auto& subtractionResult : originalBrush.subtract(
         map.worldNode().mapFormat(),
         map.worldBounds(),
         map.currentMaterialName(),
         remainingBrushes))
  {
    if (subtractionResult)
    {
      auto brush = std::move(subtractionResult).value();
      brush.cloneFaceAttributesFrom(originalBrush);
      result.push_back(std::move(brush));
    }
    else
    {
      failed = true;
    }
  }

  if (failed)
  {
    return std::nullopt;
  }

  return result;
}

std::optional<std::vector<mdl::Brush>> createSegmentedKeptChamferCutBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  mdl::Map& map)
{
  auto result = std::vector<mdl::Brush>{};
  for (const auto& edgePosition : edgePositions)
  {
    const auto cutterGeometry = createChamferCutterGeometry(
      originalBrush, edgePosition, width, segments, curve, proportion);
    if (!cutterGeometry || cutterGeometry->section.size() < 3u)
    {
      return std::nullopt;
    }

    for (size_t i = 1u; i + 1u < cutterGeometry->section.size(); ++i)
    {
      const auto section = std::vector<vm::vec3d>{
        cutterGeometry->section.front(),
        cutterGeometry->section[i],
        cutterGeometry->section[i + 1u]};

      const auto cutter = createExtrudedSectionBrush(
        section, edgePosition, cutterGeometry->edgeDir, width, map);
      if (!cutter)
      {
        return std::nullopt;
      }

      auto keptBrush = originalBrush;
      if (!keptBrush.intersect(map.worldBounds(), *cutter))
      {
        return std::nullopt;
      }

      keptBrush.cloneFaceAttributesFrom(originalBrush);
      result.push_back(std::move(keptBrush));
    }
  }

  return result;
}

std::optional<std::vector<mdl::Brush>> createQuadrangulatedKeptChamferCutBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  mdl::Map& map)
{
  auto result = std::vector<mdl::Brush>{};

  for (const auto& edgePosition : edgePositions)
  {
    const auto cutterGeometry = createChamferCutterGeometry(
      originalBrush, edgePosition, width, segments, curve, proportion);
    if (!cutterGeometry)
    {
      return std::nullopt;
    }

    auto section = std::vector<vm::vec2d>{};
    section.reserve(cutterGeometry->section.size());
    for (const auto& point : cutterGeometry->section)
    {
      section.push_back(projectToCutPlane(point, cutterGeometry->edgeDir));
    }
    section = cleanupPolygon(std::move(section));
    if (section.size() < 3u || !isConvexPolygon2D(section))
    {
      return std::nullopt;
    }
    if (signedArea2D(section) < 0.0)
    {
      std::ranges::reverse(section);
    }

    auto center = vm::vec2d{0.0, 0.0};
    for (const auto& point : section)
    {
      center = center + point;
    }
    center = center / static_cast<double>(section.size());

    const auto sectionPlane = vm::plane3d{
      vm::dot(cutterGeometry->edgeDir, edgePosition.start()), cutterGeometry->edgeDir};

    for (size_t i = 0u; i < section.size(); ++i)
    {
      const auto prev = (i + section.size() - 1u) % section.size();
      const auto next = (i + 1u) % section.size();
      const auto midPrev = (section[prev] + section[i]) / 2.0;
      const auto midNext = (section[i] + section[next]) / 2.0;
      auto cell2D = cleanupPolygon({section[i], midNext, center, midPrev});
      if (cell2D.size() != 4u || std::abs(signedArea2D(cell2D)) <= AreaEpsilon)
      {
        return std::nullopt;
      }
      if (!isConvexPolygon2D(cell2D))
      {
        return std::nullopt;
      }
      if (signedArea2D(cell2D) < 0.0)
      {
        std::ranges::reverse(cell2D);
      }

      auto cell3D = std::vector<vm::vec3d>{};
      cell3D.reserve(cell2D.size());
      for (const auto& point : cell2D)
      {
        cell3D.push_back(unprojectFromCutPlane(point, sectionPlane));
      }

      const auto cellBrush = createExtrudedSectionBrush(
        cell3D, edgePosition, cutterGeometry->edgeDir, width, map);
      if (!cellBrush)
      {
        return std::nullopt;
      }

      auto keptBrush = originalBrush;
      if (!keptBrush.intersect(map.worldBounds(), *cellBrush))
      {
        return std::nullopt;
      }

      keptBrush.cloneFaceAttributesFrom(originalBrush);
      result.push_back(std::move(keptBrush));
    }
  }

  if (result.empty())
  {
    return std::nullopt;
  }

  return result;
}

std::optional<std::vector<mdl::Brush>> createOrthogonalKeptChamferCutBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  mdl::Map& map)
{
  auto result = std::vector<mdl::Brush>{};
  for (const auto& edgePosition : edgePositions)
  {
    const auto cutterGeometry = createChamferCutterGeometry(
      originalBrush, edgePosition, width, segments, curve, proportion);
    if (!cutterGeometry)
    {
      return std::nullopt;
    }

    const auto edgeAxis = vm::find_abs_max_component(cutterGeometry->edgeDir);
    if (std::abs(cutterGeometry->edgeDir[edgeAxis]) < 0.99)
    {
      return std::nullopt;
    }

    auto cutter = std::vector<vm::vec2d>{};
    cutter.reserve(cutterGeometry->section.size());
    for (const auto& point : cutterGeometry->section)
    {
      cutter.push_back(projectToCutPlane(point, cutterGeometry->edgeDir));
    }
    cutter = cleanupPolygon(std::move(cutter));
    if (cutter.size() < 3u || !isConvexPolygon2D(cutter))
    {
      return std::nullopt;
    }
    if (signedArea2D(cutter) < 0.0)
    {
      std::ranges::reverse(cutter);
    }

    auto xCoordinates = std::vector<double>{};
    xCoordinates.reserve(cutter.size());
    for (const auto& point : cutter)
    {
      addUniqueCoordinate(xCoordinates, point.x());
    }
    std::ranges::sort(xCoordinates);
    if (xCoordinates.size() < 2u)
    {
      return std::nullopt;
    }

    const auto sectionPlane = vm::plane3d{
      vm::dot(cutterGeometry->edgeDir, edgePosition.start()), cutterGeometry->edgeDir};

    for (size_t i = 0u; i + 1u < xCoordinates.size(); ++i)
    {
      const auto x0 = xCoordinates[i];
      const auto x1 = xCoordinates[i + 1u];
      if (nearEqual(x0, x1))
      {
        continue;
      }

      const auto leftRange = convexPolygonYRangeAtX(cutter, x0);
      const auto rightRange = convexPolygonYRangeAtX(cutter, x1);
      if (!leftRange || !rightRange)
      {
        return std::nullopt;
      }

      auto cell2D = cleanupPolygon(
        {{x0, (*leftRange)[0]},
         {x1, (*rightRange)[0]},
         {x1, (*rightRange)[1]},
         {x0, (*leftRange)[1]}});
      if (cell2D.size() < 3u || std::abs(signedArea2D(cell2D)) <= AreaEpsilon)
      {
        continue;
      }
      if (!isConvexPolygon2D(cell2D))
      {
        return std::nullopt;
      }
      if (signedArea2D(cell2D) < 0.0)
      {
        std::ranges::reverse(cell2D);
      }

      auto cell3D = std::vector<vm::vec3d>{};
      cell3D.reserve(cell2D.size());
      for (const auto& point : cell2D)
      {
        cell3D.push_back(unprojectFromCutPlane(point, sectionPlane));
      }

      const auto cellBrush = createExtrudedSectionBrush(
        cell3D, edgePosition, cutterGeometry->edgeDir, width, map);
      if (!cellBrush)
      {
        return std::nullopt;
      }

      auto keptBrush = originalBrush;
      if (!keptBrush.intersect(map.worldBounds(), *cellBrush))
      {
        return std::nullopt;
      }

      keptBrush.cloneFaceAttributesFrom(originalBrush);
      result.push_back(std::move(keptBrush));
    }
  }

  if (result.empty())
  {
    return std::nullopt;
  }

  return result;
}

std::optional<std::vector<mdl::Brush>> createKeptChamferCutBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  const std::vector<const mdl::Brush*>& remainingBrushes,
  mdl::Map& map)
{
  auto keptBrushes = createQuadrangulatedKeptChamferCutBrushes(
    originalBrush, edgePositions, width, segments, curve, proportion, map);
  if (keptBrushes && !keptBrushes->empty())
  {
    return keptBrushes;
  }

  keptBrushes = createOrthogonalKeptChamferCutBrushes(
    originalBrush, edgePositions, width, segments, curve, proportion, map);
  if (keptBrushes && !keptBrushes->empty())
  {
    return keptBrushes;
  }

  keptBrushes = createSegmentedKeptChamferCutBrushes(
    originalBrush, edgePositions, width, segments, curve, proportion, map);
  if (keptBrushes && !keptBrushes->empty())
  {
    return keptBrushes;
  }

  return createExactKeptChamferCutBrushes(originalBrush, remainingBrushes, map);
}

std::optional<std::vector<mdl::Brush>> createOrthogonalInwardChamferedBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  mdl::Map& map)
{
  if (edgePositions.size() != 1u)
  {
    return std::nullopt;
  }

  const auto& edgePosition = edgePositions.front();
  const auto cutterGeometry = createChamferCutterGeometry(
    originalBrush, edgePosition, width, segments, curve, proportion);
  if (!cutterGeometry)
  {
    return std::nullopt;
  }

  const auto edgeAxis = vm::find_abs_max_component(cutterGeometry->edgeDir);
  if (std::abs(cutterGeometry->edgeDir[edgeAxis]) < 0.99)
  {
    return std::nullopt;
  }

  auto cutter = std::vector<vm::vec2d>{};
  cutter.reserve(cutterGeometry->section.size());
  for (const auto& point : cutterGeometry->section)
  {
    cutter.push_back(projectToCutPlane(point, cutterGeometry->edgeDir));
  }
  cutter = cleanupPolygon(std::move(cutter));
  if (cutter.size() < 3u || !isConvexPolygon2D(cutter))
  {
    return std::nullopt;
  }
  if (signedArea2D(cutter) < 0.0)
  {
    std::ranges::reverse(cutter);
  }

  const auto boundsVertices = originalBrush.bounds().vertices();
  auto minX = std::numeric_limits<double>::max();
  auto minY = std::numeric_limits<double>::max();
  auto maxX = std::numeric_limits<double>::lowest();
  auto maxY = std::numeric_limits<double>::lowest();
  for (const auto& vertex : boundsVertices)
  {
    const auto point = projectToCutPlane(vertex, cutterGeometry->edgeDir);
    minX = std::min(minX, point.x());
    minY = std::min(minY, point.y());
    maxX = std::max(maxX, point.x());
    maxY = std::max(maxY, point.y());
  }

  auto xCoordinates = std::vector<double>{minX, maxX};
  for (const auto& point : cutter)
  {
    if (point.x() > minX + GeometryEpsilon && point.x() < maxX - GeometryEpsilon)
    {
      addUniqueCoordinate(xCoordinates, point.x());
    }
  }
  std::ranges::sort(xCoordinates);
  if (xCoordinates.size() < 3u)
  {
    return std::nullopt;
  }

  const auto sectionPlane = vm::plane3d{
    vm::dot(cutterGeometry->edgeDir, edgePosition.start()), cutterGeometry->edgeDir};

  auto result = std::vector<mdl::Brush>{};
  const auto addCell = [&](std::vector<vm::vec2d> cell2D) -> bool {
    cell2D = cleanupPolygon(std::move(cell2D));
    if (cell2D.size() < 3u || std::abs(signedArea2D(cell2D)) <= AreaEpsilon)
    {
      return true;
    }
    if (!isConvexPolygon2D(cell2D))
    {
      return false;
    }
    if (signedArea2D(cell2D) < 0.0)
    {
      std::ranges::reverse(cell2D);
    }

    auto cell3D = std::vector<vm::vec3d>{};
    cell3D.reserve(cell2D.size());
    for (const auto& point : cell2D)
    {
      cell3D.push_back(unprojectFromCutPlane(point, sectionPlane));
    }

    const auto cellBrush = createExtrudedSectionBrush(
      cell3D, edgePosition, cutterGeometry->edgeDir, width, map);
    if (!cellBrush)
    {
      return false;
    }

    auto fragment = originalBrush;
    const auto intersection = fragment.intersect(map.worldBounds(), *cellBrush);
    if (intersection)
    {
      fragment.cloneFaceAttributesFrom(originalBrush);
      result.push_back(std::move(fragment));
    }

    return result.size() <= MaxOrthogonalInwardChamferFragments;
  };

  for (size_t i = 0u; i + 1u < xCoordinates.size(); ++i)
  {
    const auto x0 = xCoordinates[i];
    const auto x1 = xCoordinates[i + 1u];
    if (nearEqual(x0, x1))
    {
      continue;
    }

    const auto leftRange = convexPolygonYRangeAtX(cutter, x0);
    const auto rightRange = convexPolygonYRangeAtX(cutter, x1);
    if (!leftRange || !rightRange)
    {
      if (!addCell({{x0, minY}, {x1, minY}, {x1, maxY}, {x0, maxY}}))
      {
        return std::nullopt;
      }
      continue;
    }

    const auto cutBottomLeft = std::clamp((*leftRange)[0], minY, maxY);
    const auto cutTopLeft = std::clamp((*leftRange)[1], minY, maxY);
    const auto cutBottomRight = std::clamp((*rightRange)[0], minY, maxY);
    const auto cutTopRight = std::clamp((*rightRange)[1], minY, maxY);

    if (std::max(cutBottomLeft, cutBottomRight) > minY + GeometryEpsilon)
    {
      if (!addCell({{x0, minY}, {x1, minY}, {x1, cutBottomRight}, {x0, cutBottomLeft}}))
      {
        return std::nullopt;
      }
    }

    if (std::min(cutTopLeft, cutTopRight) < maxY - GeometryEpsilon)
    {
      if (!addCell({{x0, cutTopLeft}, {x1, cutTopRight}, {x1, maxY}, {x0, maxY}}))
      {
        return std::nullopt;
      }
    }
  }

  if (result.empty())
  {
    return std::nullopt;
  }

  return result;
}

std::optional<std::vector<mdl::Brush>> createInwardChamferedBrushes(
  const mdl::Brush& originalBrush,
  const std::vector<vm::segment3d>& edgePositions,
  const double width,
  const size_t segments,
  const double curve,
  const double proportion,
  mdl::Map& map)
{
  if (segments == 0u)
  {
    return std::vector<mdl::Brush>{originalBrush};
  }

  if (
    const auto orthogonalFragments = createOrthogonalInwardChamferedBrushes(
      originalBrush, edgePositions, width, segments, curve, proportion, map))
  {
    return orthogonalFragments;
  }

  auto cutters = std::vector<mdl::Brush>{};
  cutters.reserve(edgePositions.size());
  for (const auto& edgePosition : edgePositions)
  {
    auto cutter = createChamferCutterBrush(
      originalBrush, edgePosition, width, segments, curve, proportion, map);
    if (!cutter)
    {
      return std::nullopt;
    }
    cutters.push_back(std::move(*cutter));
  }

  auto cutterPtrs = std::vector<const mdl::Brush*>{};
  cutterPtrs.reserve(cutters.size());
  for (const auto& cutter : cutters)
  {
    cutterPtrs.push_back(&cutter);
  }

  auto result = std::vector<mdl::Brush>{};
  auto failed = false;
  for (auto& subtractionResult : originalBrush.subtract(
         map.worldNode().mapFormat(),
         map.worldBounds(),
         map.currentMaterialName(),
         cutterPtrs))
  {
    if (subtractionResult)
    {
      auto brush = std::move(subtractionResult).value();
      brush.cloneFaceAttributesFrom(originalBrush);
      result.push_back(std::move(brush));
    }
    else
    {
      failed = true;
    }
  }

  if (failed || result.empty())
  {
    return std::nullopt;
  }

  return result;
}

} // namespace

EdgeTool::EdgeTool(MapDocument& document)
  : BrushHandleToolBase{document}
{
}

void EdgeTool::pick(
  const vm::ray3d& pickRay,
  const gl::Camera& camera,
  const double handleRadius,
  mdl::PickResult& pickResult) const
{
  if (m_bevelTransaction)
    return;
  m_document.map().nodeHandles().pick<mdl::EdgeHandle>(
    pickResult, mdl::EdgeHandle::HandleHitType, pickRay, camera, handleRadius);
}

std::tuple<vm::vec3d, vm::vec3d> EdgeTool::handlePositionAndHitPoint(
  const std::vector<mdl::Hit>& hits) const
{
  contract_pre(!hits.empty());

  const auto& hit = hits.front();
  contract_assert(hit.hasType(mdl::EdgeHandle::HandleHitType));

  return {hit.target<mdl::EdgeHandle>().position.center(), hit.hitPoint()};
}

EdgeTool::MoveResult EdgeTool::move(const vm::vec3d& delta)
{
  if (m_bevelToolActive)
    return MoveResult::Deny;
  auto& map = m_document.map();

  const auto edgePositions =
    mdl::EdgeHandle::getPositions(map.nodeHandles().selectedHandles<mdl::EdgeHandle>());
  const auto transform = vm::translation_matrix(delta);
  if (transformEdges(map, edgePositions, transform))
  {
    m_dragHandlePosition = m_dragHandlePosition.transform(transform);
    return MoveResult::Continue;
  }
  return MoveResult::Deny;
}

std::string EdgeTool::actionName() const
{
  return kdl::str_plural(
    handleManager().selectedHandleCount<mdl::EdgeHandle>(), "Move Edge", "Move Edges");
}

void EdgeTool::removeSelection()
{
  if (m_bevelToolActive)
    return;
  auto& map = m_document.map();

  auto handles = map.nodeHandles().selectedHandles<mdl::EdgeHandle>();
  const auto vertexPositions = mdl::EdgeHandle::getVertices(handles);

  const auto commandName = kdl::str_plural(
    vertexPositions.size() / 2, "Remove Brush Edge", "Remove Brush Edges");
  removeVertices(map, commandName, vertexPositions);
}

bool EdgeTool::bevelToolActive() const
{
  return m_bevelToolActive;
}

size_t EdgeTool::bevelSegments() const
{
  return m_bevelSettings.segments;
}

double EdgeTool::bevelWidth() const
{
  return m_bevelSettings.width;
}

double EdgeTool::bevelCurve() const
{
  return m_bevelSettings.curve;
}

double EdgeTool::bevelProportion() const
{
  return m_bevelSettings.proportion;
}

bool EdgeTool::bevelKeepCutGeometry() const
{
  return m_bevelSettings.keepCutGeometry;
}

void EdgeTool::prepareBevelSelection()
{
  if (m_chamfer)
    cancelChamfer();
  auto& map = m_document.map();
  m_bevelFaces = map.selection().brushFaces;
  const auto selectedFaces =
    mdl::FaceHandle::getPositions(map.nodeHandles().selectedHandles<mdl::FaceHandle>());
  for (auto* node : map.selection().brushes)
    for (const auto& polygon : selectedFaces)
      if (const auto face = node->brush().findFace(polygon))
        m_bevelFaces.emplace_back(node, *face);
  m_bevelVertices = mdl::VertexHandle::getPositions(
    map.nodeHandles().selectedHandles<mdl::VertexHandle>());
  m_bevelEdges =
    mdl::EdgeHandle::getPositions(map.nodeHandles().selectedHandles<mdl::EdgeHandle>());
  m_bevelMode = !m_bevelFaces.empty() ? 1 : !m_bevelVertices.empty() ? 2 : 0;
  m_convexTargets.clear();
  auto nodes = map.selection().brushes;
  for (const auto& face : m_bevelFaces)
    if (std::ranges::find(nodes, face.node()) == nodes.end())
      nodes.push_back(face.node());
  for (auto* node : nodes)
    m_convexTargets.push_back({node, node->brush(), {}});
  if (!m_bevelFaces.empty())
  {
    mdl::deselectAll(map);
    mdl::selectNodes(map, std::vector<mdl::Node*>{nodes.begin(), nodes.end()});
  }
}

void EdgeTool::activateBevelTool()
{
  if (m_bevelToolActive)
    return;
  if (m_chamfer)
    cancelChamfer();
  m_bevelToolActive = true;
  m_bevelSettings.segments = 4;
  m_bevelSettings.width = std::max(1.0, grid().actualSize());
  m_bevelSettings.curve = 0.5;
  m_bevelClamp = true;
  applyConvexPreview();
  notifyBevelSettingsChanged();
}

void EdgeTool::cancelConvexPreview()
{
  if (m_bevelTransaction)
  {
    m_document.map().cancelTransaction();
    m_bevelTransaction = false;
  }
  m_bevelPreviewValid = false;
}

void EdgeTool::cancelBevelTool()
{
  cancelConvexPreview();
  if (m_chamfer)
    cancelChamfer();
  m_bevelToolActive = false;
  geometryHandles().deselectAll();
  refreshViews();
  notifyBevelSettingsChanged();
}

void EdgeTool::applyBevelTool()
{
  if (!m_bevelToolActive || !m_bevelPreviewValid)
    return;
  auto& map = m_document.map();
  if (!map.commitTransaction())
  {
    m_bevelStatus = "Could not commit Bevel";
    notifyBevelSettingsChanged();
    return;
  }
  m_bevelTransaction = false;
  m_bevelPreviewValid = false;
  m_bevelToolActive = false;
  geometryHandles().deselectAll();
  refreshViews();
  notifyBevelSettingsChanged();
}

void EdgeTool::resetBevelSettings()
{
  m_bevelSettings.segments = 4;
  m_bevelSettings.width = std::max(1.0, grid().actualSize());
  m_bevelSettings.curve = 0.5;
  m_bevelClamp = true;
  applyConvexPreview();
  notifyBevelSettingsChanged();
}
int EdgeTool::bevelMode() const
{
  return m_bevelMode;
}
void EdgeTool::setBevelMode(int mode)
{
  m_bevelMode = std::clamp(mode, 0, 2);
  applyConvexPreview();
  notifyBevelSettingsChanged();
}
bool EdgeTool::bevelClamp() const
{
  return m_bevelClamp;
}
void EdgeTool::setBevelClamp(bool enabled)
{
  m_bevelClamp = enabled;
  applyConvexPreview();
  notifyBevelSettingsChanged();
}
bool EdgeTool::bevelRadius() const
{
  return m_bevelRadius;
}
void EdgeTool::setBevelRadius(bool enabled)
{
  m_bevelRadius = enabled;
  applyConvexPreview();
  notifyBevelSettingsChanged();
}
const std::string& EdgeTool::bevelStatus() const
{
  return m_bevelStatus;
}
bool EdgeTool::bevelPreviewValid() const
{
  return m_bevelPreviewValid;
}

bool EdgeTool::applyConvexPreview()
{
  if (!m_bevelToolActive)
    return false;
  auto& map = m_document.map();
  if (!m_bevelTransaction && m_bevelMode == 0)
  {
    const auto handles = geometryHandles().selectedHandles();
    if (!handles.empty())
      m_bevelEdges = handles;
  }
  cancelConvexPreview();
  auto updates = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  auto status = std::string{};
  size_t index = 0;
  for (auto& target : m_convexTargets)
  {
    ++index;
    auto selected = mdl::BevelSelection{};
    if (m_bevelMode == 1)
    {
      auto faces = std::vector<size_t>{};
      for (const auto& face : m_bevelFaces)
        if (face.node() == target.node)
          faces.push_back(face.faceIndex());
      selected.edges = mdl::bevelFaceBoundary(target.source, faces);
    }
    else if (m_bevelMode == 2)
    {
      for (const auto& v : m_bevelVertices)
        if (target.source.hasVertex(v, 1e-5))
          selected.vertices.push_back(v);
    }
    else
      for (const auto& edge : m_bevelEdges)
        if (target.source.hasEdge(edge, 1e-5))
          selected.edges.push_back(edge);
    if (selected.edges.empty() && selected.vertices.empty())
      continue;
    const auto parameters = mdl::BevelParameters{
      m_bevelSettings.width,
      m_bevelSettings.segments,
      m_bevelSettings.curve,
      m_bevelRadius ? mdl::BevelSize::Radius : mdl::BevelSize::Offset,
      m_bevelClamp};
    auto result = mdl::bevelBrush(
      target.source,
      map.worldBounds(),
      map.worldNode().mapFormat(),
      selected,
      parameters);
    if (!result)
    {
      (void)(result | kdl::if_error([&](const auto& error) {
               m_bevelStatus = "Brush " + std::to_string(index) + ": " + error.msg;
             }));
      refreshViews();
      return false;
    }
    auto value = std::move(result).value();
    if (value.clamped)
      status += "Brush " + std::to_string(index) + ": "
                + std::to_string(value.appliedSize) + " (clamped). ";
    updates.emplace_back(target.node, mdl::NodeContents{std::move(value.brush)});
  }
  if (updates.empty())
  {
    m_bevelStatus =
      "Select face boundaries or vertices before opening Bevel, or select edges and "
      "change a parameter.";
    refreshViews();
    return false;
  }
  map.startTransaction("Bevel", mdl::TransactionScope::LongRunning);
  m_bevelTransaction = true;
  if (!mdl::updateNodeContents(map, "Bevel", std::move(updates)))
  {
    cancelConvexPreview();
    m_bevelStatus = "Could not update the selected brushes";
    return false;
  }
  m_bevelPreviewValid = true;
  m_bevelStatus =
    status.empty() ? "Preview on scene. Apply to keep, Cancel to restore." : status;
  refreshViews();
  return true;
}

void EdgeTool::setBevelSegments(const size_t segments)
{
  m_bevelSettings.segments = std::clamp(segments, size_t{1}, MaxChamferSegments);
  if (!applyBevelSettingsPreview())
  {
    notifyBevelSettingsChanged();
  }
}

void EdgeTool::setBevelWidth(const double width)
{
  m_bevelSettings.width = std::max(0.0, width);
  if (!applyBevelSettingsPreview())
  {
    notifyBevelSettingsChanged();
  }
}

void EdgeTool::setBevelCurve(const double curve)
{
  m_bevelSettings.curve = std::clamp(curve, 0.0, 0.99);
  if (!applyBevelSettingsPreview())
  {
    notifyBevelSettingsChanged();
  }
}

void EdgeTool::setBevelProportion(const double proportion)
{
  m_bevelSettings.proportion =
    std::clamp(proportion, -MaxChamferProportion, MaxChamferProportion);
  if (!applyBevelSettingsPreview())
  {
    notifyBevelSettingsChanged();
  }
}

void EdgeTool::setBevelKeepCutGeometry(const bool keepCutGeometry)
{
  m_bevelSettings.keepCutGeometry = keepCutGeometry;
  if (!applyBevelSettingsPreview())
  {
    notifyBevelSettingsChanged();
  }
}

bool EdgeTool::chamferActive() const
{
  return m_chamfer.has_value();
}

bool EdgeTool::chamferInwardActive() const
{
  return m_chamfer && m_chamfer->segments > 0u && m_chamfer->curve < -GeometryEpsilon;
}

bool EdgeTool::chamferCurveAdjustActive() const
{
  return m_chamfer && m_chamfer->segments > 0u;
}

bool EdgeTool::beginChamfer()
{
  if (m_chamfer)
  {
    return true;
  }

  auto& map = m_document.map();
  const auto handles = geometryHandles().selectedHandles();
  if (handles.empty())
  {
    return false;
  }

  auto state = ChamferState{};
  state.originalEdges = handles;
  state.minWidth = 0.0;
  state.maxWidth = std::numeric_limits<double>::max();
  state.widthStep = std::max(1.0, grid().actualSize());
  state.keepCutGeometry = m_bevelToolActive ? m_bevelSettings.keepCutGeometry : false;

  for (const auto& handle : handles)
  {
    const auto incidentBrushes = findIncidentBrushes(handle);
    if (incidentBrushes.size() != 1u)
    {
      return false;
    }

    auto* brushNode = incidentBrushes.front();
    const auto* edge = findEdge(brushNode->brush(), handle);
    if (!edge || !edge->fullySpecified())
    {
      return false;
    }

    const auto faceIndexA = edge->firstFace()->payload();
    const auto faceIndexB = edge->secondFace()->payload();
    if (!faceIndexA || !faceIndexB)
    {
      return false;
    }

    const auto edgeDir = normalized(handle.end() - handle.start());
    const auto normalA =
      edgeDir ? faceNormalInEdgePlane(brushNode->brush().face(*faceIndexA), *edgeDir)
              : std::nullopt;
    const auto normalB =
      edgeDir ? faceNormalInEdgePlane(brushNode->brush().face(*faceIndexB), *edgeDir)
              : std::nullopt;
    const auto tangentA = edgeDir && normalA && normalB
                            ? inwardFaceTangent(*edgeDir, *normalA, *normalB)
                            : std::nullopt;
    const auto tangentB = edgeDir && normalA && normalB
                            ? inwardFaceTangent(*edgeDir, *normalB, *normalA)
                            : std::nullopt;
    if (!edgeDir || !normalA || !normalB || !tangentA || !tangentB)
    {
      return false;
    }

    const auto widthRange = chamferWidthRange(
      brushNode->brush().face(*faceIndexA),
      brushNode->brush().face(*faceIndexB),
      handle,
      *tangentA,
      *tangentB,
      grid().actualSize());
    if (!widthRange)
    {
      return false;
    }

    auto brushIt = std::ranges::find_if(
      state.brushes, [&](const auto& brush) { return brush.node == brushNode; });
    if (brushIt == std::end(state.brushes))
    {
      state.brushes.push_back(ChamferBrush{brushNode, brushNode->brush(), {}});
      brushIt = std::prev(std::end(state.brushes));
    }

    brushIt->edges.push_back(ChamferEdge{handle});
    state.minWidth = std::max(state.minWidth, widthRange->min);
    state.maxWidth = std::min(state.maxWidth, widthRange->max);
    state.widthStep = std::max(state.widthStep, widthRange->step);
  }

  if (
    state.brushes.empty() || state.maxWidth <= GeometryEpsilon
    || state.minWidth > state.maxWidth)
  {
    return false;
  }

  state.width = std::clamp(state.widthStep, state.minWidth, state.maxWidth);

  map.startTransaction("Chamfer Edge", mdl::TransactionScope::LongRunning);
  m_chamfer = std::move(state);
  return true;
}

bool EdgeTool::applyChamferPreview(
  const size_t targetSegments,
  const double targetWidth,
  const double targetCurve,
  const double targetProportion,
  const bool targetKeepCutGeometry)
{
  if (!m_chamfer)
  {
    return false;
  }

  auto& map = m_document.map();
  const auto clampedSegments = std::min(targetSegments, MaxChamferSegments);
  const auto clampedWidth =
    std::clamp(targetWidth, m_chamfer->minWidth, m_chamfer->maxWidth);
  const auto clampedCurve = std::clamp(targetCurve, -1.0, 1.0);
  const auto proportionLimit = std::min(
    MaxChamferProportion,
    std::max(0.0, m_chamfer->maxWidth / std::max(clampedWidth, GeometryEpsilon) - 1.0));
  const auto clampedProportion =
    std::clamp(targetProportion, -proportionLimit, proportionLimit);

  if (clampedSegments == 0u)
  {
    map.rollbackTransaction();
    geometryHandles().deselectAll();
    geometryHandles().select(m_chamfer->originalEdges);
    m_chamfer->segments = 0u;
    m_chamfer->width = clampedWidth;
    m_chamfer->curve = clampedCurve;
    m_chamfer->proportion = clampedProportion;
    m_chamfer->keepCutGeometry = targetKeepCutGeometry;
    refreshViews();
    return true;
  }

  map.rollbackTransaction();

  if (clampedCurve >= -GeometryEpsilon)
  {
    auto nodesToUpdate = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
    auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
    auto originalNodes = std::vector<mdl::Node*>{};
    if (targetKeepCutGeometry)
    {
      originalNodes.reserve(m_chamfer->brushes.size());
    }
    else
    {
      nodesToUpdate.reserve(m_chamfer->brushes.size());
    }

    for (const auto& chamferBrush : m_chamfer->brushes)
    {
      auto edgePositions = std::vector<vm::segment3d>{};
      edgePositions.reserve(chamferBrush.edges.size());
      for (const auto& edge : chamferBrush.edges)
      {
        edgePositions.push_back(edge.originalEdge);
      }

      auto chamferedBrush = createChamferedBrush(
        chamferBrush.originalBrush,
        edgePositions,
        clampedWidth,
        clampedSegments,
        clampedCurve,
        clampedProportion,
        map);
      if (!chamferedBrush)
      {
        kdl::map_clear_and_delete(nodesToAdd);
        map.logger().error() << "Could not chamfer selected edges";
        return false;
      }

      if (targetKeepCutGeometry)
      {
        const auto remainingBrushes = std::vector<const mdl::Brush*>{&*chamferedBrush};
        auto keptBrushes = createKeptChamferCutBrushes(
          chamferBrush.originalBrush,
          edgePositions,
          clampedWidth,
          clampedSegments,
          clampedCurve,
          clampedProportion,
          remainingBrushes,
          map);
        if (!keptBrushes)
        {
          kdl::map_clear_and_delete(nodesToAdd);
          map.logger().error() << "Could not keep bevel cut geometry";
          return false;
        }

        auto* parent = chamferBrush.node->parent();
        if (!parent)
        {
          kdl::map_clear_and_delete(nodesToAdd);
          return false;
        }

        auto& parentNodes = nodesToAdd[parent];
        parentNodes.push_back(new mdl::BrushNode{std::move(*chamferedBrush)});
        for (auto& keptBrush : *keptBrushes)
        {
          parentNodes.push_back(new mdl::BrushNode{std::move(keptBrush)});
        }
        originalNodes.push_back(chamferBrush.node);
      }
      else
      {
        nodesToUpdate.emplace_back(
          chamferBrush.node, mdl::NodeContents{std::move(*chamferedBrush)});
      }
    }

    if (targetKeepCutGeometry)
    {
      mdl::deselectAll(map);
      const auto addedNodes = mdl::addNodes(map, nodesToAdd);
      if (addedNodes.empty())
      {
        return false;
      }
      mdl::removeNodes(map, originalNodes);
      mdl::selectNodes(map, addedNodes);
    }
    else
    {
      const auto success = mdl::updateNodeContents(
        map,
        kdl::str_plural(m_chamfer->originalEdges.size(), "Chamfer Edge", "Chamfer Edges"),
        std::move(nodesToUpdate));
      if (!success)
      {
        return false;
      }
    }
  }
  else
  {
    auto nodesToAdd = std::map<mdl::Node*, std::vector<mdl::Node*>>{};
    auto originalNodes = std::vector<mdl::Node*>{};
    originalNodes.reserve(m_chamfer->brushes.size());

    for (const auto& chamferBrush : m_chamfer->brushes)
    {
      auto edgePositions = std::vector<vm::segment3d>{};
      edgePositions.reserve(chamferBrush.edges.size());
      for (const auto& edge : chamferBrush.edges)
      {
        edgePositions.push_back(edge.originalEdge);
      }

      auto fragments = createInwardChamferedBrushes(
        chamferBrush.originalBrush,
        edgePositions,
        clampedWidth,
        clampedSegments,
        clampedCurve,
        clampedProportion,
        map);
      if (!fragments)
      {
        kdl::map_clear_and_delete(nodesToAdd);
        map.logger().error() << "Could not create inward chamfer fragments";
        return false;
      }

      auto keptBrushes = std::vector<mdl::Brush>{};
      if (targetKeepCutGeometry)
      {
        auto remainingBrushes = std::vector<const mdl::Brush*>{};
        remainingBrushes.reserve(fragments->size());
        for (const auto& fragment : *fragments)
        {
          remainingBrushes.push_back(&fragment);
        }

        auto kept = createKeptChamferCutBrushes(
          chamferBrush.originalBrush,
          edgePositions,
          clampedWidth,
          clampedSegments,
          clampedCurve,
          clampedProportion,
          remainingBrushes,
          map);
        if (!kept)
        {
          kdl::map_clear_and_delete(nodesToAdd);
          map.logger().error() << "Could not keep bevel cut geometry";
          return false;
        }
        keptBrushes = std::move(*kept);
      }

      auto* parent = chamferBrush.node->parent();
      if (!parent)
      {
        kdl::map_clear_and_delete(nodesToAdd);
        return false;
      }

      auto& parentNodes = nodesToAdd[parent];
      for (auto& fragment : *fragments)
      {
        parentNodes.push_back(new mdl::BrushNode{std::move(fragment)});
      }
      for (auto& keptBrush : keptBrushes)
      {
        parentNodes.push_back(new mdl::BrushNode{std::move(keptBrush)});
      }
      originalNodes.push_back(chamferBrush.node);
    }

    mdl::deselectAll(map);
    const auto addedNodes = mdl::addNodes(map, nodesToAdd);
    if (addedNodes.empty())
    {
      return false;
    }
    mdl::removeNodes(map, originalNodes);
    mdl::selectNodes(map, addedNodes);
  }

  m_chamfer->segments = clampedSegments;
  m_chamfer->width = clampedWidth;
  m_chamfer->curve = clampedCurve;
  m_chamfer->proportion = clampedProportion;
  m_chamfer->keepCutGeometry = targetKeepCutGeometry;
  geometryHandles().deselectAll();
  refreshViews();
  return true;
}

bool EdgeTool::adjustChamferSegments(const int delta)
{
  if (m_bevelToolActive)
  {
    setBevelSegments(size_t(std::clamp(int(bevelSegments()) + delta, 1, 16)));
    return true;
  }
  if (delta == 0 || !beginChamfer())
  {
    return false;
  }

  auto signedSegments = m_chamfer->curve < -GeometryEpsilon
                          ? -static_cast<int>(m_chamfer->segments)
                          : static_cast<int>(m_chamfer->segments);
  if (signedSegments == 0)
  {
    signedSegments = delta > 0 ? 1 : -1;
  }
  else
  {
    signedSegments += delta;
  }

  signedSegments = std::clamp(
    signedSegments,
    -static_cast<int>(MaxChamferSegments),
    static_cast<int>(MaxChamferSegments));

  const auto targetSegments = static_cast<size_t>(std::abs(signedSegments));
  const auto targetCurve = signedSegments < 0 ? DefaultInwardChamferCurve : 1.0;
  if (
    targetSegments == m_chamfer->segments
    && std::abs(targetCurve - m_chamfer->curve) <= GeometryEpsilon)
  {
    return false;
  }

  const auto didApply = applyChamferPreview(
    targetSegments,
    m_chamfer->width,
    targetCurve,
    m_chamfer->proportion,
    m_chamfer->keepCutGeometry);
  if (didApply)
  {
    updateBevelSettingsFromChamfer();
  }
  return didApply;
}

bool EdgeTool::adjustChamferWidth(const int delta)
{
  if (m_bevelToolActive)
  {
    setBevelWidth(std::max(0.0, bevelWidth() + delta * grid().actualSize()));
    return true;
  }
  if (delta == 0 || !beginChamfer())
  {
    return false;
  }

  const auto targetSegments = std::max(m_chamfer->segments, size_t{1u});
  const auto targetWidth = std::clamp(
    m_chamfer->width + double(delta) * m_chamfer->widthStep,
    m_chamfer->minWidth,
    m_chamfer->maxWidth);
  if (
    targetSegments == m_chamfer->segments
    && std::abs(targetWidth - m_chamfer->width) <= GeometryEpsilon)
  {
    return false;
  }

  const auto didApply = applyChamferPreview(
    targetSegments,
    targetWidth,
    m_chamfer->curve,
    m_chamfer->proportion,
    m_chamfer->keepCutGeometry);
  if (didApply)
  {
    updateBevelSettingsFromChamfer();
  }
  return didApply;
}

bool EdgeTool::adjustChamferCurve(const double delta)
{
  if (std::abs(delta) <= GeometryEpsilon || !chamferCurveAdjustActive())
  {
    return false;
  }

  const auto targetCurve =
    m_chamfer->curve < -GeometryEpsilon
      ? std::clamp(m_chamfer->curve - delta, MinInwardChamferCurve, MaxInwardChamferCurve)
      : std::clamp(
          m_chamfer->curve + delta, MinOutwardChamferCurve, MaxOutwardChamferCurve);
  if (std::abs(targetCurve - m_chamfer->curve) <= GeometryEpsilon)
  {
    return false;
  }

  const auto didApply = applyChamferPreview(
    m_chamfer->segments,
    m_chamfer->width,
    targetCurve,
    m_chamfer->proportion,
    m_chamfer->keepCutGeometry);
  if (didApply)
  {
    updateBevelSettingsFromChamfer();
  }
  return didApply;
}

bool EdgeTool::adjustChamferProportion(const double delta)
{
  if (std::abs(delta) <= GeometryEpsilon || !chamferCurveAdjustActive())
  {
    return false;
  }

  const auto targetProportion = std::clamp(
    m_chamfer->proportion + delta, -MaxChamferProportion, MaxChamferProportion);
  if (std::abs(targetProportion - m_chamfer->proportion) <= GeometryEpsilon)
  {
    return false;
  }

  const auto didApply = applyChamferPreview(
    m_chamfer->segments,
    m_chamfer->width,
    m_chamfer->curve,
    targetProportion,
    m_chamfer->keepCutGeometry);
  if (didApply)
  {
    updateBevelSettingsFromChamfer();
  }
  return didApply;
}

void EdgeTool::finishChamfer()
{
  if (!m_chamfer)
  {
    return;
  }

  if (m_chamfer->segments == 0u)
  {
    m_document.map().cancelTransaction();
  }
  else
  {
    m_document.map().commitTransaction();
  }
  m_chamfer = std::nullopt;
}

void EdgeTool::cancelChamfer()
{
  if (!m_chamfer)
  {
    return;
  }

  m_document.map().cancelTransaction();
  m_chamfer = std::nullopt;
  refreshViews();
}

bool EdgeTool::applyBevelSettingsPreview()
{
  const auto result = applyConvexPreview();
  notifyBevelSettingsChanged();
  return result;
}

void EdgeTool::updateBevelSettingsFromChamfer()
{
  if (m_chamfer)
  {
    m_bevelSettings.segments = m_chamfer->segments;
    m_bevelSettings.width = m_chamfer->width;
    m_bevelSettings.curve = m_chamfer->curve;
    m_bevelSettings.proportion = m_chamfer->proportion;
    m_bevelSettings.keepCutGeometry = m_chamfer->keepCutGeometry;
  }

  notifyBevelSettingsChanged();
}

void EdgeTool::notifyBevelSettingsChanged()
{
  bevelSettingsDidChangeNotifier(*this);
  notifyToolHandleSelectionChanged();
}

bool EdgeTool::doDeactivate()
{
  cancelConvexPreview();
  if (m_chamfer)
  {
    m_document.map().cancelTransaction();
    m_chamfer = std::nullopt;
  }

  m_bevelToolActive = false;
  m_bevelSettings.segments = 0u;
  m_bevelSettings.width = std::max(1.0, grid().actualSize());
  m_bevelSettings.curve = 1.0;
  m_bevelSettings.proportion = 0.0;
  m_bevelSettings.keepCutGeometry = false;

  return BrushHandleToolBase<mdl::EdgeHandle>::doDeactivate();
}

QWidget* EdgeTool::doCreatePage(QWidget* parent)
{
  return new EdgeBevelToolPage{*this, parent};
}

} // namespace tb::ui
