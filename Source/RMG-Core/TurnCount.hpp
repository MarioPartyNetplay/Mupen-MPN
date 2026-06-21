/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_TURNCOUNT_HPP
#define CORE_TURNCOUNT_HPP

#include "RomHeader.hpp"
#include "RomSettings.hpp"

#include <string>

struct CoreTurnCountInfo
{
    bool valid = false;
    int current = 0;
    int total = 0;
};

void CoreTurnCountSetRom(const CoreRomHeader& header, const CoreRomSettings& settings);
void CoreTurnCountClearRom(void);
void CoreTurnCountUpdateFrame(void);
CoreTurnCountInfo CoreTurnCountGetInfo(void);
std::string CoreGetTurnCountOverlayText(void);

#endif // CORE_TURNCOUNT_HPP
