/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "CreateNetplaySessionDialog.hpp"
#include "Utilities/QtMessageBox.hpp"
#include "NetplayCommon.hpp"
#include "Netplay/NetplayCoordinator.hpp"
#include "Netplay/NatTraversal/NatTraversalProtocol.hpp"
#include "Netplay/NatTraversal/NatTraversalClient.hpp"

#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QCheckBox>
#include <QNetworkDatagram>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QPushButton>
#include <QJsonObject>
#include <QJsonArray>
#include <QSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QUrlQuery>
#include <QFileInfo>
#include <QFile>
#include <QUrl>

#include <RMG-Core/Settings.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;
// Provide `Netplay::` alias for existing code that refers to that namespace
namespace Netplay = UserInterface::Netplay;


//
// Exported Functions
//


CreateNetplaySessionDialog::CreateNetplaySessionDialog(QWidget *parent, UserInterface::Netplay::NetplayCoordinator* coordinator, QMap<QString, CoreRomSettings> modelData) : QDialog(parent)
{
    this->setupUi(this);

    // Store coordinator reference
    this->coordinator = coordinator;
    
    // Connect to coordinator signals
    connect(this->coordinator, &Netplay::NetplayCoordinator::roomCreated, this,
            [this](const QString& roomId, int slot) {
        qDebug() << "Session created with room ID:" << roomId;
        QDialog::accept();
    });
    connect(this->coordinator, &Netplay::NetplayCoordinator::connectionError, this,
            [this](const QString& error) {
        QtMessageBox::Error(this, "Hosting Error", error);
        this->toggleUI(true, this->validate());
    });
    
    // prepare broadcast
    broadcastSocket.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    connect(&this->broadcastSocket, &QUdpSocket::readyRead, this, &CreateNetplaySessionDialog::on_broadcastSocket_readyRead);
    QByteArray multirequest;
    multirequest.append(1);
    broadcastSocket.writeDatagram(multirequest, QHostAddress::Broadcast, 45000);

    // change ok button name
    QPushButton* createButton = this->buttonBox->button(QDialogButtonBox::Ok);
    createButton->setText("Create");
    createButton->setEnabled(false);

    // set validator for nickname
    QRegularExpression nicknameRe(NETPLAYCOMMON_NICKNAME_REGEX);
    this->nickNameLineEdit->setValidator(new QRegularExpressionValidator(nicknameRe, this));
    this->nickNameLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));

    // Remove server combo visibility - not needed
    if (this->serverComboBox) {
        this->serverComboBox->hide();
    }
    
    // Remove server label
    QLabel* serverLabel = this->findChild<QLabel*>("label");
    if (serverLabel) {
        serverLabel->hide();
    }
    
    // Remove ping line edit - not needed
    if (this->pingLineEdit) {
        this->pingLineEdit->hide();
    }
    
    // Remove ping label
    QLabel* pingLabel = this->findChild<QLabel*>("pingLabel");
    if (pingLabel) {
        pingLabel->hide();
    }
    
    // Remove session name line edit - will use nickname instead
    if (this->sessionNameLineEdit) {
        this->sessionNameLineEdit->hide();
    }
    
    // Remove session name label
    QLabel* sessionNameLabel = this->findChild<QLabel*>("label_2");
    if (sessionNameLabel) {
        sessionNameLabel->hide();
    }
    
    // Remove password line edit - hardcoded to MPN
    if (this->passwordLineEdit) {
        this->passwordLineEdit->hide();
    }
    
    // Remove password label
    QLabel* passwordLabel = this->findChild<QLabel*>("label_3");
    if (passwordLabel) {
        passwordLabel->hide();
    }

    // Add port customization UI
    QWidget* portWidget = new QWidget(this);
    QVBoxLayout* portLayout = new QVBoxLayout(portWidget);

    this->directConnectionCheckBox = new QCheckBox("Direct connection", this);
    this->directConnectionCheckBox->setToolTip("Skip NAT traversal and allow the hosting port to be changed.");
    this->directConnectionCheckBox->setChecked(false);
    
    QLabel* portLabel = new QLabel("Hosting Port:", this);
    portLabel->setToolTip(QString("Port to listen on for incoming player connections (default: %1)")
                              .arg(Netplay::kDefaultNetplayHostingPort));
    this->hostingPortSpinBox = new QSpinBox(this);
    this->hostingPortSpinBox->setMinimum(1024);
    this->hostingPortSpinBox->setMaximum(65535);
    this->hostingPortSpinBox->setValue(Netplay::kDefaultNetplayHostingPort);
    this->hostingPortSpinBox->setEnabled(false);
    this->hostingPortSpinBox->setToolTip("Valid ports: 1024-65535. Locked unless Direct connection is enabled.");
    
    connect(this->hostingPortSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int port) { this->hostingPort = port; });

    connect(this->directConnectionCheckBox, &QCheckBox::toggled, this,
            [this](bool directConnection) {
        this->directConnection = directConnection;
        if (this->hostingPortSpinBox) {
            this->hostingPortSpinBox->setEnabled(directConnection);
            if (!directConnection) {
                this->hostingPortSpinBox->setValue(Netplay::kDefaultNetplayHostingPort);
            }
        }
        this->hostingPort = directConnection && this->hostingPortSpinBox
            ? this->hostingPortSpinBox->value()
            : Netplay::kDefaultNetplayHostingPort;
    });
    
    portLayout->addWidget(this->directConnectionCheckBox);
    portLayout->addWidget(portLabel);
    portLayout->addWidget(this->hostingPortSpinBox);
    portLayout->setContentsMargins(0, 0, 0, 0);
    
    // Find the parent layout and add the port widget
    // Since we're using setupUi, we'll add it dynamically to the dialog
    QVBoxLayout* mainLayout = qobject_cast<QVBoxLayout*>(this->layout());
    if (mainLayout) {
        // Insert before buttonBox
        mainLayout->insertWidget(mainLayout->count() - 1, portWidget);
    }
    
    // add data to widget
    for (auto it = modelData.begin(); it != modelData.end(); it++)
    {
        this->romListWidget->AddRomData(this->getGameName(QString::fromStdString(it.value().GoodName), it.key()),
                                        QString::fromStdString(it.value().MD5),
                                        it.key());
    }
    this->romListWidget->RefreshDone();

    this->validateCreateButton();
}

CreateNetplaySessionDialog::~CreateNetplaySessionDialog(void)
{
    if (this->natTraversalClient) {
        this->natTraversalClient->stopHosting(false);
    }

    QString nickname = this->nickNameLineEdit->text();
    if (!nickname.isEmpty())
    {
        CoreSettingsSetValue(SettingsID::Netplay_Nickname, nickname.toStdString());
    }

    QString server = this->serverComboBox->currentText();
}

QJsonObject CreateNetplaySessionDialog::GetSessionJson(void)
{
    return this->sessionJson;
}

QString CreateNetplaySessionDialog::GetSessionFile(void)
{
    return this->sessionFile;
}

QString CreateNetplaySessionDialog::getGameName(QString goodName, QString file)
{
    QString gameName = goodName;

    if (gameName.endsWith("(unknown rom)") ||
        gameName.endsWith("(unknown disk)"))
    {
        gameName = QFileInfo(file).fileName();
    }

    return gameName;
}

bool CreateNetplaySessionDialog::validate(void)
{
    if (this->nickNameLineEdit->text().isEmpty() ||
        this->nickNameLineEdit->text().contains(' ') ||
        this->nickNameLineEdit->text().size() > 128)
    {
        return false;
    }

    if (!this->romListWidget->IsCurrentRomValid())
    {
        return false;
    }

    return true;
}

void CreateNetplaySessionDialog::validateCreateButton(void)
{
    QPushButton* createButton = this->buttonBox->button(QDialogButtonBox::Ok);
    createButton->setEnabled(this->validate());
}

void CreateNetplaySessionDialog::createSession(void)
{
    // Start hosting a local signaling server
    QString playerName = this->nickNameLineEdit->text();
    QString gameName = this->getGameName(this->sessionGoodName, this->sessionFile);
    const bool directConnection = this->directConnectionCheckBox != nullptr && this->directConnectionCheckBox->isChecked();
    this->directConnection = directConnection;
    
    if (!this->coordinator->startHosting(this->hostingPort, playerName, gameName))
    {
        QtMessageBox::Error(this, "Hosting Error", QString("Failed to start hosting server on port %1").arg(this->hostingPort));
        this->toggleUI(true, this->validate());
        return;
    }

    // Build session JSON with hosting information
    QJsonObject json;
    json.insert("room_name", playerName);  // Room name is the host's nickname
    json.insert("password", "MPN");  // Hardcoded password
    json.insert("MD5", this->sessionMD5);
    json.insert("game_name", gameName);
    json.insert("player_name", playerName);
    json.insert("server_address", "127.0.0.1");
    json.insert("server_port", this->hostingPort);
    json.insert("public_address", "localhost");
    json.insert("public_port", this->hostingPort);
    json.insert("use_nat_traversal", !directConnection);
    json.insert("is_hosting", true);
    json.insert("slot", 0);
    json.insert("started", false);
    json.insert("player_count", 1);
    json.insert("max_players", 4);
    json.insert("lobby_size", "1/4");
    json.insert("host_name", playerName);
    {
        QJsonArray players;
        QJsonObject hostPlayer;
        hostPlayer.insert("name", playerName);
        hostPlayer.insert("slotIndex", 0);
        hostPlayer.insert("isReady", true);
        hostPlayer.insert("isSpectator", false);
        players.append(hostPlayer);
        json.insert("players", players);
    }
    json.insert("md5_hash", this->sessionMD5);  // For ROM matching
    json.insert("rom_path", this->sessionFile); // For loading cheats
    json.insert("room_id", this->coordinator->getGameSession().roomId);

    this->sessionJson = json;
    
    qDebug() << "Created session via coordinator, hosting on port" << this->hostingPort << "as" << playerName;

    if (directConnection) {
        this->finalizeSession();
        return;
    }

    this->registerNatTraversalHost();
}

void CreateNetplaySessionDialog::registerNatTraversalHost(void)
{
    this->natTraversalClient = std::make_unique<Netplay::NatTraversalClient>(this);

    connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::hostRegistered,
            this, [this](const QString& hostCode, const QString& publicAddress, int signalingPort) {
        qDebug() << "NAT traversal host code:" << hostCode << "endpoint:" << publicAddress << signalingPort;
        this->sessionJson.insert("host_code", hostCode);
        this->sessionJson.insert("use_nat_traversal", true);
        this->sessionJson.insert("server_port", signalingPort > 0 ? signalingPort : this->hostingPort);
        this->sessionJson.insert("public_port", signalingPort > 0 ? signalingPort : this->hostingPort);
        this->sessionJson.insert("connect_port", signalingPort > 0 ? signalingPort : this->hostingPort);
        if (Netplay::isUsableConnectAddress(publicAddress)) {
            this->sessionJson.insert("public_address", publicAddress);
            this->sessionJson.insert("connect_address", publicAddress);
            this->publishSessionIndex(hostCode);
            this->finalizeSession();
            return;
        }

        qDebug() << "NAT server did not provide a public IP; querying STUN";
        connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::publicAddressResolved,
                this, [this, hostCode](const QString& ip, int port) {
            Q_UNUSED(port);
            if (Netplay::isUsableConnectAddress(ip)) {
                this->sessionJson.insert("public_address", ip);
                this->sessionJson.insert("connect_address", ip);
            }
            this->publishSessionIndex(hostCode);
            this->finalizeSession();
        }, Qt::SingleShotConnection);
        connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::publicAddressFailed,
                this, [this, hostCode](const QString& reason) {
            qWarning() << "STUN after register failed:" << reason << "- using HTTP public IP lookup";
            this->pendingNatHostCode = hostCode;
            this->fetchPublicIpAddress();
        }, Qt::SingleShotConnection);
        this->natTraversalClient->queryStunServer(QStringLiteral("stun.l.google.com"), 19302);
    });

    connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::hostRegistrationFailed,
            this, [this](const QString& reason) {
        qWarning() << "NAT traversal registration failed:" << reason;
        QtMessageBox::Error(this, "NAT Traversal Failed",
                            QString("Could not register host code (%1). Attempting STUN public IP discovery.").arg(reason));
        // Try STUN first; if it fails, fall back to HTTP checkip
        connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::publicAddressResolved,
                this, [this](const QString& ip, int port) {
            qDebug() << "STUN resolved public address:" << ip << port;
            this->publicIpAddress = ip;
            this->sessionJson.insert("public_address", ip);
            this->sessionJson.insert("connect_address", ip);
            this->sessionJson.insert("server_port", port);
            this->sessionJson.insert("public_port", port);
            this->sessionJson.insert("connect_port", port);
            this->finalizeSession();
        });
        connect(this->natTraversalClient.get(), &Netplay::NatTraversalClient::publicAddressFailed,
                this, [this](const QString& reason) {
            qWarning() << "STUN public address discovery failed:" << reason << "- falling back to HTTP lookup";
            this->fetchPublicIpAddress();
        });

        // Use a common public STUN server; this is configurable later
        this->natTraversalClient->queryStunServer("stun.l.google.com", 19302);
    });

    this->natTraversalClient->startHosting(static_cast<uint16_t>(this->hostingPort));
}

void CreateNetplaySessionDialog::publishSessionIndex(const QString& hostCode)
{
    this->natIndexClient = std::make_unique<Netplay::NatTraversalIndexClient>(this);
    const QByteArray payload = QJsonDocument(this->sessionJson).toJson(QJsonDocument::Compact);

    connect(this->natIndexClient.get(), &Netplay::NatTraversalIndexClient::published,
            this, [hostCode](const QString& key) {
        qDebug() << "Published session index:" << key << "for host" << hostCode;
    });
    connect(this->natIndexClient.get(), &Netplay::NatTraversalIndexClient::publishFailed,
            this, [](const QString& reason) {
        qWarning() << "Failed to publish session index:" << reason;
    });

    this->natIndexClient->publishSession(hostCode, payload);
}

void CreateNetplaySessionDialog::fetchPublicIpAddress(void)
{
    // Set a 3-second timeout for the public IP fetch
    this->publicIpTimeoutTimerId = this->startTimer(3000);
    
    // Make GET request to AWS checkip service
    QUrl url("http://checkip.amazonaws.com/");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "RMG-Netplay/1.0");
    
    this->publicIpReply = this->networkManager.get(request);
    connect(this->publicIpReply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::errorOccurred),
            this, [this](QNetworkReply::NetworkError error) {
        qDebug() << "Public IP fetch network error:" << error;
    });
    // finished() signal doesn't pass any parameters, so we use a lambda
    connect(this->publicIpReply, &QNetworkReply::finished, this, [this]() {
        this->on_publicIpFetch_Finished(nullptr);
    });
}

void CreateNetplaySessionDialog::on_publicIpFetch_Finished(QNetworkReply* reply)
{
    // Handle timeout timer
    if (this->publicIpTimeoutTimerId != -1)
    {
        this->killTimer(this->publicIpTimeoutTimerId);
        this->publicIpTimeoutTimerId = -1;
    }
    
    // Use the stored member reply since the signal doesn't pass it
    QNetworkReply* networkReply = this->publicIpReply;
    if (!networkReply) {
        qWarning() << "Public IP reply is null";
        this->publicIpAddress = "localhost";
        this->finalizeSession();
        return;
    }
    
    if (networkReply->error() == QNetworkReply::NoError)
    {
        QString responseData = QString::fromUtf8(networkReply->readAll()).trimmed();
        qDebug() << "Public IP response:" << responseData;
        
        // AWS returns just the IP address, e.g., "203.0.113.42\n"
        if (!responseData.isEmpty())
        {
            this->publicIpAddress = responseData;
        }
        else
        {
            qWarning() << "Empty response from public IP service";
            this->publicIpAddress = "localhost";
        }
    }
    else
    {
        qWarning() << "Failed to fetch public IP:" << networkReply->errorString();
        this->publicIpAddress = "localhost";
    }
    
    networkReply->deleteLater();
    this->publicIpReply = nullptr;

    if (!this->pendingNatHostCode.isEmpty()) {
        const QString hostCode = this->pendingNatHostCode;
        this->pendingNatHostCode.clear();
        if (Netplay::isUsableConnectAddress(this->publicIpAddress)) {
            this->sessionJson.insert("public_address", this->publicIpAddress);
            this->sessionJson.insert("connect_address", this->publicIpAddress);
        }
        this->publishSessionIndex(hostCode);
        this->finalizeSession();
        return;
    }

    this->finalizeSession();
}

void CreateNetplaySessionDialog::finalizeSession(void)
{
    if (!this->sessionJson.contains("host_code")) {
        if (!this->directConnection) {
            this->sessionJson.insert("public_address", this->publicIpAddress);
        }
        this->sessionJson.insert("use_nat_traversal", false);
    }

    this->sessionFile = QJsonDocument(this->sessionJson).toJson(QJsonDocument::Compact);

    const QString connectInfo = this->sessionJson.value("host_code").toString(this->publicIpAddress);
    qDebug() << "Finalized session with connect info:" << connectInfo;
    
    // Close dialog to proceed to session screen
    QDialog::accept();
}

void CreateNetplaySessionDialog::toggleUI(bool enable, bool enableCreateButton)
{
    QPushButton* createButton = this->buttonBox->button(QDialogButtonBox::Ok);
    createButton->setEnabled(enableCreateButton);

    this->nickNameLineEdit->setReadOnly(!enable);
    if (this->directConnectionCheckBox) {
        this->directConnectionCheckBox->setEnabled(enable);
    }
    if (this->hostingPortSpinBox) {
        this->hostingPortSpinBox->setEnabled(enable && this->directConnection);
    }
}

void CreateNetplaySessionDialog::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == this->pingTimerId)
    {
        // Keep-alive is handled by coordinator internally
        // No additional ping needed
    }
    else if (event->timerId() == this->publicIpTimeoutTimerId)
    {
        // Public IP fetch timed out, use fallback
        qWarning() << "Public IP fetch timed out, using fallback";
        this->publicIpAddress = "localhost";
        
        this->killTimer(this->publicIpTimeoutTimerId);
        this->publicIpTimeoutTimerId = -1;
        
        // Cancel the ongoing request
        if (this->publicIpReply)
        {
            this->publicIpReply->abort();
            this->publicIpReply->deleteLater();
            this->publicIpReply = nullptr;
        }
        
        // Finalize the session with fallback IP
        this->finalizeSession();
    }
}

// WebSocket handlers are no longer used - we use coordinator for hosting
// Kept for reference but not actively used
void CreateNetplaySessionDialog::on_webSocket_textMessageReceived(QString message)
{
    // Deprecated - using coordinator instead
}

void CreateNetplaySessionDialog::on_webSocket_pong(quint64 elapsedTime, const QByteArray&)
{
    // Deprecated - using coordinator instead
}

void CreateNetplaySessionDialog::on_webSocket_connected()
{
    // Deprecated - using coordinator instead
}

void CreateNetplaySessionDialog::on_broadcastSocket_readyRead()
{
    while (this->broadcastSocket.hasPendingDatagrams())
    {
        QNetworkDatagram datagram = this->broadcastSocket.receiveDatagram();
        QByteArray incomingData = datagram.data();
    }
}

void CreateNetplaySessionDialog::on_nickNameLineEdit_textChanged(void)
{
    this->validateCreateButton();
}

// Removed on_sessionNameLineEdit_textChanged - session name is now auto-generated from nickname
// Removed on_passwordLineEdit_textChanged - password is now hardcoded to MPN

void CreateNetplaySessionDialog::on_romListWidget_OnRomChanged(bool valid)
{
    this->validateCreateButton();
}

void CreateNetplaySessionDialog::accept()
{
    // No need to check dispatcher - we use coordinator for hosting

    NetplayRomData romData;
    if (!this->romListWidget->GetCurrentRom(romData))
    {
        return;
    }

    // store these for use in createSession()
    this->sessionFile     = romData.File;
    this->sessionMD5      = romData.MD5;
    this->sessionGoodName = romData.GoodName;

    // disable create button while we're processing the request
    this->toggleUI(false, false);

    // Create session via coordinator
    this->createSession();
}
