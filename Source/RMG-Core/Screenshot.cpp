/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "Screenshot.hpp"
#include "Directories.hpp"
#include "RomHeader.hpp"
#include "RomSettings.hpp"
#include "Library.hpp"
#include "Error.hpp"

#include "m64p/Api.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>

//
// Local Variables
//

static std::atomic<bool> l_ScreenshotPending = false;
static CoreScreenshotBackendAvailableFunc l_ScreenshotBackendAvailable;
static int l_ScreenshotIndex = 0;

//
// Local Functions
//

static std::string sanitizeScreenshotBaseName(const std::string& name)
{
    std::string sanitized = name;

    for (char& character : sanitized)
    {
        if (character == ' ' || character == ':' || character == '<' || character == '>' ||
            character == '\"' || character == '/' || character == '\\' || character == '|' ||
            character == '?' || character == '*')
        {
            character = '_';
        }
        else
        {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }

    return sanitized;
}

//
// Exported Functions
//

CORE_EXPORT void CoreRegisterScreenshotBackend(CoreScreenshotBackendAvailableFunc func)
{
    l_ScreenshotBackendAvailable = std::move(func);
}

CORE_EXPORT void CoreResetScreenshotCounter(void)
{
    l_ScreenshotIndex = 0;
}

CORE_EXPORT bool CoreConsumeScreenshotRequest(void)
{
    return l_ScreenshotPending.exchange(false);
}

CORE_EXPORT std::filesystem::path CoreGetNextScreenshotPath(void)
{
    CoreRomHeader header;
    CoreRomSettings settings;
    std::string baseName;

    if (CoreGetCurrentRomHeader(header) && !header.Name.empty())
    {
        baseName = sanitizeScreenshotBaseName(header.Name);
    }
    else if (CoreGetCurrentRomSettings(settings) && !settings.MD5.empty())
    {
        baseName = settings.MD5;
    }
    else
    {
        return {};
    }

    const std::filesystem::path screenshotDirectory = CoreGetScreenshotDirectory();
    if (screenshotDirectory.empty())
    {
        return {};
    }

    std::error_code errorCode;
    std::filesystem::create_directories(screenshotDirectory, errorCode);

    for (; l_ScreenshotIndex < 1000; ++l_ScreenshotIndex)
    {
        char filenameBuffer[512];
        std::snprintf(filenameBuffer, sizeof(filenameBuffer), "%s-%03d.png", baseName.c_str(), l_ScreenshotIndex);
        const std::filesystem::path screenshotPath = screenshotDirectory / filenameBuffer;

        if (!std::filesystem::exists(screenshotPath))
        {
            ++l_ScreenshotIndex;
            return screenshotPath;
        }
    }

    CoreSetError("Can't save screenshot; folder already contains 1000 screenshots for this ROM");
    return {};
}

CORE_EXPORT bool CoreTakeScreenshot(void)
{
    std::string error;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (l_ScreenshotBackendAvailable && l_ScreenshotBackendAvailable())
    {
        l_ScreenshotPending = true;
        return true;
    }

    m64p_error ret = m64p::Core.DoCommand(M64CMD_TAKE_NEXT_SCREENSHOT, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreTakeScreenshot M64P::Core.DoCommand(M64CMD_TAKE_NEXT_SCREENSHOT) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}
