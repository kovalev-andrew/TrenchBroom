/*
 Copyright (C) 2026 Kristian Duske
 This file is part of TrenchBroom, licensed under the GNU General Public License,
 version 3 or later. See LICENSE.txt for details.
 */
#pragma once

#include <QString>

#include "Result.h"

#include <filesystem>

class QWidget;

namespace tb::ui
{
class AppController;
class MapWindow;

// Capture pixels from the editor itself, without reconstructing its interface.
Result<void> saveGuideScreenshot(
  QWidget& widget, const std::filesystem::path& path, bool redactPaths = false);
void showGuideScreenshotDialog(MapWindow& window);
bool startGuideCapture(AppController& controller, const QString& scriptPath);
} // namespace tb::ui
