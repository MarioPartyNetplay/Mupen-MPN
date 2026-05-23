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
#include "Library.hpp"
#include "Error.hpp"

#include "m64p/Api.hpp"


//
// Local Variables
//

static bool l_HasInitNetplay = false;
static bool l_EmbeddedNetplayActive = false;
static int l_EmbeddedNetplayLocalPlayerSlot = 0;
static CoreEmbeddedNetplaySubmitInputCallback l_EmbeddedSubmitInputCallback = nullptr;
static CoreEmbeddedNetplayGetInputCallback l_EmbeddedGetInputCallback = nullptr;
static CoreEmbeddedNetplayAdvanceFrameCallback l_EmbeddedAdvanceFrameCallback = nullptr;

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