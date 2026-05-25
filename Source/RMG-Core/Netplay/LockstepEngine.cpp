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

namespace {

// Gentle catch-up: only slow the faster peer once a peer is severely behind on inputs.
constexpr int kPeerLagThrottleOnFrames = 30;
constexpr int kPeerLagThrottleOffFrames = 20;
constexpr int kPeerLagThrottleMaxSleepMs = 1;
constexpr std::chrono::milliseconds kPeerFrameReportStale{2000};

} // namespace

LockstepEngine::LockstepEngine(const Config& config)
    : m_config(config)
    , m_currentFrameNumber(0)
    , m_isDesynchronized(false)
    , m_lastVerifiedFrame(0)
{
    if (config.numPlayers < 2 || config.numPlayers > 4) {
        std::cerr << "LockstepEngine: numPlayers must be 2-4, got " << config.numPlayers << std::endl;
    }

    m_config.stallTimeoutMilliseconds = stallTimeoutForDelayFrames(m_config.inputDelayFrames);
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

void LockstepEngine::submitLocalInput(uint32_t controllerState)
{
    const uint32_t sendFrame = getSendFrameNumber();

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        FrameInputs& frameInputs = m_frameBuffer[sendFrame];
        frameInputs.frameNumber = sendFrame;
        frameInputs.playerInputs[m_config.localPlayerSlot] = controllerState;
        m_lastKnownInputs[m_config.localPlayerSlot] = controllerState;
    }

    // Network send is handled by NetplayCoordinator (Socket.IO). WebRTC path only:
    broadcastInput(controllerState, sendFrame);
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

    // Allow slightly late packets; with input delay, early (future) packets are normal.
    constexpr uint32_t kFrameSlack = 4;
    if (frameNumber + kFrameSlack < m_currentFrameNumber) {
        return;
    }
    const uint32_t targetFrame = frameNumber;

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

void LockstepEngine::submitPeerReportedFrame(int fromSlot, uint32_t frameNumber)
{
    if (fromSlot < 0 || fromSlot >= m_config.numPlayers || fromSlot == m_config.localPlayerSlot) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    constexpr uint32_t kFrameSlack = 120;
    if (frameNumber + kFrameSlack < m_currentFrameNumber) {
        return;
    }

    m_peerReportedEmulationFrames[fromSlot] = frameNumber;
    m_peerReportedFrameTimes[fromSlot] = std::chrono::steady_clock::now();
}

bool LockstepEngine::advanceFrame()
{
    const uint32_t frameNumber = m_currentFrameNumber;

    if (m_config.numPlayers > 1) {
        // Only slow down when we already have this frame's inputs and would advance
        // ahead of the slowest peer's input stream (not while stalled waiting).
        applyPeerLagThrottleIfNeeded(frameNumber);
        bool ready = waitForAllInputs(frameNumber, m_config.stallTimeoutMilliseconds);
        if (!ready) {
            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                applyTimeoutFallback(frameNumber);
                m_stats.timeoutOccurrences++;
            }

            if (m_callbacks.peerInputTimedOut) {
                for (int slot = 0; slot < m_config.numPlayers; ++slot) {
                    if (slot == m_config.localPlayerSlot) {
                        continue;
                    }
                    m_callbacks.peerInputTimedOut(slot, frameNumber);
                }
            }
        }
    }

    std::map<int, uint32_t> frameInputs;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        frameInputs = m_frameBuffer[frameNumber].playerInputs;
        m_frameReceived.clear();
        m_currentFrameNumber++;
        pruneOldFrames(m_currentFrameNumber > 120 ? m_currentFrameNumber - 120 : 0);
    }

    if (m_callbacks.frameReady) {
        m_callbacks.frameReady(frameNumber, frameInputs);
    }

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

uint32_t LockstepEngine::getSendFrameNumber() const
{
    return m_currentFrameNumber + static_cast<uint32_t>(m_config.inputDelayFrames);
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
    const int inputLag = getMaxPeerInputLagFrames();
    const int emuLag = getMaxPeerEmulationLagFrames();
    return "Frame: " + std::to_string(m_currentFrameNumber) +
           ", Pending: " + std::to_string(getPendingInputsCount()) +
           ", InputLag: " + std::to_string(inputLag) +
           ", EmuLag: " + std::to_string(emuLag) +
           ", Throttle: " + (m_peerLagThrottlingActive ? "ON" : "OFF") +
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
    m_config.stallTimeoutMilliseconds = stallTimeoutForDelayFrames(frames);
    std::cout << "LockstepEngine: Input delay changed to " << frames << " frames" << std::endl;
}

int LockstepEngine::stallTimeoutForDelayFrames(int inputDelayFrames)
{
    if (inputDelayFrames <= 0) {
        // Zero delay: strict lockstep, short wait per frame.
        return 100;
    }
    // Higher delay gives the network more time to absorb jitter before the emulator stalls.
    const int scaledTimeout = inputDelayFrames * 25;
    const int lowerBoundTimeout = (scaledTimeout < 100) ? 100 : scaledTimeout;
    return (lowerBoundTimeout > 1500) ? 1500 : lowerBoundTimeout;
}

void LockstepEngine::pruneOldFrames(uint32_t oldestFrameToKeep)
{
    for (auto it = m_frameBuffer.begin(); it != m_frameBuffer.end();) {
        if (it->first < oldestFrameToKeep) {
            it = m_frameBuffer.erase(it);
        } else {
            ++it;
        }
    }
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
        m_lastKnownInputFrames.erase(peerSlot);
        m_peerReportedEmulationFrames.erase(peerSlot);
        m_peerReportedFrameTimes.erase(peerSlot);
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

void LockstepEngine::broadcastInput(uint32_t controllerState, uint32_t frameNumber)
{
    std::vector<uint8_t> packet(INPUT_PACKET_SIZE);

    std::memcpy(packet.data(), &frameNumber, 4);
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

int LockstepEngine::getMaxPeerInputLagFrames() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const uint32_t sendFrame = m_currentFrameNumber + static_cast<uint32_t>(m_config.inputDelayFrames);
    int maxLag = 0;

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        const auto it = m_lastKnownInputFrames.find(slot);
        if (it == m_lastKnownInputFrames.end()) {
            continue;
        }

        if (sendFrame <= it->second) {
            continue;
        }

        const int lag = static_cast<int>(sendFrame - it->second);
        if (lag > maxLag) {
            maxLag = lag;
        }
    }

    return maxLag;
}

int LockstepEngine::getMaxPeerEmulationLagFrames() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const auto now = std::chrono::steady_clock::now();
    int maxLag = 0;

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        const auto frameIt = m_peerReportedEmulationFrames.find(slot);
        const auto timeIt = m_peerReportedFrameTimes.find(slot);
        if (frameIt == m_peerReportedEmulationFrames.end() ||
            timeIt == m_peerReportedFrameTimes.end()) {
            continue;
        }

        if (now - timeIt->second > kPeerFrameReportStale) {
            continue;
        }

        if (m_currentFrameNumber <= frameIt->second) {
            continue;
        }

        const int lag = static_cast<int>(m_currentFrameNumber - frameIt->second);
        if (lag > maxLag) {
            maxLag = lag;
        }
    }

    return maxLag;
}

bool LockstepEngine::hasAllInputsForFrame(uint32_t frameNumber) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const auto frameIt = m_frameBuffer.find(frameNumber);
    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        const bool hasInputForFrame =
            (frameIt != m_frameBuffer.end() &&
             frameIt->second.playerInputs.find(slot) != frameIt->second.playerInputs.end());

        if (!hasInputForFrame) {
            return false;
        }
    }

    return true;
}

void LockstepEngine::applyPeerLagThrottleIfNeeded(uint32_t frameNumber)
{
    // When the input delay is already in the normal netplay range, let the buffer
    // absorb jitter instead of layering extra pacing on top.
    if (m_config.inputDelayFrames >= 4) {
        return;
    }

    if (!hasAllInputsForFrame(frameNumber)) {
        return;
    }

    const int peerInputLag = getMaxPeerInputLagFrames();
    const int peerEmulationLag = getMaxPeerEmulationLagFrames();
    const int maxLag = (peerInputLag > peerEmulationLag) ? peerInputLag : peerEmulationLag;

    if (maxLag >= kPeerLagThrottleOnFrames) {
        m_peerLagThrottlingActive = true;
    } else if (maxLag <= kPeerLagThrottleOffFrames) {
        m_peerLagThrottlingActive = false;
    }

    if (!m_peerLagThrottlingActive) {
        return;
    }

    const int framesOver = maxLag - (kPeerLagThrottleOnFrames - 1);
    const int sleepMs = (framesOver < kPeerLagThrottleMaxSleepMs)
        ? framesOver
        : kPeerLagThrottleMaxSleepMs;
    if (sleepMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
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