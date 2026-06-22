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

  private:
    QNetworkAccessManager* networkManager = nullptr;
    QNetworkReply* searchReply = nullptr;

    void clearResults(void);
    void setSearching(bool searching);
    void fetchProjectDetails(int projectId, const QString& projectName, int gameId);
    void addProjectCard(int projectId, const QString& projectName, const QJsonObject& details, const QPixmap& icon);
    void handleProjectDetailsReply(QNetworkReply* reply, int projectId, const QString& projectName, int gameId);
    void fetchProjectIcon(int projectId, const QJsonObject& details);

  private slots:
    void on_searchButton_clicked(void);
    void on_searchLineEdit_returnPressed(void);
    void on_searchReply_finished(void);
};

} // namespace Dialog
} // namespace UserInterface

#endif // BOARDDOWNLOADERDIALOG_HPP
