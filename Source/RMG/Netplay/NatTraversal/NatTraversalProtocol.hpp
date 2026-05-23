/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NATTRAVERSALPROTOCOL_HPP
#define NATTRAVERSALPROTOCOL_HPP

#include <QRegularExpression>
#include <QString>
#include <cstdint>

namespace UserInterface::Netplay {

// Nat has to implement server protocol in Source/Server.
// as of writing im not sure if im going to make it public...
// Maybe unelss abuse arises...
static constexpr const char* kNatTraversalHost = "216.225.154.31";
static constexpr int kNatTraversalPort = 9290;
static constexpr const char* kNatTraversalProtocol = "N02TRAV1";
static constexpr const char* kNatIndexProtocol = "N02IDX1";
static constexpr int kNatIndexHttpPort = 9291;
static constexpr uint32_t kNatTraversalMaxHostCode = 0x0FFFFFFF;

inline bool isValidIndexKey(const QString& key)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._/-]{1,127}$"));
    return pattern.match(key.trimmed()).hasMatch();
}

inline QString formatHostCode(uint32_t hostCode)
{
    return QString("%1").arg(hostCode & kNatTraversalMaxHostCode, 7, 16, QLatin1Char('0')).toUpper();
}

inline bool isHexHostCodeString(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9A-Fa-f]{7}$"));
    return pattern.match(text.trimmed()).hasMatch();
}

inline bool parseHostCodeString(const QString& text, uint32_t& hostCodeOut)
{
    const QString trimmed = text.trimmed().toUpper();
    if (!isHexHostCodeString(trimmed)) {
        return false;
    }

    bool ok = false;
    const uint32_t parsed = trimmed.toUInt(&ok, 16);
    if (!ok || parsed > kNatTraversalMaxHostCode) {
        return false;
    }

    hostCodeOut = parsed;
    return true;
}

inline QString normalizeTraversalCode(const QString& input)
{
    uint32_t hostCode = 0;
    if (!parseHostCodeString(input, hostCode)) {
        return {};
    }
    return formatHostCode(hostCode);
}

inline bool looksLikeTraversalCode(const QString& input)
{
    return !normalizeTraversalCode(input).isEmpty();
}

inline QString sessionIndexKey(const QString& hostCode)
{
    const QString normalized = normalizeTraversalCode(hostCode);
    if (normalized.isEmpty()) {
        return {};
    }
    return QStringLiteral("session/") + normalized;
}

inline QString natTraversalServerHostname()
{
    const QByteArray overrideHost = qgetenv("RMG_NAT_TRAVERSAL_HOST");
    if (!overrideHost.isEmpty()) {
        return QString::fromUtf8(overrideHost);
    }
    return QString::fromLatin1(kNatTraversalHost);
}

} // namespace UserInterface::Netplay

#endif // NATTRAVERSALPROTOCOL_HPP
