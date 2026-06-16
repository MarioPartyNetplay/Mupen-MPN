/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NATTRAVERSALPROTOCOL_HPP
#define NATTRAVERSALPROTOCOL_HPP

#include <QHostAddress>
#include <QAbstractSocket>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QString>
#include <cstdint>

namespace UserInterface::Netplay {

// Nat has to implement server protocol in Source/Server.
// as of writing im not sure if im going to make it public...
// Maybe unelss abuse arises...
static constexpr const char* kNatTraversalHost = "216.225.154.31";
// NAT traversal service (Source/Server/nat_traversal_server) — not the game port.
static constexpr int kNatTraversalPort = 9290;   // UDP: N02TRAV1 + N02IDX1
static constexpr int kNatIndexHttpPort = 9291;   // HTTP index browser
// Socket.IO netplay host listens here; register this port with NAT (REGISTER|port).
static constexpr int kDefaultNetplayHostingPort = 2626;
static constexpr const char* kNatTraversalProtocol = "N02TRAV1";
static constexpr const char* kNatIndexProtocol = "N02IDX1";
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

inline bool looksLikeIpAddress(const QString& text)
{
    QHostAddress address;
    return address.setAddress(text.trimmed());
}

inline bool isUsableConnectAddress(const QString& text)
{
    QHostAddress address;
    if (!address.setAddress(text.trimmed())) {
        return false;
    }
    if (address.isLoopback()) {
        return false;
    }
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = address.toIPv4Address();
        const quint8 b0 = static_cast<quint8>((ip >> 24) & 0xFF);
        const quint8 b1 = static_cast<quint8>((ip >> 16) & 0xFF);
        if (b0 == 10) {
            return false;
        }
        if (b0 == 192 && b1 == 168) {
            return false;
        }
        if (b0 == 172 && b1 >= 16 && b1 <= 31) {
            return false;
        }
    }
    return true;
}

inline bool sessionConnectEndpoint(const QJsonObject& session, QString* addressOut, int* portOut)
{
    QString address = session.value(QStringLiteral("connect_address")).toString().trimmed();
    if (address.isEmpty() || looksLikeTraversalCode(address) || !isUsableConnectAddress(address)) {
        address = session.value(QStringLiteral("public_address")).toString().trimmed();
    }
    if (address.isEmpty() || looksLikeTraversalCode(address) || !isUsableConnectAddress(address)) {
        return false;
    }

    const int port = session.value(QStringLiteral("connect_port"))
                         .toInt(session.value(QStringLiteral("server_port"))
                                    .toInt(session.value(QStringLiteral("public_port"))
                                               .toInt(kDefaultNetplayHostingPort)));
    if (port < 1024 || port > 65535) {
        return false;
    }

    if (addressOut) {
        *addressOut = address;
    }
    if (portOut) {
        *portOut = port;
    }
    return true;
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

inline QString stunServerHostname()
{
    const QByteArray overrideHost = qgetenv("RMG_STUN_SERVER");
    if (!overrideHost.isEmpty()) {
        return QString::fromUtf8(overrideHost);
    }

    const QByteArray legacyOverrideHost = qgetenv("STUN_SERVER");
    if (!legacyOverrideHost.isEmpty()) {
        return QString::fromUtf8(legacyOverrideHost);
    }

    return QStringLiteral("stun.dolphin-emu.org:6262");
}

inline QStringList turnServerUrls()
{
    const QByteArray overrideUrls = qgetenv("RMG_TURN_URLS");
    if (!overrideUrls.isEmpty()) {
        const QStringList urls = QString::fromUtf8(overrideUrls).split(QRegularExpression(QStringLiteral("[;,\\n\\r]+")), Qt::SkipEmptyParts);
        if (!urls.isEmpty()) {
            return urls;
        }
    }

    const QByteArray legacyOverrideUrls = qgetenv("TURN_URLS");
    if (!legacyOverrideUrls.isEmpty()) {
        const QStringList urls = QString::fromUtf8(legacyOverrideUrls).split(QRegularExpression(QStringLiteral("[;,\\n\\r]+")), Qt::SkipEmptyParts);
        if (!urls.isEmpty()) {
            return urls;
        }
    }

    const QByteArray legacySingleUrl = qgetenv("TURN_URL");
    if (!legacySingleUrl.isEmpty()) {
        return { QString::fromUtf8(legacySingleUrl) };
    }

    return {};
}

inline QString turnServerUsername()
{
    const QByteArray overrideUsername = qgetenv("RMG_TURN_USERNAME");
    if (!overrideUsername.isEmpty()) {
        return QString::fromUtf8(overrideUsername);
    }

    const QByteArray legacyOverrideUsername = qgetenv("TURN_USERNAME");
    if (!legacyOverrideUsername.isEmpty()) {
        return QString::fromUtf8(legacyOverrideUsername);
    }

    return {};
}

inline QString turnServerCredential()
{
    const QByteArray overrideCredential = qgetenv("RMG_TURN_CREDENTIAL");
    if (!overrideCredential.isEmpty()) {
        return QString::fromUtf8(overrideCredential);
    }

    const QByteArray legacyOverrideCredential = qgetenv("TURN_CREDENTIAL");
    if (!legacyOverrideCredential.isEmpty()) {
        return QString::fromUtf8(legacyOverrideCredential);
    }

    return {};
}

} // namespace UserInterface::Netplay

#endif // NATTRAVERSALPROTOCOL_HPP
