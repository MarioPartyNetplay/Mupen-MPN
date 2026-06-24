/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_SCREENSHOT_HPP
#define CORE_SCREENSHOT_HPP

#include <filesystem>
#include <functional>

// takes a screenshot on the next rendered frame
bool CoreTakeScreenshot(void);

// returns true when a screenshot should be captured this frame
bool CoreConsumeScreenshotRequest(void);

// builds the next screenshot file path
std::filesystem::path CoreGetNextScreenshotPath(void);

// resets screenshot numbering when a new ROM is opened
void CoreResetScreenshotCounter(void);

// registers whether the frontend should capture screenshots from the render surface
using CoreScreenshotBackendAvailableFunc = std::function<bool()>;
void CoreRegisterScreenshotBackend(CoreScreenshotBackendAvailableFunc func);

#endif // CORE_SCREENSHOT_HPP
