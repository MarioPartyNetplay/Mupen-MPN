/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef MODIFICATIONSDIALOG_HPP
#define MODIFICATIONSDIALOG_HPP

#include <QDialog>
#include <QString>

#include <RMG-Core/Cheats.hpp>

#include "ui_ModificationsDialog.h"

namespace UserInterface
{
namespace Dialog
{
class ModificationsDialog : public QDialog, private Ui::ModificationsDialog
{
    Q_OBJECT

  public:
    explicit ModificationsDialog(QWidget* parent = nullptr);
    ~ModificationsDialog(void);

    bool HasFailed(void) const { return this->failedToLoad; }

  private:
    bool failedToLoad = false;
    bool loadingCodes = false;
    QString currentInternalName;

    void loadGames(void);
    void loadCodes(void);
    void resizeGamesListWidth(void);
    void applyDialogStyle(void);
    void updateCodeDetails(QListWidgetItem* item);
    void clearCodeDetails(void);
    QString formatCodeDisplayName(const CoreCheat& cheat) const;
    QString formatDescriptionText(const CoreCheat& cheat) const;
    CoreCheat currentCheat(QListWidgetItem* item) const;
    void openCheatOptions(QListWidgetItem* item);

  private slots:
    void on_gamesListWidget_currentRowChanged(int row);
    void on_codesListWidget_currentItemChanged(QListWidgetItem* current, QListWidgetItem* previous);
    void on_codesListWidget_itemChanged(QListWidgetItem* item);
    void on_codesListWidget_itemDoubleClicked(QListWidgetItem* item);
    void on_addCodeButton_clicked(void);
    void on_editCodeButton_clicked(void);
    void on_removeCodeButton_clicked(void);
    void accept(void) Q_DECL_OVERRIDE;
};
} // namespace Dialog
} // namespace UserInterface

#endif // MODIFICATIONSDIALOG_HPP
