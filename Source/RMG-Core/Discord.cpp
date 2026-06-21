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
#include "m64p/Api.hpp"
#include "MPNMemory.h"
#include "Settings.hpp"

#include <discord_rpc.h>

#include <ctime>
#include <cstdint>
#include <cstring>
#include <string>

// Define the global variables so the linker can find them
int m_DiscordCurrentPlayers = 1; 
const char* m_DiscordNetplaySecret = ""; 
bool m_DiscordIsNetplayActive = false;

namespace
{
constexpr const char* DISCORD_APP_ID = "888655408623943731";

static bool l_DiscordInitialized = false;
static bool l_DiscordRomCached = false;
static CoreRomHeader l_DiscordHeader;
static CoreRomSettings l_DiscordSettings;
static int64_t l_DiscordStartTimestamp = 0;
static int64_t l_DiscordNextPost = 0;

static int64_t current_timestamp(void)
{
    return static_cast<int64_t>(std::time(nullptr));
}

static bool contains_title(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

static std::string preferred_title(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    if (!settings.GoodName.empty())
    {
        return settings.GoodName;
    }

    if (!settings.InternalName.empty())
    {
        return settings.InternalName;
    }

    return header.Name;
}

static bool is_mario_party_1(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return contains_title(settings.GoodName, "MarioParty") ||
           contains_title(settings.GoodName, "Mario Party") ||
           contains_title(settings.InternalName, "MARIO PARTY") ||
           header.GameID == "CLBE";
}

static bool is_mario_party_2(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return contains_title(settings.GoodName, "MarioParty2") ||
           contains_title(settings.GoodName, "Mario Party 2") ||
           contains_title(settings.InternalName, "MARIO PARTY 2") ||
           header.GameID == "NMWE";
}

static bool is_mario_party_3(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return header.GameID == "NMVE" ||
           contains_title(settings.GoodName, "MarioParty3") ||
           contains_title(settings.GoodName, "Mario Party 3") ||
           contains_title(settings.InternalName, "MARIO PARTY 3");
}

static bool is_mario_kart_64(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return contains_title(settings.GoodName, "MARIOKART64") ||
           contains_title(settings.GoodName, "Mario Kart 64") ||
           contains_title(settings.InternalName, "MARIO KART 64") ||
           header.GameID == "NKTE";
}

static bool is_super_smash_bros(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return contains_title(settings.GoodName, "SMASH BROTHERS") ||
           contains_title(settings.GoodName, "Super Smash Bros") ||
           contains_title(settings.InternalName, "SUPER SMASH BROS") ||
           header.GameID == "NAEE";
}

static const uint8_t* get_rdram_pointer(void)
{
    if (m64p::Core.DebugMemGetPointer == nullptr)
    {
        return nullptr;
    }

    return static_cast<const uint8_t*>(m64p::Core.DebugMemGetPointer(M64P_DBG_PTR_RDRAM));
}

static uint8_t safe_read_rdram(const uint8_t* rdram, uint32_t offset)
{
    if (rdram == nullptr || offset >= 0x800000)
    {
        return 0;
    }
    return rdram[offset];
}

static void update_presence_from_memory(void)
{
    const uint8_t* rdram = get_rdram_pointer();
    if (rdram == nullptr)
    {
        return;
    }

    const CoreTurnCountInfo turnInfo = CoreTurnCountGetInfo();

    std::string details = preferred_title(l_DiscordHeader, l_DiscordSettings);
    std::string state = "Setting Up...";
    
    const char* imageKey = "rmg";
    const char* imageText = details.c_str();
    const char* smallImageKey = nullptr;
    const char* smallImageText = nullptr;

    details = "Players: (" + std::to_string(m_DiscordCurrentPlayers) + "/4)";

    if (is_mario_party_3(l_DiscordHeader, l_DiscordSettings))
    {
        imageKey = "box-mp3";
        imageText = "Mario Party 3";

        const uint8_t boardId = safe_read_rdram(rdram, MP3_MEM_BOARD);
        const uint8_t gameType = safe_read_rdram(rdram, MP3_MEM_GAMETYPE);
        const uint8_t totalTurns = safe_read_rdram(rdram, MP3_MEM_TOTAL_TURNS);

        if (gameType >= 1 && gameType <= 6 && totalTurns > 0 && boardId < (sizeof(MP3_BOARDS) / sizeof(MP3_BOARDS[0])))
        {
            if (gameType == 1 || gameType == 5)
                state = MP3_BOARDS[boardId];
            else if (gameType == 2 || gameType == 6)
                state = MP3_BOARDS_DUEL[boardId];
            else
                state = MP3_BOARDS[boardId];

            if (turnInfo.valid)
            {
                state += " Turn: " + std::to_string(turnInfo.current) + "/" + std::to_string(turnInfo.total);
            }
            
            if (boardId < (sizeof(MP3_BOARDS_THUMB) / sizeof(MP3_BOARDS_THUMB[0])))
            {
                smallImageKey = MP3_BOARDS_THUMB[boardId];
                smallImageText = state.c_str();
            }
        }
        else
        {
            state = "Setting Up...";
        }
    }
    else if (is_mario_party_2(l_DiscordHeader, l_DiscordSettings))
    {
        imageKey = "box-mp2";
        imageText = "Mario Party 2";

        const uint8_t boardId = safe_read_rdram(rdram, MP2_MEM_BOARD);
        const uint8_t totalTurns = safe_read_rdram(rdram, MP2_MEM_TOTAL_TURNS);
        const uint8_t gameState = safe_read_rdram(rdram, MP2_MEM_GAMESTATE);

        if (gameState > 1 && totalTurns > 0 && boardId < (sizeof(MP2_BOARDS) / sizeof(MP2_BOARDS[0])))
        {
            if (boardId != 7)
            {
                state = MP2_BOARDS[boardId];
                if (turnInfo.valid)
                {
                    state += " Turn: " + std::to_string(turnInfo.current) + "/" + std::to_string(turnInfo.total);
                }
            }
            else
            {
                state = "Mini-Game Coaster";
            }

            if (boardId < (sizeof(MP2_BOARDS_THUMB) / sizeof(MP2_BOARDS_THUMB[0])))
            {
                smallImageKey = MP2_BOARDS_THUMB[boardId];
                smallImageText = MP2_BOARDS[boardId];
            }
        }
        else
        {
            state = "Setting Up...";
        }
    }
    else if (is_mario_party_1(l_DiscordHeader, l_DiscordSettings))
    {
        imageKey = "box-mp1";
        imageText = "Mario Party";

        const uint8_t boardId = safe_read_rdram(rdram, MP1_MEM_BOARD);
        const uint8_t currentTurn = safe_read_rdram(rdram, MP1_MEM_CURRENT_TURN);
        const uint8_t totalTurns = safe_read_rdram(rdram, MP1_MEM_TOTAL_TURNS);

        if (totalTurns > 0 && totalTurns <= 50 && currentTurn <= totalTurns &&
            boardId < (sizeof(MP1_BOARDS) / sizeof(MP1_BOARDS[0])))
        {
            if (boardId != 10)
            {
                state = MP1_BOARDS[boardId];
                if (turnInfo.valid)
                {
                    state += " Turn: " + std::to_string(turnInfo.current) + "/" + std::to_string(turnInfo.total);
                }
            }
            else
            {
                state = "Mini-Game Island";
            }

            if (boardId < (sizeof(MP1_BOARDS_THUMB) / sizeof(MP1_BOARDS_THUMB[0])))
            {
                smallImageKey = MP1_BOARDS_THUMB[boardId];
                smallImageText = MP1_BOARDS[boardId];
            }
        }
        else
        {
            state = "Setting Up...";
        }
    }
    else if (is_super_smash_bros(l_DiscordHeader, l_DiscordSettings))
    {
        const uint8_t stageId = safe_read_rdram(rdram, SSB_MEM_STAGE);
        imageKey = "box-ssb";
        imageText = "Super Smash Bros.";
        if (stageId < (sizeof(SSB_STAGES) / sizeof(SSB_STAGES[0])))
        {
            state = SSB_STAGES[stageId];
        }
    }
    else if (is_mario_kart_64(l_DiscordHeader, l_DiscordSettings))
    {
        const uint8_t cupId = safe_read_rdram(rdram, MK64_MEM_CUP);
        const uint8_t musicId = safe_read_rdram(rdram, MK64_MEM_MUSIC);
        const uint8_t speedId = safe_read_rdram(rdram, MK64_MEM_SPEED);
        const uint8_t trackId = safe_read_rdram(rdram, MK64_MEM_TRACK);
        imageKey = "box-mk64";
        imageText = "Mario Kart 64";

        if (musicId > 2 && cupId < 5 && trackId < 5)
        {
            const char* speed = (speedId < (sizeof(MK64_SPEEDS) / sizeof(MK64_SPEEDS[0]))) ? MK64_SPEEDS[speedId] : MK64_SPEEDS[0];
            state = MK64_TRACKS[(cupId * 4) + trackId];
            details += " (" + std::string(speed) + ")";
        }
    }
    else
    {
        state = !l_DiscordHeader.Region.empty() ? l_DiscordHeader.Region : std::string("In game");
        imageKey = !l_DiscordHeader.GameID.empty() ? l_DiscordHeader.GameID.c_str() : "rmg";
    }

    // Build structure parameters directly targeting the global Discord API
    DiscordRichPresence presence = {};
    presence.details = details.c_str();
    presence.state = state.c_str();
    presence.startTimestamp = l_DiscordStartTimestamp;
    presence.largeImageKey = imageKey;
    presence.largeImageText = imageText;
    presence.smallImageKey = smallImageKey;
    presence.smallImageText = smallImageText;
    presence.instance = 1;

    Discord_UpdatePresence(&presence);
    Discord_RunCallbacks();
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

    l_DiscordHeader = header;
    l_DiscordSettings = settings;
    l_DiscordRomCached = true;
    l_DiscordStartTimestamp = current_timestamp();
    l_DiscordNextPost = l_DiscordStartTimestamp;

    const std::string details = preferred_title(header, settings);
    std::string state = !header.Region.empty() ? header.Region : "In game";
    const char* imageKey = !header.GameID.empty() ? header.GameID.c_str() : "rmg";

    if (is_mario_party_3(header, settings))
    {
        state = "Mario Party 3";
        imageKey = "box-mp3";
    }
    else if (is_mario_party_2(header, settings))
    {
        state = "Mario Party 2";
        imageKey = "box-mp2";
    }
    else if (is_mario_party_1(header, settings))
    {
        state = "Mario Party";
        imageKey = "box-mp1";
    }
    else if (is_super_smash_bros(header, settings))
    {
        state = "Super Smash Bros.";
        imageKey = "box-ssb";
    }
    else if (is_mario_kart_64(header, settings))
    {
        state = "Mario Kart 64";
        imageKey = "box-mk64";
    }

    DiscordRichPresence presence = {};
    presence.details = details.c_str();
    presence.state = state.c_str();
    presence.startTimestamp = l_DiscordStartTimestamp;
    presence.largeImageKey = imageKey;
    presence.largeImageText = details.c_str();
    presence.instance = 1;

    Discord_UpdatePresence(&presence);
    Discord_RunCallbacks();

    return true;
}

void CoreDiscordUpdateFrame(int frame)
{
    (void)frame;

    if (!l_DiscordInitialized || !l_DiscordRomCached)
    {
        return;
    }

    if (!CoreSettingsGetBoolValue(SettingsID::GUI_EnableDiscordRPC))
    {
        Discord_ClearPresence();
        Discord_RunCallbacks();
        Discord_Shutdown();
        l_DiscordInitialized = false;
        return;
    }

    if (current_timestamp() < l_DiscordNextPost)
    {
        return;
    }

    l_DiscordNextPost = current_timestamp() + 5;
    update_presence_from_memory();
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
    l_DiscordRomCached = false;
}