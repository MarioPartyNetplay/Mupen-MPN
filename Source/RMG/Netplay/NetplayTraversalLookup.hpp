/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYTRAVERSALLOOKUP_HPP
#define NETPLAYTRAVERSALLOOKUP_HPP

#include <QString>

namespace UserInterface::Netplay {

struct TraversalLookupResult {
    bool success = false;
    QString address;
    int port = 0;
    QString error;
};

TraversalLookupResult lookupTraversalHost(const QString& hostCode);

} // namespace UserInterface::Netplay

#endif // NETPLAYTRAVERSALLOOKUP_HPP
