/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include "mdl/Brush.h"

class QCheckBox;
class QSlider;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace tb::ui
{
class FracturePreview;

class FractureDialog : public QDialog
{
private:
  mdl::Brush m_source;
  vm::bbox3d m_worldBounds;
  mdl::MapFormat m_mapFormat;
  std::vector<mdl::Brush> m_fragments;
  std::vector<mdl::Brush> m_baseFragments;
  QSlider* m_separation;
  QCheckBox* m_noCollide;
  QSpinBox* m_count;
  QSpinBox* m_seed;
  QLineEdit* m_material;
  QLabel* m_status;
  QDialogButtonBox* m_buttons;
  FracturePreview* m_preview;

public:
  FractureDialog(
    const mdl::Brush& source,
    const vm::bbox3d& worldBounds,
    mdl::MapFormat mapFormat,
    const std::string& material,
    QWidget* parent);

  std::vector<mdl::Brush> takeFragments();
  bool createFuncWalls() const;

private:
  void updateSeparation();
  void invalidatePreview();
  void generatePreview();
};
} // namespace tb::ui
