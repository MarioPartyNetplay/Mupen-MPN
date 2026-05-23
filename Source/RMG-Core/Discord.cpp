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
#include "Settings.hpp"

#include <discord_rpc.h>

#include <ctime>

namespace
{
constexpr const char* DISCORD_APP_ID = "888655408623943731";

static bool l_DiscordInitialized = false;

static int64_t current_timestamp(void)
{
    return static_cast<int64_t>(std::time(nullptr));
}
} // namespace

bool CoreDiscordStart(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    if (!CoreSettingsGetBoolValue(SettingsID::GUI_EnableDiscordRPC))
    {
        return false;
    }

    if (!l_DiscordInitialized)
    {
        DiscordEventHandlers handlers = {};
        Discord_Initialize(DISCORD_APP_ID, &handlers, 1, nullptr);
        l_DiscordInitialized = true;
    }

    const std::string details = !settings.GoodName.empty() ? settings.GoodName :
                                (!settings.InternalName.empty() ? settings.InternalName : header.Name);
    const std::string state = !header.Region.empty() ? header.Region : "In game";
    const std::string imageKey = !header.GameID.empty() ? header.GameID : "rmg";

    DiscordRichPresence presence = {};
    presence.details = details.c_str();
    presence.state = state.c_str();
    presence.startTimestamp = current_timestamp();
    presence.largeImageKey = imageKey.c_str();
    presence.largeImageText = details.c_str();
    presence.instance = 1;

    Discord_UpdatePresence(&presence);
    Discord_RunCallbacks();

    return true;
}

void CoreDiscordShutdown(void)
{
    if (!l_DiscordInitialized)
    {
        return;
    }

    Discord_ClearPresence();
    Discord_RunCallbacks();
    Discord_Shutdown();
    l_DiscordInitialized = false;
}