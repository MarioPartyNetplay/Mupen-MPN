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

#include <QRegularExpressionValidator>
#include <QRegularExpression>
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
    
    QLabel* portLabel = new QLabel("Hosting Port:", this);
    portLabel->setToolTip("Port to listen on for incoming player connections (default: 27886)");
    QSpinBox* portSpinBox = new QSpinBox(this);
    portSpinBox->setMinimum(1024);
    portSpinBox->setMaximum(65535);
    portSpinBox->setValue(27886);
    portSpinBox->setToolTip("Valid ports: 1024-65535. Use 27886 for default.");
    
    connect(portSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), 
            this, [this](int port) { this->hostingPort = port; });
    
    portLayout->addWidget(portLabel);
    portLayout->addWidget(portSpinBox);
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
    json.insert("public_address", "localhost");  // Will be updated by fetchPublicIpAddress()
    json.insert("public_port", this->hostingPort);
    json.insert("is_hosting", true);
    json.insert("slot", 0);
    json.insert("md5_hash", this->sessionMD5);  // For ROM matching
    json.insert("rom_path", this->sessionFile); // For loading cheats
    
    this->sessionJson = json;
    
    qDebug() << "Created session via coordinator, hosting on port" << this->hostingPort << "as" << playerName;
    
    // Fetch public IP address from AWS
    this->fetchPublicIpAddress();
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
    
    // Finalize the session with the fetched public IP
    this->finalizeSession();
}

void CreateNetplaySessionDialog::finalizeSession(void)
{
    // Update session JSON with the public IP address
    this->sessionJson.insert("public_address", this->publicIpAddress);
    this->sessionFile = QJsonDocument(this->sessionJson).toJson(QJsonDocument::Compact);
    
    qDebug() << "Finalized session with public IP:" << this->publicIpAddress;
    
    // Close dialog to proceed to session screen
    QDialog::accept();
}

void CreateNetplaySessionDialog::toggleUI(bool enable, bool enableCreateButton)
{
    QPushButton* createButton = this->buttonBox->button(QDialogButtonBox::Ok);
    createButton->setEnabled(enableCreateButton);

    this->nickNameLineEdit->setReadOnly(!enable);
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

    // Create session via coordinator (will host locally on port 27886)
    this->createSession();
}
