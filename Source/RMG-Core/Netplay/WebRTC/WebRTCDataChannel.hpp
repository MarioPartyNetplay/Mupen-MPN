/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef WEBRTCDATACHANNEL_HPP
#define WEBRTCDATACHANNEL_HPP

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>
#include "../../Library.hpp"

namespace UserInterface::Netplay {

class CORE_EXPORT WebRTCDataChannel {
public:
    enum class ChannelState {
        Connecting,
        Open,
        Closing,
        Closed
    };

    using BinaryMessageCallback = std::function<void(const std::vector<uint8_t>&)>;
    using ClosedCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using TextMessageCallback = std::function<void(const std::string&)>;
    using StateChangedCallback = std::function<void(ChannelState)>;
    using BufferedAmountLowCallback = std::function<void()>;
    using SendBinaryHandler = std::function<bool(const std::vector<uint8_t>&)>;
    using SendTextHandler = std::function<bool(const std::string&)>;
    using CloseHandler = std::function<void()>;

    explicit WebRTCDataChannel(const std::string& label);
    ~WebRTCDataChannel();

    bool sendBinary(const std::vector<uint8_t>& data);
    bool sendText(const std::string& text);
    void close();
    void open();
    void notifyOpen();
    void notifyClosed();
    void notifyError(const std::string& error);
    void setBackendHandlers(SendBinaryHandler sendBinary, SendTextHandler sendText, CloseHandler close);
    /** Drop libdatachannel send/close hooks without touching channel state. */
    void detachBackendHandlers();

    ChannelState getState() const;
    const std::string& getLabel() const;
    bool isOpen() const;
    uint64_t getBufferedAmount() const;

    BinaryMessageCallback onBinaryMessageReceived;
    ClosedCallback onClosed;
    ErrorCallback onError;
    TextMessageCallback onTextMessageReceived;
    StateChangedCallback onStateChanged;
    BufferedAmountLowCallback onBufferedAmountLow;

private:
    std::string m_label;
    ChannelState m_state;
    SendBinaryHandler m_sendBinaryHandler;
    SendTextHandler m_sendTextHandler;
    CloseHandler m_closeHandler;
};

} // namespace UserInterface::Netplay

#endif // WEBRTCDATACHANNEL_HPP
