#ifndef JOINROOM_H
#define JOINROOM_H

#include <QDialog>
#include <QComboBox>
#include <QtNetwork>
#include <QTableWidget>
#include <QWebSocket>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <RMG/UserInterface/Widget/RomBrowser/RomBrowserWidget.hpp>

class Join : public QDialog
{
    Q_OBJECT
public:
    Join(QWidget *parent = nullptr, UserInterface::Widget::RomBrowserWidget *romBrowser = nullptr);
signals:
    void roomClosed();
private slots:
    void downloadFinished(QNetworkReply *reply);
    void serverChanged(int index);
    void onConnected();
    void processTextMessage(QString message);
    void refresh();
    void joinGame();
    void onFinished(int result);
    void processBroadcast();
    void sendPing();
    void updatePing(quint64 elapsedTime, const QByteArray&);
private:
    void resetList();
    QString romGoodName;
    QString findRomFilePath(const QString& gameName);
    QComboBox *serverChooser;
    QNetworkAccessManager manager;
    QTableWidget *listWidget;
    QWebSocket *webSocket = nullptr;
    QLineEdit *playerNameEdit;
    QLineEdit *passwordEdit;
    QPushButton *joinButton;
    QPushButton *refreshButton;
    QLabel *pingLabel;
    QLabel *pNameLabel;
    QList<QJsonObject> rooms;
    int row = 0;
    int launched;
    QString filename;
    QUdpSocket broadcastSocket;
    QTimer *connectionTimer;
    QString customServerAddress;
    UserInterface::Widget::RomBrowserWidget *romBrowserWidget; // Add a member for the RomBrowserWidget
};

#endif // JOINROOM_H
