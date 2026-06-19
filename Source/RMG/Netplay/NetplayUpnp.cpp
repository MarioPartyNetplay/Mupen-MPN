/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayUpnp.hpp"

#include <QDebug>

namespace UserInterface::Netplay {

namespace {
quint16 g_mappedPort = 0;
} // namespace

bool netplayUpnpMapPort(quint16 port)
{
    if (port == 0) {
        return false;
    }

    // UPnP IGD port mapping is only used for direct hosting, not traversal-server play.
    qWarning() << "NetplayUpnp: UPnP port mapping is not available on this build (requested port" << port << ")";
    g_mappedPort = 0;
    return false;
}

void netplayUpnpUnmapPort()
{
    if (g_mappedPort == 0) {
        return;
    }

    g_mappedPort = 0;
}

} // namespace UserInterface::Netplay
