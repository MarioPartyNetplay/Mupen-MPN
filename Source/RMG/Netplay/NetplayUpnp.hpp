/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYUPNP_HPP
#define NETPLAYUPNP_HPP

#include <QtGlobal>

namespace UserInterface::Netplay {

bool netplayUpnpMapPort(quint16 port);
void netplayUpnpUnmapPort();

} // namespace UserInterface::Netplay

#endif // NETPLAYUPNP_HPP
