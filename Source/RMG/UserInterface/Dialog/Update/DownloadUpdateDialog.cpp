#include "DownloadUpdateDialog.hpp"
#include "InstallUpdateDialog.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMessageBox>
#include <QFile>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QLabel>
#include <QThread>

DownloadUpdateDialog::DownloadUpdateDialog(QWidget* parent, const QString& url, const QString& filename)
    : QDialog(parent), filename(filename), networkAccessManager(new QNetworkAccessManager(this))
{
    setWindowTitle(QStringLiteral("Downloading %1").arg(this->filename));
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    QLabel* label = new QLabel(QStringLiteral("Downloading %1...").arg(this->filename), this);
    layout->addWidget(label);
    
    progressBar = new QProgressBar(this);
    layout->addWidget(progressBar);
    
    setLayout(layout);
    setMinimumSize(300, 100);
    
    temporaryDirectory = QDir::tempPath();
    file.setFileName(temporaryDirectory + QDir::separator() + filename);
    
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    
    QNetworkReply* reply = networkAccessManager->get(request);
    
    connect(reply, &QNetworkReply::downloadProgress, this, &DownloadUpdateDialog::updateProgress);
    connect(reply, &QNetworkReply::finished, this, &DownloadUpdateDialog::onDownloadFinished);
    connect(reply, &QNetworkReply::errorOccurred, this, &DownloadUpdateDialog::handleError);
}

DownloadUpdateDialog::~DownloadUpdateDialog()
{
}

void DownloadUpdateDialog::handleError(QNetworkReply::NetworkError error)
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply) {
        QMessageBox::critical(this, tr("Error"), tr("Download failed: %1").arg(reply->errorString()));
        reply->deleteLater();
    }
    reject();
}

void DownloadUpdateDialog::onDownloadFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        if (file.open(QIODevice::WriteOnly)) {
            file.write(reply->readAll());
            file.close();
        }
        
        #ifdef _WIN32
        installationDirectory = QCoreApplication::applicationDirPath();
        #endif
        #ifdef __APPLE__
        installationDirectory = QCoreApplication::applicationDirPath() + QStringLiteral("/../../../");
        #endif
        
        temporaryDirectory = QDir::tempPath();
        InstallUpdateDialog* installDialog = new InstallUpdateDialog(this, installationDirectory, temporaryDirectory, this->filename);
        installDialog->exec();
        accept();
    }

    reply->deleteLater();
}

void DownloadUpdateDialog::updateProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    if (bytesTotal > 0) {
        progressBar->setMaximum(bytesTotal);
        progressBar->setValue(bytesReceived);
    }
}