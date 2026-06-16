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
#include <QUrlQuery>
#include <QUrl>
#include <QString>
#include <cstdint>

namespace UserInterface::Netplay {

// Nat has to implement server protocol in Source/Server.
// as of writing im not sure if im going to make it public...
// Maybe unelss abuse arises...
static constexpr const char* kNatTraversalHost = "216.201.72.143";
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

inline QUrl natTraversalRoomsUrl(bool waitingOnly = true)
{
    QUrl url(QStringLiteral("http://%1:%2/rooms")
                 .arg(natTraversalServerHostname())
                 .arg(kNatIndexHttpPort));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("waiting"), waitingOnly ? QStringLiteral("1") : QStringLiteral("0"));
    url.setQuery(query);
    return url;
}

static constexpr quint16 kDefaultStunPort = 6262;

inline QString stunServerConfig()
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

inline QString stunServerHostname()
{
    return stunServerConfig();
}

inline quint16 stunServerPort()
{
    const QByteArray overridePort = qgetenv("RMG_STUN_PORT");
    if (!overridePort.isEmpty()) {
        bool ok = false;
        const int parsed = QString::fromUtf8(overridePort).toInt(&ok);
        if (ok && parsed >= 1 && parsed <= 65535) {
            return static_cast<quint16>(parsed);
        }
    }

    QString config = stunServerConfig().trimmed();
    if (config.startsWith('[') && config.contains(']')) {
        const int closeIndex = config.indexOf(']');
        const QString portPart = config.mid(closeIndex + 1).trimmed();
        if (portPart.startsWith(':')) {
            bool ok = false;
            const int parsed = portPart.mid(1).toInt(&ok);
            if (ok && parsed >= 1 && parsed <= 65535) {
                return static_cast<quint16>(parsed);
            }
        }
    } else {
        const int colonIndex = config.lastIndexOf(':');
        if (colonIndex > 0 && config.indexOf(':') == colonIndex) {
            bool ok = false;
            const int parsed = config.mid(colonIndex + 1).toInt(&ok);
            if (ok && parsed >= 1 && parsed <= 65535) {
                return static_cast<quint16>(parsed);
            }
        }
    }

    return kDefaultStunPort;
}

inline QString stunServerHost()
{
    QString host = stunServerConfig().trimmed();

    if (host.startsWith("stun://", Qt::CaseInsensitive)) {
        host = host.mid(7);
    } else if (host.startsWith("stuns://", Qt::CaseInsensitive)) {
        host = host.mid(8);
    } else if (host.startsWith("stun:", Qt::CaseInsensitive)) {
        host = host.mid(5);
    } else if (host.startsWith("stuns:", Qt::CaseInsensitive)) {
        host = host.mid(6);
    }

    const int slashIndex = host.indexOf('/');
    if (slashIndex != -1) {
        host = host.left(slashIndex);
    }

    if (host.startsWith('[') && host.contains(']')) {
        const int closeIndex = host.indexOf(']');
        return host.mid(1, closeIndex - 1).trimmed();
    }

    const int colonIndex = host.lastIndexOf(':');
    if (colonIndex > 0 && host.indexOf(':') == colonIndex) {
        return host.left(colonIndex);
    }

    return host;
}

} // namespace UserInterface::Netplay

#endif // NATTRAVERSALPROTOCOL_HPP
