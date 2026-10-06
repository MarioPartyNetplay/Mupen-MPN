/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef INPUTPLUGINDIALOG_HPP
#define INPUTPLUGINDIALOG_HPP

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;

namespace UserInterface
{
namespace Dialog
{
class InputPluginDialog : public QDialog
{
    Q_OBJECT

  public:
    InputPluginDialog(QWidget* parent);

  private:
    QComboBox* pluginComboBox = nullptr;
    QLabel* descriptionLabel = nullptr;
    QPushButton* configureButton = nullptr;
    QString loadedFile;

    void populatePlugins(void);
    void updateDetails(void);
    bool applySelection(void);

  private slots:
    void on_PluginComboBox_currentIndexChanged(int index);
    void on_ConfigureButton_clicked(void);
};
} // namespace Dialog
} // namespace UserInterface

#endif // INPUTPLUGINDIALOG_HPP
