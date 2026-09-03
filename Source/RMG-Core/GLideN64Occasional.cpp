/*
 * Mupen MPN - occasional GLideN64 setting overrides
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL

#include "GLideN64Occasional.hpp"
#include "Library.hpp"
#include "Settings.hpp"
#include "m64p/Api.hpp"
#include "MPNMemory.h"

namespace
{
constexpr const char* l_Section = "MPN-GLideN64";
constexpr const char* l_N64DepthCompareKey = "N64DepthCompare";
constexpr const char* l_HalosRemovalKey = "EnableHalosRemoval";
constexpr const char* l_NativeResTexrectsKey = "EnableNativeResTexrects";
constexpr int l_NoOverride = -1;
constexpr int l_DepthCompareCompatible = 2;
constexpr uint8_t l_Mp3CurtainCallId = 22;
constexpr uint8_t l_Mp2MiniId = 0x16;

static bool l_RomCached = false;
static bool l_IsMarioParty2 = false;
static bool l_IsMarioParty3 = false;
static bool l_Mp2OverrideActive = false;
static bool l_Mp3OverrideActive = false;

static bool contains_title(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
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

static uint8_t read_minigame_id(uint32_t offset)
{
    return safe_read_rdram(get_rdram_pointer(), offset);
}

static void set_override(const char* key, int value)
{
    CoreSettingsSetValue(l_Section, key, value);
}

static void clear_all_overrides(void)
{
    set_override(l_N64DepthCompareKey, l_NoOverride);
    set_override(l_HalosRemovalKey, l_NoOverride);
    set_override(l_NativeResTexrectsKey, l_NoOverride);
}

static void apply_mp2_override(bool enable)
{
    set_override(l_HalosRemovalKey, enable ? 1 : l_NoOverride);
    set_override(l_NativeResTexrectsKey, enable ? 1 : l_NoOverride);
}

static void apply_mp3_override(bool enable)
{
    set_override(l_N64DepthCompareKey, enable ? l_DepthCompareCompatible : l_NoOverride);
}
} // namespace

CORE_EXPORT void CoreGLideN64OccasionalSetRom(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    l_IsMarioParty2 = is_mario_party_2(header, settings);
    l_IsMarioParty3 = is_mario_party_3(header, settings);
    l_RomCached = true;
    l_Mp2OverrideActive = false;
    l_Mp3OverrideActive = false;
    clear_all_overrides();
}

CORE_EXPORT void CoreGLideN64OccasionalClearRom(void)
{
    if (l_Mp2OverrideActive || l_Mp3OverrideActive)
    {
        clear_all_overrides();
    }

    l_RomCached = false;
    l_IsMarioParty2 = false;
    l_IsMarioParty3 = false;
    l_Mp2OverrideActive = false;
    l_Mp3OverrideActive = false;
}

CORE_EXPORT void CoreGLideN64OccasionalUpdateFrame(void)
{
    if (!l_RomCached)
    {
        return;
    }

    if (l_IsMarioParty2)
    {
        const bool wantOverride = read_minigame_id(MP2_MEM_MINI_STATE) == l_Mp2MiniId;
        if (wantOverride != l_Mp2OverrideActive)
        {
            apply_mp2_override(wantOverride);
            l_Mp2OverrideActive = wantOverride;
        }
    }

    if (l_IsMarioParty3)
    {
        const bool wantOverride = read_minigame_id(MP3_MEM_MINI_STATE) == l_Mp3CurtainCallId;
        if (wantOverride != l_Mp3OverrideActive)
        {
            apply_mp3_override(wantOverride);
            l_Mp3OverrideActive = wantOverride;
        }
    }
}
