#include "NetplaySessionDialog.hpp"
#include "NetplayCommon.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QJsonDocument>
#include <QPushButton>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <RMG-Core/Core.hpp>
#include <RMG-Core/m64p/Api.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

NetplaySessionDialog::NetplaySessionDialog(QWidget *parent, QWebSocket* webSocket, QJsonObject json, QString sessionFile, QJsonArray cheats) : QDialog(parent)
{
    this->setupUi(this);

    this->webSocket = webSocket;
    this->cheats = cheats;
    
    QJsonObject session = json.value("room").toObject();

    this->nickName    = json.value("player_name").toString();
    this->sessionPort = session.value("port").toInt();
    this->sessionName = session.value("room_name").toString();
    this->sessionFile = sessionFile;

    // Check if the current user is the host
    bool isHost = (this->nickName == session.value("features").toObject().value("host_name").toString());

    // Hide bufferSpinBox and its label if not host
    if (!isHost) {
        this->bufferSpinBox->setVisible(false);
        this->bufferLabel->setVisible(false);
    }

    // Store cheats
    QJsonObject featuresObject = session.value("features").toObject();
    QString cheatsString = featuresObject.value("cheats").toString();
    QJsonDocument cheatsDoc = QJsonDocument::fromJson(cheatsString.toUtf8());
    if (cheatsDoc.isArray()) {
        this->cheats = cheatsDoc.array();
    }

    this->sessionNameLineEdit->setText(this->sessionName);
    this->gameNameLineEdit->setText(session.value("game_name").toString());

    connect(this->webSocket, &QWebSocket::textMessageReceived, this, &NetplaySessionDialog::on_webSocket_textMessageReceived);

    // reset json objects
    session = {};
    json    = {};

    // request server motd
    json.insert("type", "request_motd");
    json.insert("room_name", this->sessionName);
    NetplayCommon::AddCommonJson(json);
    webSocket->sendTextMessage(QJsonDocument(json).toJson());

    // request players
    json = {};
    session.insert("port", this->sessionPort);
    json.insert("type", "request_players");
    json.insert("room", session);
    NetplayCommon::AddCommonJson(json);
    webSocket->sendTextMessage(QJsonDocument(json).toJson());

    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
    startButton->setText("Start");
    startButton->setEnabled(false);

    emit this->bufferSpinBox->valueChanged(5);
    connect(this->bufferSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &NetplaySessionDialog::onBufferSizeChanged);

}

NetplaySessionDialog::~NetplaySessionDialog(void)
{
}

void NetplaySessionDialog::onBufferSizeChanged(int value)
{
    QString message = QString("<b>Buffer</b>: Changed the buffer to %1").arg(value);
    this->chatPlainTextEdit->appendHtml(message);
        
    // Send the updated buffer size to the server
    QJsonObject json;
    json.insert("type", "update_buffer_size");
    json.insert("buffer_size", value);
    this->webSocket->sendTextMessage(QJsonDocument(json).toJson());
}

void NetplaySessionDialog::on_webSocket_textMessageReceived(QString message)
{
    QJsonDocument jsonDocument = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject json = jsonDocument.object();
    QString     type = json.value("type").toString();

    if (type == "reply_players")
    {
        if (json.contains("player_names"))
        {
            this->listWidget->clear();
            QString name;
            QJsonArray names = json.value("player_names").toArray();
            for (int i = 0; i < 4; i++)
            {
                name = names.at(i).toString();
                if (!name.isEmpty())
                {
                    this->listWidget->addItem(name);
                    if (this->nickName == name)
                    {
                        this->sessionNumber = i + 1;
                    }
                }
                if (i == 0)
                {
                    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
                    startButton->setEnabled(this->nickName == name);
                }
            }
        }
    }
    else if (type == "reply_chat_message")
    {
        this->chatPlainTextEdit->appendHtml(json.value("message").toString());
    }
    else if (type == "reply_begin_game")
    {
        emit OnPlayGame(this->sessionFile, this->webSocket->peerAddress().toString(), this->sessionPort, this->sessionNumber, this->cheats);
        QDialog::accept();
    }
    else if (type == "reply_motd")
    {
        QString message = "<b>Notice 1: </b>Servers are funded by Tabitha! Use this <a href='https://ko-fi.com/tabithahanegan'>link</a> to help fund the process.</div>";
        QString message2 = "<b>Notice 2: </b>Please set up your cheats before participating in a NetPlay session. Cheats do not sync if configured in-game or while in the lobby.</div>";
        QString message3 = "<b>Notice 3: </b>Buffer can only be set before your game starts, sorry for the inconvience.</div>";
        this->chatPlainTextEdit->appendHtml(message);
        this->chatPlainTextEdit->appendHtml(message2);
        this->chatPlainTextEdit->appendHtml(message3);
        this->chatPlainTextEdit->setTextInteractionFlags(Qt::TextBrowserInteraction);
    }
}

void NetplaySessionDialog::on_chatLineEdit_textChanged(QString text)
{
    this->sendPushButton->setEnabled(!text.isEmpty() && text.size() <= 256);
    this->sendPushButton->setDefault(this->sendPushButton->isEnabled());
}

void NetplaySessionDialog::on_sendPushButton_clicked(void)
{
    QJsonObject json;
    QJsonObject session;
    session.insert("port", this->sessionPort);
    json.insert("type", "request_chat_message");
    json.insert("player_name", this->nickName);
    json.insert("message", this->chatLineEdit->text());
    json.insert("room", session);
    NetplayCommon::AddCommonJson(json);

    webSocket->sendTextMessage(QJsonDocument(json).toJson());
    this->chatLineEdit->clear();
}

void NetplaySessionDialog::accept()
{
    QPushButton* startButton = this->buttonBox->button(QDialogButtonBox::Ok);
    startButton->setEnabled(false);

    QJsonObject json;
    QJsonObject session;
    session.insert("port", this->sessionPort);
    json.insert("type", "request_begin_game");
    json.insert("room", session);
    NetplayCommon::AddCommonJson(json);

    webSocket->sendTextMessage(QJsonDocument(json).toJson());
}