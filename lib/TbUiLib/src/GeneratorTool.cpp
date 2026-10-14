#include "ui/GeneratorTool.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

#include "PreferenceManager.h"
#include "Preferences.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "ui/MapDocument.h"
#include "ui/RockGeneratorDialog.h"
#include "ui/TerrainGeneratorDialog.h"

#include <algorithm>
#include <cmath>

namespace tb::ui
{
GeneratorTool::GeneratorTool(MapDocument& document, const Kind kind)
  : Tool{false}
  , m_document{document}
  , m_kind{kind}
{
}

bool GeneratorTool::canActivate() const
{
  return active() || m_kind == Kind::GridRotation
         || m_document.map().selection().hasOnlyBrushes();
}

bool GeneratorTool::doActivate()
{
  if (!canActivate() || !m_page)
  {
    return false;
  }
  switch (m_kind)
  {
  case Kind::Terrain:
    static_cast<TerrainGeneratorDialog*>(m_page.data())->startPreview();
    break;
  case Kind::Rock:
    static_cast<RockGeneratorDialog*>(m_page.data())->startPreview();
    break;
  case Kind::GridRotation:
    m_oldRotation = pref(Preferences::GridRotation);
    m_oldUnifiedGrid = pref(Preferences::GridUseUnified3D);
    m_gridApplied = false;
    m_angle->setValue(m_oldRotation);
    m_unifiedGrid->setChecked(m_oldUnifiedGrid);
    break;
  }
  return true;
}

bool GeneratorTool::doDeactivate()
{
  if (!m_page)
  {
    return true;
  }
  switch (m_kind)
  {
  case Kind::Terrain:
    static_cast<TerrainGeneratorDialog*>(m_page.data())->cancelPreview();
    break;
  case Kind::Rock:
    static_cast<RockGeneratorDialog*>(m_page.data())->cancelPreview();
    break;
  case Kind::GridRotation:
    if (!m_gridApplied)
    {
      setPref(Preferences::GridRotation, m_oldRotation);
      setPref(Preferences::GridUseUnified3D, m_oldUnifiedGrid);
    }
    break;
  }
  return true;
}

QWidget* GeneratorTool::doCreatePage(QWidget* parent)
{
  const auto finished = [this] { didRequestDeactivateNotifier(*this); };
  if (m_kind == Kind::Terrain)
  {
    auto* page = new TerrainGeneratorDialog{m_document, parent};
    QObject::connect(page, &TerrainGeneratorDialog::finished, page, finished);
    m_page = page;
    return page;
  }
  if (m_kind == Kind::Rock)
  {
    auto* page = new RockGeneratorDialog{m_document, parent};
    QObject::connect(page, &RockGeneratorDialog::finished, page, finished);
    m_page = page;
    return page;
  }

  auto* page = new QWidget{parent};
  m_page = page;
  m_angle = new QDoubleSpinBox{};
  m_angle->setRange(0.0, 90.0);
  m_angle->setDecimals(1);
  m_angle->setSingleStep(1.0);
  m_angle->setSuffix(QObject::tr(" deg"));
  m_angle->setKeyboardTracking(false);
  m_angleSlider = new QSlider{Qt::Horizontal};
  m_angleSlider->setRange(0, 900);
  m_angleSlider->setMaximumWidth(250);
  m_unifiedGrid = new QCheckBox{QObject::tr("Unified brush face grid")};
  auto* defaults = new QPushButton{QObject::tr("Default")};
  auto* apply = new QPushButton{QObject::tr("Apply")};
  auto* cancel = new QPushButton{QObject::tr("Cancel")};
  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(new QLabel{QObject::tr("Grid Rotation - Angle")});
  layout->addWidget(m_angle);
  layout->addWidget(m_angleSlider);
  layout->addWidget(m_unifiedGrid);
  layout->addWidget(defaults);
  layout->addWidget(apply);
  layout->addWidget(cancel);
  layout->addStretch(1);
  page->setLayout(layout);
  QObject::connect(
    m_angle,
    qOverload<double>(&QDoubleSpinBox::valueChanged),
    page,
    [this](const double value) {
      const auto blocker = QSignalBlocker{m_angleSlider};
      m_angleSlider->setValue(static_cast<int>(std::round(value * 10.0)));
      setPref(Preferences::GridRotation, static_cast<float>(value));
    });
  QObject::connect(m_angleSlider, &QSlider::valueChanged, page, [this](const int value) {
    m_angle->setValue(value / 10.0);
  });
  QObject::connect(m_unifiedGrid, &QCheckBox::toggled, page, [](const bool checked) {
    setPref(Preferences::GridUseUnified3D, checked);
  });
  QObject::connect(defaults, &QPushButton::clicked, page, [this] {
    m_angle->setValue(0.0);
    m_unifiedGrid->setChecked(true);
  });
  QObject::connect(apply, &QPushButton::clicked, page, [this, finished] {
    m_gridApplied = true;
    finished();
  });
  QObject::connect(cancel, &QPushButton::clicked, page, finished);
  return page;
}
} // namespace tb::ui
