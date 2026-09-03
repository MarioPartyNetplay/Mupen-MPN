/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ModificationsDialog.hpp"

#include "UserInterface/Dialog/Cheats/AddCheatDialog.hpp"
#include "UserInterface/Dialog/Cheats/ChooseCheatOptionDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QFontMetrics>
#include <QIcon>
#include <QListWidgetItem>
#include <QPalette>
#include <QSignalBlocker>
#include <QSplitter>

#include <RMG-Core/Rom.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/Settings.hpp>

Q_DECLARE_METATYPE(CoreCheat);

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace {

constexpr int kCheatRole = Qt::UserRole;

QString stripDisplayPrefix(const QString& name)
{
    if (name.startsWith("aQOL"))
    {
        return name.mid(1);
    }

    return name;
}

} // namespace

ModificationsDialog::ModificationsDialog(QWidget* parent)
    : QDialog(parent)
{
    qRegisterMetaType<CoreCheat>();

    this->setupUi(this);
    this->splitter->setStretchFactor(0, 0);
    this->splitter->setStretchFactor(1, 1);
    this->splitter->setHandleWidth(1);
    this->setWindowIcon(QIcon::fromTheme("modifications", QIcon(":Resource/RMG.png")));
    this->addCodeButton->setEnabled(false);
    this->loadGames();
}

ModificationsDialog::~ModificationsDialog(void)
{
}

void ModificationsDialog::loadGames(void)
{
    std::vector<CoreModificationGame> games;
    if (!CoreGetModificationGames(games))
    {
        QtMessageBox::Error(this, "CoreGetModificationGames() Failed", QString::fromStdString(CoreGetError()));
        this->failedToLoad = true;
        return;
    }

    this->gamesListWidget->clear();

    for (const CoreModificationGame& game : games)
    {
        QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(game.displayName));
        item->setData(kCheatRole, QString::fromStdString(game.internalName));
        this->gamesListWidget->addItem(item);
    }

    if (this->gamesListWidget->count() > 0)
    {
        this->gamesListWidget->setCurrentRow(0);
    }

    this->resizeGamesListWidth();
}

void ModificationsDialog::resizeGamesListWidth(void)
{
    const QFontMetrics metrics(this->gamesListWidget->font());
    int contentWidth = 0;

    for (int row = 0; row < this->gamesListWidget->count(); ++row)
    {
        const QListWidgetItem* item = this->gamesListWidget->item(row);
        if (item == nullptr)
        {
            continue;
        }

        contentWidth = qMax(contentWidth, metrics.horizontalAdvance(item->text()));
    }

    const int framePadding = 24;
    const int gamesListWidth = contentWidth + framePadding;
    this->gamesListWidget->setFixedWidth(gamesListWidth);

    QList<int> splitterSizes = this->splitter->sizes();
    if (splitterSizes.size() == 2)
    {
        const int totalWidth = qMax(splitterSizes.at(0) + splitterSizes.at(1), this->width());
        this->splitter->setSizes({gamesListWidth, totalWidth - gamesListWidth});
    }
}


void ModificationsDialog::loadCodes(void)
{
    if (this->currentInternalName.isEmpty())
    {
        return;
    }

    std::vector<CoreCheat> cheats;
    if (!CoreGetModificationCheats(this->currentInternalName.toStdString(), cheats))
    {
        QtMessageBox::Error(this, "CoreGetModificationCheats() Failed", QString::fromStdString(CoreGetError()));
        this->failedToLoad = true;
        return;
    }

    this->loadingCodes = true;
    {
        QSignalBlocker blocker(this->codesListWidget);
        this->codesListWidget->clear();

        for (const CoreCheat& cheat : cheats)
        {
            QListWidgetItem* item = new QListWidgetItem(this->formatCodeDisplayName(cheat));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(CoreIsModificationCheatEnabled(this->currentInternalName.toStdString(), cheat)
                                    ? Qt::Checked
                                    : Qt::Unchecked);
            item->setData(kCheatRole, QVariant::fromValue(cheat));
            this->codesListWidget->addItem(item);
        }
    }
    this->loadingCodes = false;

    this->clearCodeDetails();
    this->addCodeButton->setEnabled(true);
    this->editCodeButton->setEnabled(false);
    this->removeCodeButton->setEnabled(false);
}

QString ModificationsDialog::formatCodeDisplayName(const CoreCheat& cheat) const
{
    QString displayName = stripDisplayPrefix(QString::fromStdString(cheat.Name));

    if (!cheat.HasOptions)
    {
        return displayName;
    }

    CoreCheatOption cheatOption;
    if (!CoreHasModificationCheatOptionSet(this->currentInternalName.toStdString(), cheat) ||
        !CoreGetModificationCheatOption(this->currentInternalName.toStdString(), cheat, cheatOption))
    {
        return displayName + " (=> ???? - Not Set)";
    }

    return displayName + " (=> " + QString::fromStdString(cheatOption.Name) + ")";
}

QString ModificationsDialog::formatDescriptionText(const CoreCheat& cheat) const
{
    return QString::fromStdString(cheat.Note).trimmed();
}

CoreCheat ModificationsDialog::currentCheat(QListWidgetItem* item) const
{
    if (item == nullptr || !item->data(kCheatRole).isValid())
    {
        return {};
    }

    return item->data(kCheatRole).value<CoreCheat>();
}

void ModificationsDialog::updateCodeDetails(QListWidgetItem* item)
{
    const CoreCheat cheat = this->currentCheat(item);
    if (cheat.Name.empty())
    {
        this->clearCodeDetails();
        this->editCodeButton->setEnabled(false);
        this->removeCodeButton->setEnabled(false);
        return;
    }

    this->nameLineEdit->setText(stripDisplayPrefix(QString::fromStdString(cheat.Name)));
    this->descriptionTextEdit->setPlainText(this->formatDescriptionText(cheat));

    std::vector<std::string> codeLines;
    std::vector<std::string> optionLines;
    if (CoreGetCheatLines(cheat, codeLines, optionLines))
    {
        QStringList lines;
        for (const std::string& line : codeLines)
        {
            lines.append(QString::fromStdString(line));
        }
        this->codeTextEdit->setPlainText(lines.join('\n'));
    }
    else
    {
        this->codeTextEdit->clear();
    }

    this->editCodeButton->setEnabled(true);
    this->removeCodeButton->setEnabled(
        CoreCanRemoveModificationCheat(this->currentInternalName.toStdString(), cheat));
}

void ModificationsDialog::clearCodeDetails(void)
{
    this->nameLineEdit->clear();
    this->descriptionTextEdit->clear();
    this->codeTextEdit->clear();
}

void ModificationsDialog::openCheatOptions(QListWidgetItem* item)
{
    const CoreCheat cheat = this->currentCheat(item);
    if (cheat.Name.empty() || !cheat.HasOptions)
    {
        return;
    }

    ChooseCheatOptionDialog dialog(this, QString(), cheat, false, {}, this->currentInternalName);
    dialog.exec();

    item->setText(this->formatCodeDisplayName(cheat));
    this->updateCodeDetails(item);
}

void ModificationsDialog::on_gamesListWidget_currentRowChanged(int row)
{
    QListWidgetItem* item = this->gamesListWidget->item(row);
    if (item == nullptr)
    {
        this->currentInternalName.clear();
        this->codesListWidget->clear();
        this->clearCodeDetails();
        this->addCodeButton->setEnabled(false);
        this->editCodeButton->setEnabled(false);
        this->removeCodeButton->setEnabled(false);
        return;
    }

    this->currentInternalName = item->data(kCheatRole).toString();
    this->loadCodes();
}

void ModificationsDialog::on_codesListWidget_currentItemChanged(QListWidgetItem* current, QListWidgetItem* previous)
{
    Q_UNUSED(previous);
    this->updateCodeDetails(current);
}

void ModificationsDialog::on_codesListWidget_itemChanged(QListWidgetItem* item)
{
    if (this->loadingCodes || item == nullptr)
    {
        return;
    }

    const CoreCheat cheat = this->currentCheat(item);
    if (cheat.Name.empty())
    {
        return;
    }

    const bool enabled = item->checkState() == Qt::Checked;
    if (enabled && cheat.HasOptions &&
        !CoreHasModificationCheatOptionSet(this->currentInternalName.toStdString(), cheat))
    {
        this->openCheatOptions(item);

        if (!CoreHasModificationCheatOptionSet(this->currentInternalName.toStdString(), cheat))
        {
            QSignalBlocker blocker(this->codesListWidget);
            item->setCheckState(Qt::Unchecked);
            return;
        }
    }

    if (!CoreEnableModificationCheat(this->currentInternalName.toStdString(), cheat, enabled))
    {
        QtMessageBox::Error(this, "CoreEnableModificationCheat() Failed", QString::fromStdString(CoreGetError()));
        QSignalBlocker blocker(this->codesListWidget);
        item->setCheckState(enabled ? Qt::Unchecked : Qt::Checked);
        return;
    }

    item->setText(this->formatCodeDisplayName(cheat));
}

void ModificationsDialog::on_codesListWidget_itemDoubleClicked(QListWidgetItem* item)
{
    this->openCheatOptions(item);
}

void ModificationsDialog::on_addCodeButton_clicked(void)
{
    if (this->currentInternalName.isEmpty())
    {
        return;
    }

    AddCheatDialog dialog(this, QString());
    dialog.setModificationInternalName(this->currentInternalName);
    if (dialog.exec() == QDialog::Accepted)
    {
        this->loadCodes();
    }
}

void ModificationsDialog::on_editCodeButton_clicked(void)
{
    QListWidgetItem* item = this->codesListWidget->currentItem();
    const CoreCheat cheat = this->currentCheat(item);
    if (cheat.Name.empty())
    {
        return;
    }

    AddCheatDialog dialog(this, QString());
    dialog.setModificationInternalName(this->currentInternalName);
    dialog.SetCheat(cheat);
    if (dialog.exec() == QDialog::Accepted)
    {
        this->loadCodes();
    }
}

void ModificationsDialog::on_removeCodeButton_clicked(void)
{
    QListWidgetItem* item = this->codesListWidget->currentItem();
    const CoreCheat cheat = this->currentCheat(item);
    if (cheat.Name.empty())
    {
        return;
    }

    if (!CoreRemoveModificationCheat(this->currentInternalName.toStdString(), cheat))
    {
        QtMessageBox::Error(this, "CoreRemoveModificationCheat() Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    this->loadCodes();
}

void ModificationsDialog::accept(void)
{
    CoreSettingsSave();

    if (CoreHasRomOpen() && !CoreApplyCheats())
    {
        QtMessageBox::Error(this, "CoreApplyCheats() Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    QDialog::accept();
}
