/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef BOARDDOWNLOADERDETAILDIALOG_HPP
#define BOARDDOWNLOADERDETAILDIALOG_HPP

#include "BoardDownloaderCommon.hpp"

#include <QDialog>
#include <QJsonObject>
#include <QPixmap>

#include "ui_BoardDownloaderDetailDialog.h"

namespace UserInterface
{
namespace Dialog
{

class BoardDownloaderDetailDialog : public QDialog, private Ui::BoardDownloaderDetailDialog
{
    Q_OBJECT

  public:
    BoardDownloaderDetailDialog(QWidget* parent, int projectId, const QJsonObject& details, const QPixmap& icon = QPixmap());
    ~BoardDownloaderDetailDialog(void) override = default;

  private:
    int projectId = 0;
    QJsonObject details;
    QPixmap iconPixmap;

    MarioPartyTarget targetGame(void) const;
    void populateDetails(void);
    bool downloadLatestBoardFile(QString& localPath, QString& remoteFileName);
    bool patchRom(const QString& boardFilePath, const QString& romFilePath, const QString& outputFilePath);

  private slots:
    void on_downloadButton_clicked(void);
    void on_patchButton_clicked(void);

  signals:
    void romPatched(void);
};

} // namespace Dialog
} // namespace UserInterface

#endif // BOARDDOWNLOADERDETAILDIALOG_HPP
