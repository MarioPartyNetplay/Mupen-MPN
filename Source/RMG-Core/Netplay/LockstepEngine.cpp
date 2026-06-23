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
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

using namespace UserInterface::Netplay;
using namespace RMGCore;

namespace {

constexpr uint32_t kMinInputFrameSlack = 8;
constexpr uint32_t kMinInputDelayFrames = 1;
// Cap how many future frames we publish per emulated frame so a large buffer
// setting does not burst hundreds of WebRTC/signaling packets at once.
constexpr uint32_t kMaxInputPrefillPerSubmit = 8;

uint32_t inputFrameSlackForDelay(int inputDelayFrames)
{
    if (inputDelayFrames < 1) {
        inputDelayFrames = 1;
    }

    return std::max(kMinInputFrameSlack,
                    static_cast<uint32_t>(inputDelayFrames) + kMinInputFrameSlack);
}

void detachDataChannelCallbacks(
    const std::shared_ptr<WebRTCDataChannel>& channel)
{
    if (!channel) {
        return;
    }

    channel->onBinaryMessageReceived = nullptr;
    channel->onClosed = nullptr;
    channel->onError = nullptr;
    channel->onStateChanged = nullptr;
    channel->onTextMessageReceived = nullptr;
    channel->onBufferedAmountLow = nullptr;
}

} // namespace

LockstepEngine::LockstepEngine(const Config& config)
    : m_config(config)
    , m_currentFrameNumber(0)
    , m_isDesynchronized(false)
{
    if (m_config.numPlayers < 2) {
        m_config.numPlayers = 2;
    }

    if (m_config.numPlayers > 4) {
        m_config.numPlayers = 4;
    }

    m_dataChannels.resize(m_config.numPlayers);

    if (m_config.inputDelayFrames < static_cast<int>(kMinInputDelayFrames)) {
        m_config.inputDelayFrames = static_cast<int>(kMinInputDelayFrames);
    }

    m_config.stallTimeoutMilliseconds =
        stallTimeoutForDelayFrames(m_config.inputDelayFrames);
}

LockstepEngine::~LockstepEngine()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    for (auto& channel : m_dataChannels) {
        detachDataChannelCallbacks(channel);
        channel.reset();
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

    if (peerSlot < 0 || peerSlot >= static_cast<int>(m_dataChannels.size())) {
        return;
    }

    detachDataChannelCallbacks(m_dataChannels[peerSlot]);
    m_dataChannels[peerSlot] = std::move(channel);

    m_inputCv.notify_all();

    if (m_dataChannels[peerSlot]) {
        const int boundSlot = peerSlot;
        auto& boundChannel = m_dataChannels[peerSlot];

        boundChannel->onBinaryMessageReceived =
            [this, boundSlot](const std::vector<uint8_t>& data) {
                onDataChannelBinaryMessageReceived(boundSlot, data);
            };

        boundChannel->onClosed =
            [this, boundSlot]() {
                onDataChannelClosed(boundSlot);
            };

        boundChannel->onError =
            [this, boundSlot](const std::string& error) {
                onDataChannelError(boundSlot, error);
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
        // Prefill is spread across emulated frames so high buffer values do
        // not flood peers and drop the data channel or signaling connection.
        uint32_t prefilled = 0;
        for (uint32_t frame = m_currentFrameNumber;
             frame <= sendFrame &&
             prefilled < kMaxInputPrefillPerSubmit;
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
            ++prefilled;
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

    if (frameNumber + inputFrameSlackForDelay(m_config.inputDelayFrames) <
        m_currentFrameNumber) {
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

void LockstepEngine::recordLocalFrameSync(uint32_t frameNumber, uint32_t stateHash)
{
    if (!m_config.desyncDetectionEnabled || frameNumber == 0 || stateHash == 0) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_localFrameSyncHashes[frameNumber] = stateHash;

    for (auto pendingIt = m_pendingPeerFrameSyncHashes.begin();
         pendingIt != m_pendingPeerFrameSyncHashes.end();
         ++pendingIt) {
        const auto peerIt = pendingIt->second.find(frameNumber);
        if (peerIt != pendingIt->second.end()) {
            comparePeerFrameSyncUnlocked(
                pendingIt->first,
                frameNumber,
                peerIt->second);
            // Erase by key: comparePeerFrameSyncUnlocked may already remove
            // the entry when hashes match, invalidating peerIt.
            pendingIt->second.erase(frameNumber);
        }
    }

    pruneOldFrameSyncDataUnlocked(
        frameNumber > 120 ? frameNumber - 120 : 0);
}

void LockstepEngine::submitPeerFrameSync(
    int fromSlot,
    uint32_t frameNumber,
    uint32_t stateHash)
{
    if (fromSlot < 0 ||
        fromSlot >= m_config.numPlayers ||
        fromSlot == m_config.localPlayerSlot ||
        !m_config.desyncDetectionEnabled ||
        frameNumber == 0 ||
        stateHash == 0) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    comparePeerFrameSyncUnlocked(fromSlot, frameNumber, stateHash);
}

bool LockstepEngine::advanceFrame()
{
    const uint32_t frameNumber = m_currentFrameNumber;

    if (m_config.numPlayers > 1) {
        int timeoutMs = m_config.stallTimeoutMilliseconds;
        // During ROM boot peers may still be connecting; allow extra time on
        // the first few seconds of frames before falling back to last input.
        if (frameNumber < 300) {
            timeoutMs = std::max(timeoutMs, 8000);
        }

        const bool ready = waitForAllInputs(frameNumber, timeoutMs);
        if (!ready) {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            applyTimeoutFallbackUnlocked(frameNumber);
            ++m_stats.timeoutOccurrences;
        }
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

void LockstepEngine::checkDesync(uint32_t stateHash)
{
    if (!m_config.desyncDetectionEnabled || stateHash == 0) {
        return;
    }

    recordLocalFrameSync(m_currentFrameNumber, stateHash);
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

    if (frames < static_cast<int>(kMinInputDelayFrames)) {
        frames = static_cast<int>(kMinInputDelayFrames);
    } else if (frames > 99) {
        frames = 99;
    }

    m_config.inputDelayFrames = frames;

    m_config.stallTimeoutMilliseconds =
        stallTimeoutForDelayFrames(frames);
}

int LockstepEngine::stallTimeoutForDelayFrames(int inputDelayFrames)
{
    if (inputDelayFrames < static_cast<int>(kMinInputDelayFrames)) {
        inputDelayFrames = static_cast<int>(kMinInputDelayFrames);
    }

    // Brief hitches should only add lag. A hard infinite wait lets emulation sit
    // inside advanceFrame() until signaling/WebRTC times out, which feels like a
    // crash. Fall back to last-known input after a generous delay.
    return 4000 + inputDelayFrames * 500;
}

void LockstepEngine::wakeInputWaiters()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_inputCv.notify_all();
}

void LockstepEngine::releaseCurrentFrameWait()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    applyTimeoutFallbackUnlocked(m_currentFrameNumber);
    m_inputCv.notify_all();
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

    m_inputCv.notify_all();
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

    m_inputCv.notify_all();
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

        if (!m_dataChannels[slot]->isOpen()) {
            continue;
        }

        // Signaling still relays inputs; treat send backpressure as transient.
        (void)m_dataChannels[slot]->sendBinary(packet);
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

    if (frameNumber + inputFrameSlackForDelay(m_config.inputDelayFrames) <
        m_currentFrameNumber) {
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

void LockstepEngine::comparePeerFrameSyncUnlocked(
    int fromSlot,
    uint32_t frameNumber,
    uint32_t peerHash)
{
    const auto localIt = m_localFrameSyncHashes.find(frameNumber);
    if (localIt == m_localFrameSyncHashes.end()) {
        m_pendingPeerFrameSyncHashes[fromSlot][frameNumber] = peerHash;
        return;
    }

    if (localIt->second == peerHash) {
        m_peerHashMismatchStreak[fromSlot] = 0;
        m_pendingPeerFrameSyncHashes[fromSlot].erase(frameNumber);
        return;
    }

    ++m_peerHashMismatchStreak[fromSlot];
    constexpr int kRequiredMismatchStreak = 3;
    if (m_peerHashMismatchStreak[fromSlot] < kRequiredMismatchStreak) {
        return;
    }

    reportStateHashMismatchUnlocked(
        fromSlot,
        frameNumber,
        localIt->second,
        peerHash);
}

void LockstepEngine::reportStateHashMismatchUnlocked(
    int fromSlot,
    uint32_t frameNumber,
    uint32_t localHash,
    uint32_t peerHash)
{
    const uint64_t mismatchKey =
        (static_cast<uint64_t>(frameNumber) << 32) |
        static_cast<uint32_t>(fromSlot);

    if (m_reportedHashMismatches.count(mismatchKey) != 0) {
        return;
    }

    m_reportedHashMismatches.insert(mismatchKey);
    m_stats.desyncDetections++;
    m_isDesynchronized = true;

    if (m_callbacks.desyncDetected) {
        char localHex[11];
        char peerHex[11];
        std::snprintf(localHex, sizeof(localHex), "%08x", localHash);
        std::snprintf(peerHex, sizeof(peerHex), "%08x", peerHash);

        m_callbacks.desyncDetected(
            frameNumber,
            "State hash mismatch with player " +
                std::to_string(fromSlot) +
                " at frame " +
                std::to_string(frameNumber) +
                " (local=0x" + localHex +
                ", peer=0x" + peerHex + ")");
    }

    if (m_config.resyncEnabled) {
        requestResync();
    }
}

void LockstepEngine::pruneOldFrameSyncDataUnlocked(uint32_t oldestFrameToKeep)
{
    for (auto it = m_localFrameSyncHashes.begin();
         it != m_localFrameSyncHashes.end();) {
        if (it->first < oldestFrameToKeep) {
            it = m_localFrameSyncHashes.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& [slot, pendingHashes] : m_pendingPeerFrameSyncHashes) {
        for (auto it = pendingHashes.begin(); it != pendingHashes.end();) {
            if (it->first < oldestFrameToKeep) {
                it = pendingHashes.erase(it);
            } else {
                ++it;
            }
        }
        (void)slot;
    }

    for (auto it = m_reportedHashMismatches.begin();
         it != m_reportedHashMismatches.end();) {
        if ((*it >> 32) < oldestFrameToKeep) {
            it = m_reportedHashMismatches.erase(it);
        } else {
            ++it;
        }
    }
}

void LockstepEngine::notifyInputProgressUnlocked(uint32_t frameNumber)
{
    if (frameNumber + inputFrameSlackForDelay(m_config.inputDelayFrames) >=
        m_currentFrameNumber) {
        m_inputCv.notify_all();
    }
}

void LockstepEngine::applyTimeoutFallbackUnlocked(uint32_t frameNumber)
{
    FrameInputs& frameInputs = m_frameBuffer[frameNumber];
    frameInputs.frameNumber = frameNumber;

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        if (frameInputs.playerInputs.find(slot) != frameInputs.playerInputs.end()) {
            continue;
        }

        const auto lastKnown = m_lastKnownInputs.find(slot);
        frameInputs.playerInputs[slot] =
            lastKnown != m_lastKnownInputs.end()
                ? lastKnown->second
                : FALLBACK_INPUT;

        m_stats.stallFrameNumbers.push_back(frameNumber);

        const auto lastNotified = m_lastStallCallbackFrame.find(slot);
        const bool firstStallInBurst =
            lastNotified == m_lastStallCallbackFrame.end() ||
            lastNotified->second + 1 != frameNumber;

        if (firstStallInBurst && m_callbacks.peerInputStalled) {
            m_callbacks.peerInputStalled(slot, frameNumber);
        }

        m_lastStallCallbackFrame[slot] = frameNumber;
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
        if (m_callbacks.pumpNetwork) {
            // Release the lock before pumping Qt/socket events. pumpNetwork runs
            // on the UI thread and may deliver controllerInput -> submitRemoteInput,
            // which also needs m_mutex. Holding it here deadlocks at frame 0.
            lock.unlock();
            m_callbacks.pumpNetwork();
            lock.lock();
        }

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