/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CHEATS_DIALOG_HPP
#define CHEATS_DIALOG_HPP

#include <QDialog>
#include <QTreeWidgetItem>

#include <RMG-Core/Core.hpp>

#include "ui_CheatsDialog.h"

namespace UserInterface {
namespace Dialog {

class CheatsDialog : public QDialog, private Ui::CheatsDialog
{
    Q_OBJECT

public:
    explicit CheatsDialog(QWidget *parent = nullptr);
    ~CheatsDialog(void);

    bool HasFailed(void);

private:
    void loadCheats(void);
    QTreeWidgetItem* findItem(QStringList sections, int size, QString itemText);
    QString getTreeWidgetItemTextFromCheat(CoreCheat cheat);
    void showErrorMessage(QString error, QString details);
    void sortTreeWidgetItems(QTreeWidget* treeWidget);

private slots:
    void on_cheatsTreeWidget_itemChanged(QTreeWidgetItem *item, int column);
    void on_cheatsTreeWidget_currentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void on_cheatsTreeWidget_itemDoubleClicked(QTreeWidgetItem *item, int column);
    void on_addCheatButton_clicked(void);
    void on_editCheatButton_clicked(void);
    void on_removeCheatButton_clicked(void);
    void accept(void);

private:
    bool failedToParseCheats = false;
};

} // namespace Dialog
} // namespace UserInterface

#endif // CHEATS_DIALOG_HPP