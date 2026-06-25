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
#include "Discord.hpp"
#include "TurnCount.hpp"
#include "ConvertStringEncoding.hpp"
#include "Callback.hpp"
#include "Library.hpp"

#include <iostream>
#include <mutex>

//
// Local Structs
//

struct l_DebugCallbackMessage
{
    std::string Context;
    int         Level = 0;
    std::string Message;
};

//
// Local Variables
//

static bool l_SetupCallbacks = false;
static std::function<void(enum CoreDebugMessageType, std::string, std::string)> l_DebugCallbackFunc;
static std::function<void(enum CoreStateCallbackType, int)> l_StateCallbackFunc;
static bool l_PrintCallbacks = false;
static std::vector<l_DebugCallbackMessage> l_PendingCallbacks;
static std::mutex l_callbackMutex;


//
// Internal Functions
//

void CoreDebugCallback(void* context, int level, const char* message)
{
    std::string contextString(static_cast<char*>(context));
    std::string messageString(message);

    std::function<void(enum CoreDebugMessageType, std::string, std::string)> debugCallback;
    bool setupCallbacks = false;
    bool printCallbacks = false;
    {
        std::lock_guard<std::mutex> lock(l_callbackMutex);
        setupCallbacks = l_SetupCallbacks;
        if (!setupCallbacks)
        {
            l_PendingCallbacks.push_back({contextString, level, message});
            return;
        }
        printCallbacks = l_PrintCallbacks;
        debugCallback = l_DebugCallbackFunc;
    }

    if (printCallbacks)
    {
        std::cout << contextString << messageString << std::endl;
    }

    // convert string encoding accordingly
    if (messageString.rfind("IS64:", 0) == 0)
    {
        messageString = CoreConvertStringEncoding(message, CoreStringEncoding::EUC_JP);
    }
    else if (contextString.rfind("[CORE]", 0) == 0)
    {
        messageString = CoreConvertStringEncoding(message, CoreStringEncoding::Shift_JIS);
    }

    if (debugCallback)
    {
        debugCallback((CoreDebugMessageType)level, contextString, messageString);
    }
}

void CoreStateCallback(void*, m64p_core_param param, int value)
{
    std::function<void(enum CoreStateCallbackType, int)> stateCallback;
    {
        std::lock_guard<std::mutex> lock(l_callbackMutex);
        if (!l_SetupCallbacks)
        {
            return;
        }
        stateCallback = l_StateCallbackFunc;
    }

    if (param == static_cast<m64p_core_param>(CoreStateCallbackType::Frame))
    {
        CoreTurnCountUpdateFrame();
        CoreDiscordUpdateFrame(value);
    }

    if (stateCallback)
    {
        stateCallback((CoreStateCallbackType)param, value);
    }
}

//
// Exported Functions
//

CORE_EXPORT bool CoreSetupCallbacks(std::function<void(enum CoreDebugMessageType, std::string, std::string)> debugCallbackFunc,
                        std::function<void(enum CoreStateCallbackType, int)> stateCallbackFunc)
{
    std::vector<l_DebugCallbackMessage> pendingCallbacks;
    {
        std::lock_guard<std::mutex> lock(l_callbackMutex);
        l_DebugCallbackFunc = std::move(debugCallbackFunc);
        l_StateCallbackFunc = std::move(stateCallbackFunc);
        l_SetupCallbacks = true;
        pendingCallbacks.swap(l_PendingCallbacks);
    }
    
    // send pending messages
    for (const auto& callback : pendingCallbacks)
    {
        CoreDebugCallback(const_cast<char*>(callback.Context.c_str()), callback.Level, callback.Message.c_str());
    }

    return true;
}

CORE_EXPORT void CoreSetPrintDebugCallback(bool enabled)
{
    l_PrintCallbacks = enabled;
}

CORE_EXPORT void CoreAddCallbackMessage(CoreDebugMessageType type, std::string message)
{
    CoreDebugCallback(const_cast<char*>("[GUI]   "), static_cast<int>(type), message.c_str());
}

CORE_EXPORT void CoreNotifyScreenshotCaptured(bool success)
{
    std::function<void(enum CoreStateCallbackType, int)> stateCallback;
    {
        std::lock_guard<std::mutex> lock(l_callbackMutex);
        if (!l_SetupCallbacks)
        {
            return;
        }
        stateCallback = l_StateCallbackFunc;
    }

    if (stateCallback)
    {
        stateCallback(CoreStateCallbackType::ScreenshotCaptured, success ? 1 : 0);
    }
}
