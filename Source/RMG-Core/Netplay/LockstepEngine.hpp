/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef LOCKSTEPENGINE_HPP
#define LOCKSTEPENGINE_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <unordered_set>
#include "../Library.hpp"

namespace UserInterface::Netplay {
class WebRTCDataChannel;
}

namespace RMGCore {

class CORE_EXPORT LockstepEngine
    : public std::enable_shared_from_this<LockstepEngine> {
public:

    struct Config {
        int numPlayers = 2;
        int localPlayerSlot = 0;
        int inputDelayFrames = 1;
        bool desyncDetectionEnabled = true;
        bool resyncEnabled = false;
        int resyncCheckIntervalFrames = 180;
        int stallTimeoutMilliseconds = 0;
    };

    struct FrameInputs {
        uint32_t frameNumber = 0;
        std::map<int, uint32_t> playerInputs;
        std::chrono::steady_clock::time_point receivedTime;
    };

    struct Stats {
        uint32_t totalFramesProcessed = 0;
        uint32_t timeoutOccurrences = 0;
        uint32_t desyncDetections = 0;
        uint32_t resyncAttempts = 0;
        double averageInputLatencyMs = 0.0;
        std::vector<uint32_t> stallFrameNumbers;
    };

    struct Callbacks {
        std::function<void(uint32_t, const std::map<int, uint32_t>&)> frameReady;
        std::function<void(int, uint32_t)> peerInputTimedOut;
        std::function<void(int, uint32_t)> peerInputStalled;
        std::function<void(int, uint32_t, uint32_t)> inputReceived;
        std::function<void(uint32_t, const std::string&)> desyncDetected;
        std::function<void()> attemptingResync;
        std::function<void()> resyncSucceeded;
        std::function<void(const std::string&)> resyncFailed;
        std::function<void(const std::string&)> engineStatusChanged;
        std::function<void()> pumpNetwork;
    };

    explicit LockstepEngine(const Config& config);
    ~LockstepEngine();

    void setCallbacks(Callbacks callbacks);
    void setDataChannel(int peerSlot, std::shared_ptr<UserInterface::Netplay::WebRTCDataChannel> channel);
    /** Stores local input and returns (frame, state) pairs that need network relay. */
    std::vector<std::pair<uint32_t, uint32_t>> submitLocalInput(uint32_t controllerState);
    void submitRemoteInput(int fromSlot, uint32_t frameNumber, uint32_t controllerState);
    /** Peer-reported frame sync hash for a specific lockstep frame. */
    void submitPeerFrameSync(int fromSlot, uint32_t frameNumber, uint32_t stateHash);
    /** Stores this client's hash for a lockstep frame before broadcasting it. */
    void recordLocalFrameSync(uint32_t frameNumber, uint32_t stateHash);
    bool advanceFrame();
    void checkDesync(uint32_t stateHash);
    void requestResync();

    uint32_t getCurrentFrameNumber() const;
    /** Frame number to tag outbound packets with (current + input delay). */
    uint32_t getSendFrameNumber() const;
    std::map<int, uint32_t> getCurrentFrameInputs() const;
    std::map<int, uint32_t> getFrameInputs(uint32_t frameNumber) const;
    /** True when every remote session peer has input buffered for frameNumber. */
    bool hasAllRemoteInputsForFrame(uint32_t frameNumber) const;
    /** Snapshot of locally authored (frame, state) entries still in the buffer. */
    std::vector<std::pair<uint32_t, uint32_t>> copyLocalBufferedInputs() const;
    /** Resend every locally buffered input over WebRTC data channels. */
    void rebroadcastLocalBufferedInputs();
    bool isDesynchronized() const;
    int getPendingInputsCount() const;
    std::string getEngineStatus() const;
    Stats getStatistics() const;
    void resetStatistics();
    void setInputDelayFrames(int frames);
    void setNumPlayers(int numPlayers);
    void setLocalPlayerSlot(int slot);
    /** Unblock advanceFrame() waiters (e.g. after signaling loss). */
    void wakeInputWaiters();
    /** Whether a remote player slot is still in the session (signaling present). */
    void setPeerSessionActive(int slot, bool active);
    /** Fill missing peer inputs for the current frame and wake waiters. */
    void releaseCurrentFrameWait();
    /** True when at least one remote peer has an open WebRTC input channel. */
    bool hasOpenRemoteDataChannels() const;
    /** Detach callbacks/channels and unblock any waiters (safe during teardown). */
    void shutdown();

    /** Stall timeout (ms) when waiting for missing inputs; 0 waits indefinitely. */
    static int stallTimeoutForDelayFrames(int inputDelayFrames);

private:
    static const int INPUT_PACKET_SIZE = 8;
    static const uint32_t FALLBACK_INPUT = 0x00000000;

    void onDataChannelBinaryMessageReceived(int peerSlot, const std::vector<uint8_t>& data);
    void onDataChannelClosed(int peerSlot);
    void onDataChannelError(int peerSlot, const std::string& error);

    void broadcastInput(uint32_t controllerState, uint32_t frameNumber);
    void pruneOldFrames(uint32_t oldestFrameToKeep);
    void processInputPacket(int fromSlot, const std::vector<uint8_t>& packet);
    bool waitForAllInputs(uint32_t frameNumber, int timeoutMs);
    bool hasAllInputsForFrameUnlocked(uint32_t frameNumber) const;
    void notifyInputProgressUnlocked(uint32_t frameNumber);
    void comparePeerFrameSyncUnlocked(int fromSlot, uint32_t frameNumber, uint32_t peerHash);
    void reportStateHashMismatchUnlocked(
        int fromSlot,
        uint32_t frameNumber,
        uint32_t localHash,
        uint32_t peerHash);
    void pruneOldFrameSyncDataUnlocked(uint32_t oldestFrameToKeep);
    void applyTimeoutFallbackUnlocked(uint32_t frameNumber);
    void notifyPendingCallbacks();
    bool hasAllRemoteDataChannelsConnectedUnlocked() const;
    bool hasReceivedBootstrapInputFromAllRemotesUnlocked() const;
    bool allMissingInputsAreFromDisconnectedPeersUnlocked(
        uint32_t frameNumber) const;
    int computeInputWaitTimeoutMsUnlocked(uint32_t frameNumber) const;
    bool isAlive() const { return !m_shutdown.load(); }

    Config m_config;
    uint32_t m_currentFrameNumber = 0;
    mutable std::recursive_mutex m_mutex;
    std::condition_variable_any m_inputCv;
    std::atomic<bool> m_shutdown{false};
    bool m_isDesynchronized = false;
    Stats m_stats;
    std::map<uint32_t, FrameInputs> m_frameBuffer;
    std::map<int, bool> m_frameReceived;
    std::map<int, uint32_t> m_lastKnownInputs;
    std::map<int, uint32_t> m_lastKnownInputFrames;
    std::map<int, uint32_t> m_lastStallCallbackFrame;
    std::map<int, bool> m_peerSessionActive;
    std::map<uint32_t, uint32_t> m_localFrameSyncHashes;
    std::map<int, std::map<uint32_t, uint32_t>> m_pendingPeerFrameSyncHashes;
    std::unordered_set<uint64_t> m_reportedHashMismatches;
    std::map<int, int> m_peerHashMismatchStreak;
    std::vector<std::pair<int, uint32_t>> m_pendingStallNotifications;
    std::pair<uint32_t, std::string> m_pendingDesyncNotification;
    bool m_hasPendingDesyncNotification = false;
    bool m_pendingResync = false;
    std::vector<std::shared_ptr<UserInterface::Netplay::WebRTCDataChannel>> m_dataChannels;
    Callbacks m_callbacks;
};

} // namespace RMGCore

#endif // LOCKSTEPENGINE_HPP
