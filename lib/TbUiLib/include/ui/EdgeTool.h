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

#pragma once

#include "mdl/Brush.h"
#include "ui/BrushHandleToolBase.h"

#include <optional>
#include <string>
#include <vector>

class QWidget;
namespace tb::ui
{
class MapDocument;

class EdgeTool : public BrushHandleToolBase<mdl::EdgeHandle>
{
public:
  explicit EdgeTool(MapDocument& document);

  using BrushHandleToolBase::findIncidentNodes;

public:
  void pick(
    const vm::ray3d& pickRay,
    const gl::Camera& camera,
    double handleRadius,
    mdl::PickResult& pickResult) const override;

public:
  std::tuple<vm::vec3d, vm::vec3d> handlePositionAndHitPoint(
    const std::vector<mdl::Hit>& hits) const override;

  MoveResult move(const vm::vec3d& delta) override;

  std::string actionName() const override;

  void removeSelection();

  bool chamferActive() const;
  bool chamferInwardActive() const;
  bool chamferCurveAdjustActive() const;
  bool adjustChamferSegments(int delta);
  bool adjustChamferWidth(int delta);
  bool adjustChamferCurve(double delta);
  bool adjustChamferProportion(double delta);
  void finishChamfer();
  void cancelChamfer();

  bool bevelToolActive() const;
  size_t bevelSegments() const;
  double bevelWidth() const;
  double bevelCurve() const;
  double bevelProportion() const;
  bool bevelKeepCutGeometry() const;
  void activateBevelTool();
  void cancelBevelTool();
  void applyBevelTool();
  void resetBevelSettings();
  void setBevelSegments(size_t segments);
  void setBevelWidth(double width);
  void setBevelCurve(double curve);
  void setBevelProportion(double proportion);
  void setBevelKeepCutGeometry(bool keepCutGeometry);

  Notifier<EdgeTool&> bevelSettingsDidChangeNotifier;

private:
  struct ChamferEdge
  {
    vm::segment3d originalEdge;
  };

  struct ChamferBrush
  {
    mdl::BrushNode* node = nullptr;
    mdl::Brush originalBrush;
    std::vector<ChamferEdge> edges;
  };

  struct ChamferState
  {
    std::vector<ChamferBrush> brushes;
    std::vector<vm::segment3d> originalEdges;
    size_t segments = 0u;
    double width = 0.0;
    double minWidth = 0.0;
    double maxWidth = 0.0;
    double widthStep = 1.0;
    double curve = 1.0;
    double proportion = 0.0;
    bool keepCutGeometry = false;
  };

  struct BevelSettings
  {
    size_t segments = 0u;
    double width = 0.0;
    double curve = 1.0;
    double proportion = 0.0;
    bool keepCutGeometry = false;
  };

  std::optional<ChamferState> m_chamfer;
  BevelSettings m_bevelSettings;
  bool m_bevelToolActive = false;

  bool beginChamfer();
  bool applyChamferPreview(
    size_t targetSegments,
    double targetWidth,
    double targetCurve,
    double targetProportion,
    bool targetKeepCutGeometry);
  bool applyBevelSettingsPreview();
  void updateBevelSettingsFromChamfer();
  void notifyBevelSettingsChanged();

private: // Tool interface
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};

} // namespace tb::ui
