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
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace UserInterface::Netplay;
using namespace RMGCore;

namespace {

// Accept slightly late inputs; reject only packets that cannot affect the timeline.
constexpr uint32_t kInputFrameSlack = 8;
constexpr uint32_t kPeerFrameReportSlack = 120;

} // namespace

LockstepEngine::LockstepEngine(const Config& config)
    : m_config(config)
    , m_currentFrameNumber(0)
    , m_isDesynchronized(false)
    , m_lastVerifiedFrame(0)
{
    if (m_config.numPlayers < 2) {
        m_config.numPlayers = 2;
    }

    if (m_config.numPlayers > 4) {
        m_config.numPlayers = 4;
    }

    m_dataChannels.resize(m_config.numPlayers);

    if (m_config.inputDelayFrames < 0) {
        m_config.inputDelayFrames = 0;
    }

    m_config.stallTimeoutMilliseconds =
        stallTimeoutForDelayFrames(m_config.inputDelayFrames);
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

void LockstepEngine::setDataChannel(
    int peerSlot,
    std::shared_ptr<WebRTCDataChannel> channel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (peerSlot < 0 || peerSlot >= m_config.numPlayers) {
        return;
    }

    m_dataChannels[peerSlot] = channel;

    if (channel) {
        channel->onBinaryMessageReceived =
            [this, peerSlot](const std::vector<uint8_t>& data) {
                onDataChannelBinaryMessageReceived(peerSlot, data);
            };

        channel->onClosed =
            [this, peerSlot]() {
                onDataChannelClosed(peerSlot);
            };

        channel->onError =
            [this, peerSlot](const std::string& error) {
                onDataChannelError(peerSlot, error);
            };
    }
}

std::vector<std::pair<uint32_t, uint32_t>>
LockstepEngine::submitLocalInput(uint32_t controllerState)
{
    std::vector<std::pair<uint32_t, uint32_t>> outbound;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        const uint32_t sendFrame =
            m_currentFrameNumber +
            static_cast<uint32_t>(m_config.inputDelayFrames);

        // Dolphin-style pad buffer: keep the delay window filled so frame 0
        // has usable input instead of waiting for the delay to elapse.
        for (uint32_t frame = m_currentFrameNumber;
             frame <= sendFrame;
             ++frame) {

            FrameInputs& frameInputs = m_frameBuffer[frame];
            const auto existing =
                frameInputs.playerInputs.find(m_config.localPlayerSlot);

            if (existing != frameInputs.playerInputs.end()) {
                continue;
            }

            frameInputs.frameNumber = frame;
            frameInputs.playerInputs[m_config.localPlayerSlot] =
                controllerState;

            outbound.emplace_back(frame, controllerState);
            notifyInputProgressUnlocked(frame);
        }

        m_lastKnownInputs[m_config.localPlayerSlot] = controllerState;
        m_lastKnownInputFrames[m_config.localPlayerSlot] = sendFrame;
    }

    for (const auto& [frameNumber, state] : outbound) {
        broadcastInput(state, frameNumber);
    }

    return outbound;
}

void LockstepEngine::submitRemoteInput(
    int fromSlot,
    uint32_t frameNumber,
    uint32_t controllerState)
{
    if (fromSlot < 0 ||
        fromSlot >= m_config.numPlayers ||
        fromSlot == m_config.localPlayerSlot) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (frameNumber + kInputFrameSlack < m_currentFrameNumber) {
        return;
    }

    FrameInputs& frameInputs = m_frameBuffer[frameNumber];
    const auto existing = frameInputs.playerInputs.find(fromSlot);
    if (existing != frameInputs.playerInputs.end() &&
        existing->second != controllerState) {
        m_stats.desyncDetections++;
        m_isDesynchronized = true;
        if (m_callbacks.desyncDetected) {
            m_callbacks.desyncDetected(
                frameNumber,
                "Conflicting remote input for frame " +
                    std::to_string(frameNumber));
        }
        return;
    }

    frameInputs.frameNumber = frameNumber;
    frameInputs.playerInputs[fromSlot] = controllerState;
    frameInputs.receivedTime = std::chrono::steady_clock::now();

    m_lastKnownInputs[fromSlot] = controllerState;
    m_lastKnownInputFrames[fromSlot] = frameNumber;

    if (frameNumber == m_currentFrameNumber) {
        m_frameReceived[fromSlot] = true;
    }

    notifyInputProgressUnlocked(frameNumber);

    if (m_callbacks.inputReceived) {
        m_callbacks.inputReceived(
            fromSlot,
            frameNumber,
            controllerState);
    }
}

void LockstepEngine::submitPeerReportedFrame(
    int fromSlot,
    uint32_t frameNumber)
{
    if (fromSlot < 0 ||
        fromSlot >= m_config.numPlayers ||
        fromSlot == m_config.localPlayerSlot) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (frameNumber + kPeerFrameReportSlack < m_currentFrameNumber) {
        return;
    }

    m_peerReportedEmulationFrames[fromSlot] = frameNumber;
    m_peerReportedFrameTimes[fromSlot] =
        std::chrono::steady_clock::now();
}

bool LockstepEngine::advanceFrame()
{
    const uint32_t frameNumber = m_currentFrameNumber;

    if (m_config.numPlayers > 1) {
        waitForAllInputs(
            frameNumber,
            m_config.stallTimeoutMilliseconds);
    }

    std::map<int, uint32_t> frameInputs;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        auto it = m_frameBuffer.find(frameNumber);

        if (it != m_frameBuffer.end()) {
            frameInputs = it->second.playerInputs;
        }

        m_frameReceived.clear();

        m_currentFrameNumber++;

        pruneOldFrames(
            m_currentFrameNumber > 120
                ? m_currentFrameNumber - 120
                : 0);

        m_stats.totalFramesProcessed++;
    }

    if (m_callbacks.frameReady) {
        m_callbacks.frameReady(
            frameNumber,
            frameInputs);
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

        const SyncCheckpoint& prev =
            m_syncCheckpoints[m_syncCheckpoints.size() - 2];

        if (prev.romChecksum != romChecksum &&
            m_lastVerifiedFrame != prev.frameNumber) {

            m_stats.desyncDetections++;
            m_isDesynchronized = true;

            if (m_callbacks.desyncDetected) {
                m_callbacks.desyncDetected(
                    m_currentFrameNumber,
                    "ROM state mismatch");
            }

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
}

uint32_t LockstepEngine::getCurrentFrameNumber() const
{
    return m_currentFrameNumber;
}

uint32_t LockstepEngine::getSendFrameNumber() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return
        m_currentFrameNumber +
        static_cast<uint32_t>(m_config.inputDelayFrames);
}

std::map<int, uint32_t>
LockstepEngine::getCurrentFrameInputs() const
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

    int pending = 0;

    for (int i = 0; i < m_config.numPlayers; ++i) {

        if (i == m_config.localPlayerSlot) {
            continue;
        }

        bool found = false;

        if (frameIt != m_frameBuffer.end()) {
            found =
                frameIt->second.playerInputs.find(i) !=
                frameIt->second.playerInputs.end();
        }

        if (!found) {
            pending++;
        }
    }

    return pending;
}

std::string LockstepEngine::getEngineStatus() const
{
    return
        "Frame: " +
        std::to_string(m_currentFrameNumber) +
        ", Pending: " +
        std::to_string(getPendingInputsCount()) +
        ", Desync: " +
        (m_isDesynchronized ? "YES" : "NO");
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
    }

    if (frames > 99) {
        frames = 99;
    }

    m_config.inputDelayFrames = frames;

    m_config.stallTimeoutMilliseconds =
        stallTimeoutForDelayFrames(frames);
}

int LockstepEngine::stallTimeoutForDelayFrames(int)
{
    // Dolphin-style lockstep: wait indefinitely for peer inputs.
    return 0;
}

void LockstepEngine::pruneOldFrames(uint32_t oldestFrameToKeep)
{
    for (auto it = m_frameBuffer.begin();
         it != m_frameBuffer.end();) {

        if (it->first < oldestFrameToKeep) {
            it = m_frameBuffer.erase(it);
        } else {
            ++it;
        }
    }
}

void LockstepEngine::onDataChannelBinaryMessageReceived(
    int peerSlot,
    const std::vector<uint8_t>& data)
{
    processInputPacket(peerSlot, data);
}

void LockstepEngine::onDataChannelClosed(int peerSlot)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (peerSlot >= 0 &&
        peerSlot < static_cast<int>(m_dataChannels.size())) {

        m_dataChannels[peerSlot] = nullptr;
    }
}

void LockstepEngine::onDataChannelError(
    int peerSlot,
    const std::string& error)
{
    std::cerr
        << "LockstepEngine: DataChannel error on slot "
        << peerSlot
        << ": "
        << error
        << std::endl;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (peerSlot >= 0 &&
        peerSlot < static_cast<int>(m_dataChannels.size())) {

        m_dataChannels[peerSlot] = nullptr;
    }
}

void LockstepEngine::broadcastInput(
    uint32_t controllerState,
    uint32_t frameNumber)
{
    std::vector<uint8_t> packet(INPUT_PACKET_SIZE);

    std::memcpy(packet.data(), &frameNumber, 4);
    std::memcpy(packet.data() + 4, &controllerState, 4);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    for (int slot = 0;
         slot < m_config.numPlayers;
         ++slot) {

        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        if (!m_dataChannels[slot]) {
            continue;
        }

        m_dataChannels[slot]->sendBinary(packet);
    }
}

void LockstepEngine::processInputPacket(
    int fromSlot,
    const std::vector<uint8_t>& packet)
{
    if (packet.size() != INPUT_PACKET_SIZE) {
        return;
    }

    uint32_t frameNumber;
    uint32_t controllerState;

    std::memcpy(&frameNumber, packet.data(), 4);
    std::memcpy(&controllerState, packet.data() + 4, 4);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (fromSlot < 0 ||
        fromSlot >= m_config.numPlayers) {
        return;
    }

    if (frameNumber + kInputFrameSlack < m_currentFrameNumber) {
        return;
    }

    FrameInputs& frameInputs =
        m_frameBuffer[frameNumber];

    const auto existing = frameInputs.playerInputs.find(fromSlot);
    if (existing != frameInputs.playerInputs.end() &&
        existing->second != controllerState) {
        m_stats.desyncDetections++;
        m_isDesynchronized = true;
        if (m_callbacks.desyncDetected) {
            m_callbacks.desyncDetected(
                frameNumber,
                "Conflicting WebRTC input for frame " +
                    std::to_string(frameNumber));
        }
        return;
    }

    frameInputs.frameNumber = frameNumber;
    frameInputs.playerInputs[fromSlot] = controllerState;
    frameInputs.receivedTime =
        std::chrono::steady_clock::now();

    m_lastKnownInputs[fromSlot] = controllerState;
    m_lastKnownInputFrames[fromSlot] = frameNumber;

    if (frameNumber == m_currentFrameNumber) {
        m_frameReceived[fromSlot] = true;
    }

    notifyInputProgressUnlocked(frameNumber);
}

void LockstepEngine::notifyInputProgressUnlocked(uint32_t frameNumber)
{
    if (frameNumber + kInputFrameSlack >= m_currentFrameNumber) {
        m_inputCv.notify_all();
    }
}

bool LockstepEngine::hasAllInputsForFrameUnlocked(
    uint32_t frameNumber) const
{
    const auto frameIt = m_frameBuffer.find(frameNumber);

    for (int slot = 0;
         slot < m_config.numPlayers;
         ++slot) {

        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        bool found = false;

        if (frameIt != m_frameBuffer.end()) {
            found =
                frameIt->second.playerInputs.find(slot) !=
                frameIt->second.playerInputs.end();
        }

        if (!found) {
            return false;
        }
    }

    return true;
}

bool LockstepEngine::waitForAllInputs(
    uint32_t frameNumber,
    int timeoutMs)
{
    const bool hasDeadline = timeoutMs > 0;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    while (!hasAllInputsForFrameUnlocked(frameNumber)) {
        if (hasDeadline &&
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        m_inputCv.wait_for(
            lock,
            std::chrono::milliseconds(2),
            [this, frameNumber]() {
                return hasAllInputsForFrameUnlocked(frameNumber);
            });
    }

    return true;
}

void LockstepEngine::setNumPlayers(int numPlayers)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (numPlayers < 2) {
        numPlayers = 2;
    }

    if (numPlayers > 4) {
        numPlayers = 4;
    }

    m_config.numPlayers = numPlayers;

    m_dataChannels.resize(numPlayers);
}

void LockstepEngine::setLocalPlayerSlot(int slot)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (slot < 0) {
        slot = 0;
    }

    if (slot >= m_config.numPlayers) {
        slot = m_config.numPlayers - 1;
    }

    m_config.localPlayerSlot = slot;
}