/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <QDialog>

#include "mdl/BrushPerforate.h"
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QDialogButtonBox;
namespace tb::ui
{
class PerforateDialog : public QDialog
{
  mdl::Brush m_source;
  size_t m_faceIndex;
  vm::bbox3d m_bounds;
  mdl::MapFormat m_format;
  mdl::Perforation m_result;
  QSpinBox* m_count;
  QSpinBox* m_sides;
  QSpinBox* m_seed;
  QDoubleSpinBox* m_min;
  QDoubleSpinBox* m_max;
  QDoubleSpinBox* m_spacing;
  QCheckBox* m_keep;
  QLabel* m_status;
  QDialogButtonBox* m_buttons;
  QWidget* m_preview;
  void invalidate();
  void generate();

public:
  PerforateDialog(
    const mdl::Brush& source,
    size_t faceIndex,
    const vm::bbox3d& bounds,
    mdl::MapFormat format,
    QWidget* parent);
  mdl::Perforation takePerforation();
  bool keepPlugs() const;
};
} // namespace tb::ui
