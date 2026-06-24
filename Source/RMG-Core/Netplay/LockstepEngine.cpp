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
// Reject remote inputs whose frame number is implausibly far ahead of the
// current frame. In strict lockstep a peer can only ever lead by roughly the
// input delay window, so anything beyond this is a corrupted/stale/garbage
// packet. Without this guard, the gap-fill loop below would try to allocate
// one std::map node per skipped frame and exhaust the heap (malloc crash).
constexpr uint32_t kMaxFutureFrameLead = 1024;

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

    m_config.stallTimeoutMilliseconds = 0;

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        m_peerSessionActive[slot] = true;
    }
}

LockstepEngine::~LockstepEngine()
{
    shutdown();
}

void LockstepEngine::shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_shutdown.exchange(true)) {
        return;
    }

    m_callbacks = {};

    for (auto& channel : m_dataChannels) {
        detachDataChannelCallbacks(channel);
        channel.reset();
    }

    m_inputCv.notify_all();
}

void LockstepEngine::setCallbacks(Callbacks callbacks)
{
    if (!isAlive()) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_callbacks = std::move(callbacks);
}

void LockstepEngine::setDataChannel(
    int peerSlot,
    std::shared_ptr<WebRTCDataChannel> channel)
{
    if (!isAlive()) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (peerSlot < 0 || peerSlot >= static_cast<int>(m_dataChannels.size())) {
        return;
    }

    detachDataChannelCallbacks(m_dataChannels[peerSlot]);
    m_dataChannels[peerSlot] = std::move(channel);

    m_inputCv.notify_all();

    if (m_dataChannels[peerSlot]) {
        const int boundSlot = peerSlot;
        std::weak_ptr<LockstepEngine> weakSelf = weak_from_this();
        auto& boundChannel = m_dataChannels[peerSlot];

        boundChannel->onBinaryMessageReceived =
            [weakSelf, boundSlot](const std::vector<uint8_t>& data) {
                if (auto self = weakSelf.lock()) {
                    self->onDataChannelBinaryMessageReceived(boundSlot, data);
                }
            };

        boundChannel->onClosed =
            [weakSelf, boundSlot]() {
                if (auto self = weakSelf.lock()) {
                    self->onDataChannelClosed(boundSlot);
                }
            };

        boundChannel->onError =
            [weakSelf, boundSlot](const std::string& error) {
                if (auto self = weakSelf.lock()) {
                    self->onDataChannelError(boundSlot, error);
                }
            };
    }
}

std::vector<std::pair<uint32_t, uint32_t>>
LockstepEngine::submitLocalInput(uint32_t controllerState)
{
    if (!isAlive()) {
        return {};
    }

    std::vector<std::pair<uint32_t, uint32_t>> outbound;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        const uint32_t sendFrame =
            m_currentFrameNumber +
            static_cast<uint32_t>(m_config.inputDelayFrames);

        const auto assignLocalInputUnlocked =
            [&](uint32_t frame, uint32_t state) {
                FrameInputs& frameInputs = m_frameBuffer[frame];
                const auto existing =
                    frameInputs.playerInputs.find(m_config.localPlayerSlot);

                if (existing != frameInputs.playerInputs.end() &&
                    existing->second == state) {
                    return;
                }

                frameInputs.frameNumber = frame;
                frameInputs.playerInputs[m_config.localPlayerSlot] = state;
                outbound.emplace_back(frame, state);
                notifyInputProgressUnlocked(frame);
            };

        // Always refresh the live send frame so per-frame keyboard/gamepad
        // changes are not stuck on the first sample written into the buffer.
        assignLocalInputUnlocked(sendFrame, controllerState);

        // Dolphin-style pad buffer: keep the delay window filled so frame 0
        // has usable input instead of waiting for the delay to elapse.
        // Prefill is spread across emulated frames so high buffer values do
        // not flood peers and drop the data channel or signaling connection.
        uint32_t prefilled = 0;
        for (uint32_t frame = m_currentFrameNumber;
             frame < sendFrame &&
             prefilled < kMaxInputPrefillPerSubmit;
             ++frame) {

            if (m_frameBuffer[frame].playerInputs.find(m_config.localPlayerSlot) !=
                m_frameBuffer[frame].playerInputs.end()) {
                continue;
            }

            assignLocalInputUnlocked(frame, controllerState);
            ++prefilled;
        }

        m_lastKnownInputs[m_config.localPlayerSlot] = controllerState;
        m_lastKnownInputFrames[m_config.localPlayerSlot] = sendFrame;
    }

    if (!outbound.empty()) {
        broadcastInput(controllerState, outbound.back().first);
    }

    return outbound;
}

void LockstepEngine::submitRemoteInput(
    int fromSlot,
    uint32_t frameNumber,
    uint32_t controllerState)
{
    if (!isAlive()) {
        return;
    }

    if (fromSlot < 0 ||
        fromSlot >= m_config.numPlayers ||
        fromSlot == m_config.localPlayerSlot) {
        return;
    }

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (frameNumber + inputFrameSlackForDelay(m_config.inputDelayFrames) <
        m_currentFrameNumber) {
        return;
    }

    // Drop implausibly far-future frames so the gap-fill loop below cannot be
    // tricked into allocating a near-unbounded number of frame buffer entries.
    if (frameNumber > m_currentFrameNumber + kMaxFutureFrameLead) {
        return;
    }

    for (uint32_t frame = m_currentFrameNumber; frame < frameNumber; ++frame) {
        FrameInputs& priorFrameInputs = m_frameBuffer[frame];
        if (priorFrameInputs.playerInputs.find(fromSlot) ==
            priorFrameInputs.playerInputs.end()) {
            priorFrameInputs.frameNumber = frame;
            priorFrameInputs.playerInputs[fromSlot] = controllerState;
            if (frame == m_currentFrameNumber) {
                m_frameReceived[fromSlot] = true;
            }
        }
    }

    FrameInputs& frameInputs = m_frameBuffer[frameNumber];
    const auto existing = frameInputs.playerInputs.find(fromSlot);
    if (existing != frameInputs.playerInputs.end() &&
        existing->second != controllerState &&
        frameNumber <= m_currentFrameNumber) {
        m_stats.desyncDetections++;
        m_isDesynchronized = true;
        const std::string desyncReason =
            "Conflicting remote input for frame " +
            std::to_string(frameNumber);
        auto desyncCallback = m_callbacks.desyncDetected;
        lock.unlock();
        if (desyncCallback) {
            desyncCallback(frameNumber, desyncReason);
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

    auto inputReceivedCallback = m_callbacks.inputReceived;
    lock.unlock();
    if (inputReceivedCallback) {
        inputReceivedCallback(fromSlot, frameNumber, controllerState);
    }
}

void LockstepEngine::recordLocalFrameSync(uint32_t frameNumber, uint32_t stateHash)
{
    if (!m_config.desyncDetectionEnabled || frameNumber == 0 || stateHash == 0) {
        return;
    }

    {
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
    notifyPendingCallbacks();
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

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        comparePeerFrameSyncUnlocked(fromSlot, frameNumber, stateHash);
    }
    notifyPendingCallbacks();
}

bool LockstepEngine::advanceFrame()
{
    if (m_shutdown.load()) {
        return false;
    }

    uint32_t frameNumber = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        frameNumber = m_currentFrameNumber;
    }

    if (m_config.numPlayers > 1) {
        int timeoutMs = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            timeoutMs = computeInputWaitTimeoutMsUnlocked(frameNumber);
        }

        const bool ready = waitForAllInputs(frameNumber, timeoutMs);
        if (!ready) {
            if (m_shutdown.load()) {
                return false;
            }

            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                applyTimeoutFallbackUnlocked(frameNumber);
                ++m_stats.timeoutOccurrences;
            }
            notifyPendingCallbacks();
        }
    }

    std::map<int, uint32_t> frameInputs;
    // Snapshot the callback under the lock so shutdown() on another thread
    // cannot destroy it between the null-check and the actual call.
    std::function<void(uint32_t, const std::map<int, uint32_t>&)> frameReadyCb;

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
        frameReadyCb = m_callbacks.frameReady;  // safe copy under lock
    }

    if (m_shutdown.load()) {
        return false;
    }

    if (frameReadyCb) {
        frameReadyCb(frameNumber, frameInputs);
    }

    return isAlive();
}

void LockstepEngine::checkDesync(uint32_t stateHash)
{
    if (!m_config.desyncDetectionEnabled || stateHash == 0) {
        return;
    }

    uint32_t frameNumber = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        frameNumber = m_currentFrameNumber;
    }

    recordLocalFrameSync(frameNumber, stateHash);
}

void LockstepEngine::requestResync()
{
    std::function<void()> cb;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_stats.resyncAttempts++;
        cb = m_callbacks.attemptingResync;
    }
    if (cb) {
        cb();
    }
}

uint32_t LockstepEngine::getCurrentFrameNumber() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto it = m_frameBuffer.find(m_currentFrameNumber);

    if (it == m_frameBuffer.end()) {
        return {};
    }

    return it->second.playerInputs;
}

std::map<int, uint32_t>
LockstepEngine::getFrameInputs(uint32_t frameNumber) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto it = m_frameBuffer.find(frameNumber);

    if (it == m_frameBuffer.end()) {
        return {};
    }

    return it->second.playerInputs;
}

bool LockstepEngine::isDesynchronized() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
    m_config.stallTimeoutMilliseconds = 0;
    m_inputCv.notify_all();
}

int LockstepEngine::stallTimeoutForDelayFrames(int inputDelayFrames)
{
    (void)inputDelayFrames;
    return 0;
}

void LockstepEngine::setPeerSessionActive(int slot, bool active)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (slot < 0 || slot >= m_config.numPlayers) {
        return;
    }

    m_peerSessionActive[slot] = active;
    m_inputCv.notify_all();
}

void LockstepEngine::wakeInputWaiters()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_inputCv.notify_all();
}

void LockstepEngine::releaseCurrentFrameWait()
{
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        applyTimeoutFallbackUnlocked(m_currentFrameNumber);
        m_inputCv.notify_all();
    }
    notifyPendingCallbacks();
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
    if (!isAlive()) {
        return;
    }

    processInputPacket(peerSlot, data);
}

void LockstepEngine::onDataChannelClosed(int peerSlot)
{
    if (!isAlive()) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (peerSlot >= 0 &&
        peerSlot < static_cast<int>(m_dataChannels.size())) {

        m_dataChannels[peerSlot] = nullptr;
    }

    if (peerSlot >= 0 && peerSlot < m_config.numPlayers &&
        peerSlot != m_config.localPlayerSlot) {
        m_peerSessionActive[peerSlot] = false;
    }

    m_inputCv.notify_all();
}

void LockstepEngine::onDataChannelError(
    int peerSlot,
    const std::string& error)
{
    if (!isAlive()) {
        return;
    }

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

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (fromSlot < 0 ||
        fromSlot >= m_config.numPlayers) {
        return;
    }

    if (frameNumber + inputFrameSlackForDelay(m_config.inputDelayFrames) <
        m_currentFrameNumber) {
        return;
    }

    // Drop implausibly far-future frames; a garbage/stale frame number here
    // would insert a frame buffer entry that never gets pruned and lets a
    // single bad packet balloon memory use.
    if (frameNumber > m_currentFrameNumber + kMaxFutureFrameLead) {
        return;
    }

    FrameInputs& frameInputs =
        m_frameBuffer[frameNumber];

    const auto existing = frameInputs.playerInputs.find(fromSlot);
    if (existing != frameInputs.playerInputs.end() &&
        existing->second != controllerState &&
        frameNumber <= m_currentFrameNumber) {
        m_stats.desyncDetections++;
        m_isDesynchronized = true;
        const std::string desyncReason =
            "Conflicting WebRTC input for frame " +
            std::to_string(frameNumber);
        auto desyncCallback = m_callbacks.desyncDetected;
        lock.unlock();
        if (desyncCallback) {
            desyncCallback(frameNumber, desyncReason);
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

    char localHex[11];
    char peerHex[11];
    std::snprintf(localHex, sizeof(localHex), "%08x", localHash);
    std::snprintf(peerHex, sizeof(peerHex), "%08x", peerHash);

    m_pendingDesyncNotification = {
        frameNumber,
        "State hash mismatch with player " +
            std::to_string(fromSlot) +
            " at frame " +
            std::to_string(frameNumber) +
            " (local=0x" + localHex +
            ", peer=0x" + peerHex + ")"};
    m_hasPendingDesyncNotification = true;

    if (m_config.resyncEnabled) {
        m_pendingResync = true;
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

        if (firstStallInBurst) {
            m_pendingStallNotifications.emplace_back(slot, frameNumber);
        }

        m_lastStallCallbackFrame[slot] = frameNumber;
    }
}

void LockstepEngine::notifyPendingCallbacks()
{
    std::vector<std::pair<int, uint32_t>> stalls;
    std::pair<uint32_t, std::string> desyncNotification;
    bool hasDesyncNotification = false;
    bool pendingResync = false;
    // Snapshot all callbacks under the lock – shutdown() can destroy them at
    // any time on another thread and calling a destroyed std::function is UB.
    std::function<void(int, uint32_t)> stallCb;
    std::function<void(uint32_t, const std::string&)> desyncCb;
    std::function<void()> resyncCb;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        stalls.swap(m_pendingStallNotifications);
        if (m_hasPendingDesyncNotification) {
            desyncNotification = std::move(m_pendingDesyncNotification);
            hasDesyncNotification = true;
            m_hasPendingDesyncNotification = false;
        }
        pendingResync = m_pendingResync;
        m_pendingResync = false;
        stallCb   = m_callbacks.peerInputStalled;
        desyncCb  = m_callbacks.desyncDetected;
        resyncCb  = m_callbacks.attemptingResync;
    }

    for (const auto& [slot, stalledFrame] : stalls) {
        if (stallCb) {
            stallCb(slot, stalledFrame);
        }
    }

    if (hasDesyncNotification && desyncCb) {
        desyncCb(desyncNotification.first, desyncNotification.second);
    }

    if (pendingResync) {
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_stats.resyncAttempts++;
        }
        if (resyncCb) {
            resyncCb();
        }
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

bool LockstepEngine::hasAllRemoteDataChannelsConnectedUnlocked() const
{
    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        const auto activeIt = m_peerSessionActive.find(slot);
        const bool sessionActive =
            activeIt != m_peerSessionActive.end() && activeIt->second;
        if (!sessionActive) {
            continue;
        }

        // Inputs can arrive over the signaling relay when WebRTC is down.
        if (m_lastKnownInputFrames.find(slot) != m_lastKnownInputFrames.end()) {
            continue;
        }

        if (!m_dataChannels[slot] || !m_dataChannels[slot]->isOpen()) {
            return false;
        }
    }

    return true;
}

bool LockstepEngine::hasReceivedBootstrapInputFromAllRemotesUnlocked() const
{
    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        if (m_lastKnownInputFrames.find(slot) == m_lastKnownInputFrames.end()) {
            return false;
        }
    }

    return true;
}

bool LockstepEngine::hasOpenRemoteDataChannels() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        if (m_dataChannels[slot] && m_dataChannels[slot]->isOpen()) {
            return true;
        }
    }

    return false;
}

int LockstepEngine::computeInputWaitTimeoutMsUnlocked(
    uint32_t frameNumber) const
{
    (void)frameNumber;

    if (!hasAllRemoteDataChannelsConnectedUnlocked() ||
        !hasReceivedBootstrapInputFromAllRemotesUnlocked()) {
        return 8000;
    }

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
        if (slot == m_config.localPlayerSlot) {
            continue;
        }

        const auto activeIt = m_peerSessionActive.find(slot);
        const bool sessionActive =
            activeIt != m_peerSessionActive.end() && activeIt->second;
        if (!sessionActive) {
            continue;
        }

        // WebRTC dropped but the peer is still in-session via signaling.
        if (!m_dataChannels[slot] || !m_dataChannels[slot]->isOpen()) {
            return 250;
        }
    }

    // Dolphin-style: stall on lag spikes instead of advancing with stale inputs.
    return 0;
}

bool LockstepEngine::allMissingInputsAreFromDisconnectedPeersUnlocked(
    uint32_t frameNumber) const
{
    const auto frameIt = m_frameBuffer.find(frameNumber);

    for (int slot = 0; slot < m_config.numPlayers; ++slot) {
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
            const auto activeIt = m_peerSessionActive.find(slot);
            const bool sessionActive =
                activeIt != m_peerSessionActive.end() && activeIt->second;
            if (sessionActive) {
                return false;
            }
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
        if (m_shutdown.load()) {
            return false;
        }

        if (allMissingInputsAreFromDisconnectedPeersUnlocked(frameNumber)) {
            applyTimeoutFallbackUnlocked(frameNumber);
            lock.unlock();
            notifyPendingCallbacks();
            return true;
        }

        {
            // Copy pumpNetwork under the lock. Another thread can call shutdown()
            // and destroy m_callbacks between the null-check and the actual call
            // unless we hold our own reference to the std::function.
            auto pumpCb = m_callbacks.pumpNetwork;
            if (pumpCb) {
                // Release the lock before pumping Qt/socket events. pumpNetwork
                // may deliver controllerInput -> submitRemoteInput, which also
                // needs m_mutex. Holding it here deadlocks at frame 0.
                lock.unlock();
                pumpCb();
                lock.lock();
            }
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
    m_peerSessionActive.clear();
    for (int slot = 0; slot < numPlayers; ++slot) {
        m_peerSessionActive[slot] = true;
    }
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