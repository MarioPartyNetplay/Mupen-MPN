/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "BoardDownloaderDetailDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <RMG-Core/Settings.hpp>

#include <QEventLoop>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QUrlQuery>

#include <algorithm>
#include <optional>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace
{

QByteArray blockingNetworkGet(const QUrl& url, QString& error)
{
    QNetworkAccessManager networkManager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QEventLoop loop;
    QNetworkReply* reply = networkManager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
    {
        error = reply->errorString();
        reply->deleteLater();
        return {};
    }

    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

QString difficultyStars(int difficulty)
{
    const int clamped = std::clamp(difficulty, 1, 5);
    return QString(QStringLiteral("★").repeated(clamped) + QStringLiteral("☆").repeated(5 - clamped));
}

QString absoluteNativePath(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

struct BoardVersionFile
{
    QString fileName;
    QString downloadUrl;
    QString fileSize;
    QString subFileId;
};

QList<BoardVersionFile> parseVersionFiles(const QJsonObject& versionObject)
{
    const QJsonArray filesArray = versionObject.value(QStringLiteral("files")).toArray();
    if (!filesArray.isEmpty())
    {
        QList<BoardVersionFile> files;
        files.reserve(filesArray.size());
        for (const QJsonValue& value : filesArray)
        {
            const QJsonObject fileObject = value.toObject();
            BoardVersionFile file;
            file.fileName = fileObject.value(QStringLiteral("file_name")).toString();
            file.fileSize = fileObject.value(QStringLiteral("file_size")).toString();
            file.downloadUrl = fileObject.value(QStringLiteral("download_link")).toString();
            file.subFileId = fileObject.value(QStringLiteral("sub_file_id")).toVariant().toString();
            if (!file.fileName.isEmpty())
            {
                files.push_back(file);
            }
        }
        return files;
    }

    BoardVersionFile file;
    file.fileName = versionObject.value(QStringLiteral("file_name")).toString(QStringLiteral("board.json"));
    file.downloadUrl = versionObject.value(QStringLiteral("download_link")).toString();
    return {file};
}

bool versionFilesNeedMetadata(const QList<BoardVersionFile>& files)
{
    return std::all_of(files.begin(), files.end(), [](const BoardVersionFile& file) {
        return file.downloadUrl.isEmpty();
    });
}

QString resolveBoardFileDownloadUrl(int projectId,
                                    const QJsonObject& versionObject,
                                    const BoardVersionFile& file,
                                    QString& error)
{
    if (!file.downloadUrl.isEmpty())
    {
        return file.downloadUrl;
    }

    const QString fileId = versionObject.value(QStringLiteral("file_id")).toVariant().toString();
    if (fileId.isEmpty())
    {
        return {};
    }

    QUrl metadataUrl(boardDownloaderApiBaseUrl() +
                     QStringLiteral("/project/%1/files/%2").arg(projectId).arg(fileId));
    if (!file.subFileId.isEmpty())
    {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("file"), file.subFileId);
        metadataUrl.setQuery(query);
    }

    const QByteArray metadataData = blockingNetworkGet(metadataUrl, error);
    if (metadataData.isEmpty())
    {
        return {};
    }

    const QJsonObject metadataObject = QJsonDocument::fromJson(metadataData).object();

    if (!file.subFileId.isEmpty())
    {
        const QJsonArray filesArray = metadataObject.value(QStringLiteral("files")).toArray();
        for (const QJsonValue& value : filesArray)
        {
            const QJsonObject fileObject = value.toObject();
            if (fileObject.value(QStringLiteral("sub_file_id")).toVariant().toString() == file.subFileId)
            {
                return fileObject.value(QStringLiteral("download_link")).toString();
            }
        }
    }

    return metadataObject.value(QStringLiteral("download_link")).toString();
}

bool pickBoardVersionFile(QWidget* parent, const QList<BoardVersionFile>& files, BoardVersionFile& selected)
{
    if (files.isEmpty())
    {
        return false;
    }

    if (files.size() == 1)
    {
        selected = files.first();
        return true;
    }

    QStringList labels;
    labels.reserve(files.size());
    for (const BoardVersionFile& file : files)
    {
        if (file.fileSize.isEmpty())
        {
            labels.push_back(file.fileName);
        }
        else
        {
            labels.push_back(QStringLiteral("%1 (%2)").arg(file.fileName, file.fileSize));
        }
    }

    bool accepted = false;
    const QString choice = QInputDialog::getItem(parent,
                                                 QStringLiteral("Select Board File"),
                                                 QStringLiteral("This version includes multiple board files:"),
                                                 labels,
                                                 0,
                                                 false,
                                                 &accepted);
    if (!accepted || choice.isEmpty())
    {
        return false;
    }

    const int index = labels.indexOf(choice);
    selected = files.at(index >= 0 ? index : 0);
    return true;
}

} // namespace

BoardDownloaderDetailDialog::BoardDownloaderDetailDialog(QWidget* parent, int projectId, const QJsonObject& details, const QPixmap& icon)
    : QDialog(parent), projectId(projectId), details(details), iconPixmap(icon)
{
    this->setupUi(this);
    this->setWindowIcon(QIcon::fromTheme("download-cloud-line", QIcon(":Resource/RMG.png")));
    this->populateDetails();
}

MarioPartyTarget BoardDownloaderDetailDialog::targetGame(void) const
{
    return marioPartyTargetFromGameId(gameIdFromJson(this->details));
}

void BoardDownloaderDetailDialog::populateDetails(void)
{
    const QString name = this->details.value(QStringLiteral("name")).toString();
    const QString author = this->details.value(QStringLiteral("author")).toString();
    const QString creationDate = formatBoardDate(this->details.value(QStringLiteral("creation_date")).toString());
    const int difficulty = this->details.value(QStringLiteral("difficulty")).toInt(1);
    const QString turns = this->details.value(QStringLiteral("recommended_turns")).toVariant().toString();
    const bool customEvents = this->details.value(QStringLiteral("custom_events")).toBool();
    const bool customMusic = this->details.value(QStringLiteral("custom_music")).toBool();
    const QString description = this->details.value(QStringLiteral("description")).toString(
        QStringLiteral("No description available"));

    this->setWindowTitle(name);
    this->titleLabel->setText(name);
    this->authorLabel->setText(QStringLiteral("Author: %1").arg(author.isEmpty() ? QStringLiteral("Unknown") : author));
    this->createdLabel->setText(QStringLiteral("Created on: %1").arg(creationDate));
    this->difficultyLabel->setText(QStringLiteral("Difficulty: %1").arg(difficultyStars(difficulty)));
    this->turnsLabel->setText(QStringLiteral("Recommended Turns: %1").arg(turns.isEmpty() ? QStringLiteral("N/A") : turns));
    this->eventsLabel->setText(QStringLiteral("Custom Events: %1").arg(customEvents ? QStringLiteral("Yes") : QStringLiteral("No")));
    this->musicLabel->setText(QStringLiteral("Custom Music: %1").arg(customMusic ? QStringLiteral("Yes") : QStringLiteral("No")));
    this->descriptionTextEdit->setPlainText(description);

    const MarioPartyTarget target = this->targetGame();
    if (target == MarioPartyTarget::Unknown)
    {
        this->targetGameLabel->setText(QStringLiteral("Target Game: Unknown"));
        this->baseRomLabel->setText(QStringLiteral("Base ROM: game not specified by the API"));
        this->patchButton->setEnabled(false);
    }
    else
    {
        this->targetGameLabel->setText(QStringLiteral("Target Game: %1").arg(marioPartyTargetLabel(target)));

        const std::optional<MarioPartyRomMatch> romMatch = findBestMarioPartyRom(target);
        if (romMatch.has_value())
        {
            this->baseRomLabel->setText(QStringLiteral("Base ROM: %1").arg(romMatch->goodName));
            this->patchButton->setEnabled(true);
            this->patchButton->setToolTip(romMatch->path);
        }
        else
        {
            this->baseRomLabel->setText(QStringLiteral("Base ROM: not found in ROM directory for %1")
                                            .arg(marioPartyTargetLabel(target)));
            this->patchButton->setEnabled(false);
        }
    }

    if (!this->iconPixmap.isNull())
    {
        this->iconLabel->setPixmap(this->iconPixmap);
    }
    else
    {
        this->iconLabel->setPixmap(QIcon::fromTheme("image-line").pixmap(128, 128));
    }
}

bool BoardDownloaderDetailDialog::downloadLatestBoardFile(QString& localPath, QString& remoteFileName)
{
    QString error;
    const QUrl filesUrl = QUrl(boardDownloaderApiBaseUrl() + QStringLiteral("/project/%1/files").arg(this->projectId));
    const QByteArray filesData = blockingNetworkGet(filesUrl, error);
    if (filesData.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to fetch board files"), error);
        return false;
    }

    const QJsonObject filesObject = QJsonDocument::fromJson(filesData).object();
    const QJsonArray versionsArray = filesObject.value(QStringLiteral("versions")).toArray();
    if (versionsArray.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("No board versions found"), QString());
        return false;
    }

    QList<QJsonObject> versions;
    versions.reserve(versionsArray.size());
    for (const QJsonValue& value : versionsArray)
    {
        versions.push_back(value.toObject());
    }

    std::sort(versions.begin(), versions.end(), [](const QJsonObject& left, const QJsonObject& right) {
        return left.value(QStringLiteral("release_date")).toString() >
               right.value(QStringLiteral("release_date")).toString();
    });

    QJsonObject versionData = versions.first();
    QList<BoardVersionFile> versionFiles = parseVersionFiles(versionData);

    if (versionFilesNeedMetadata(versionFiles))
    {
        const QString fileId = versionData.value(QStringLiteral("file_id")).toVariant().toString();
        if (fileId.isEmpty())
        {
            QtMessageBox::Error(this, QStringLiteral("Latest board version is missing a download link"), QString());
            return false;
        }

        const QUrl metadataUrl = QUrl(boardDownloaderApiBaseUrl() +
                                      QStringLiteral("/project/%1/files/%2").arg(this->projectId).arg(fileId));
        const QByteArray metadataData = blockingNetworkGet(metadataUrl, error);
        if (metadataData.isEmpty())
        {
            QtMessageBox::Error(this, QStringLiteral("Failed to fetch board metadata"), error);
            return false;
        }

        versionData = QJsonDocument::fromJson(metadataData).object();
        versionFiles = parseVersionFiles(versionData);
    }

    BoardVersionFile selectedFile;
    if (!pickBoardVersionFile(this, versionFiles, selectedFile))
    {
        return false;
    }

    remoteFileName = selectedFile.fileName.isEmpty() ? QStringLiteral("board.json") : selectedFile.fileName;

    const QString downloadUrl = resolveBoardFileDownloadUrl(this->projectId, versionData, selectedFile, error);
    if (downloadUrl.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("Board version did not include a download link"), error);
        return false;
    }

    const QByteArray boardData = blockingNetworkGet(QUrl(downloadUrl), error);
    if (boardData.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to download board file"), error);
        return false;
    }

    QTemporaryFile temporaryFile;
    temporaryFile.setAutoRemove(false);
    if (!temporaryFile.open())
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to create temporary board file"), QString());
        return false;
    }

    temporaryFile.write(boardData);
    temporaryFile.close();
    localPath = temporaryFile.fileName();
    return true;
}

bool BoardDownloaderDetailDialog::patchRom(const QString& boardFilePath,
                                           const QString& romFilePath,
                                           const QString& outputFilePath)
{
    return patchMarioPartyBoardRom(this, boardFilePath, romFilePath, outputFilePath);
}

void BoardDownloaderDetailDialog::on_downloadButton_clicked(void)
{
    QString localPath;
    QString remoteFileName;
    if (!this->downloadLatestBoardFile(localPath, remoteFileName))
    {
        return;
    }

    const QString savePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Board File"),
        remoteFileName,
        QStringLiteral("JSON Files (*.json);;All Files (*)"));

    if (savePath.isEmpty())
    {
        QFile::remove(localPath);
        return;
    }

    if (QFile::exists(savePath))
    {
        QFile::remove(savePath);
    }

    if (!QFile::copy(localPath, savePath))
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to save board file"), savePath);
    }

    QFile::remove(localPath);
}

void BoardDownloaderDetailDialog::on_patchButton_clicked(void)
{
    const MarioPartyTarget target = this->targetGame();
    if (target == MarioPartyTarget::Unknown)
    {
        QtMessageBox::Error(this, QStringLiteral("Cannot patch ROM"), QStringLiteral("The API did not specify a target game for this board."));
        return;
    }

    const std::optional<MarioPartyRomMatch> romMatch = findBestMarioPartyRom(target);
    QString romFilePath;

    if (romMatch.has_value())
    {
        romFilePath = romMatch->path;
    }
    else
    {
        romFilePath = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select Base ROM"),
            QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory)),
            QStringLiteral("Nintendo 64 ROM (*.z64 *.n64 *.v64);;All Files (*)"));
    }

    if (romFilePath.isEmpty())
    {
        return;
    }

    QString boardFilePath;
    QString remoteFileName;
    if (!this->downloadLatestBoardFile(boardFilePath, remoteFileName))
    {
        return;
    }

    const QString romDirectory = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory));
    const QString defaultOutputName = sanitizeBoardFileName(this->details.value(QStringLiteral("name")).toString()) +
                                      QStringLiteral(" (patched).z64");
    const QString defaultOutputPath = QDir(romDirectory).filePath(defaultOutputName);

    QString outputFilePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Patched ROM"),
        defaultOutputPath,
        QStringLiteral("Patched ROM (*.z64);;All Files (*)"));

    if (outputFilePath.isEmpty())
    {
        QFile::remove(boardFilePath);
        return;
    }

    if (!outputFilePath.endsWith(QStringLiteral(".z64"), Qt::CaseInsensitive))
    {
        outputFilePath += QStringLiteral(".z64");
    }

    if (!this->patchRom(boardFilePath, romFilePath, outputFilePath))
    {
        QFile::remove(boardFilePath);
        return;
    }

    QFile::remove(boardFilePath);
    QtMessageBox::Info(this,
                       QStringLiteral("Patched ROM saved successfully"),
                       romMatch.has_value()
                           ? QStringLiteral("Used %1 from your ROM directory.").arg(romMatch->goodName)
                           : QString());
    emit romPatched();
}
