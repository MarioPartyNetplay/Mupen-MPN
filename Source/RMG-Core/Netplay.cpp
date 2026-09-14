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
#ifdef _WIN32
#define _CRT_RAND_S
#include <cstdlib>
#endif // _WIN32
#include "Netplay.hpp"
#include "CachedRomHeaderAndSettings.hpp"
#include "RomSettings.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Error.hpp"
#include "Emulation.hpp"
#include "Plugins.hpp"
#include "Netplay/LockstepEngine.hpp"

#include <algorithm>
#include <cstring>

#include "m64p/Api.hpp"


//
// Local Variables
//

static bool l_HasInitNetplay = false;
static bool l_EmbeddedNetplayActive = false;
static int l_EmbeddedNetplayLocalPlayerSlot = 0;
static int l_EmbeddedNetplayInputDelayFrames = 6;
static CoreEmbeddedNetplaySubmitInputCallback l_EmbeddedSubmitInputCallback = nullptr;
static CoreEmbeddedNetplayGetInputCallback l_EmbeddedGetInputCallback = nullptr;
static CoreEmbeddedNetplayAdvanceFrameCallback l_EmbeddedAdvanceFrameCallback = nullptr;
static CoreNetplaySyncSettings l_NetplaySyncSettings = {};
static bool l_HasNetplaySyncSettings = false;

//
// Local Functions
//

static constexpr int kNetplayCpuEmulatorDynarec = 2;

static std::string basename_only(std::string path)
{
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos)
    {
        path = path.substr(slash + 1);
    }
    return path;
}

static std::string resolve_plugin_name(CorePluginType type, const std::string& md5)
{
    SettingsID gameSetting = SettingsID::Game_GFX_Plugin;
    SettingsID coreSetting = SettingsID::Core_GFX_Plugin;
    if (type == CorePluginType::Rsp)
    {
        gameSetting = SettingsID::Game_RSP_Plugin;
        coreSetting = SettingsID::Core_RSP_Plugin;
    }

    std::string fileName;
    if (!md5.empty())
    {
        fileName = CoreSettingsGetStringValue(gameSetting, md5);
    }
    if (fileName.empty())
    {
        fileName = CoreSettingsGetStringValue(coreSetting);
    }
    fileName = basename_only(std::move(fileName));
    if (fileName.empty())
    {
        return {};
    }

    for (const CorePlugin& plugin : CoreGetAllPlugins())
    {
        if (plugin.Type == type && plugin.File == fileName)
        {
            return plugin.Name;
        }
    }
    return fileName;
}

static void apply_synced_core_config(const CoreNetplaySyncSettings& sync)
{
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, false);
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, sync.countPerOp);
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, sync.countPerOpDenomPot);
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, sync.disableExtraMem);
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, sync.siDmaDuration);
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, kNetplayCpuEmulatorDynarec);
    CoreSettingsSetValue(std::string("Core"), std::string("NoCompiledJump"), false);
    CoreSettingsSetValue(SettingsID::Core_EnableDebugger, false);
    CoreSettingsSetValue(SettingsID::Core_GbCameraVideoCaptureBackend1, std::string(""));

    // Keep GLideN64 RDRAM feedback identical even if Emulation overlay runs later.
    const std::string glide("Video-GLideN64");
    CoreSettingsSetValue(glide, std::string("DisableFBInfo"), true);
    CoreSettingsSetValue(glide, std::string("EnableCopyColorToRDRAM"), 0);
    CoreSettingsSetValue(glide, std::string("EnableCopyDepthToRDRAM"), 0);
    CoreSettingsSetValue(glide, std::string("EnableCopyAuxiliaryToRDRAM"), false);
    CoreSettingsSetValue(glide, std::string("EnableCopyColorFromRDRAM"), false);
    CoreSettingsSetValue(glide, std::string("EnableCustomSettings"), false);
    CoreSettingsSetValue(glide, std::string("ThreadedVideo"), false);
}

//
// Exported Functions
//

CORE_EXPORT bool CoreInitNetplay(std::string address, int port, int player)
{
#ifdef NETPLAY
    std::string error;
    m64p_error ret;
    uint32_t id = 0;
    int requestedPlayer = player;

    if (requestedPlayer < 1)
    {
        requestedPlayer = 1;
    }
    else if (requestedPlayer > 4)
    {
        requestedPlayer = 4;
    }

    // initialize random ID
    while (id == 0)
    {
#ifdef _WIN32
        rand_s(&id);
#else
        id = rand();
#endif
        id &= ~0x7;
        id |= requestedPlayer;
    }

    uint32_t version;
    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_GET_VERSION, 0x010001, &version);
    if (ret != M64ERR_SUCCESS)
    { 
        error = "CoreInitNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_GET_VERSION) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_INIT, port, const_cast<char*>(address.c_str()));
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreInitNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_INIT) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    auto tryRegisterPlayer = [&](int playerIndex, uint32_t& playerId) -> m64p_error
    {
        playerId &= ~0x7;
        playerId |= static_cast<uint32_t>(playerIndex);
        return m64p::Core.DoCommand(M64CMD_NETPLAY_CONTROL_PLAYER, playerIndex, &playerId);
    };

    ret = tryRegisterPlayer(requestedPlayer, id);

    if (ret == M64ERR_INPUT_ASSERT)
    {
        // Requested slot is likely occupied; try all remaining slots before failing.
        for (int fallbackPlayer = 1; fallbackPlayer <= 4; fallbackPlayer++)
        {
            if (fallbackPlayer == requestedPlayer)
            {
                continue;
            }

            uint32_t fallbackId = id;
            m64p_error fallbackRet = tryRegisterPlayer(fallbackPlayer, fallbackId);
            if (fallbackRet == M64ERR_SUCCESS)
            {
                id = fallbackId;
                ret = M64ERR_SUCCESS;
                break;
            }
        }
    }

    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreInitNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_CONTROL_PLAYER) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        CoreShutdownNetplay();
        return false;
    }

    l_HasInitNetplay = true;
    return true;
#else
    return false;
#endif // NETPLAY
}

CORE_EXPORT bool CoreHasInitNetplay(void)
{
#ifdef NETPLAY
    return l_HasInitNetplay;
#else
    return false;
#endif // NETPLAY
}

CORE_EXPORT bool CoreShutdownNetplay(void)
{
#ifdef NETPLAY
    std::string error;
    m64p_error ret;

    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_CLOSE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreShutdownNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_CLOSE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    l_HasInitNetplay = false;
    return true;
#else
    return false;
#endif // NETPLAY
}

CORE_EXPORT void CoreSetEmbeddedNetplayState(bool active, int localPlayerSlot)
{
    l_EmbeddedNetplayActive = active;

    if (localPlayerSlot < 0)
    {
        l_EmbeddedNetplayLocalPlayerSlot = 0;
    }
    else if (localPlayerSlot > 3)
    {
        l_EmbeddedNetplayLocalPlayerSlot = 3;
    }
    else
    {
        l_EmbeddedNetplayLocalPlayerSlot = localPlayerSlot;
    }
}

CORE_EXPORT bool CoreIsEmbeddedNetplayActive(void)
{
    return l_EmbeddedNetplayActive;
}

CORE_EXPORT int CoreGetEmbeddedNetplayLocalPlayerSlot(void)
{
    return l_EmbeddedNetplayLocalPlayerSlot;
}

CORE_EXPORT void CoreSetEmbeddedNetplayInputDelayFrames(int frames)
{
    if (frames < 1) {
        frames = 1;
    } else if (frames > 99) {
        frames = 99;
    }

    l_EmbeddedNetplayInputDelayFrames = frames;
}

CORE_EXPORT int CoreGetEmbeddedNetplayInputDelayFrames(void)
{
    return l_EmbeddedNetplayInputDelayFrames;
}

CORE_EXPORT int CoreGetEmbeddedNetplayInputWaitTimeoutMs(void)
{
    return RMGCore::LockstepEngine::stallTimeoutForDelayFrames(
        l_EmbeddedNetplayInputDelayFrames);
}

CORE_EXPORT int CoreGetEmbeddedNetplayMaxFrameAdvanceWaitMs(void)
{
    const int stallTimeout =
        RMGCore::LockstepEngine::stallTimeoutForDelayFrames(
            l_EmbeddedNetplayInputDelayFrames);
    // Soft upper bound for UI/plugin helpers only. Lockstep itself waits
    // indefinitely for active peers rather than inventing fallback inputs.
    return std::max(stallTimeout, 2500);
}

CORE_EXPORT void CoreSetEmbeddedNetplayCallbacks(
    CoreEmbeddedNetplaySubmitInputCallback submitInput,
    CoreEmbeddedNetplayGetInputCallback getInput,
    CoreEmbeddedNetplayAdvanceFrameCallback advanceFrame)
{
    l_EmbeddedSubmitInputCallback = submitInput;
    l_EmbeddedGetInputCallback = getInput;
    l_EmbeddedAdvanceFrameCallback = advanceFrame;
}

CORE_EXPORT void CoreSubmitEmbeddedNetplayFrameInput(uint32_t controllerState)
{
    if (l_EmbeddedSubmitInputCallback != nullptr)
    {
        l_EmbeddedSubmitInputCallback(controllerState);
    }
}

CORE_EXPORT uint32_t CoreGetEmbeddedNetplayFrameInput(int playerSlot)
{
    if (l_EmbeddedGetInputCallback != nullptr)
    {
        return l_EmbeddedGetInputCallback(playerSlot);
    }

    return 0;
}

CORE_EXPORT bool CoreAdvanceEmbeddedNetplayFrame()
{
    if (l_EmbeddedAdvanceFrameCallback != nullptr)
    {
        return l_EmbeddedAdvanceFrameCallback();
    }

    return true;
}

// C ABI for plugins that cannot link RMG-Core as C++ (e.g. raphnetraw).
extern "C" {

CORE_EXPORT int RMG_Netplay_IsActive(void)
{
    return CoreIsEmbeddedNetplayActive() ? 1 : 0;
}

CORE_EXPORT int RMG_Netplay_LocalSlot(void)
{
    return CoreGetEmbeddedNetplayLocalPlayerSlot();
}

CORE_EXPORT void RMG_Netplay_SubmitInput(unsigned int controllerState)
{
    CoreSubmitEmbeddedNetplayFrameInput(controllerState);
}

CORE_EXPORT unsigned int RMG_Netplay_GetInput(int playerSlot)
{
    return CoreGetEmbeddedNetplayFrameInput(playerSlot);
}

CORE_EXPORT int RMG_Netplay_AdvanceFrame(void)
{
    return CoreAdvanceEmbeddedNetplayFrame() ? 1 : 0;
}

} // extern "C"

CORE_EXPORT bool CoreBuildNetplaySyncSettings(std::filesystem::path romPath, CoreNetplaySyncSettings& out)
{
    CoreRomType type = {};
    CoreRomHeader header = {};
    CoreRomSettings defaultSettings = {};
    CoreRomSettings gameSettings = {};

    if (!CoreGetCachedRomHeaderAndSettings(romPath, &type, &header, &defaultSettings, &gameSettings))
    {
        return false;
    }

    int countPerOp = CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOp);
    int countPerOpDenomPot = CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOpDenomPot);
    bool disableExtraMem = CoreSettingsGetBoolValue(SettingsID::CoreOverlay_DisableExtraMem);
    int siDmaDuration = CoreSettingsGetIntValue(SettingsID::CoreOverlay_SiDmaDuration);

    std::string section;
    const int format = CoreSettingsGetIntValue(SettingsID::Core_SaveFileNameFormat);
    if (format == 0)
    {
        section = gameSettings.InternalName;
    }
    else
    {
        section = gameSettings.MD5;
    }

    if (CoreSettingsGetBoolValue(SettingsID::Game_OverrideCoreSettings, section))
    {
        countPerOpDenomPot = CoreSettingsGetIntValue(SettingsID::Game_CountPerOpDenomPot, section);
    }

    if (gameSettings.DisableExtraMem)
    {
        disableExtraMem = true;
    }

    if (countPerOp <= 0)
    {
        countPerOp = gameSettings.CountPerOp > 0 ? gameSettings.CountPerOp : defaultSettings.CountPerOp;
    }

    if (siDmaDuration < 0)
    {
        siDmaDuration = gameSettings.SiDMADuration >= 0 ? gameSettings.SiDMADuration : defaultSettings.SiDMADuration;
    }

    if (countPerOp <= 0)
    {
        countPerOp = 2;
    }

    if (siDmaDuration < 0)
    {
        siDmaDuration = 2304;
    }

    out.countPerOp = countPerOp;
    out.countPerOpDenomPot = countPerOpDenomPot;
    out.disableExtraMem = disableExtraMem;
    out.siDmaDuration = siDmaDuration;
    out.cpuEmulator = kNetplayCpuEmulatorDynarec;
    out.saveType = gameSettings.SaveType;
    out.transferPak = gameSettings.TransferPak;
    out.rspPluginName = resolve_plugin_name(CorePluginType::Rsp, gameSettings.MD5);
    out.gfxPluginName = resolve_plugin_name(CorePluginType::Gfx, gameSettings.MD5);
    out.valid = true;
    return true;
}

CORE_EXPORT void CoreSetNetplaySyncSettings(const CoreNetplaySyncSettings& settings)
{
    l_NetplaySyncSettings = settings;
    l_NetplaySyncSettings.cpuEmulator = kNetplayCpuEmulatorDynarec;
    l_HasNetplaySyncSettings = settings.valid;
}

CORE_EXPORT void CoreClearNetplaySyncSettings(void)
{
    l_NetplaySyncSettings = {};
    l_HasNetplaySyncSettings = false;
}

CORE_EXPORT bool CoreHasNetplaySyncSettings(void)
{
    return l_HasNetplaySyncSettings;
}

CORE_EXPORT bool CoreGetNetplaySyncSettings(CoreNetplaySyncSettings& out)
{
    if (!l_HasNetplaySyncSettings)
    {
        return false;
    }

    out = l_NetplaySyncSettings;
    return true;
}

CORE_EXPORT void CoreApplyNetplaySyncedRomSettings(void)
{
    if (!l_HasNetplaySyncSettings)
    {
        return;
    }

    CoreRomSettings romSettings;
    if (!CoreGetCurrentRomSettings(romSettings))
    {
        return;
    }

    romSettings.CountPerOp = l_NetplaySyncSettings.countPerOp;
    romSettings.SiDMADuration = l_NetplaySyncSettings.siDmaDuration;
    romSettings.DisableExtraMem = l_NetplaySyncSettings.disableExtraMem ? 1 : 0;
    romSettings.SaveType = static_cast<uint16_t>(l_NetplaySyncSettings.saveType);
    romSettings.TransferPak = l_NetplaySyncSettings.transferPak;
    CoreApplyRomSettings(romSettings);
}

CORE_EXPORT void CoreApplyNetplaySyncedCoreSettings(void)
{
    if (!l_HasNetplaySyncSettings)
    {
        return;
    }

    apply_synced_core_config(l_NetplaySyncSettings);
}

namespace {

constexpr int kGprRegisterCount = 32;
constexpr int kCp0RegisterCount = 32;
constexpr int kCp1FgrCount = 32;
// Timer/interrupt CP0 tracks cycle skew, not gameplay. MP1 extra SI polls
// nudge COUNT/COMPARE/CAUSE/RANDOM even when GPRs and PC still match, which
// showed up as on/off HUD desyncs. Skip r0 (hardwired 0) the same way.
constexpr int kCp0RandomReg = 1;
constexpr int kCp0CountReg = 9;
constexpr int kCp0CompareReg = 11;
constexpr int kCp0CauseReg = 13;
// Sparse RDRAM sample catches video-plugin feedback splits without hashing 4–8MB.
constexpr size_t kRdramSampleStride = 4096;
constexpr size_t kRdramSampleBytes = 4 * 1024 * 1024;

uint32_t fnv1a32(uint32_t hash, uint32_t value)
{
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

uint32_t fnv1a32_u64(uint32_t hash, uint64_t value)
{
    hash = fnv1a32(hash, static_cast<uint32_t>(value));
    hash = fnv1a32(hash, static_cast<uint32_t>(value >> 32));
    return hash;
}

bool isVolatileCp0Register(int index)
{
    return index == kCp0RandomReg ||
           index == kCp0CountReg ||
           index == kCp0CompareReg ||
           index == kCp0CauseReg;
}

} // namespace

CORE_EXPORT uint32_t CoreGetNetplayFrameSyncHash(void)
{
    if (!m64p::Core.IsHooked() || m64p::Core.DebugGetCPUDataPtr == nullptr)
    {
        return 0;
    }

    if (!CoreIsEmulationRunning())
    {
        return 0;
    }

    // GPRs/HI/LO are int64_t. Hashing them as uint32_t[32] only covered the
    // first 16 registers and mixed in dirty upper halves that dynarec does
    // not keep consistent across machines.
    const auto* const gpr =
        static_cast<const int64_t*>(m64p::Core.DebugGetCPUDataPtr(M64P_CPU_REG_REG));
    const auto* const hi =
        static_cast<const int64_t*>(m64p::Core.DebugGetCPUDataPtr(M64P_CPU_REG_HI));
    const auto* const lo =
        static_cast<const int64_t*>(m64p::Core.DebugGetCPUDataPtr(M64P_CPU_REG_LO));
    const auto* const cp0 =
        static_cast<const uint32_t*>(m64p::Core.DebugGetCPUDataPtr(M64P_CPU_REG_COP0));
    const auto* const pc =
        static_cast<const uint32_t*>(m64p::Core.DebugGetCPUDataPtr(M64P_CPU_PC));
    const auto* const fgr =
        static_cast<const uint64_t*>(m64p::Core.DebugGetCPUDataPtr(M64P_CPU_REG_COP1_FGR_64));

    if (gpr == nullptr ||
        hi == nullptr ||
        lo == nullptr ||
        cp0 == nullptr ||
        pc == nullptr)
    {
        return 0;
    }

    uint32_t hash = 2166136261u;
    for (int i = 1; i < kGprRegisterCount; ++i)
    {
        // Low 32 only: dynarec leaves dirty upper halves inconsistent across hosts.
        hash = fnv1a32(hash, static_cast<uint32_t>(gpr[i]));
    }
    hash = fnv1a32(hash, static_cast<uint32_t>(*hi));
    hash = fnv1a32(hash, static_cast<uint32_t>(*lo));

    for (int i = 0; i < kCp0RegisterCount; ++i)
    {
        if (isVolatileCp0Register(i))
        {
            continue;
        }
        hash = fnv1a32(hash, cp0[i]);
    }

    hash = fnv1a32(hash, *pc);

    if (fgr != nullptr)
    {
        for (int i = 0; i < kCp1FgrCount; ++i)
        {
            hash = fnv1a32_u64(hash, fgr[i]);
        }
    }

    if (m64p::Core.DebugMemGetPointer != nullptr)
    {
        const auto* const rdram =
            static_cast<const uint8_t*>(m64p::Core.DebugMemGetPointer(M64P_DBG_PTR_RDRAM));
        if (rdram != nullptr)
        {
            for (size_t offset = 0; offset + 4 <= kRdramSampleBytes; offset += kRdramSampleStride)
            {
                uint32_t word = 0;
                std::memcpy(&word, rdram + offset, sizeof(word));
                hash = fnv1a32(hash, word);
            }
        }
    }

    return hash;
}