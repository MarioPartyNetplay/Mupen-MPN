/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef BOARDDOWNLOADERDIALOG_HPP
#define BOARDDOWNLOADERDIALOG_HPP

#include <QDialog>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPixmap>

#include "ui_BoardDownloaderDialog.h"

namespace UserInterface
{
namespace Dialog
{

class BoardDownloaderDialog : public QDialog, private Ui::BoardDownloaderDialog
{
    Q_OBJECT

  public:
    explicit BoardDownloaderDialog(QWidget* parent = nullptr);
    ~BoardDownloaderDialog(void) override;

  signals:
    void romListRefreshRequested(void);

  private:
    struct ProjectEntry
    {
        int projectId = 0;
        int gameId = 0;
        QString name;
        QString author;
        QJsonObject details;
        QPixmap icon;
        bool hasDetails = false;
        bool iconLoaded = false;
        bool revealed = false;
    };

    QNetworkAccessManager* networkManager = nullptr;
    QNetworkReply* projectListReply = nullptr;
    QNetworkReply* activeProjectReply = nullptr;
    QNetworkReply* activeGameIdReply = nullptr;
    QNetworkReply* activeIconReply = nullptr;
    QList<ProjectEntry> projects;
    QList<int> loadQueue;
    int loadQueueIndex = 0;
    bool isLoadingProject = false;
    quint64 loadSessionId = 0;
    quint64 activeSessionId = 0;

    bool isStaleSession(quint64 sessionId) const;
    bool isCanceledReply(QNetworkReply* reply) const;
    void abortReply(QNetworkReply*& reply);
    void releaseReply(QNetworkReply*& member, QNetworkReply* reply);
    void setLoading(bool loading);
    void abortActiveRequests(void);
    void loadTopProjects(void);
    void loadSearchProjects(const QString& searchTerm);
    void beginProjectListRequest(const QUrl& url, const QString& loadingText);
    void handleProjectListReply(QNetworkReply* reply, quint64 sessionId);
    void queueProjectDetails(const QJsonArray& results, quint64 sessionId);
    void processLoadQueue(void);
    ProjectEntry* findProjectEntry(int projectId);
    const ProjectEntry* findProjectEntry(int projectId) const;
    QListWidgetItem* findListItem(int projectId) const;
    void fetchProjectDetails(int projectId, const QString& projectName, int gameId, const QString& author, quint64 sessionId);
    void handleProjectDetailsReply(QNetworkReply* reply,
                                   int projectId,
                                   const QString& projectName,
                                   int gameId,
                                   const QString& author,
                                   quint64 sessionId);
    void fetchProjectGameId(int projectId, const QString& projectName, const QJsonObject& details, quint64 sessionId);
    void fetchProjectIcon(int projectId, const QJsonObject& details, quint64 sessionId);
    void handleProjectIconReply(QNetworkReply* reply, int projectId, const QJsonObject& details, quint64 sessionId);
    void finishCurrentProjectLoad(void);
    void updateListItem(const ProjectEntry& entry);
    int resolvedGameId(const ProjectEntry& entry) const;
    bool passesGameFilter(const ProjectEntry& entry) const;
    void refreshResultsList(void);
    void updateStatusLabel(void);
    void openProjectDetails(int projectId);
    QString projectListText(const ProjectEntry& entry) const;
    QIcon projectListIcon(const ProjectEntry& entry) const;

  private slots:
    void on_searchButton_clicked(void);
    void on_searchLineEdit_returnPressed(void);
    void on_gameFilterComboBox_currentIndexChanged(int index);
    void on_resultsListWidget_itemDoubleClicked(void);
    void on_romPatched(void);
};

} // namespace Dialog
} // namespace UserInterface

#endif // BOARDDOWNLOADERDIALOG_HPP
