/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>

#include "mdl/BrushSettle.h"

class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

namespace tb::ui
{
class SettlePreview;
class SettleDialog : public QDialog
{
  std::vector<mdl::Brush> m_source;
  std::vector<mdl::Brush> m_obstacles;
  vm::bbox3d m_bounds;
  std::vector<mdl::SettleTriangle> m_patchTriangles;
  std::unique_ptr<mdl::BrushSettleSimulation> m_simulation;
  std::vector<mdl::Brush> m_result;
  std::vector<vm::mat4x4d> m_transforms;
  QDoubleSpinBox* m_friction;
  QDoubleSpinBox* m_bounce;
  QDoubleSpinBox* m_kick;
  QDoubleSpinBox* m_duration;
  QSpinBox* m_seed;
  QLabel* m_status;
  QPushButton* m_pause;
  QDialogButtonBox* m_buttons;
  QTimer* m_timer;
  SettlePreview* m_preview;

public:
  SettleDialog(
    std::vector<mdl::Brush> source,
    std::vector<mdl::Brush> obstacles,
    const vm::bbox3d& bounds,
    QWidget* parent,
    std::vector<mdl::SettleTriangle> patchTriangles = {});
  ~SettleDialog() override;
  std::vector<mdl::Brush> takeBrushes();

private:
  void reset();
  void start();
  void tick();
  void pause();
  void validate();
};
} // namespace tb::ui
