/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#ifndef NETPLAYPROTOCOL_HPP
#define NETPLAYPROTOCOL_HPP

#include <QHostAddress>
#include <QAbstractSocket>
#include <QNetworkInterface>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QUrlQuery>
#include <QUrl>
#include <QString>
#include <cstdint>

namespace UserInterface::Netplay {

static constexpr int kDefaultNetplayHostingPort = 2626; // UDP/ENet signaling
static constexpr const char* kNetplayIndexHost = "216.201.73.203";
static constexpr int kNetplayRegistryPort = 9150;
static constexpr int kNetplayIndexHttpPort = 9151;
static constexpr const char* kNetplayRegistryProtocol = "N02TRAV1";
static constexpr uint32_t kNetplayHostCodeMax = 0x0FFFFFFF;

inline QString localNetworkAddress()
{
    QString linkLocalFallback;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback()) {
                continue;
            }
            if (addr.isLinkLocal()) {
                if (linkLocalFallback.isEmpty()) {
                    linkLocalFallback = addr.toString();
                }
                continue;
            }
            return addr.toString();
        }
    }

    return linkLocalFallback;
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

inline bool isValidIndexKey(const QString& key)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._/-]{1,127}$"));
    return pattern.match(key.trimmed()).hasMatch();
}

inline QString formatHostCode(uint32_t hostCode)
{
    return QString("%1").arg(hostCode & kNetplayHostCodeMax, 7, 16, QLatin1Char('0')).toUpper();
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
    if (!ok || parsed > kNetplayHostCodeMax) {
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

inline QJsonObject sessionIndexPublishObject(const QJsonObject& session)
{
    static const QStringList keys = {
        QStringLiteral("room_name"),
        QStringLiteral("room_id"),
        QStringLiteral("roomId"),
        QStringLiteral("host_name"),
        QStringLiteral("host_code"),
        QStringLiteral("player_name"),
        QStringLiteral("game_name"),
        QStringLiteral("gameId"),
        QStringLiteral("md5_hash"),
        QStringLiteral("MD5"),
        QStringLiteral("rom_path"),
        QStringLiteral("connect_address"),
        QStringLiteral("public_address"),
        QStringLiteral("server_address"),
        QStringLiteral("connect_port"),
        QStringLiteral("public_port"),
        QStringLiteral("server_port"),
        QStringLiteral("use_nat_traversal"),
        QStringLiteral("connection_mode"),
        QStringLiteral("show_in_browser"),
        QStringLiteral("started"),
        QStringLiteral("player_count"),
        QStringLiteral("max_players"),
        QStringLiteral("lobby_size"),
        QStringLiteral("players"),
        QStringLiteral("is_hosting"),
        QStringLiteral("password"),
    };

    QJsonObject published;
    for (const QString& key : keys) {
        if (session.contains(key)) {
            published.insert(key, session.value(key));
        }
    }
    return published;
}

inline QString netplayIndexServerHostname()
{
    const QByteArray overrideHost = qgetenv("RMG_NAT_TRAVERSAL_HOST");
    if (!overrideHost.isEmpty()) {
        return QString::fromUtf8(overrideHost);
    }
    return QString::fromLatin1(kNetplayIndexHost);
}

inline QUrl netplayRoomsUrl(bool waitingOnly = true)
{
    QUrl url(QStringLiteral("http://%1:%2/rooms")
                 .arg(netplayIndexServerHostname())
                 .arg(kNetplayIndexHttpPort));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("waiting"), waitingOnly ? QStringLiteral("1") : QStringLiteral("0"));
    url.setQuery(query);
    return url;
}

inline QUrl netplaySessionIndexUrl(const QString& hostCode)
{
    const QString key = sessionIndexKey(hostCode);
    if (key.isEmpty()) {
        return {};
    }

    return QUrl(QStringLiteral("http://%1:%2/index/%3")
                    .arg(netplayIndexServerHostname())
                    .arg(kNetplayIndexHttpPort)
                    .arg(key));
}

inline QUrl netplaySessionIndexPutUrl(const QString& key)
{
    if (!isValidIndexKey(key)) {
        return {};
    }

    return QUrl(QStringLiteral("http://%1:%2/index/%3")
                    .arg(netplayIndexServerHostname())
                    .arg(kNetplayIndexHttpPort)
                    .arg(key));
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

inline bool turnBrokerDisabled()
{
    const QByteArray disabled = qgetenv("RMG_TURN_BROKER_DISABLED");
    return !disabled.isEmpty() && disabled != QByteArrayLiteral("0");
}

inline QUrl netplayTurnIceServersUrl()
{
    const QByteArray overrideUrl = qgetenv("RMG_TURN_BROKER_URL");
    if (!overrideUrl.isEmpty()) {
        return QUrl(QString::fromUtf8(overrideUrl));
    }

    if (turnBrokerDisabled()) {
        return {};
    }

    return QUrl(QStringLiteral("http://%1:%2/turn/ice-servers")
                    .arg(netplayIndexServerHostname())
                    .arg(kNetplayIndexHttpPort));
}

inline bool turnCredentialsAvailable()
{
    return !netplayTurnIceServersUrl().isEmpty();
}

enum class NetplayConnectionMode {
    Direct,
};

inline QString netplayConnectionModeToString(NetplayConnectionMode mode)
{
    Q_UNUSED(mode);
    return QStringLiteral("direct");
}

inline NetplayConnectionMode netplayConnectionModeFromString(const QString& value)
{
    Q_UNUSED(value);
    return NetplayConnectionMode::Direct;
}

inline bool sessionUsesNatTraversal(const QJsonObject& session)
{
    Q_UNUSED(session);
    return false;
}

inline QString sessionTraversalHostCode(const QJsonObject& session)
{
    return normalizeTraversalCode(session.value(QStringLiteral("host_code")).toString());
}

inline bool sessionConnectEndpoint(const QJsonObject& session, QString* addressOut, int* portOut)
{
    QString address = session.value(QStringLiteral("connect_address")).toString().trimmed();
    if (address.isEmpty() || !isUsableConnectAddress(address)) {
        address = session.value(QStringLiteral("public_address")).toString().trimmed();
    }
    if (address.isEmpty() || !isUsableConnectAddress(address)) {
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

} // namespace UserInterface::Netplay

#endif // NETPLAYPROTOCOL_HPP
