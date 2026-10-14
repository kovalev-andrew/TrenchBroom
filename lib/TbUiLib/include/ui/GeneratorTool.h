#pragma once

#include <QPointer>

#include "ui/Tool.h"

class QCheckBox;
class QDoubleSpinBox;
class QSlider;

namespace tb::ui
{
class MapDocument;

class GeneratorTool : public Tool
{
public:
  enum class Kind
  {
    Terrain,
    Rock,
    GridRotation
  };

  Notifier<GeneratorTool&> didRequestDeactivateNotifier;

  GeneratorTool(MapDocument& document, Kind kind);
  bool canActivate() const;

private:
  MapDocument& m_document;
  Kind m_kind;
  QPointer<QWidget> m_page;
  QDoubleSpinBox* m_angle = nullptr;
  QSlider* m_angleSlider = nullptr;
  QCheckBox* m_unifiedGrid = nullptr;
  float m_oldRotation = 0.0f;
  bool m_oldUnifiedGrid = false;
  bool m_gridApplied = false;

  bool doActivate() override;
  bool doDeactivate() override;
  QWidget* doCreatePage(QWidget* parent) override;
};
} // namespace tb::ui
