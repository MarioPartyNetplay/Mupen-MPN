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

#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <functional>
#include "../Library.hpp"

namespace UserInterface::Netplay {
class WebRTCDataChannel;
}

namespace RMGCore {

class CORE_EXPORT LockstepEngine {
public:

    struct Config {
        int numPlayers = 2;
        int localPlayerSlot = 0;
        int inputDelayFrames = 0;
        bool resyncEnabled = false;
        int resyncCheckIntervalFrames = 60;
        int stallTimeoutMilliseconds = 1000; // allow up to 1 second for peer input
    };

    struct FrameInputs {
        uint32_t frameNumber = 0;
        std::map<int, uint32_t> playerInputs;
        std::chrono::steady_clock::time_point receivedTime;
    };

    struct SyncCheckpoint {
        uint32_t frameNumber = 0;
        uint32_t romChecksum = 0;
        std::chrono::steady_clock::time_point timestamp;
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
    };

    explicit LockstepEngine(const Config& config);
    ~LockstepEngine();

    void setCallbacks(Callbacks callbacks);
    void setDataChannel(int peerSlot, std::shared_ptr<UserInterface::Netplay::WebRTCDataChannel> channel);
    void submitLocalInput(uint32_t controllerState);
    void submitRemoteInput(int fromSlot, uint32_t frameNumber, uint32_t controllerState);
    bool advanceFrame();
    void checkDesync(uint32_t romChecksum);
    void requestResync();

    uint32_t getCurrentFrameNumber() const;
    std::map<int, uint32_t> getCurrentFrameInputs() const;
    bool isDesynchronized() const;
    int getPendingInputsCount() const;
    std::string getEngineStatus() const;
    Stats getStatistics() const;
    void resetStatistics();
    void setInputDelayFrames(int frames);
    void setNumPlayers(int numPlayers);
    void setLocalPlayerSlot(int slot);
    
private:
    static const int INPUT_PACKET_SIZE = 8;
    static const uint32_t FALLBACK_INPUT = 0x00000000;

    void onDataChannelBinaryMessageReceived(int peerSlot, const std::vector<uint8_t>& data);
    void onDataChannelClosed(int peerSlot);
    void onDataChannelError(int peerSlot, const std::string& error);

    void broadcastInput(uint32_t controllerState);
    void processInputPacket(int fromSlot, const std::vector<uint8_t>& packet);
    bool waitForAllInputs(uint32_t frameNumber, int timeoutMs);
    void applyTimeoutFallback(uint32_t frameNumber);
    
    Config m_config;
    uint32_t m_currentFrameNumber = 0;
    mutable std::recursive_mutex m_mutex;
    bool m_isDesynchronized = false;
    uint32_t m_lastVerifiedFrame = 0;
    Stats m_stats;
    std::map<uint32_t, FrameInputs> m_frameBuffer;
    std::map<int, bool> m_frameReceived;
    std::map<int, uint32_t> m_lastKnownInputs;
    std::map<int, uint32_t> m_lastKnownInputFrames;
    std::vector<std::shared_ptr<UserInterface::Netplay::WebRTCDataChannel>> m_dataChannels;
    std::vector<SyncCheckpoint> m_syncCheckpoints;
    Callbacks m_callbacks;
};

} // namespace RMGCore

#endif // LOCKSTEPENGINE_HPP
