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

#include "TurnCount.hpp"
#include "Library.hpp"
#include "Directories.hpp"
#include "Settings.hpp"
#include "m64p/Api.hpp"
#include "MPNMemory.h"

#include <fstream>

namespace
{
static bool l_RomCached = false;
static CoreRomHeader l_Header;
static CoreRomSettings l_Settings;
static CoreTurnCountInfo l_Info;
static std::string l_OverlayText;
static std::string l_LastFileText;

static bool contains_title(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
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

static CoreTurnCountInfo read_turn_count(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    CoreTurnCountInfo info;

    const uint8_t* rdram = get_rdram_pointer();
    if (rdram == nullptr)
    {
        return info;
    }

    if (is_mario_party_3(header, settings))
    {
        const uint8_t boardId = safe_read_rdram(rdram, MP3_MEM_BOARD);
        const uint8_t gameType = safe_read_rdram(rdram, MP3_MEM_GAMETYPE);
        const uint8_t currentTurn = safe_read_rdram(rdram, MP3_MEM_CURRENT_TURN);
        const uint8_t totalTurns = safe_read_rdram(rdram, MP3_MEM_TOTAL_TURNS);

        if (gameType >= 1 && gameType <= 6 && totalTurns > 0 &&
            boardId < (sizeof(MP3_BOARDS) / sizeof(MP3_BOARDS[0])) &&
            gameType != 5 && gameType != 6)
        {
            info.valid = true;
            info.current = currentTurn;
            info.total = totalTurns;
        }
    }
    else if (is_mario_party_2(header, settings))
    {
        const uint8_t boardId = safe_read_rdram(rdram, MP2_MEM_BOARD);
        const uint8_t currentTurn = safe_read_rdram(rdram, MP2_MEM_CURRENT_TURN);
        const uint8_t totalTurns = safe_read_rdram(rdram, MP2_MEM_TOTAL_TURNS);
        const uint8_t gameState = safe_read_rdram(rdram, MP2_MEM_GAMESTATE);

        if (gameState > 1 && totalTurns > 0 && boardId != 7 &&
            boardId < (sizeof(MP2_BOARDS) / sizeof(MP2_BOARDS[0])))
        {
            info.valid = true;
            info.current = currentTurn;
            info.total = totalTurns;
        }
    }
    else if (is_mario_party_1(header, settings))
    {
        const uint8_t boardId = safe_read_rdram(rdram, MP1_MEM_BOARD);
        const uint8_t currentTurn = safe_read_rdram(rdram, MP1_MEM_CURRENT_TURN);
        const uint8_t totalTurns = safe_read_rdram(rdram, MP1_MEM_TOTAL_TURNS);

        if (totalTurns > 0 && totalTurns <= 50 && currentTurn <= totalTurns && boardId != 10 &&
            boardId < (sizeof(MP1_BOARDS) / sizeof(MP1_BOARDS[0])))
        {
            info.valid = true;
            info.current = currentTurn;
            info.total = totalTurns;
        }
    }

    return info;
}

static std::string format_overlay_text(const CoreTurnCountInfo& info)
{
    if (!info.valid)
    {
        return "";
    }

    return "Turn: " + std::to_string(info.current) + " / " + std::to_string(info.total);
}

static void write_turncount_file(const std::string& text)
{
    if (text == l_LastFileText)
    {
        return;
    }

    l_LastFileText = text;

    const std::filesystem::path filePath = CoreGetUserDataDirectory() / "TurnCount.txt";
    std::ofstream fileStream(filePath);
    if (!fileStream.is_open())
    {
        return;
    }

    fileStream << text;
}
} // namespace

CORE_EXPORT void CoreTurnCountSetRom(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    l_Header = header;
    l_Settings = settings;
    l_RomCached = true;
    l_Info = {};
    l_OverlayText.clear();
    l_LastFileText.clear();
}

CORE_EXPORT void CoreTurnCountClearRom(void)
{
    l_RomCached = false;
    l_Info = {};
    l_OverlayText.clear();
    write_turncount_file("");
}

CORE_EXPORT void CoreTurnCountUpdateFrame(void)
{
    if (!l_RomCached)
    {
        return;
    }

    l_Info = read_turn_count(l_Header, l_Settings);
    l_OverlayText = format_overlay_text(l_Info);

    if (CoreSettingsGetBoolValue(SettingsID::GUI_TurnCountFile))
    {
        write_turncount_file(l_OverlayText);
    }
    else if (!l_LastFileText.empty())
    {
        write_turncount_file("");
    }
}

CORE_EXPORT CoreTurnCountInfo CoreTurnCountGetInfo(void)
{
    return l_Info;
}

CORE_EXPORT std::string CoreGetTurnCountOverlayText(void)
{
    return l_OverlayText;
}
