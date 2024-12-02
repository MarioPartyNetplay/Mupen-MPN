#include "Host.hpp"
#include "Lobby.hpp"
#include "NetFoundation.hpp"
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
#include <QProcess>

#include <RMG-Core/m64p/Api.hpp>
#include <RMG-Core/Settings/Settings.hpp>
#include <RMG-Core/Cheats.hpp>

Host::Host(QWidget *parent)
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
    else {
        playerNameEdit->setText("MPN Player");
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
    connect(romBrowser, &UserInterface::Widget::RomBrowser::romDoubleClicked, this, &Host::handleCreateButton); // Directly connect to handleCreateButton
    QFrame* lineH1 = new QFrame(this);
    lineH1->setFrameShape(QFrame::HLine);
    lineH1->setFrameShadow(QFrame::Sunken);
    layout->addWidget(lineH1, 3, 0, 1, 6);

    // Add the promotional label
    QLabel *promoLabel = new QLabel(this);
    promoLabel->setText("<p style='text-align:center;'>Servers are funded by Nayla! Use this <a href='https://ko-fi.com/naylahanegan'>link</a> to help fund the process.</p>");
    promoLabel->setTextFormat(Qt::RichText);
    promoLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    promoLabel->setOpenExternalLinks(true);
    layout->addWidget(promoLabel, 4, 0, 1, 3);

    // Add the "Host local server" button
    QPushButton *hostServerButton = new QPushButton("Host Local Server", this);
    layout->addWidget(hostServerButton, 4, 3, 1, 1);
    connect(hostServerButton, &QPushButton::clicked, this, &Host::hostLocalServer);


    setLayout(layout);

    connect(&manager, SIGNAL(finished(QNetworkReply*)),
            SLOT(downloadFinished(QNetworkReply*)));

    connect(this, SIGNAL (finished(int)), this, SLOT (onFinished(int)));

    QNetworkRequest request(QUrl(QStringLiteral("https://gist.githubusercontent.com/RainbowTabitha/509752dc059d5b868e9403fb30218315/raw/c10dafddb99f89854e00c223f0687485a5e9009c/")));
    manager.get(request);

    broadcastSocket.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    connect(&broadcastSocket, &QUdpSocket::readyRead, this, &Host::processBroadcast);
    QByteArray multirequest;
    multirequest.append(1);
    broadcastSocket.writeDatagram(multirequest, QHostAddress::Broadcast, 45000);

    launched = 0;
}

void Host::hostLocalServer()
{
    bool ok;
    int port = QInputDialog::getInt(this, "Host Local Server", "Enter port number:", 45000, 1024, 65535, 1, &ok);
    if (ok)
    {
        selectedPort = port;

        QString program = "Netplay-Server.exe";
        QStringList arguments;
        arguments << "-baseport" << QString::number(port);

        serverProcess = new QProcess(this);
        serverProcess->start(program, arguments);

        if (!serverProcess->waitForStarted())
        {
            QMessageBox::critical(this, "Error", "Failed to start Netplay-Server.exe");
            delete serverProcess;
            serverProcess = nullptr;
        }
        else
        {
            refreshServerList();
            QMessageBox::information(this, "Server Started", "Local server started on port " + QString::number(port));
        }
    }
}

void Host::refreshServerList()
{
    QString localhostEntry = QString("ws://127.0.0.1:%1").arg(selectedPort);;
    QString localhostEntryName = "Localhost";
    serverChooser->insertItem(0, localhostEntryName, localhostEntry);
    serverChooser->setCurrentIndex(0);
}

void Host::processBroadcast()
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

void Host::onFinished(int)
{
    broadcastSocket.close();
    m64p::Core.DoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    if (!launched && webSocket)
    {
        webSocket->close();
        webSocket->deleteLater();
    }
}

void Host::handleCreateButton(const QString& filename)
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
            connect(webSocket, &QWebSocket::connected, this, &Host::createRoom);
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

void Host::downloadFinished(QNetworkReply *reply)
{
    if (!reply->error())
    {
        QJsonDocument json_doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject json = json_doc.object();
        QStringList servers = json.keys();
        for (int i = 0; i < servers.size(); ++i) {
            QString serverName = servers.at(i);
            // Trim the first two characters only if the server is not "Localhost" or "Custom"
            if (serverName != "Localhost" && serverName != "Custom") {
                serverName = serverName.mid(2); // Trim the first two characters
            }
            serverChooser->addItem(serverName, json.value(servers.at(i)).toString());
        }
        serverChooser->addItem(QString("Custom"), QString("Custom"));
    }

    reply->deleteLater();
}

void Host::handleServerChanged(int index)
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
    connect(webSocket, &QWebSocket::pong, this, &Host::updatePing);
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Host::sendPing);
    connect(webSocket, &QWebSocket::disconnected, timer, &QTimer::stop);
    connect(webSocket, &QObject::destroyed, timer, &QTimer::stop);

    connect(webSocket, &QWebSocket::textMessageReceived, this, &Host::processTextMessage);

    timer->start(2500);
    QString serverAddress = serverChooser->itemData(index) == "Custom" ? customServerHost.prepend("ws://") : serverChooser->itemData(index).toString();
    QUrl serverUrl = QUrl(serverAddress);
    if (serverChooser->itemData(index) == "Custom" && serverUrl.port() < 0)     
        // Be forgiving of custom server addresses that forget the port
        serverUrl.setPort(45000);
    webSocket->open(serverUrl);

}

void Host::updatePing(quint64 elapsedTime, const QByteArray&)
{
    pingValue->setText(QString::number(elapsedTime) + " ms");
    romBrowser->setEnabled(true); // Enable the ROM button after ping is updated

}

void Host::sendPing()
{
    webSocket->ping();
}

void Host::connectionFailed()
{
    QMessageBox msgBox;
    msgBox.setText("Connection to server failed.");
    msgBox.exec();
    romBrowser->setEnabled(true);
}

QString Host::generateRandomHexChar() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    for (int i = 0; i < 8; ++i) {
        int randomNum = dis(gen);
        if (randomNum < 10) {
            ss << randomNum;
        } else {
            ss << static_cast<char>('a' + (randomNum - 10));
        }
    }

    QString randomRoomName = QString::fromStdString(ss.str());
    return randomRoomName;
}

void Host::createRoom() {
    connectionTimer->stop();
    QJsonObject json;
    QString playerNameID = playerNameEdit->text();
    CoreSettingsSetValue(SettingsID::Core_Netplay_Name, playerNameID.toStdString());
    json.insert("type", "request_create_room");
    json.insert("room_name", generateRandomHexChar());
    json.insert("player_name", playerNameEdit->text());
    json.insert("password", "MPN");
    json.insert("MD5", QString(rom_settings.MD5));
    json.insert("game_name", QFileInfo(romName).baseName());
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

void Host::processTextMessage(QString message)
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
            Lobby *waitRoom = new Lobby(romName, json, webSocket, parentWidget());
            waitRoom->show();
            accept();
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
