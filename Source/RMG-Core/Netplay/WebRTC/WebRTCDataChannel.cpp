/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "WebRTCDataChannel.hpp"
#include <iostream>
#include <utility>

using namespace UserInterface::Netplay;

WebRTCDataChannel::WebRTCDataChannel(const std::string& label)
    : m_label(label)
    , m_state(ChannelState::Connecting)
{
    std::cerr << "WebRTCDataChannel created: " << label << std::endl;
}

WebRTCDataChannel::~WebRTCDataChannel()
{
    // Do not call close() here. WebRTCPeer tears down the libdatachannel backend
    // first; invoking the stored close handler afterwards double-frees it.
    onBinaryMessageReceived = nullptr;
    onClosed = nullptr;
    onError = nullptr;
    onTextMessageReceived = nullptr;
    onStateChanged = nullptr;
    onBufferedAmountLow = nullptr;
    detachBackendHandlers();
    m_state.store(ChannelState::Closed);
}

bool WebRTCDataChannel::sendBinary(const std::vector<uint8_t>& data)
{
    if (m_state.load() != ChannelState::Open) {
        std::cerr << "WebRTCDataChannel: Cannot send - channel not open" << std::endl;
        return false;
    }

    // Copy the handler under the lock so a concurrent setBackendHandlers()/
    // detachBackendHandlers()/close() on the Qt thread cannot free the target
    // while we invoke it on the emulator thread. Invoke outside the lock to
    // avoid blocking channel teardown on a backend send.
    SendBinaryHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        handler = m_sendBinaryHandler;
    }

    if (handler) {
        try {
            return handler(data);
        } catch (const std::exception& exception) {
            // sendBinary runs on the emulator thread; do not invoke the
            // onError callback here since it is owned/mutated by the Qt thread.
            std::cerr << "WebRTCDataChannel: sendBinary failed: "
                      << exception.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "WebRTCDataChannel: Unknown sendBinary exception"
                      << std::endl;
            return false;
        }
    }

    return true;
}

bool WebRTCDataChannel::sendText(const std::string& text)
{
    if (m_state.load() != ChannelState::Open) {
        std::cerr << "WebRTCDataChannel: Cannot send - channel not open" << std::endl;
        return false;
    }

    SendTextHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        handler = m_sendTextHandler;
    }

    if (handler) {
        try {
            return handler(text);
        } catch (const std::exception& exception) {
            std::cerr << "WebRTCDataChannel: sendText failed: "
                      << exception.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "WebRTCDataChannel: Unknown sendText exception"
                      << std::endl;
            return false;
        }
    }

    return true;
}

void WebRTCDataChannel::close()
{
    if (m_state.load() == ChannelState::Closed) {
        return;
    }

    std::cerr << "WebRTCDataChannel: Closing " << m_label << std::endl;

    CloseHandler closeHandler;
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        closeHandler = std::move(m_closeHandler);
        m_sendBinaryHandler = nullptr;
        m_sendTextHandler = nullptr;
    }

    if (closeHandler) {
        try {
            closeHandler();
        } catch (const std::exception& exception) {
            notifyError(exception.what());
        } catch (...) {
            notifyError("Unknown DataChannel close exception");
        }
    }

    notifyClosed();
}

void WebRTCDataChannel::detachBackendHandlers()
{
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_sendBinaryHandler = nullptr;
    m_sendTextHandler = nullptr;
    m_closeHandler = nullptr;
}

void WebRTCDataChannel::open()
{
    if (m_state.load() == ChannelState::Open) {
        return;
    }

    m_state.store(ChannelState::Open);
    if (onStateChanged) {
        onStateChanged(m_state.load());
    }
}

void WebRTCDataChannel::notifyOpen()
{
    if (m_state.load() == ChannelState::Open) {
        return;
    }

    m_state.store(ChannelState::Open);
    if (onStateChanged) {
        onStateChanged(m_state.load());
    }
}

void WebRTCDataChannel::notifyClosed()
{
    if (m_state.load() == ChannelState::Closed) {
        return;
    }

    m_state.store(ChannelState::Closed);
    if (onClosed) {
        onClosed();
    }
    if (onStateChanged) {
        onStateChanged(m_state.load());
    }
}

void WebRTCDataChannel::notifyError(const std::string& error)
{
    if (onError) {
        onError(error);
    }
}

void WebRTCDataChannel::setBackendHandlers(SendBinaryHandler sendBinary, SendTextHandler sendText, CloseHandler close)
{
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_sendBinaryHandler = std::move(sendBinary);
    m_sendTextHandler = std::move(sendText);
    m_closeHandler = std::move(close);
}

WebRTCDataChannel::ChannelState WebRTCDataChannel::getState() const
{
    return m_state.load();
}

const std::string& WebRTCDataChannel::getLabel() const
{
    return m_label;
}

bool WebRTCDataChannel::isOpen() const
{
    return m_state.load() == ChannelState::Open;
}

uint64_t WebRTCDataChannel::getBufferedAmount() const
{
    return 0;
}
