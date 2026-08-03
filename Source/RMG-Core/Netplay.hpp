/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_NETPLAY_HPP
#define CORE_NETPLAY_HPP

#include <cstdint>
#include <filesystem>
#include <string>

// attempts to initialize netplay
bool CoreInitNetplay(std::string address, int port, int player);

// returns whether netplay has been initialized
bool CoreHasInitNetplay(void);

// attempts to shutdown netplay
bool CoreShutdownNetplay(void);

// sets whether embedded (P2P) netplay is active and which local player slot owns input [0..3]
void CoreSetEmbeddedNetplayState(bool active, int localPlayerSlot);

// returns whether embedded (P2P) netplay is active
bool CoreIsEmbeddedNetplayActive(void);

// returns local player slot [0..3] for embedded (P2P) netplay
int CoreGetEmbeddedNetplayLocalPlayerSlot(void);

// embedded netplay lockstep callback types
using CoreEmbeddedNetplaySubmitInputCallback = void(*)(uint32_t controllerState);
using CoreEmbeddedNetplayGetInputCallback = uint32_t(*)(int playerSlot);
using CoreEmbeddedNetplayAdvanceFrameCallback = bool(*)();

// registers embedded netplay lockstep callbacks used by input plugin
void CoreSetEmbeddedNetplayCallbacks(CoreEmbeddedNetplaySubmitInputCallback submitInput,
	CoreEmbeddedNetplayGetInputCallback getInput,
	CoreEmbeddedNetplayAdvanceFrameCallback advanceFrame);

// lockstep bridge used by plugins to submit/retrieve synchronized controller state
void CoreSubmitEmbeddedNetplayFrameInput(uint32_t controllerState);
uint32_t CoreGetEmbeddedNetplayFrameInput(int playerSlot);
bool CoreAdvanceEmbeddedNetplayFrame();

#ifdef __cplusplus
extern "C" {
#endif
/* Stable C ABI for input plugins that resolve symbols dynamically. */
int RMG_Netplay_IsActive(void);
int RMG_Netplay_LocalSlot(void);
void RMG_Netplay_SubmitInput(unsigned int controllerState);
unsigned int RMG_Netplay_GetInput(int playerSlot);
int RMG_Netplay_AdvanceFrame(void);
#ifdef __cplusplus
}
#endif

// core timing settings synced from host (matches classic mupen64plus netplay fields)
struct CoreNetplaySyncSettings
{
    int countPerOp = 0;
    int countPerOpDenomPot = 0;
    bool disableExtraMem = false;
    int siDmaDuration = -1;
    int cpuEmulator = 2;
    bool valid = false;
};

// builds resolved host core settings for the given ROM (before emulation starts)
bool CoreBuildNetplaySyncSettings(std::filesystem::path romPath, CoreNetplaySyncSettings& out);

// stores settings received from the host; cleared when the session ends
void CoreSetNetplaySyncSettings(const CoreNetplaySyncSettings& settings);
void CoreClearNetplaySyncSettings(void);
bool CoreHasNetplaySyncSettings(void);
bool CoreGetNetplaySyncSettings(CoreNetplaySyncSettings& out);

// applies synced ROM-side settings after CoreOpenRom (clients only)
void CoreApplyNetplaySyncedRomSettings(void);

// applies synced core config values before emulation starts (clients only)
void CoreApplyNetplaySyncedCoreSettings(void);

// hashes emulated CPU state (CP0 + PC) for embedded netplay desync checks
uint32_t CoreGetNetplayFrameSyncHash(void);

#endif // CORE_NETPLAY_HPP