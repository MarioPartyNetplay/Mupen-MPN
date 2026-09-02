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
#include "Netplay/NetplayProtocol.hpp"
#include "Netplay/WebRTC/TurnCredentialClient.hpp"
#include "Netplay/NetplayUpnp.hpp"

#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <QCheckBox>
#include <QNetworkDatagram>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QSharedPointer>
#include <QTimer>
#include <QPushButton>
#include <QJsonObject>
#include <QJsonArray>
#include <QSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
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


CreateNetplaySessionDialog::CreateNetplaySessionDialog(QWidget *parent, UserInterface::Netplay::NetplayCoordinator* coordinator, const QMap<QString, CoreRomSettings>& modelData) : QWidget(parent)
{
    this->setupUi(this);

    // Store coordinator reference
    this->coordinator = coordinator;
    
    // Connect to coordinator signals
    connect(this->coordinator, &Netplay::NetplayCoordinator::roomCreated, this,
            [](const QString& roomId, int) {
        qDebug() << "Session created with room ID:" << roomId;
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

    // Add hosting / connection UI at the top (above nickname)
    QWidget* portWidget = new QWidget(this);
    QVBoxLayout* portLayout = new QVBoxLayout(portWidget);

    this->showInBrowserCheckBox = new QCheckBox("Show in Browser", this);
    this->showInBrowserCheckBox->setToolTip("List this room in the public session browser.");
    this->showInBrowserCheckBox->setChecked(true);

    this->useUpnpCheckBox = new QCheckBox("Use UPnP port mapping", this);
    this->useUpnpCheckBox->setToolTip(
        QStringLiteral("Attempt to open the hosting port on your router via UPnP."));
    this->useUpnpCheckBox->setChecked(false);

    QLabel* portLabel = new QLabel("Hosting Port:", this);
    portLabel->setObjectName(QStringLiteral("portLabel"));
    portLabel->setToolTip(QString("Port to listen on for incoming player connections (default: %1)")
                              .arg(Netplay::kDefaultNetplayHostingPort));
    this->hostingPortSpinBox = new QSpinBox(this);
    this->hostingPortSpinBox->setMinimum(1024);
    this->hostingPortSpinBox->setMaximum(65535);
    this->hostingPortSpinBox->setValue(Netplay::kDefaultNetplayHostingPort);
    this->hostingPortSpinBox->setToolTip("Valid ports: 1024-65535. Forward this port on your router for WAN play.");
    
    connect(this->hostingPortSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int port) { this->hostingPort = port; });
    
    portLayout->addWidget(this->showInBrowserCheckBox);
    portLayout->addWidget(this->useUpnpCheckBox);
    portLayout->addWidget(portLabel);
    portLayout->addWidget(this->hostingPortSpinBox);
    portLayout->setContentsMargins(0, 0, 0, 0);
    
    QVBoxLayout* mainLayout = qobject_cast<QVBoxLayout*>(this->layout());
    if (mainLayout) {
        mainLayout->insertWidget(0, portWidget);
    }
    
    // add data to widget
    for (auto it = modelData.begin(); it != modelData.end(); it++)
    {
        this->romListWidget->AddRomData(this->getGameName(QString::fromStdString(it.value().GoodName), it.key()),
                                        QString::fromStdString(it.value().MD5),
                                        it.key());
    }
    this->romListWidget->RefreshDone();

    this->updateConnectionModeUi();
    this->validateCreateButton();
}

void CreateNetplaySessionDialog::setEmbeddedMode(bool embedded)
{
    this->embeddedMode = embedded;

    if (this->label_4) {
        this->label_4->setVisible(!embedded);
    }
    if (this->nickNameLineEdit) {
        this->nickNameLineEdit->setVisible(!embedded);
    }
}

void CreateNetplaySessionDialog::setNickname(const QString& nickname)
{
    if (this->nickNameLineEdit == nullptr || this->coordinator == nullptr) {
        return;
    }

    if (this->nickNameLineEdit->text() == nickname) {
        return;
    }

    this->nickNameLineEdit->setText(nickname);
    this->coordinator->setPlayerName(nickname);
}

bool CreateNetplaySessionDialog::canSubmit(void) const
{
    return this->validate();
}

CreateNetplaySessionDialog::~CreateNetplaySessionDialog(void)
{
    Netplay::netplayUpnpUnmapPort();

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

bool CreateNetplaySessionDialog::validate(void) const
{
    if (!this->embeddedMode) {
        if (!NetplayCommon::IsValidNickname(this->nickNameLineEdit->text()))
        {
            return false;
        }
    }

    if (!this->romListWidget->IsCurrentRomValid())
    {
        return false;
    }

    return true;
}

void CreateNetplaySessionDialog::validateCreateButton(void)
{
    emit this->canSubmitChanged(this->validate());
}

void CreateNetplaySessionDialog::updateConnectionModeUi(void)
{
    Q_UNUSED(this);
}

void CreateNetplaySessionDialog::on_connectionModeComboBox_currentIndexChanged(int index)
{
    Q_UNUSED(index);
}

void CreateNetplaySessionDialog::createSession(void)
{
    const bool useUpnp = this->useUpnpCheckBox != nullptr && this->useUpnpCheckBox->isChecked();

    Netplay::applyNetplayConnectionSettings(Netplay::NetplayConnectionMode::Direct, useUpnp);

    // Start hosting a local signaling server
    QString playerName = this->nickNameLineEdit->text();
    QString gameName = this->getGameName(this->sessionGoodName, this->sessionFile);
    this->hostingPort = this->hostingPortSpinBox != nullptr
        ? this->hostingPortSpinBox->value()
        : Netplay::kDefaultNetplayHostingPort;
    const bool showInBrowser =
        this->showInBrowserCheckBox != nullptr && this->showInBrowserCheckBox->isChecked();

    if (useUpnp) {
        Netplay::netplayUpnpMapPort(static_cast<quint16>(this->hostingPort));
    }
    
    if (!this->coordinator->startHosting(this->hostingPort, playerName, gameName))
    {
        QString message = this->coordinator->lastHostingError();
        if (message.isEmpty())
        {
            message = QString("Failed to start hosting server on port %1.").arg(this->hostingPort);
        }
        QtMessageBox::Error(this, "Hosting Error", message);
        this->toggleUI(true, this->validate());
        return;
    }

    // Publish the host ROM hash immediately so early joiners get a rooms-list MD5
    // before the session dialog opens and re-reports it.
    if (!this->sessionMD5.isEmpty()) {
        this->coordinator->reportLocalRomMd5(this->sessionMD5);
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
    json.insert("public_port", this->hostingPort);
    json.insert("connect_port", this->hostingPort);
    json.insert("use_nat_traversal", false);
    json.insert("connection_mode", Netplay::netplayConnectionModeToString(Netplay::NetplayConnectionMode::Direct));
    json.insert("use_upnp", useUpnp);
    json.insert("use_connection_reversal", false);
    json.insert("show_in_browser", showInBrowser);
    json.insert("is_hosting", true);
    json.insert("slot", 0);
    const QString localAddress = Netplay::localNetworkAddress();
    const QString address = localAddress.isEmpty() ? QStringLiteral("127.0.0.1") : localAddress;
    json.insert("public_address", address);
    json.insert("connect_address", address);
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
    json.insert("local_md5", this->sessionMD5);
    json.insert("rom_path", this->sessionFile); // For loading cheats
    json.insert("room_id", this->coordinator->getGameSession().roomId);

    this->sessionJson = json;
    
    qDebug() << "Created session via coordinator, hosting on port" << this->hostingPort << "as" << playerName;

    this->finalizeSession();
}

void CreateNetplaySessionDialog::finalizeSession(void)
{
    this->sessionFile = QJsonDocument(this->sessionJson).toJson(QJsonDocument::Compact);

    const QString connectInfo = QStringLiteral("%1:%2")
        .arg(this->sessionJson.value("public_address").toString(),
             QString::number(this->sessionJson.value("server_port").toInt()));
    qDebug() << "Finalized session with connect info:" << connectInfo;

    emit this->sessionAccepted();
}

void CreateNetplaySessionDialog::toggleUI(bool enable, bool enableCreateButton)
{
    emit this->canSubmitChanged(enableCreateButton);

    if (!this->embeddedMode) {
        this->nickNameLineEdit->setReadOnly(!enable);
    }
    if (this->showInBrowserCheckBox) {
        this->showInBrowserCheckBox->setEnabled(enable);
    }
    if (this->hostingPortSpinBox) {
        this->hostingPortSpinBox->setEnabled(enable);
    }
}

void CreateNetplaySessionDialog::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == this->pingTimerId)
    {
        // Keep-alive is handled by coordinator internally
    }
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

void CreateNetplaySessionDialog::submit()
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
