#include "Lobby.hpp"
#include <QGridLayout>
#include <QMessageBox>
#include <QJsonArray>
#include <RMG-Core/m64p/Api.hpp>
#include <RMG-Core/Settings/Settings.hpp>

Lobby::Lobby(QString filename, QJsonObject room, QWebSocket *socket, QWidget *parent)
    : QDialog(parent)
{
    QJsonObject featuresObject = room.value("features").toObject();
    QString cheatsString = featuresObject.value("cheats").toString();
    QJsonDocument cheatsDoc = QJsonDocument::fromJson(cheatsString.toUtf8());
    QJsonObject cheatsObject = cheatsDoc.object();

    // Initialize cheats member
    cheats = cheatsObject;

    setWindowTitle("RMG NetPlay");
    w = dynamic_cast<UserInterface::MainWindow*>(parent);

    this->resize(640, 480);

    player_name = room.value("player_name").toString();
    room_port = room.value("port").toInt();
    room_name = room.value("room_name").toString();
    file_name = filename;
    started = 0;

    webSocket = socket;
    connect(webSocket, &QWebSocket::textMessageReceived, this, [this](QString message){ processTextMessage(message, cheats); });
    connect(webSocket, &QWebSocket::pong, this, &Lobby::updatePing);

    QGridLayout *layout = new QGridLayout(this);

    QLabel *gameLabel = new QLabel("Game Name:", this);
    layout->addWidget(gameLabel, 0, 0);
    QLabel *gameName = new QLabel(room.value("game_name").toString(), this);
    layout->addWidget(gameName, 0, 1);

    QLabel *pingLabel = new QLabel("Your Ping:", this);
    layout->addWidget(pingLabel, 1, 0);
    pingValue = new QLabel(this);
    layout->addWidget(pingValue, 1, 1);

    QLabel *p1Label = new QLabel("Player 1:", this);
    layout->addWidget(p1Label, 3, 0);

    QLabel *p2Label = new QLabel("Player 2:", this);
    layout->addWidget(p2Label, 4, 0);

    QLabel *p3Label = new QLabel("Player 3:", this);
    layout->addWidget(p3Label, 5, 0);

    QLabel *p4Label = new QLabel("Player 4:", this);
    layout->addWidget(p4Label, 6, 0);

    for (int i = 0; i < 4; ++i)
    {
        pName[i] = new QLabel(this);
        layout->addWidget(pName[i], i + 3, 1);
    }

    chatWindow = new QPlainTextEdit(this);
    chatWindow->setReadOnly(true);
    layout->addWidget(chatWindow, 7, 0, 3, 2);

    chatEdit = new QLineEdit(this);
    chatEdit->setPlaceholderText("Enter chat message here");
    connect(chatEdit, &QLineEdit::returnPressed, this, &Lobby::sendChat);
    layout->addWidget(chatEdit, 10, 0, 1, 2);

    startGameButton = new QPushButton(this);
    startGameButton->setText("Start Game");
    startGameButton->setAutoDefault(false);
    connect(startGameButton, &QPushButton::released, this, &Lobby::startGame);
    layout->addWidget(startGameButton, 11, 0, 1, 2);

    // Add the promotional label
    QLabel *promoLabel = new QLabel(this);
    promoLabel->setText("<p style='text-align:center;'>Servers are donated by BisectHosting! Use this <a href='https://bisecthosting.com/endangerednayla'>link</a> for 25% off your own server.</p>");
    promoLabel->setTextFormat(Qt::RichText);
    promoLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    promoLabel->setOpenExternalLinks(true);
    layout->addWidget(promoLabel, 12, 0, 1, 2);

    setLayout(layout);

    connect(this, &QDialog::finished, this, &Lobby::onFinished);

    QJsonObject json;
    json.insert("type", "request_players");
    json.insert("port", room_port);
    QJsonDocument json_doc = QJsonDocument(json);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Lobby::sendPing);
    timer->start(5000);

    webSocket->sendTextMessage(json_doc.toJson());
}

void Lobby::sendPing()
{
    webSocket->ping();
}

void Lobby::updatePing(quint64 elapsedTime, const QByteArray&)
{
    pingValue->setText(QString::number(elapsedTime) + " ms");
}

void Lobby::startGame()
{
    if (player_name == pName[0]->text())
    {
        startGameButton->setEnabled(false);

        QJsonObject json;
        json.insert("type", "request_begin_game");
        json.insert("port", room_port);
        QJsonDocument json_doc = QJsonDocument(json);
        webSocket->sendTextMessage(json_doc.toJson());
    }
    else
    {
        QMessageBox::information(this, "Information", "Only Player 1 can start the game");
    }
}

void Lobby::sendChat()
{
    if (!chatEdit->text().isEmpty())
    {
        QJsonObject json;
        json.insert("type", "request_chat_message");
        json.insert("port", room_port);
        json.insert("player_name", player_name);
        json.insert("message", chatEdit->text());
        QJsonDocument json_doc = QJsonDocument(json);
        webSocket->sendTextMessage(json_doc.toJson());
        chatEdit->clear();
    }
}

void Lobby::onFinished(int)
{
    timer->stop();
    webSocket->close();
    webSocket->deleteLater();
}

void Lobby::processTextMessage(QString message, QJsonObject cheats)
{
    QJsonDocument json_doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject json = json_doc.object();
    if (json.value("type").toString() == "reply_players")
    {
        if (json.contains("player_names"))
        {
            for (int i = 0; i < 4; ++i)
            {
                pName[i]->setText(json.value("player_names").toArray().at(i).toString());
                if (pName[i]->text() == player_name)
                    player_number = i + 1;
            }
        }
    }
    else if (json.value("type").toString() == "reply_chat_message")
    {
        chatWindow->appendPlainText(json.value("message").toString());
    }
    else if (json.value("type").toString() == "reply_begin_game")
    {
        started = 1;
        w->OpenROMNetplay(file_name, webSocket->peerAddress().toString(), room_port, player_number, cheats);
        accept();
    }
}