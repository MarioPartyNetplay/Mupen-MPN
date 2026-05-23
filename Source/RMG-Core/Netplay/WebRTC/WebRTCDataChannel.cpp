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

using namespace UserInterface::Netplay;

WebRTCDataChannel::WebRTCDataChannel(const std::string& label)
    : m_label(label)
    , m_state(ChannelState::Connecting)
{
    std::cerr << "WebRTCDataChannel created: " << label << std::endl;
}

WebRTCDataChannel::~WebRTCDataChannel()
{
    close();
}

bool WebRTCDataChannel::sendBinary(const std::vector<uint8_t>& data)
{
    if (m_state != ChannelState::Open) {
        std::cerr << "WebRTCDataChannel: Cannot send - channel not open" << std::endl;
        return false;
    }

    // TODO: Send binary data using libdatachannel
    return true;
}

bool WebRTCDataChannel::sendText(const std::string& text)
{
    if (m_state != ChannelState::Open) {
        std::cerr << "WebRTCDataChannel: Cannot send - channel not open" << std::endl;
        return false;
    }

    // TODO: Send text data using libdatachannel
    return true;
}

void WebRTCDataChannel::close()
{
    if (m_state == ChannelState::Closed) {
        return;
    }

    std::cerr << "WebRTCDataChannel: Closing " << m_label << std::endl;
    m_state = ChannelState::Closed;
    if (onClosed) {
        onClosed();
    }
}

void WebRTCDataChannel::open()
{
    if (m_state == ChannelState::Open) {
        return;
    }

    m_state = ChannelState::Open;
    if (onStateChanged) {
        onStateChanged(m_state);
    }
}

WebRTCDataChannel::ChannelState WebRTCDataChannel::getState() const
{
    return m_state;
}

const std::string& WebRTCDataChannel::getLabel() const
{
    return m_label;
}

bool WebRTCDataChannel::isOpen() const
{
    return m_state == ChannelState::Open;
}

uint64_t WebRTCDataChannel::getBufferedAmount() const
{
    return 0;
}
