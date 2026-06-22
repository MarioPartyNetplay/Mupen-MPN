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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace
{

QString difficultyStars(int difficulty)
{
    const int clamped = std::clamp(difficulty, 1, 5);
    return QString(QStringLiteral("★").repeated(clamped) + QStringLiteral("☆").repeated(5 - clamped));
}

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
    const MarioPartyTarget gameIdTarget = marioPartyTargetFromGameId(this->details.value(QStringLiteral("gameId")).toInt());
    if (gameIdTarget != MarioPartyTarget::Unknown)
    {
        return gameIdTarget;
    }

    MarioPartyTarget target = detectMarioPartyTargetFromText(this->details.value(QStringLiteral("name")).toString());
    if (target != MarioPartyTarget::Unknown)
    {
        return target;
    }

    target = detectMarioPartyTargetFromText(this->details.value(QStringLiteral("description")).toString());
    if (target != MarioPartyTarget::Unknown)
    {
        return target;
    }

    return MarioPartyTarget::MarioParty1;
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
        this->baseRomLabel->setText(QStringLiteral("Base ROM: not found in ROM directory for %1").arg(marioPartyTargetLabel(target)));
        this->patchButton->setEnabled(false);
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

    const QJsonObject latestVersion = versions.first();
    remoteFileName = latestVersion.value(QStringLiteral("file_name")).toString(QStringLiteral("board.json"));

    QString downloadUrl = latestVersion.value(QStringLiteral("download_link")).toString();
    if (downloadUrl.isEmpty())
    {
        const QString fileId = latestVersion.value(QStringLiteral("file_id")).toVariant().toString();
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

        downloadUrl = QJsonDocument::fromJson(metadataData).object().value(QStringLiteral("download_link")).toString();
    }

    if (downloadUrl.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("Board version did not include a download link"), QString());
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

bool BoardDownloaderDetailDialog::ensurePartyPlannerCli(QString& cliPath)
{
    const QString configDirectory = boardDownloaderConfigDirectory();
    QDir().mkpath(configDirectory);

#ifdef Q_OS_WIN
    cliPath = configDirectory + QStringLiteral("/partyplanner-cli.exe");
    const QUrl cliUrl(QStringLiteral("https://github.com/PartyPlanner64/PartyPlanner64/releases/download/v0.8.2/partyplanner64-cli-win.exe"));
#else
    cliPath = configDirectory + QStringLiteral("/partyplanner-cli.exe");
    const QUrl cliUrl(QStringLiteral("https://github.com/PartyPlanner64/PartyPlanner64/releases/download/v0.8.2/partyplanner64-cli-win.exe"));
#endif

    if (QFile::exists(cliPath))
    {
        return true;
    }

    QString error;
    const QByteArray cliData = blockingNetworkGet(cliUrl, error);
    if (cliData.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to download PartyPlanner64 CLI"), error);
        return false;
    }

    QFile cliFile(cliPath);
    if (!cliFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to write PartyPlanner64 CLI"), QString());
        return false;
    }

    cliFile.write(cliData);
    cliFile.close();
    return true;
}

bool BoardDownloaderDetailDialog::patchRom(const QString& boardFilePath, const QString& romFilePath, const QString& outputFilePath)
{
    QString cliPath;
    if (!this->ensurePartyPlannerCli(cliPath))
    {
        return false;
    }

    QProcess process;
    QStringList arguments;
    arguments << QStringLiteral("overwrite")
              << QStringLiteral("--rom-file") << romFilePath
              << QStringLiteral("--target-board-index") << QStringLiteral("0")
              << QStringLiteral("--board-file") << boardFilePath
              << QStringLiteral("--output-file") << outputFilePath;

#ifdef Q_OS_WIN
    process.setProgram(cliPath);
    process.setArguments(arguments);
#else
    process.setProgram(QStringLiteral("wine"));
    QStringList wineArguments;
    wineArguments << cliPath;
    wineArguments.append(arguments);
    process.setArguments(wineArguments);
#endif

    process.start();
    if (!process.waitForStarted())
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to start PartyPlanner64 CLI"),
                            process.errorString());
        return false;
    }

    if (!process.waitForFinished(-1))
    {
        QtMessageBox::Error(this, QStringLiteral("PartyPlanner64 CLI timed out"), QString());
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to patch ROM"),
                            QString::fromUtf8(process.readAllStandardError()));
        return false;
    }

    return true;
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
    const QString defaultOutputName = QFileInfo(this->details.value(QStringLiteral("name")).toString()).completeBaseName() +
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

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to create temporary directory"), QString());
        QFile::remove(boardFilePath);
        return;
    }

    const QString temporaryOutputPath = temporaryDirectory.filePath(QStringLiteral("patched.z64"));
    if (!this->patchRom(boardFilePath, romFilePath, temporaryOutputPath))
    {
        QFile::remove(boardFilePath);
        return;
    }

    if (QFile::exists(outputFilePath))
    {
        QFile::remove(outputFilePath);
    }

    if (!QFile::copy(temporaryOutputPath, outputFilePath))
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to save patched ROM"), outputFilePath);
        QFile::remove(boardFilePath);
        return;
    }

    QFile::remove(boardFilePath);
    QtMessageBox::Info(this,
                       QStringLiteral("Patched ROM saved successfully"),
                       romMatch.has_value()
                           ? QStringLiteral("Used %1 from your ROM directory.").arg(romMatch->goodName)
                           : QString());
}
