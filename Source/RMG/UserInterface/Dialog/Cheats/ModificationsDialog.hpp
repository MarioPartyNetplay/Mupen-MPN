#ifndef MODIFICATIONS_DIALOG_HPP
#define MODIFICATIONS_DIALOG_HPP

#include <QDialog>
#include <QTreeWidgetItem>
#include <filesystem>
#include <vector>
#include <string>

#include <RMG-Core/Core.hpp>

#include "ui_ModificationsDialog.h"

namespace UserInterface {
namespace Dialog {

class ModificationsDialog : public QDialog, private Ui::ModificationsDialog
{
    Q_OBJECT

public:
    explicit ModificationsDialog(QWidget *parent = nullptr);
    ~ModificationsDialog(void);

    bool HasFailed(void);

private:
    void loadCheats(void);
    QTreeWidgetItem* findItem(QStringList sections, int size, QString itemText);
    QString getTreeWidgetItemTextFromCheat(CoreCheat cheat);
    void showErrorMessage(QString error, QString details);
    void sortTreeWidgetItems(QTreeWidget* treeWidget);

private slots:
    //void onTabChanged(int index);
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

#endif // MODIFICATIONS_DIALOG_HPP