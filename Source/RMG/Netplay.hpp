/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef NETPLAY_COORDINATOR_HPP
#define NETPLAY_COORDINATOR_HPP

#include <cstdint>

class QByteArray;

// Forward declarations for Emulation.cpp integration
namespace UserInterface::Netplay {

class NetplayCoordinator;

/**
 * Global netplay coordinator instance
 * Set/accessed by MainWindow during game startup
 */
extern NetplayCoordinator* g_netplayCoordinator;

/**
 * Check if netplay is active
 */
bool isNetplayActive();

/**
 * Returns true when focus-loss pause/resume should be ignored.
 */
bool shouldBlockFocusLossPause();

/**
 * Returns true when modification cheats must not be edited.
 */
bool shouldBlockModifications();

/**
 * Submit frame input during emulation
 * Called from emulation loop with current controller state
 */
void submitNetplayFrameInput(uint32_t controllerState);

/**
 * Get inputs for current frame (lockstep mode)
 * Called from input plugin/emulation to get synchronized inputs
 */
uint32_t getNetplayFrameInput(int playerSlot);

/**
 * Advance frame synchronously across all peers
 * Called at end of frame from emulation loop
 * Returns true if all inputs received, false if timeout fallback occurred
 */
bool advanceNetplayFrame();

/**
 * Sample CPU state and publish frame sync after the emulated frame completes.
 * Called from the video extension at buffer swap (not during input polling).
 */
void submitNetplayEndOfFrameSync();

/**
 * Verify game synchronization
 * Called periodically with ROM state checksum
 * May trigger desync recovery
 */
void verifyNetplaySync(uint32_t romChecksum);

/**
 * Notify netplay of save state (for resync operations)
 */
void notifyNetplaySaveState(const QByteArray& saveState);

/**
 * Install the core bridge callbacks used during embedded lockstep netplay.
 */
void installEmbeddedNetplayCallbacks();

} // namespace UserInterface::Netplay

#endif // NETPLAY_COORDINATOR_HPP
