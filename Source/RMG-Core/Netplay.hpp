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

#endif // CORE_NETPLAY_HPP