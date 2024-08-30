/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_CHEATS_HPP
#define CORE_CHEATS_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

struct CoreCheatCode
{
    uint32_t Address = 0;
    int32_t  Value   = 0;
    bool UseOptions  = false;
    int  OptionIndex = 0;
    int  OptionSize  = 0;

    bool operator==(const CoreCheatCode& other) const
    {
        return Address == other.Address &&
                Value == other.Value &&
                UseOptions == other.UseOptions &&
                OptionIndex == other.OptionIndex &&
                OptionSize == other.OptionSize;
    }
};

struct CoreCheatOption
{
    std::string Name;
    uint32_t    Value = 0;
    int32_t     Size  = 0;

    bool operator==(const CoreCheatOption& other) const
    {
        return Name == other.Name &&
                Value == other.Value &&
                Size == other.Size;
    }
};

struct CoreCheat
{
    std::string Name;
    std::string Author;
    std::string Note;
    bool HasOptions = false;
    std::vector<CoreCheatOption> CheatOptions;
    std::vector<CoreCheatCode> CheatCodes;

    bool operator==(const CoreCheat& other) const
    {
        return Name == other.Name &&
                Author == other.Author &&
                Note == other.Note &&
                HasOptions == other.HasOptions &&
                CheatOptions == other.CheatOptions &&
                CheatCodes == other.CheatCodes;
    }
};

struct CoreCheatFile
{
    uint32_t CRC1 = 0;
    uint32_t CRC2 = 0;
    uint32_t CountryCode = 0;
    std::string MD5;
    std::string Name;
    std::vector<CoreCheat> Cheats;
};

bool CoreGetCurrentCheats(std::vector<CoreCheat>& cheats);
bool CoreParseCheat(const std::vector<std::string>& lines, CoreCheat& cheat);
bool CoreGetCheatLines(CoreCheat cheat, std::vector<std::string>& codeLines, std::vector<std::string>& optionLines);
bool CoreAddCheat(CoreCheat cheat);
bool CoreUpdateCheat(CoreCheat oldCheat, CoreCheat newCheat);
bool CoreCanRemoveCheat(CoreCheat cheat);
bool CoreRemoveCheat(CoreCheat cheat);
bool CoreEnableCheat(CoreCheat cheat, bool enabled);
bool CoreIsCheatEnabled(CoreCheat cheat);
bool CoreHasCheatOptionSet(CoreCheat cheat);
bool CoreSetCheatOption(CoreCheat cheat, CoreCheatOption option);
bool CoreGetCheatOption(CoreCheat cheat, CoreCheatOption& option);
bool CoreResetCheatOption(CoreCheat cheat);
bool CoreApplyCheats(void);
bool CoreApplyCheatsRuntime(const std::vector<CoreCheat>& cheats);
bool CoreClearCheats(void);
bool CorePressGamesharkButton(bool enabled);
bool read_file_lines(std::filesystem::path file, std::vector<std::string>& lines);
bool parse_cheat_file(const std::vector<std::string>& lines, CoreCheatFile& cheatFile);

#endif // CORE_CHEATS_HPP