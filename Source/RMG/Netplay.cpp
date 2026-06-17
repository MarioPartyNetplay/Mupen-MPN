/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Netplay.hpp"
#include <RMG-Core/Netplay.hpp>

#ifdef WEBRTC_P2P
#include "Netplay/NetplayCoordinator.hpp"
#include <QDebug>

using namespace UserInterface::Netplay;

namespace {
void CoreEmbeddedSubmitInputCallback(uint32_t controllerState)
{
    UserInterface::Netplay::submitNetplayFrameInput(controllerState);
}

uint32_t CoreEmbeddedGetInputCallback(int playerSlot)
{
    return UserInterface::Netplay::getNetplayFrameInput(playerSlot);
}

bool CoreEmbeddedAdvanceFrameCallback()
{
    return UserInterface::Netplay::advanceNetplayFrame();
}

struct CoreEmbeddedNetplayBridgeInitializer
{
    CoreEmbeddedNetplayBridgeInitializer()
    {
        CoreSetEmbeddedNetplayCallbacks(
            CoreEmbeddedSubmitInputCallback,
            CoreEmbeddedGetInputCallback,
            CoreEmbeddedAdvanceFrameCallback);
    }
};

CoreEmbeddedNetplayBridgeInitializer s_coreEmbeddedNetplayBridgeInitializer;
} // namespace

// Global netplay coordinator instance
NetplayCoordinator* UserInterface::Netplay::g_netplayCoordinator = nullptr;

bool UserInterface::Netplay::isNetplayActive()
{
    return g_netplayCoordinator != nullptr && 
           g_netplayCoordinator->isInGame();
}

void UserInterface::Netplay::submitNetplayFrameInput(uint32_t controllerState)
{
    if (g_netplayCoordinator && g_netplayCoordinator->isInGame()) {
        g_netplayCoordinator->submitFrameInput(controllerState);
    }
}

uint32_t UserInterface::Netplay::getNetplayFrameInput(int playerSlot)
{
    if (!g_netplayCoordinator || !g_netplayCoordinator->isInGame()) {
        return 0;
    }

    return g_netplayCoordinator->getSyncedInput(playerSlot);
}

bool UserInterface::Netplay::advanceNetplayFrame()
{
    if (!g_netplayCoordinator || !g_netplayCoordinator->isInGame()) {
        return true;
    }

    return g_netplayCoordinator->advanceFrame();
}

void UserInterface::Netplay::verifyNetplaySync(uint32_t romChecksum)
{
    if (g_netplayCoordinator && g_netplayCoordinator->isInGame()) {
        g_netplayCoordinator->verifyGameSync(romChecksum);
    }
}

void UserInterface::Netplay::notifyNetplaySaveState(const QByteArray& saveState)
{
    if (g_netplayCoordinator && g_netplayCoordinator->isInGame()) {
        // TODO: Send save state for resync operations
        qDebug() << "NetplayCoordinator: Save state notified, size:" << saveState.size();
    }
}

#else

// Stubs for non-WEBRTC_P2P builds
NetplayCoordinator* UserInterface::Netplay::g_netplayCoordinator = nullptr;

bool UserInterface::Netplay::isNetplayActive()
{
    return false;
}

void UserInterface::Netplay::submitNetplayFrameInput(uint32_t)
{
}

uint32_t UserInterface::Netplay::getNetplayFrameInput(int)
{
    return 0;
}

bool UserInterface::Netplay::advanceNetplayFrame()
{
    return true;
}

void UserInterface::Netplay::verifyNetplaySync(uint32_t)
{
}

void UserInterface::Netplay::notifyNetplaySaveState(const QByteArray&)
{
}

#endif // WEBRTC_P2P
