/*
*  Dolphin for Mario Party Netplay
*  Copyright (C) 2025 Tabitha Hanegan <tabithahanegan.com>
*/

#pragma once

#include <QDialog>
#include <QString>
#include <QProgressBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>

class DownloadUpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DownloadUpdateDialog(QWidget* parent, const QString& url, const QString& filename);
    ~DownloadUpdateDialog();
    QString GetTempDirectory() const { return temporaryDirectory; }
    QString GetFileName() const { return filename; }
public slots:
    void updateProgress(qint64 bytesReceived, qint64 bytesTotal);
    void handleError(QNetworkReply::NetworkError error);
    void onDownloadFinished();

private:
    QString filename;
    QProgressBar* progressBar;
    QString installationDirectory;
    QString temporaryDirectory;
    QNetworkAccessManager* networkAccessManager;
    QFile file;
};