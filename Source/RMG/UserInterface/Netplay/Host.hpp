#ifndef CREATEROOM_H
#define CREATEROOM_H

#include <QDialog>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QComboBox>
#include <QtNetwork>
#include <QLineEdit>
#include <QLabel>
#include <RMG-Core/m64p/Api.hpp>
#include <QPushButton>
#include <QUdpSocket>
#include <QTimer>
#include "RomBrowser.hpp"

class Host : public QDialog
{
    Q_OBJECT
public:
    Host(QWidget *parent = nullptr);

signals:
    void roomClosed();

private slots:
    void createRoom();
    void downloadFinished(QNetworkReply *reply);
    void processTextMessage(QString message);
    void onFinished(int result);
    void processBroadcast();
    void handleServerChanged(int index);
    void connectionFailed();
    void sendPing();
    void updatePing(quint64 elapsedTime, const QByteArray&);
    void handleCreateButton(const QString& filename);

private:
    QPushButton *createButton;
    QWebSocket *webSocket = nullptr;
    QNetworkAccessManager manager;
    QComboBox *serverChooser;
    m64p_rom_settings rom_settings;
    QLineEdit *playerNameEdit;
    QLabel *pingLabel;
    QLabel *pingValue;
    int launched;
    QString filename;
    QString romName;
    QUdpSocket broadcastSocket;
    QString customServerHost;
    QTimer *connectionTimer;
    UserInterface::Widget::RomBrowser *romBrowser; // Declare romBrowser
    QString generateRandomHexChar();
};

#endif // CREATEROOM_H