/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "BoardDownloaderDialog.hpp"
#include "BoardDownloaderDetailDialog.hpp"
#include "BoardDownloaderCommon.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <RMG-Core/Settings.hpp>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalBlocker>
#include <QUrlQuery>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace
{

constexpr int kProjectIdRole = Qt::UserRole;

QJsonObject normalizeProjectDetails(const QJsonObject& details,
                                    const QString& projectName,
                                    int gameId,
                                    const QString& author)
{
    QJsonObject normalized = details;
    normalized.insert(QStringLiteral("name"), normalized.value(QStringLiteral("name")).toString(projectName));
    normalized.insert(QStringLiteral("author"), normalized.value(QStringLiteral("author")).toString(author));
    normalized.insert(QStringLiteral("creation_date"), normalized.value(QStringLiteral("creation_date")).toString());

    const int resolvedGameId = gameId > 0 ? gameId : gameIdFromJson(normalized);
    if (resolvedGameId > 0)
    {
        normalized.insert(QStringLiteral("gameId"), resolvedGameId);
    }

    normalized.insert(QStringLiteral("custom_events"),
                      normalized.contains(QStringLiteral("customEvents"))
                          ? normalized.value(QStringLiteral("customEvents")).toBool()
                          : normalized.value(QStringLiteral("custom_events")).toInt() != 0);
    normalized.insert(QStringLiteral("custom_music"),
                      normalized.contains(QStringLiteral("customMusic"))
                          ? normalized.value(QStringLiteral("customMusic")).toBool()
                          : normalized.value(QStringLiteral("custom_music")).toInt() != 0);
    return normalized;
}

QJsonArray projectListResultsFromDocument(const QJsonDocument& document, QString& errorMessage)
{
    errorMessage.clear();

    if (document.isArray())
    {
        return document.array();
    }

    if (document.isObject())
    {
        const QJsonObject object = document.object();
        if (object.contains(QStringLiteral("error")))
        {
            errorMessage = object.value(QStringLiteral("error")).toString();
            return {};
        }
    }

    errorMessage = QStringLiteral("Unexpected response from board API.");
    return {};
}

} // namespace

BoardDownloaderDialog::BoardDownloaderDialog(QWidget* parent) : QDialog(parent)
{
    this->setupUi(this);
    this->setWindowIcon(QIcon::fromTheme("download-cloud-line", QIcon(":Resource/RMG.png")));
    this->networkManager = new QNetworkAccessManager(this);

    this->loadTopProjects();
}

BoardDownloaderDialog::~BoardDownloaderDialog(void)
{
    this->abortActiveRequests();
}

bool BoardDownloaderDialog::isStaleSession(quint64 sessionId) const
{
    return sessionId != this->loadSessionId;
}

bool BoardDownloaderDialog::isCanceledReply(QNetworkReply* reply) const
{
    return reply != nullptr && reply->error() == QNetworkReply::OperationCanceledError;
}

void BoardDownloaderDialog::releaseReply(QNetworkReply*& member, QNetworkReply* reply)
{
    if (reply == nullptr)
    {
        return;
    }

    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    if (member == reply)
    {
        member = nullptr;
    }
}

void BoardDownloaderDialog::abortReply(QNetworkReply*& reply)
{
    if (reply == nullptr)
    {
        return;
    }

    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    reply = nullptr;
}

void BoardDownloaderDialog::abortActiveRequests(void)
{
    this->abortReply(this->projectListReply);
    this->abortReply(this->activeProjectReply);
    this->abortReply(this->activeGameIdReply);
    this->abortReply(this->activeIconReply);
    this->isLoadingProject = false;
}

void BoardDownloaderDialog::setLoading(bool loading)
{
    this->searchButton->setEnabled(!loading);
    this->searchLineEdit->setEnabled(!loading);
    this->updatePaginationControls(loading);
}

void BoardDownloaderDialog::loadTopProjects(void)
{
    this->currentSearchTerm.clear();
    this->listOffset = 0;
    this->loadCurrentPage();
}

void BoardDownloaderDialog::loadSearchProjects(const QString& searchTerm)
{
    this->currentSearchTerm = searchTerm;
    this->listOffset = 0;
    this->loadCurrentPage();
}

void BoardDownloaderDialog::loadCurrentPage(void)
{
    QUrl url;
    QString loadingText;

    if (this->currentSearchTerm.isEmpty())
    {
        url = QUrl(boardDownloaderApiBaseUrl() + QStringLiteral("/project/top"));
        loadingText = QStringLiteral("Loading top boards...");
    }
    else
    {
        url = QUrl(boardDownloaderApiBaseUrl() + QStringLiteral("/project/search"));
        QUrlQuery searchQuery;
        searchQuery.addQueryItem(QStringLiteral("searchTerm"), this->currentSearchTerm);
        url.setQuery(searchQuery);
        loadingText = QStringLiteral("Searching for \"%1\"...").arg(this->currentSearchTerm);
    }

    QUrlQuery query(url);
    if (query.isEmpty())
    {
        query = QUrlQuery();
    }
    query.addQueryItem(QStringLiteral("max"), QString::number(boardDownloaderDefaultPageSize()));
    query.addQueryItem(QStringLiteral("offset"), QString::number(this->listOffset));
    url.setQuery(query);

    this->beginProjectListRequest(url, loadingText);
}

void BoardDownloaderDialog::beginProjectListRequest(const QUrl& url, const QString& loadingText)
{
    this->loadSessionId++;
    this->abortActiveRequests();

    this->projects.clear();
    this->loadQueue.clear();
    this->loadQueueIndex = 0;
    this->activeSessionId = 0;
    this->resultsListWidget->clear();
    this->setLoading(true);
    this->statusLabel->setText(loadingText);

    const quint64 sessionId = this->loadSessionId;

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = this->networkManager->get(request);
    this->projectListReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, sessionId, reply]() {
        this->handleProjectListReply(reply, sessionId);
    });
}

void BoardDownloaderDialog::handleProjectListReply(QNetworkReply* reply, quint64 sessionId)
{
    this->setLoading(false);

    if (reply == nullptr || this->isStaleSession(sessionId))
    {
        this->releaseReply(this->projectListReply, reply);
        return;
    }

    if (reply->error() != QNetworkReply::NoError)
    {
        if (!this->isCanceledReply(reply))
        {
            QtMessageBox::Error(this, QStringLiteral("Failed to load boards"), reply->errorString());
            this->statusLabel->setText(QStringLiteral("Failed to load boards."));
        }

        this->releaseReply(this->projectListReply, reply);
        return;
    }

    QString apiError;
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    const QJsonArray results = projectListResultsFromDocument(document, apiError);
    this->releaseReply(this->projectListReply, reply);

    if (!apiError.isEmpty())
    {
        this->statusLabel->setText(apiError);
        return;
    }

    if (results.isEmpty())
    {
        this->lastPageResultCount = 0;
        this->updatePaginationControls();
        this->statusLabel->setText(QStringLiteral("No projects found."));
        return;
    }

    this->lastPageResultCount = results.size();
    this->updatePaginationControls();
    this->queueProjectDetails(results, sessionId);
}

void BoardDownloaderDialog::queueProjectDetails(const QJsonArray& results, quint64 sessionId)
{
    if (this->isStaleSession(sessionId))
    {
        return;
    }

    this->activeSessionId = sessionId;
    this->statusLabel->setText(QStringLiteral("Found %1 project(s). Loading boards...").arg(results.size()));

    for (const QJsonValue& value : results)
    {
        const QJsonObject project = value.toObject();
        const int projectId = project.contains(QStringLiteral("projectId"))
                                  ? project.value(QStringLiteral("projectId")).toInt()
                                  : project.value(QStringLiteral("id")).toInt();
        const QString projectName = project.value(QStringLiteral("name")).toString();
        const int gameId = project.value(QStringLiteral("gameId")).toInt();
        const QString author = project.contains(QStringLiteral("author"))
                                   ? project.value(QStringLiteral("author")).toString()
                                   : project.value(QStringLiteral("creator")).toString();

        if (projectId <= 0)
        {
            continue;
        }

        ProjectEntry entry;
        entry.projectId = projectId;
        entry.gameId = gameId;
        entry.name = projectName;
        entry.author = author;
        this->projects.push_back(entry);
        this->loadQueue.push_back(projectId);
    }

    this->processLoadQueue();
}

void BoardDownloaderDialog::processLoadQueue(void)
{
    if (this->isStaleSession(this->activeSessionId))
    {
        return;
    }

    if (this->isLoadingProject || this->loadQueueIndex >= this->loadQueue.size())
    {
        if (!this->isLoadingProject && this->loadQueueIndex >= this->loadQueue.size() && !this->loadQueue.isEmpty())
        {
            this->updateStatusLabel();
        }
        return;
    }

    const int projectId = this->loadQueue.at(this->loadQueueIndex);
    ProjectEntry* entry = this->findProjectEntry(projectId);
    if (entry == nullptr)
    {
        this->loadQueueIndex++;
        this->processLoadQueue();
        return;
    }

    this->isLoadingProject = true;
    this->updateStatusLabel();

    if (this->passesGameFilter(*entry))
    {
        entry->revealed = true;
        this->updateListItem(*entry);
    }

    this->fetchProjectDetails(projectId, entry->name, entry->gameId, entry->author, this->activeSessionId);
}

BoardDownloaderDialog::ProjectEntry* BoardDownloaderDialog::findProjectEntry(int projectId)
{
    for (ProjectEntry& entry : this->projects)
    {
        if (entry.projectId == projectId)
        {
            return &entry;
        }
    }

    return nullptr;
}

const BoardDownloaderDialog::ProjectEntry* BoardDownloaderDialog::findProjectEntry(int projectId) const
{
    for (const ProjectEntry& entry : this->projects)
    {
        if (entry.projectId == projectId)
        {
            return &entry;
        }
    }

    return nullptr;
}

QListWidgetItem* BoardDownloaderDialog::findListItem(int projectId) const
{
    for (int row = 0; row < this->resultsListWidget->count(); row++)
    {
        QListWidgetItem* item = this->resultsListWidget->item(row);
        if (item != nullptr && item->data(kProjectIdRole).toInt() == projectId)
        {
            return item;
        }
    }

    return nullptr;
}

void BoardDownloaderDialog::fetchProjectDetails(int projectId,
                                                const QString& projectName,
                                                int gameId,
                                                const QString& author,
                                                quint64 sessionId)
{
    if (this->isStaleSession(sessionId))
    {
        return;
    }

    const QUrl url(boardDownloaderApiBaseUrl() + QStringLiteral("/project/%1").arg(projectId));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = this->networkManager->get(request);
    this->activeProjectReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, sessionId, projectId, projectName, gameId, author, reply]() {
        this->handleProjectDetailsReply(reply, projectId, projectName, gameId, author, sessionId);
    });
}

void BoardDownloaderDialog::handleProjectDetailsReply(QNetworkReply* reply,
                                                      int projectId,
                                                      const QString& projectName,
                                                      int gameId,
                                                      const QString& author,
                                                      quint64 sessionId)
{
    if (this->isStaleSession(sessionId))
    {
        this->releaseReply(this->activeProjectReply, reply);
        return;
    }

    QJsonObject details;
    if (reply != nullptr && reply->error() == QNetworkReply::NoError)
    {
        details = QJsonDocument::fromJson(reply->readAll()).object();
    }

    this->releaseReply(this->activeProjectReply, reply);

    if (this->isStaleSession(sessionId))
    {
        return;
    }

    details = normalizeProjectDetails(details, projectName, gameId, author);

    ProjectEntry* entry = this->findProjectEntry(projectId);
    if (entry != nullptr)
    {
        entry->details = details;
        entry->hasDetails = true;
        entry->gameId = gameIdFromJson(details);
        if (entry->gameId <= 0)
        {
            entry->gameId = gameId;
        }

        if (this->passesGameFilter(*entry))
        {
            entry->revealed = true;
            this->updateListItem(*entry);
        }
    }

    if (entry != nullptr && entry->gameId <= 0)
    {
        this->fetchProjectGameId(projectId, projectName, details, sessionId);
        return;
    }

    this->fetchProjectIcon(projectId, sessionId);
}

void BoardDownloaderDialog::fetchProjectGameId(int projectId, const QString& projectName, const QJsonObject& details, quint64 sessionId)
{
    if (this->isStaleSession(sessionId))
    {
        return;
    }

    QUrl url(boardDownloaderApiBaseUrl() + QStringLiteral("/project/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("searchTerm"), projectName);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = this->networkManager->get(request);
    this->activeGameIdReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, sessionId, projectId, details, reply]() {
        if (this->isStaleSession(sessionId))
        {
            this->releaseReply(this->activeGameIdReply, reply);
            return;
        }

        QJsonObject updatedDetails = details;
        int resolvedGameId = 0;

        if (reply != nullptr && reply->error() == QNetworkReply::NoError)
        {
            QString apiError;
            const QJsonArray results = projectListResultsFromDocument(QJsonDocument::fromJson(reply->readAll()), apiError);
            Q_UNUSED(apiError);

            for (const QJsonValue& value : results)
            {
                const QJsonObject project = value.toObject();
                const int resultProjectId = project.value(QStringLiteral("projectId")).toInt();
                if (resultProjectId != projectId)
                {
                    continue;
                }

                resolvedGameId = gameIdFromJson(project);
                break;
            }
        }

        this->releaseReply(this->activeGameIdReply, reply);

        if (this->isStaleSession(sessionId))
        {
            return;
        }

        if (resolvedGameId > 0)
        {
            updatedDetails.insert(QStringLiteral("gameId"), resolvedGameId);
        }

        ProjectEntry* entry = this->findProjectEntry(projectId);
        if (entry != nullptr)
        {
            entry->details = updatedDetails;
            entry->gameId = resolvedGameId;

            if (this->passesGameFilter(*entry))
            {
                entry->revealed = true;
                this->updateListItem(*entry);
            }
        }

        this->fetchProjectIcon(projectId, sessionId);
    });
}

void BoardDownloaderDialog::fetchProjectIcon(int projectId, quint64 sessionId)
{
    if (this->isStaleSession(sessionId))
    {
        return;
    }

    const QUrl iconRequestUrl = projectIconUrl(projectId);
    QNetworkRequest request(iconRequestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = this->networkManager->get(request);
    this->activeIconReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, sessionId, projectId, reply]() {
        this->handleProjectIconReply(reply, projectId, sessionId);
    });
}

void BoardDownloaderDialog::handleProjectIconReply(QNetworkReply* reply, int projectId, quint64 sessionId)
{

    if (this->isStaleSession(sessionId))
    {
        this->releaseReply(this->activeIconReply, reply);
        return;
    }

    QPixmap icon;
    if (reply != nullptr && reply->error() == QNetworkReply::NoError)
    {
        icon.loadFromData(reply->readAll());
    }

    this->releaseReply(this->activeIconReply, reply);

    if (this->isStaleSession(sessionId))
    {
        return;
    }

    ProjectEntry* entry = this->findProjectEntry(projectId);
    if (entry != nullptr)
    {
        entry->icon = icon;
        entry->iconLoaded = true;

        if (this->passesGameFilter(*entry))
        {
            this->updateListItem(*entry);
        }
    }

    this->finishCurrentProjectLoad();
}

void BoardDownloaderDialog::finishCurrentProjectLoad(void)
{
    if (this->isStaleSession(this->activeSessionId))
    {
        this->isLoadingProject = false;
        return;
    }

    this->isLoadingProject = false;
    this->loadQueueIndex++;
    this->processLoadQueue();
}

int BoardDownloaderDialog::resolvedGameId(const ProjectEntry& entry) const
{
    if (entry.gameId > 0)
    {
        return entry.gameId;
    }

    if (entry.hasDetails)
    {
        return gameIdFromJson(entry.details);
    }

    return 0;
}

bool BoardDownloaderDialog::passesGameFilter(const ProjectEntry& entry) const
{
    const int filterGameId = this->gameFilterComboBox->currentIndex();
    if (filterGameId == 0)
    {
        return true;
    }

    return this->resolvedGameId(entry) == filterGameId;
}

QString BoardDownloaderDialog::projectListText(const ProjectEntry& entry) const
{
    QStringList parts;
    parts << entry.name;

    if (!entry.author.isEmpty())
    {
        parts << QStringLiteral("by %1").arg(entry.author);
    }

    const int gameId = this->resolvedGameId(entry);
    if (gameId > 0)
    {
        parts << marioPartyTargetLabel(marioPartyTargetFromGameId(gameId));
    }

    return parts.join(QStringLiteral("  •  "));
}

QIcon BoardDownloaderDialog::projectListIcon(const ProjectEntry& entry) const
{
    if (entry.iconLoaded && !entry.icon.isNull())
    {
        return QIcon(entry.icon);
    }

    return QIcon::fromTheme(QStringLiteral("image-line"));
}

void BoardDownloaderDialog::updateListItem(const ProjectEntry& entry)
{
    if (!this->passesGameFilter(entry))
    {
        QListWidgetItem* existingItem = this->findListItem(entry.projectId);
        if (existingItem != nullptr)
        {
            const int row = this->resultsListWidget->row(existingItem);
            delete this->resultsListWidget->takeItem(row);
        }
        return;
    }

    QListWidgetItem* item = this->findListItem(entry.projectId);
    if (item == nullptr)
    {
        item = new QListWidgetItem(this->projectListText(entry));
        item->setData(kProjectIdRole, entry.projectId);
        this->resultsListWidget->addItem(item);
    }
    else
    {
        item->setText(this->projectListText(entry));
    }

    item->setIcon(this->projectListIcon(entry));
}

void BoardDownloaderDialog::refreshResultsList(void)
{
    const int selectedProjectId = this->resultsListWidget->currentItem() != nullptr
                                      ? this->resultsListWidget->currentItem()->data(kProjectIdRole).toInt()
                                      : -1;

    QSignalBlocker blocker(this->resultsListWidget);
    this->resultsListWidget->clear();

    for (const ProjectEntry& entry : this->projects)
    {
        if (!entry.revealed || !this->passesGameFilter(entry))
        {
            continue;
        }

        auto* item = new QListWidgetItem(this->projectListText(entry));
        item->setData(kProjectIdRole, entry.projectId);
        item->setIcon(this->projectListIcon(entry));
        this->resultsListWidget->addItem(item);

        if (entry.projectId == selectedProjectId)
        {
            this->resultsListWidget->setCurrentItem(item);
        }
    }
}

void BoardDownloaderDialog::updatePaginationControls(bool loading)
{
    const int pageSize = boardDownloaderDefaultPageSize();
    const int pageNumber = this->listOffset / pageSize + 1;
    const int rangeStart = this->lastPageResultCount > 0 ? this->listOffset + 1 : 0;
    const int rangeEnd = this->listOffset + this->lastPageResultCount;

    if (this->lastPageResultCount > 0)
    {
        this->paginationLabel->setText(QStringLiteral("Page %1  (%2–%3)")
                                           .arg(pageNumber)
                                           .arg(rangeStart)
                                           .arg(rangeEnd));
    }
    else
    {
        this->paginationLabel->setText(QStringLiteral("Page %1").arg(pageNumber));
    }

    this->prevPageButton->setEnabled(!loading && this->listOffset > 0);
    this->nextPageButton->setEnabled(!loading && this->lastPageResultCount >= pageSize);
}

void BoardDownloaderDialog::updateStatusLabel(void)
{
    if (this->isLoadingProject && this->loadQueueIndex < this->loadQueue.size())
    {
        const ProjectEntry* entry = this->findProjectEntry(this->loadQueue.at(this->loadQueueIndex));
        const QString projectName = entry != nullptr ? entry->name : QStringLiteral("board");
        this->statusLabel->setText(QStringLiteral("Loading %1 of %2: %3...")
                                       .arg(this->loadQueueIndex + 1)
                                       .arg(this->loadQueue.size())
                                       .arg(projectName));
        return;
    }

    int visibleCount = 0;
    for (const ProjectEntry& entry : this->projects)
    {
        if (this->passesGameFilter(entry))
        {
            visibleCount++;
        }
    }

    this->statusLabel->setText(QStringLiteral("Showing %1 of %2 project(s).")
                                   .arg(visibleCount)
                                   .arg(this->projects.size()));
}

void BoardDownloaderDialog::openProjectDetails(int projectId)
{
    for (const ProjectEntry& entry : this->projects)
    {
        if (entry.projectId != projectId)
        {
            continue;
        }

        QJsonObject details = entry.hasDetails
                                  ? entry.details
                                  : QJsonObject{
                                        {QStringLiteral("name"), entry.name},
                                        {QStringLiteral("author"), entry.author},
                                        {QStringLiteral("gameId"), entry.gameId},
                                        {QStringLiteral("description"), QStringLiteral("No description available")},
                                    };

        BoardDownloaderDetailDialog dialog(this, projectId, details, entry.icon);
        connect(&dialog, &BoardDownloaderDetailDialog::romPatched, this, &BoardDownloaderDialog::on_romPatched);
        dialog.exec();
        return;
    }
}

void BoardDownloaderDialog::on_searchButton_clicked(void)
{
    const QString searchTerm = this->searchLineEdit->text().trimmed();
    if (searchTerm.isEmpty())
    {
        this->loadTopProjects();
        return;
    }

    this->loadSearchProjects(searchTerm);
}

void BoardDownloaderDialog::on_searchLineEdit_returnPressed(void)
{
    this->on_searchButton_clicked();
}

void BoardDownloaderDialog::on_prevPageButton_clicked(void)
{
    const int pageSize = boardDownloaderDefaultPageSize();
    if (this->listOffset < pageSize)
    {
        return;
    }

    this->listOffset -= pageSize;
    this->loadCurrentPage();
}

void BoardDownloaderDialog::on_nextPageButton_clicked(void)
{
    this->listOffset += boardDownloaderDefaultPageSize();
    this->loadCurrentPage();
}

void BoardDownloaderDialog::on_gameFilterComboBox_currentIndexChanged(int index)
{
    Q_UNUSED(index);
    this->refreshResultsList();
    this->updateStatusLabel();
}

void BoardDownloaderDialog::on_resultsListWidget_itemDoubleClicked(void)
{
    QListWidgetItem* item = this->resultsListWidget->currentItem();
    if (item == nullptr)
    {
        return;
    }

    this->openProjectDetails(item->data(kProjectIdRole).toInt());
}

void BoardDownloaderDialog::on_uploadJsonButton_clicked(void)
{
    const QString boardFilePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Upload Board JSON"),
        QString(),
        QStringLiteral("JSON Files (*.json);;All Files (*)"));
    if (boardFilePath.isEmpty())
    {
        return;
    }

    QString boardName = QFileInfo(boardFilePath).completeBaseName();
    MarioPartyTarget target = MarioPartyTarget::Unknown;

    QFile boardFile(boardFilePath);
    if (boardFile.open(QIODevice::ReadOnly))
    {
        const QJsonObject boardObject = QJsonDocument::fromJson(boardFile.readAll()).object();
        boardFile.close();

        if (!boardObject.isEmpty())
        {
            target = marioPartyTargetFromGameId(gameIdFromJson(boardObject));
            const QString jsonName = boardObject.value(QStringLiteral("name")).toString();
            if (!jsonName.isEmpty())
            {
                boardName = jsonName;
            }
        }
    }

    if (target == MarioPartyTarget::Unknown)
    {
        const QStringList games = {
            marioPartyTargetLabel(MarioPartyTarget::MarioParty1),
            marioPartyTargetLabel(MarioPartyTarget::MarioParty2),
            marioPartyTargetLabel(MarioPartyTarget::MarioParty3),
        };

        bool ok = false;
        const QString choice = QInputDialog::getItem(
            this,
            QStringLiteral("Select Target Game"),
            QStringLiteral("Which Mario Party game is this board for?"),
            games,
            0,
            false,
            &ok);
        if (!ok || choice.isEmpty())
        {
            return;
        }

        if (choice == games.at(1))
        {
            target = MarioPartyTarget::MarioParty2;
        }
        else if (choice == games.at(2))
        {
            target = MarioPartyTarget::MarioParty3;
        }
        else
        {
            target = MarioPartyTarget::MarioParty1;
        }
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

    const QString romDirectory = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory));
    const QString defaultOutputName = sanitizeBoardFileName(boardName) + QStringLiteral(" (patched).z64");
    const QString defaultOutputPath = QDir(romDirectory).filePath(defaultOutputName);

    QString outputFilePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Patched ROM"),
        defaultOutputPath,
        QStringLiteral("Patched ROM (*.z64);;All Files (*)"));
    if (outputFilePath.isEmpty())
    {
        return;
    }

    if (!outputFilePath.endsWith(QStringLiteral(".z64"), Qt::CaseInsensitive))
    {
        outputFilePath += QStringLiteral(".z64");
    }

    if (!patchMarioPartyBoardRom(this, boardFilePath, romFilePath, outputFilePath))
    {
        return;
    }

    QtMessageBox::Info(this,
                       QStringLiteral("Patched ROM saved successfully"),
                       romMatch.has_value()
                           ? QStringLiteral("Used %1 from your ROM directory.").arg(romMatch->goodName)
                           : QString());
    this->on_romPatched();
}

void BoardDownloaderDialog::on_romPatched(void)
{
    this->emit romListRefreshRequested();
}
