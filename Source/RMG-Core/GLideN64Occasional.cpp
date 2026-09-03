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
#include "Plugins.hpp"
#include "Settings.hpp"
#include "m64p/Api.hpp"
#include "MPNMemory.h"

#include <cstring>

namespace
{
constexpr const char* l_Section = "MPN-GLideN64";
constexpr const char* l_N64DepthCompareKey = "N64DepthCompare";
constexpr const char* l_HalosRemovalKey = "EnableHalosRemoval";
constexpr const char* l_NativeResTexrectsKey = "EnableNativeResTexrects";
constexpr int l_NoOverride = -1;
constexpr int l_DepthCompareCompatible = 2;
constexpr uint8_t l_Mp3CurtainCallId = 0x22;
constexpr uint8_t l_Mp2LightsOutId = 0x16;
#if defined(_MSC_VER) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
constexpr uint32_t l_N64ByteXor = 3;
#else
constexpr uint32_t l_N64ByteXor = 0;
#endif

#ifdef _WIN32
using MpnSetOverridesFn = void (__cdecl *)(int, int, int);
#else
using MpnSetOverridesFn = void (*)(int, int, int);
#endif

static bool l_RomCached = false;
static bool l_IsMarioParty2 = false;
static bool l_IsMarioParty3 = false;
static bool l_Mp2OverrideActive = false;
static bool l_Mp3OverrideActive = false;
static MpnSetOverridesFn l_SetOverrides = nullptr;

static bool contains_title(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

static bool game_id_prefix(const std::string& gameId, const char* prefix)
{
    return gameId.size() >= 3 && gameId.compare(0, 3, prefix) == 0;
}

static bool is_mario_party_2(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return game_id_prefix(header.GameID, "NMW") ||
           contains_title(header.Name, "MARIO PARTY 2") ||
           contains_title(settings.GoodName, "MarioParty2") ||
           contains_title(settings.GoodName, "Mario Party 2") ||
           contains_title(settings.InternalName, "MARIO PARTY 2") ||
           contains_title(settings.InternalName, "MarioParty2");
}

static bool is_mario_party_3(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    return game_id_prefix(header.GameID, "NMV") ||
           contains_title(header.Name, "MARIO PARTY 3") ||
           contains_title(settings.GoodName, "MarioParty3") ||
           contains_title(settings.GoodName, "Mario Party 3") ||
           contains_title(settings.InternalName, "MARIO PARTY 3") ||
           contains_title(settings.InternalName, "MarioParty3");
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

static uint8_t read_n64_u8(uint32_t offset)
{
    return safe_read_rdram(get_rdram_pointer(), offset ^ l_N64ByteXor);
}

static uint32_t read_n64_u32(uint32_t offset)
{
    const uint8_t* rdram = get_rdram_pointer();
    const uint32_t aligned = offset & ~3u;
    if (rdram == nullptr || aligned + 3 >= 0x800000)
    {
        return 0;
    }

    uint32_t word = 0;
    std::memcpy(&word, rdram + aligned, sizeof(word));
    return word;
}

static bool minigame_id_is(uint32_t offset, uint8_t id)
{
    if (read_n64_u8(offset) == id)
    {
        return true;
    }

    // Host-order fallback if the constant is already a swapped offset.
    if (safe_read_rdram(get_rdram_pointer(), offset) == id)
    {
        return true;
    }

    const uint32_t word = read_n64_u32(offset);
    return (word & 0xFFu) == id || ((word >> 24) & 0xFFu) == id;
}

static uint8_t mp2_lights_out_id(void)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(sizeof(MP2_MINIS) / sizeof(MP2_MINIS[0])); ++i)
    {
        if (std::strcmp(MP2_MINIS[i], "Lights Out") == 0)
        {
            return i;
        }
    }
    return l_Mp2LightsOutId;
}

static uint8_t mp3_curtain_call_id(void)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(sizeof(MP3_MINIS) / sizeof(MP3_MINIS[0])); ++i)
    {
        if (std::strcmp(MP3_MINIS[i], "Curtain Call") == 0)
        {
            return i;
        }
    }
    return l_Mp3CurtainCallId;
}

static void hook_gliden64_overrides(void)
{
    if (l_SetOverrides != nullptr)
    {
        return;
    }

    void* handle = CoreGetPluginLibraryHandle(CorePluginType::Gfx);
    if (handle == nullptr)
    {
        return;
    }

    l_SetOverrides = reinterpret_cast<MpnSetOverridesFn>(
        CoreGetLibrarySymbol(static_cast<CoreLibraryHandle>(handle), "MPN_GLideN64_SetOverrides"));
}

static void set_override(const char* key, int value)
{
    CoreSettingsSetValue(l_Section, key, value);
}

static void sync_plugin_overrides(void)
{
    const int depth = l_Mp3OverrideActive ? l_DepthCompareCompatible : l_NoOverride;
    const int halos = l_Mp2OverrideActive ? 1 : l_NoOverride;
    const int texrects = l_Mp2OverrideActive ? 1 : l_NoOverride;

    set_override(l_N64DepthCompareKey, depth);
    set_override(l_HalosRemovalKey, halos);
    set_override(l_NativeResTexrectsKey, texrects);

    hook_gliden64_overrides();
    if (l_SetOverrides != nullptr)
    {
        l_SetOverrides(depth, halos, texrects);
    }
}

static void clear_all_overrides(void)
{
    l_Mp2OverrideActive = false;
    l_Mp3OverrideActive = false;
    sync_plugin_overrides();
}
} // namespace

CORE_EXPORT void CoreGLideN64OccasionalSetRom(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    l_IsMarioParty2 = is_mario_party_2(header, settings);
    l_IsMarioParty3 = is_mario_party_3(header, settings);
    l_RomCached = true;
    l_SetOverrides = nullptr;
    hook_gliden64_overrides();
    clear_all_overrides();
}

CORE_EXPORT void CoreGLideN64OccasionalClearRom(void)
{
    if (l_Mp2OverrideActive || l_Mp3OverrideActive || l_SetOverrides != nullptr)
    {
        clear_all_overrides();
    }

    l_RomCached = false;
    l_IsMarioParty2 = false;
    l_IsMarioParty3 = false;
    l_Mp2OverrideActive = false;
    l_Mp3OverrideActive = false;
    l_SetOverrides = nullptr;
}

CORE_EXPORT void CoreGLideN64OccasionalUpdateFrame(void)
{
    if (!l_RomCached)
    {
        return;
    }

    bool changed = false;

    if (l_IsMarioParty2)
    {
        const bool wantOverride = minigame_id_is(MP2_MEM_MINI_STATE, mp2_lights_out_id());
        if (wantOverride != l_Mp2OverrideActive)
        {
            l_Mp2OverrideActive = wantOverride;
            changed = true;
        }
    }

    if (l_IsMarioParty3)
    {
        const bool wantOverride = minigame_id_is(MP3_MEM_MINI_STATE, mp3_curtain_call_id());
        if (wantOverride != l_Mp3OverrideActive)
        {
            l_Mp3OverrideActive = wantOverride;
            changed = true;
        }
    }

    if (changed)
    {
        sync_plugin_overrides();
    }
}
