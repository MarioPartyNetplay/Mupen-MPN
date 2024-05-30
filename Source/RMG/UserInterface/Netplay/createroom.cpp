#include "createroom.h"
#include "waitroom.h"
#include "netplay_common.h"
#include "RomBrowser.hpp"
#include <QGridLayout>
#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonDocument>
#include <QInputDialog>
#include <QString>
#include <QLineEdit>

#include <RMG-Core/m64p/Api.hpp>
#include <RMG-Core/Settings/Settings.hpp>
#include <RMG-Core/Cheats.hpp>

CreateRoom::CreateRoom(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("NetPlay Setup");
    setMinimumWidth(1000);
    setMinimumHeight(500);

    QGridLayout *layout = new QGridLayout(this);

    QRegularExpression rx("[a-zA-Z0-9]+");
    QValidator *validator = new QRegularExpressionValidator(rx, this);

    // Player Name
    QLabel *playerNameLabel = new QLabel("Nickname: ", this);
    layout->addWidget(playerNameLabel, 0, 0);
    playerNameEdit = new QLineEdit(this);
    playerNameEdit->setValidator(validator);
    playerNameEdit->setMaxLength(30);    
    std::string netplayName;
    netplayName = CoreSettingsGetStringValue(SettingsID::Core_Netplay_Name);    
    if (!netplayName.empty()) {
        playerNameEdit->setText(QString::fromStdString(netplayName));
    }
    layout->addWidget(playerNameEdit, 0, 1);

    // Server Selection
    QLabel *serverLabel = new QLabel("Server", this);
    layout->addWidget(serverLabel, 0, 2);
    serverChooser = new QComboBox(this);
    serverChooser->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    layout->addWidget(serverChooser, 0, 3);
    connect(serverChooser, SIGNAL(currentIndexChanged(int)), this, SLOT(handleServerChanged(int)));

    // Ping
    pingValue = new QLabel(this);
    pingValue->setText("(Calculating)");
    layout->addWidget(pingValue, 0, 4);

    // ROM Browser
    romBrowser = new UserInterface::Widget::RomBrowser(this); // Use the fully qualified name
    layout->addWidget(romBrowser, 2, 0, 1, 6);
    romBrowser->setEnabled(false);
    connect(romBrowser, &UserInterface::Widget::RomBrowser::romDoubleClicked, this, &CreateRoom::handleCreateButton); // Directly connect to handleCreateButton
    QFrame* lineH1 = new QFrame(this);
    lineH1->setFrameShape(QFrame::HLine);
    lineH1->setFrameShadow(QFrame::Sunken);
    layout->addWidget(lineH1, 3, 0, 1, 6);

    setLayout(layout);

    connect(&manager, SIGNAL(finished(QNetworkReply*)),
            SLOT(downloadFinished(QNetworkReply*)));

    connect(this, SIGNAL (finished(int)), this, SLOT (onFinished(int)));

    QNetworkRequest request(QUrl(QStringLiteral("https://gist.githubusercontent.com/EndangeredNayla/509752dc059d5b868e9403fb30218315/raw/c10dafddb99f89854e00c223f0687485a5e9009c/")));
    manager.get(request);

    broadcastSocket.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    connect(&broadcastSocket, &QUdpSocket::readyRead, this, &CreateRoom::processBroadcast);
    QByteArray multirequest;
    multirequest.append(1);
    broadcastSocket.writeDatagram(multirequest, QHostAddress::Broadcast, 27886);

    launched = 0;
}

void CreateRoom::processBroadcast()
{
    while (broadcastSocket.hasPendingDatagrams())
    {
        QNetworkDatagram datagram = broadcastSocket.receiveDatagram();
        QByteArray incomingData = datagram.data();
        QJsonDocument json_doc = QJsonDocument::fromJson(incomingData);
        QJsonObject json = json_doc.object();
        QStringList servers = json.keys();
        for (int i = 0; i < servers.size(); ++i)
            serverChooser->addItem(servers.at(i), json.value(servers.at(i)).toString());
    }
}

void CreateRoom::onFinished(int)
{
    broadcastSocket.close();
    m64p::Core.DoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    if (!launched && webSocket)
    {
        webSocket->close();
        webSocket->deleteLater();
    }
}

void CreateRoom::handleCreateButton(const QString& filename)
{
    CoreAddCallbackMessage(CoreDebugMessageType::Info, ("handleCreateButton called with filename: " + filename).toStdString().c_str());
    
    romName = filename;

    if (serverChooser->currentData() == "Custom" && customServerHost.isEmpty())
    {

        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Custom Server Address is invalid");
        QMessageBox msgBox;
        msgBox.setText("Custom Server Address is invalid");
        msgBox.exec();
        return;
    }

    if (loadROM(filename) == M64ERR_SUCCESS)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Info, "ROM loaded successfully");
        romBrowser->setEnabled(false);
        m64p::Core.DoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(rom_settings), &rom_settings);

        connectionTimer = new QTimer(this);
        connectionTimer->setSingleShot(true);
        connectionTimer->start(5000);
        connect(connectionTimer, SIGNAL(timeout()), this, SLOT(connectionFailed()));
        connect(webSocket, &QWebSocket::disconnected, connectionTimer, &QTimer::stop);
        connect(webSocket, &QObject::destroyed, connectionTimer, &QTimer::stop);

        if (webSocket->isValid())
        {
            createRoom();
        }
        else
        {
            connect(webSocket, &QWebSocket::connected, this, &CreateRoom::createRoom);
        }
    }
    else
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to load ROM");
        QMessageBox msgBox;
        msgBox.setText("Could not open ROM");
        msgBox.exec();
    }
}

void CreateRoom::downloadFinished(QNetworkReply *reply)
{
    if (!reply->error())
    {
        QJsonDocument json_doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject json = json_doc.object();
        QStringList servers = json.keys();
        for (int i = 0; i < servers.size(); ++i)
            serverChooser->addItem(servers.at(i), json.value(servers.at(i)).toString());
        serverChooser->addItem(QString("Custom"), QString("Custom"));
    }

    reply->deleteLater();
}

void CreateRoom::handleServerChanged(int index)
{
    if (serverChooser->itemData(index) == "Custom") {
        bool ok;
        QString host = QInputDialog::getText(this, "Custom Netplay Server", "IP Address / Host:", QLineEdit::Normal, "", &ok);

        if (ok && !host.isEmpty()) {
            customServerHost = host;
        }
    }

    if (webSocket)
    {
        webSocket->close();
        webSocket->deleteLater();
    }

    pingValue->setText("(Calculating)");

    webSocket = new QWebSocket;
    connect(webSocket, &QWebSocket::pong, this, &CreateRoom::updatePing);
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &CreateRoom::sendPing);
    connect(webSocket, &QWebSocket::disconnected, timer, &QTimer::stop);
    connect(webSocket, &QObject::destroyed, timer, &QTimer::stop);

    connect(webSocket, &QWebSocket::textMessageReceived, this, &CreateRoom::processTextMessage);

    timer->start(2500);
    QString serverAddress = serverChooser->itemData(index) == "Custom" ? customServerHost.prepend("ws://") : serverChooser->itemData(index).toString();
    QUrl serverUrl = QUrl(serverAddress);
    if (serverChooser->itemData(index) == "Custom" && serverUrl.port() < 0)
        // Be forgiving of custom server addresses that forget the port
        serverUrl.setPort(45000);

    webSocket->open(serverUrl);

}

void CreateRoom::updatePing(quint64 elapsedTime, const QByteArray&)
{
    pingValue->setText(QString::number(elapsedTime) + " ms");
    romBrowser->setEnabled(true); // Enable the ROM button after ping is updated

}

void CreateRoom::sendPing()
{
    webSocket->ping();
}

void CreateRoom::connectionFailed()
{
    QMessageBox msgBox;
    msgBox.setText("Connection to server failed.");
    msgBox.exec();
    romBrowser->setEnabled(true);
}

void CreateRoom::createRoom() {
    connectionTimer->stop();
    QJsonObject json;
    json.insert("type", "request_create_room");
    json.insert("room_name", playerNameEdit->text());
    json.insert("player_name", playerNameEdit->text());
    json.insert("password", "MPN");
    json.insert("MD5", QString(rom_settings.MD5));
    json.insert("game_name", QString(rom_settings.goodname));
    json.insert("client_sha", "demo");
    json.insert("netplay_version", NETPLAY_VER);
    json.insert("emulator", "MPN");

    // Step 1: Retrieve cheats
    std::vector<CoreCheat> coreCheats;
    if (CoreGetCurrentCheats(coreCheats)) {
        // Step 2: Format cheats into JSON
        QJsonArray cheatsString;
        for (const auto& cheat : coreCheats) {
            if (CoreIsCheatEnabled(cheat)) { // Check if the cheat is enabled
                for (const auto& code : cheat.CheatCodes) {
                    QString codeStr;
                    if (code.UseOptions) {
                        CoreCheatOption currentOption;
                        if (CoreGetCheatOption(cheat, currentOption)) {
                            QString codeValueString = QString("%1").arg(code.Value, 4, 16, QChar('0')).toUpper();
                            QString optionValueString = QString("%1").arg(currentOption.Value, code.OptionSize * 2, 16, QChar('0')).toUpper();

                            // Ensure the size matches
                            if (optionValueString.size() == code.OptionSize * 2) {
                                codeValueString.replace(code.OptionIndex * 2, code.OptionSize * 2, optionValueString);
                            } else {
                                continue;
                            }

                            codeStr = QString("%1 %2").arg(code.Address, 8, 16, QChar('0')).arg(codeValueString).toUpper();
                        } else {
                            continue;
                        }
                    } else {
                        codeStr = QString("%1 %2").arg(code.Address, 8, 16, QChar('0')).arg(code.Value, 4, 16, QChar('0')).toUpper();
                    }
                    cheatsString.append(codeStr);
                }
            }
        }

        if (!cheatsString.isEmpty()) {
            QString customFront = "{\"custom\":"; // Custom front
            QString customBack = "}"; // Custom back
            QJsonObject featuresObject;
            featuresObject.insert("cheats", customFront + QString::fromUtf8(QJsonDocument(cheatsString).toJson(QJsonDocument::Compact)) + customBack);
            json.insert("features", featuresObject);
        } else {
            CoreAddCallbackMessage(CoreDebugMessageType::Info, "No cheats available.");
        }
    } else {
        CoreAddCallbackMessage(CoreDebugMessageType::Info, "Failed to retrieve cheats.");
    }

    // Step 3: Send JSON message through WebSocket
    QJsonDocument jsonDoc(json);
    webSocket->sendTextMessage(jsonDoc.toJson(QJsonDocument::Compact));
}

void CreateRoom::processTextMessage(QString message)
{
    QMessageBox msgBox;
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setTextInteractionFlags(Qt::TextBrowserInteraction);
    QJsonDocument json_doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject json = json_doc.object();
    if (json.value("type").toString() == "reply_create_room")
    {
        if (json.value("accept").toInt() == 0)
        {
            json.remove("type");
            launched = 1;
            WaitRoom *waitRoom = new WaitRoom(romName, json, webSocket, parentWidget());
            waitRoom->show();
            accept();
            emit roomClosed(); // Emit the signal to close NetplayUI
        }
        else
        {
            m64p::Core.DoCommand(M64CMD_ROM_CLOSE, 0, NULL);
            msgBox.setText(json.value("message").toString());
            msgBox.exec();
            createButton->setEnabled(true);
        }
    }
}
