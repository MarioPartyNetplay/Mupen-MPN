/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 */
#include "NetplayUpnp.hpp"
#include "NetplayProtocol.hpp"

#include <algorithm>

#include <QDebug>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QPair>
#include <QNetworkRequest>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QXmlStreamReader>

namespace UserInterface::Netplay {

namespace {

struct UpnpService
{
    QString type;
    QUrl controlUrl;
};

struct ActiveMapping
{
    quint16 port = 0;
    QString protocol;
    QString serviceType;
    QUrl controlUrl;
};

ActiveMapping g_mapping;

QString headerValue(const QString& message, const QString& name)
{
    const QString prefix = name + QLatin1Char(':');
    const QStringList lines = message.split(QStringLiteral("\r\n"));
    for (const QString& line : lines) {
        if (line.startsWith(prefix, Qt::CaseInsensitive)) {
            return line.mid(prefix.size()).trimmed();
        }
    }
    return {};
}

QString localAddressForGateway(const QHostAddress& gateway)
{
    if (gateway.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 gatewayAddress = gateway.toIPv4Address();
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            if (!(iface.flags() & QNetworkInterface::IsUp) ||
                !(iface.flags() & QNetworkInterface::IsRunning) ||
                (iface.flags() & QNetworkInterface::IsLoopBack)) {
                continue;
            }

            for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                const QHostAddress address = entry.ip();
                if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) {
                    continue;
                }

                const QHostAddress netmask = entry.netmask();
                if (netmask.protocol() != QAbstractSocket::IPv4Protocol) {
                    continue;
                }

                const quint32 mask = netmask.toIPv4Address();
                if ((address.toIPv4Address() & mask) == (gatewayAddress & mask)) {
                    return address.toString();
                }
            }
        }
    }

    return localNetworkAddress();
}

bool discoverGateway(QUrl* locationOut, QHostAddress* gatewayOut)
{
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "UPnP: failed to bind discovery socket:" << socket.errorString();
        return false;
    }
    socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 2);

    const QHostAddress multicast(QStringLiteral("239.255.255.250"));
    const QList<QByteArray> searches = {
        QByteArrayLiteral(
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 1\r\n"
            "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
            "\r\n"),
        QByteArrayLiteral(
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 1\r\n"
            "ST: urn:schemas-upnp-org:service:WANIPConnection:1\r\n"
            "\r\n"),
    };

    for (const QByteArray& search : searches) {
        if (socket.writeDatagram(search, multicast, 1900) < 0) {
            qWarning() << "UPnP: failed to send discovery:" << socket.errorString();
        }
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
    timer.start(1500);

    while (timer.isActive()) {
        if (!socket.hasPendingDatagrams()) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        while (socket.hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(static_cast<int>(socket.pendingDatagramSize()));
            QHostAddress sender;
            socket.readDatagram(datagram.data(), datagram.size(), &sender);

            const QString location = headerValue(QString::fromUtf8(datagram), QStringLiteral("LOCATION"));
            const QUrl locationUrl(location);
            if (!locationUrl.isValid() || locationUrl.scheme().isEmpty()) {
                continue;
            }

            *locationOut = locationUrl;
            *gatewayOut = sender;
            return true;
        }
    }

    qWarning() << "UPnP: no Internet Gateway Device answered";
    return false;
}

QByteArray httpExchange(const QNetworkRequest& request, const QByteArray& body, bool post, int* statusOut, QString* errorOut)
{
    QNetworkAccessManager manager;
    QNetworkReply* reply = post ? manager.post(request, body) : manager.get(request);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    if (!reply->isFinished()) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }

    if (!reply->isFinished()) {
        reply->abort();
        *errorOut = QStringLiteral("timed out");
        *statusOut = 0;
        reply->deleteLater();
        return {};
    }

    *statusOut = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    *errorOut = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    const QByteArray payload = reply->readAll();
    reply->deleteLater();
    return payload;
}

int servicePreference(const QString& type)
{
    if (type.contains(QStringLiteral("WANIPConnection:1"))) {
        return 0;
    }
    if (type.contains(QStringLiteral("WANIPConnection:2"))) {
        return 1;
    }
    if (type.contains(QStringLiteral("WANPPPConnection:1"))) {
        return 2;
    }
    if (type.contains(QStringLiteral("WANPPPConnection:2"))) {
        return 3;
    }
    return -1;
}

QUrl resolveControlUrl(const QUrl& descriptionUrl, const QString& urlBase, const QString& control)
{
    const QUrl controlUrl(control);
    if (controlUrl.isValid() && !controlUrl.scheme().isEmpty()) {
        return controlUrl;
    }

    QUrl base = descriptionUrl;
    if (!urlBase.isEmpty()) {
        const QUrl parsedBase(urlBase);
        if (parsedBase.isValid() && !parsedBase.scheme().isEmpty()) {
            base = parsedBase;
        }
    }
    return base.resolved(controlUrl);
}

QList<UpnpService> readServices(const QUrl& descriptionUrl, const QByteArray& xml)
{
    QString urlBase;
    QString serviceType;
    QString controlUrl;
    bool inService = false;
    QList<QPair<QString, QString>> rawServices;

    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name == QStringLiteral("service")) {
                inService = true;
                serviceType.clear();
                controlUrl.clear();
            } else if (name == QStringLiteral("URLBase")) {
                urlBase = reader.readElementText().trimmed();
            } else if (inService && name == QStringLiteral("serviceType")) {
                serviceType = reader.readElementText().trimmed();
            } else if (inService && name == QStringLiteral("controlURL")) {
                controlUrl = reader.readElementText().trimmed();
            }
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("service")) {
            inService = false;
            if (servicePreference(serviceType) >= 0 && !controlUrl.isEmpty()) {
                rawServices.append(qMakePair(serviceType, controlUrl));
            }
        }
    }

    QList<UpnpService> services;
    services.reserve(rawServices.size());
    for (const QPair<QString, QString>& raw : rawServices) {
        services.append(UpnpService{
            raw.first,
            resolveControlUrl(descriptionUrl, urlBase, raw.second),
        });
    }

    std::sort(services.begin(), services.end(), [](const UpnpService& left, const UpnpService& right) {
        return servicePreference(left.type) < servicePreference(right.type);
    });
    return services;
}

bool soapAction(const UpnpService& service, const QString& action, const QString& body, QString* errorOut)
{
    QNetworkRequest request(service.controlUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    request.setRawHeader("SOAPAction", QStringLiteral("\"%1#%2\"").arg(service.type, action).toUtf8());
    request.setTransferTimeout(3000);

    int status = 0;
    QString error;
    const QByteArray response = httpExchange(request, body.toUtf8(), true, &status, &error);
    if (status == 200 && !response.contains("Fault")) {
        return true;
    }

    const QString detail = QString::fromUtf8(response.left(300)).simplified();
    *errorOut = detail.isEmpty() ? (error.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : error) : detail;
    return false;
}

QString portMappingBody(const QString& serviceType, const QString& action, quint16 port, const QString& protocol, const QString& internalClient)
{
    if (action == QStringLiteral("DeletePortMapping")) {
        return QStringLiteral(
                   "<?xml version=\"1.0\"?>"
                   "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                   "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                   "<s:Body><u:DeletePortMapping xmlns:u=\"%1\">"
                   "<NewRemoteHost></NewRemoteHost>"
                   "<NewExternalPort>%2</NewExternalPort>"
                   "<NewProtocol>%3</NewProtocol>"
                   "</u:DeletePortMapping></s:Body></s:Envelope>")
            .arg(serviceType)
            .arg(port)
            .arg(protocol);
    }

    return QStringLiteral(
               "<?xml version=\"1.0\"?>"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
               "<s:Body><u:AddPortMapping xmlns:u=\"%1\">"
               "<NewRemoteHost></NewRemoteHost>"
               "<NewExternalPort>%2</NewExternalPort>"
               "<NewProtocol>%3</NewProtocol>"
               "<NewInternalPort>%2</NewInternalPort>"
               "<NewInternalClient>%4</NewInternalClient>"
               "<NewEnabled>1</NewEnabled>"
               "<NewPortMappingDescription>Mupen-MPN</NewPortMappingDescription>"
               "<NewLeaseDuration>0</NewLeaseDuration>"
               "</u:AddPortMapping></s:Body></s:Envelope>")
        .arg(serviceType)
        .arg(port)
        .arg(protocol)
        .arg(internalClient);
}

} // namespace

bool netplayUpnpMapPort(quint16 port)
{
    if (port == 0) {
        return false;
    }

    netplayUpnpUnmapPort();

    QUrl location;
    QHostAddress gateway;
    if (!discoverGateway(&location, &gateway)) {
        return false;
    }

    QNetworkRequest descriptionRequest(location);
    descriptionRequest.setTransferTimeout(3000);
    int status = 0;
    QString error;
    const QByteArray description = httpExchange(descriptionRequest, QByteArray(), false, &status, &error);
    if (status != 200 || description.isEmpty()) {
        qWarning() << "UPnP: failed to read gateway description:" << (error.isEmpty() ? QString::number(status) : error);
        return false;
    }

    const QList<UpnpService> services = readServices(location, description);
    if (services.isEmpty()) {
        qWarning() << "UPnP: gateway has no WAN IP connection service";
        return false;
    }

    const QString internalClient = localAddressForGateway(gateway);
    if (internalClient.isEmpty()) {
        qWarning() << "UPnP: could not determine the local address";
        return false;
    }

    const QString protocol = QStringLiteral("UDP");
    QString soapError;
    for (const UpnpService& service : services) {
        const QString body = portMappingBody(service.type, QStringLiteral("AddPortMapping"), port, protocol, internalClient);
        if (!soapAction(service, QStringLiteral("AddPortMapping"), body, &soapError)) {
            // A leftover mapping from a previous session is replaced once.
            if (soapError.contains(QStringLiteral("718")) ||
                soapError.contains(QStringLiteral("ConflictInMappingEntry"), Qt::CaseInsensitive)) {
                const QString removeBody = portMappingBody(service.type, QStringLiteral("DeletePortMapping"), port, protocol, internalClient);
                QString ignored;
                soapAction(service, QStringLiteral("DeletePortMapping"), removeBody, &ignored);
                if (soapAction(service, QStringLiteral("AddPortMapping"), body, &soapError)) {
                    g_mapping = ActiveMapping{port, protocol, service.type, service.controlUrl};
                    qInfo() << "UPnP: mapped external UDP" << port << "to" << internalClient;
                    return true;
                }
            }
            qWarning() << "UPnP: AddPortMapping failed on" << service.type << soapError;
            continue;
        }

        g_mapping = ActiveMapping{port, protocol, service.type, service.controlUrl};
        qInfo() << "UPnP: mapped external UDP" << port << "to" << internalClient;
        return true;
    }

    return false;
}

void netplayUpnpUnmapPort()
{
    if (g_mapping.port == 0 || !g_mapping.controlUrl.isValid()) {
        g_mapping = {};
        return;
    }

    const UpnpService service{g_mapping.serviceType, g_mapping.controlUrl};
    const QString body = portMappingBody(service.type, QStringLiteral("DeletePortMapping"), g_mapping.port, g_mapping.protocol, QString());
    QString error;
    if (!soapAction(service, QStringLiteral("DeletePortMapping"), body, &error)) {
        qWarning() << "UPnP: DeletePortMapping failed:" << error;
    } else {
        qInfo() << "UPnP: removed UDP mapping for port" << g_mapping.port;
    }

    g_mapping = {};
}

} // namespace UserInterface::Netplay
