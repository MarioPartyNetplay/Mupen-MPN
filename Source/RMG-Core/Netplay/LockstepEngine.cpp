/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "LockstepEngine.hpp"
#include "WebRTC/WebRTCDataChannel.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>

using namespace UserInterface::Netplay;
using namespace RMGCore;

LockstepEngine::LockstepEngine(const Config& config)
    : m_config(config)
    , m_currentFrameNumber(0)
    , m_isDesynchronized(false)
    , m_lastVerifiedFrame(0)
{
    if (config.numPlayers < 2 || config.numPlayers > 4) {
        std::cerr << "LockstepEngine: numPlayers must be 2-4, got " << config.numPlayers << std::endl;
    }

    m_dataChannels.resize(config.numPlayers);
}

LockstepEngine::~LockstepEngine()
{
    for (auto& channel : m_dataChannels) {
        if (channel) {
            channel->close();
        }
    }
}

void LockstepEngine::setCallbacks(Callbacks callbacks)
{
    m_callbacks = std::move(callbacks);
}

void LockstepEngine::setDataChannel(int peerSlot, std::shared_ptr<WebRTCDataChannel> channel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (peerSlot < 0 || peerSlot >= m_config.numPlayers) {
        std::cerr << "LockstepEngine: invalid peer slot " << peerSlot << std::endl;
        return;
    }

    m_dataChannels[peerSlot] = channel;

    if (channel) {
        channel->onBinaryMessageReceived = [this, peerSlot](const std::vector<uint8_t>& data) {
            onDataChannelBinaryMessageReceived(peerSlot, data);
        };
        channel->onClosed = [this, peerSlot]() {
            onDataChannelClosed(peerSlot);
        };
        channel->onError = [this, peerSlot](const std::string& error) {
            onDataChannelError(peerSlot, error);
        };
    }
}

void LockstepEngine::submitLocalInput(uint32_t controllerState) {
    broadcastInput(controllerState);

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_frameBuffer[m_currentFrameNumber].playerInputs[m_config.localPlayerSlot] = controllerState;
        m_frameReceived[m_config.localPlayerSlot] = true;
        m_lastKnownInputs[m_config.localPlayerSlot] = controllerState;
    }

    // --- THE FIX: BLOCK THE EMULATOR UNTIL NETWORK DATA ARRIVES ---
    if (m_config.numPlayers > 1) {
        bool ready = waitForAllInputs(m_currentFrameNumber, m_config.stallTimeoutMilliseconds);
        if (!ready) {
            applyTimeoutFallback(m_currentFrameNumber);
        }
    }

    // Now everyone's input is securely in the buffer. 
    // Fire the callback so Coordinator can save it for getSyncedInput!
    if (m_callbacks.frameReady) {
        m_callbacks.frameReady(m_currentFrameNumber, m_frameBuffer[m_currentFrameNumber].playerInputs);
    }
}

void LockstepEngine::submitRemoteInput(int fromSlot, uint32_t frameNumber, uint32_t controllerState)
{
    if (fromSlot < 0 || fromSlot >= m_config.numPlayers || fromSlot == m_config.localPlayerSlot) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Socket.IO-relayed inputs and direct WebRTC packets both reach this path.
    // Do not require a WebRTC channel here, or host-relayed inputs will be dropped
    // before they can enter the lockstep buffer.

    // Handle slight frame number mismatches (e.g., input for frame N when we expect N-1)
    // This provides tolerance for network jitter.
    uint32_t targetFrame = frameNumber;
    if (frameNumber < m_currentFrameNumber && (m_currentFrameNumber - frameNumber) <= 2) {
        // Input is slightly behind; store it but mark as out-of-order
        targetFrame = frameNumber;
    } else if (frameNumber >= m_currentFrameNumber) {
        // Input is current or future frame
        targetFrame = frameNumber;
    } else {
        // Input is too far in the past; skip it
        return;
    }

    FrameInputs& frameInputs = m_frameBuffer[targetFrame];
    frameInputs.frameNumber = targetFrame;
    frameInputs.playerInputs[fromSlot] = controllerState;
    frameInputs.receivedTime = std::chrono::steady_clock::now();
    m_lastKnownInputs[fromSlot] = controllerState;
    m_lastKnownInputFrames[fromSlot] = targetFrame;

    if (targetFrame == m_currentFrameNumber) {
        m_frameReceived[fromSlot] = true;
    }

    if (m_callbacks.inputReceived) {
        m_callbacks.inputReceived(fromSlot, targetFrame, controllerState);
    }
}

bool LockstepEngine::advanceFrame()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // Clear the received flags so we are forced to wait for fresh packets next frame
    m_frameReceived.clear(); 
    
    m_currentFrameNumber++;
    return true; 
}

void LockstepEngine::checkDesync(uint32_t romChecksum)
{
    SyncCheckpoint checkpoint;
    checkpoint.frameNumber = m_currentFrameNumber;
    checkpoint.romChecksum = romChecksum;
    checkpoint.timestamp = std::chrono::steady_clock::now();
    m_syncCheckpoints.push_back(checkpoint);

    if (m_syncCheckpoints.size() >= 2) {
        const SyncCheckpoint& prev = m_syncCheckpoints[m_syncCheckpoints.size() - 2];
        if (prev.romChecksum != romChecksum && m_lastVerifiedFrame != prev.frameNumber) {
            m_stats.desyncDetections++;
            m_isDesynchronized = true;
            if (m_callbacks.desyncDetected) {
                m_callbacks.desyncDetected(m_currentFrameNumber, "ROM state mismatch");
            }
            std::cerr << "LockstepEngine: DESYNC detected at frame " << m_currentFrameNumber << std::endl;

            if (m_config.resyncEnabled) {
                requestResync();
            }
        }
    }
    m_lastVerifiedFrame = m_currentFrameNumber;
}

void LockstepEngine::requestResync()
{
    m_stats.resyncAttempts++;
    if (m_callbacks.attemptingResync) {
        m_callbacks.attemptingResync();
    }
    std::cerr << "LockstepEngine: Attempting resync..." << std::endl;
}

uint32_t LockstepEngine::getCurrentFrameNumber() const
{
    return m_currentFrameNumber;
}

std::map<int, uint32_t> LockstepEngine::getCurrentFrameInputs() const
{
    auto it = m_frameBuffer.find(m_currentFrameNumber);
    if (it == m_frameBuffer.end()) {
        return {};
    }
    return it->second.playerInputs;
}

bool LockstepEngine::isDesynchronized() const
{
    return m_isDesynchronized;
}

int LockstepEngine::getPendingInputsCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto frameIt = m_frameBuffer.find(m_currentFrameNumber);
    const std::map<int, uint32_t>* inputs = (frameIt != m_frameBuffer.end())
        ? &frameIt->second.playerInputs
        : nullptr;

    int pending = 0;
    for (int i = 0; i < m_config.numPlayers; ++i) {
        if (i == m_config.localPlayerSlot) {
            continue;
        }

        if (!inputs || inputs->find(i) == inputs->end()) {
            pending++;
        }
    }
    return pending;
}

std::string LockstepEngine::getEngineStatus() const
{
    return "Frame: " + std::to_string(m_currentFrameNumber) +
           ", Pending: " + std::to_string(getPendingInputsCount()) +
           ", Desync: " + (m_isDesynchronized ? "YES" : "NO");
}

LockstepEngine::Stats LockstepEngine::getStatistics() const
{
    return m_stats;
}

void LockstepEngine::resetStatistics()
{
    m_stats.totalFramesProcessed = 0;
    m_stats.timeoutOccurrences = 0;
    m_stats.desyncDetections = 0;
    m_stats.resyncAttempts = 0;
    m_stats.averageInputLatencyMs = 0.0;
    m_stats.stallFrameNumbers.clear();
}

void LockstepEngine::setInputDelayFrames(int frames)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (frames < 0) {
        frames = 0;
    } else if (frames > 99) {
        frames = 99;
    }
    m_config.inputDelayFrames = frames;
    // Higher buffer waits longer for peer input before timing out (smoother FPS, more latency).
    m_config.stallTimeoutMilliseconds = frames == 0 ? 100 : std::min(frames * 33, 3300);
    std::cout << "LockstepEngine: Input delay changed to " << frames << " frames" << std::endl;
}

void LockstepEngine::onDataChannelBinaryMessageReceived(int peerSlot, const std::vector<uint8_t>& data)
{
    processInputPacket(peerSlot, data);
}

void LockstepEngine::onDataChannelClosed(int peerSlot)
{
    std::cerr << "LockstepEngine: DataChannel closed for slot " << peerSlot << std::endl;
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (peerSlot >= 0 && peerSlot < m_dataChannels.size()) {
        m_dataChannels[peerSlot] = nullptr; 
        m_lastKnownInputs[peerSlot] = 0; 
        m_frameReceived[peerSlot] = true; // Let the engine advance immediately
    }
}

void LockstepEngine::onDataChannelError(int peerSlot, const std::string& error)
{
    std::cerr << "LockstepEngine: Socket Error on slot " << peerSlot << ": " << error << std::endl;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Treat an error as a disconnect to prevent the engine from hanging/spamming
    if (peerSlot >= 0 && peerSlot < m_dataChannels.size()) {
        m_dataChannels[peerSlot] = nullptr;
    }
}

void LockstepEngine::broadcastInput(uint32_t controllerState) {
    std::vector<uint8_t> packet(INPUT_PACKET_SIZE);
    uint32_t frameNum;
    
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        frameNum = m_currentFrameNumber;
    }

    std::memcpy(packet.data(), &frameNum, 4);
    std::memcpy(packet.data() + 4, &controllerState, 4);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot != m_config.localPlayerSlot && m_dataChannels[slot]) {
            m_dataChannels[slot]->sendBinary(packet);
        }
    }
}

void LockstepEngine::processInputPacket(int fromSlot, const std::vector<uint8_t>& packet) {
    if (packet.size() != INPUT_PACKET_SIZE) return;

    uint32_t frameNumber;
    uint32_t controllerState;
    std::memcpy(&frameNumber, packet.data(), 4);
    std::memcpy(&controllerState, packet.data() + 4, 4);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (fromSlot < 0 || fromSlot >= m_config.numPlayers || !m_dataChannels[fromSlot]) {
        return;
    }    
    // Only update if it's relevant to our current timeline
    FrameInputs& frameInputs = m_frameBuffer[frameNumber];
    frameInputs.frameNumber = frameNumber;
    frameInputs.playerInputs[fromSlot] = controllerState;
    
    if (frameNumber == m_currentFrameNumber) {
        m_frameReceived[fromSlot] = true;
    }

    m_lastKnownInputs[fromSlot] = controllerState;
    m_lastKnownInputFrames[fromSlot] = frameNumber;
}

bool LockstepEngine::waitForAllInputs(uint32_t frameNumber, int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            auto frameIt = m_frameBuffer.find(frameNumber);
            bool allReceived = true;
            for (int slot = 0; slot < m_config.numPlayers; ++slot) {
                if (slot == m_config.localPlayerSlot) {
                    continue;
                }

                // Check the frame buffer directly so pre-received future inputs are
                // honored when that frame becomes current.
                const bool hasInputForFrame =
                    (frameIt != m_frameBuffer.end() &&
                     frameIt->second.playerInputs.find(slot) != frameIt->second.playerInputs.end());

                if (!hasInputForFrame) {
                    allReceived = false;
                    break;
                }
            }
            if (allReceived) return true;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count() > timeoutMs) return false;

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void LockstepEngine::applyTimeoutFallback(uint32_t frameNumber)
{
    FrameInputs& frameInputs = m_frameBuffer[frameNumber];
    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        if (frameInputs.playerInputs.find(slot) == frameInputs.playerInputs.end()) {
            // Use last known input instead of zeroing out to maintain continuity during network jitter
            if (m_lastKnownInputs.count(slot)) {
                frameInputs.playerInputs[slot] = m_lastKnownInputs[slot];
            } else {
                frameInputs.playerInputs[slot] = FALLBACK_INPUT;
            }
            m_stats.stallFrameNumbers.push_back(frameNumber);
            if (m_callbacks.peerInputStalled) {
                m_callbacks.peerInputStalled(slot, frameNumber);
            }
        }
    }
}

void LockstepEngine::setNumPlayers(int numPlayers)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_config.numPlayers != numPlayers) {
        std::cout << "LockstepEngine: Player count changed from " 
                  << m_config.numPlayers << " to " << numPlayers << std::endl;
        m_config.numPlayers = numPlayers;
        
        // Note: If you are using a fixed-size array for buffers, 
        // you might need to clear or re-validate the frame buffer here.
    }
}

void LockstepEngine::setLocalPlayerSlot(int slot)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_config.localPlayerSlot = slot;
}