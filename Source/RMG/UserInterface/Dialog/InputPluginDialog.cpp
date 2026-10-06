/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "InputPluginDialog.hpp"

#include "Utilities/PluginDisplayName.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/Plugins.hpp>
#include <RMG-Core/Settings.hpp>

#include <algorithm>

using namespace UserInterface::Dialog;

InputPluginDialog::InputPluginDialog(QWidget* parent) : QDialog(parent)
{
    this->setWindowTitle(QStringLiteral("Input"));
    this->setMinimumWidth(480);

    this->loadedFile = QFileInfo(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin))).fileName();

    auto* layout = new QVBoxLayout(this);

    auto* sourceLabel = new QLabel(QStringLiteral("Controller source"), this);
    this->pluginComboBox = new QComboBox(this);
    this->descriptionLabel = new QLabel(this);
    this->descriptionLabel->setWordWrap(true);

    layout->addWidget(sourceLabel);
    layout->addWidget(this->pluginComboBox);
    layout->addWidget(this->descriptionLabel);

    auto* buttons = new QDialogButtonBox(this);
    this->configureButton = buttons->addButton(QStringLiteral("Configure..."), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(this->pluginComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &InputPluginDialog::on_PluginComboBox_currentIndexChanged);
    connect(this->configureButton, &QPushButton::clicked, this, &InputPluginDialog::on_ConfigureButton_clicked);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    this->populatePlugins();
}

void InputPluginDialog::populatePlugins(void)
{
    struct PluginChoice
    {
        QString file;
        QString name;
    };

    QList<PluginChoice> choices;
    for (const CorePlugin& plugin : CoreGetAllPlugins())
    {
        if (plugin.Type != CorePluginType::Input)
        {
            continue;
        }

        const QString file = QString::fromStdString(plugin.File);
        choices.append({file, Utilities::InputPluginDisplayName(file, QString::fromStdString(plugin.Name))});
    }

    std::sort(choices.begin(), choices.end(), [](const PluginChoice& a, const PluginChoice& b)
    {
        const int rankA = Utilities::InputPluginSortRank(a.name);
        const int rankB = Utilities::InputPluginSortRank(b.name);
        if (rankA != rankB)
        {
            return rankA < rankB;
        }
        return a.name < b.name;
    });

    const QSignalBlocker blocker(this->pluginComboBox);
    this->pluginComboBox->clear();

    int currentIndex = -1;
    for (const PluginChoice& choice : choices)
    {
        this->pluginComboBox->addItem(choice.name, choice.file);
        if (choice.file == this->loadedFile)
        {
            currentIndex = this->pluginComboBox->count() - 1;
        }
    }

    if (currentIndex < 0 && !this->loadedFile.isEmpty())
    {
        const QString name = Utilities::InputPluginDisplayName(this->loadedFile, this->loadedFile) + QStringLiteral(" (not found)");
        this->pluginComboBox->addItem(name, this->loadedFile);
        currentIndex = this->pluginComboBox->count() - 1;
    }

    if (currentIndex >= 0)
    {
        this->pluginComboBox->setCurrentIndex(currentIndex);
    }

    this->updateDetails();
}

void InputPluginDialog::updateDetails(void)
{
    const QString name = this->pluginComboBox->currentText();
    const QString file = this->pluginComboBox->currentData().toString();
    const bool missing = name.endsWith(QStringLiteral("(not found)"));
    const QString lookupName = missing ? name.chopped(QStringLiteral(" (not found)").size()) : name;
    QString description = Utilities::InputPluginDescription(lookupName);
    const bool running = CoreIsEmulationRunning() || CoreIsEmulationPaused();
    const bool configurable = !missing && name != QStringLiteral("N64 Adapter") && !file.isEmpty();

    if (this->pluginComboBox->count() == 0)
    {
        description = QStringLiteral("No input plugins were found.");
    }
    else if (running && !file.isEmpty() && file != this->loadedFile)
    {
        const QString pending = QStringLiteral("This change is used the next time a game starts.");
        description = description.isEmpty() ? pending : description + QStringLiteral(" ") + pending;
    }

    this->descriptionLabel->setText(description);
    this->configureButton->setEnabled(configurable && (!running || file == this->loadedFile));
}

bool InputPluginDialog::applySelection(void)
{
    const QString file = this->pluginComboBox->currentData().toString();
    if (file.isEmpty())
    {
        return false;
    }

    const QString settingsFile = QFileInfo(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin))).fileName();
    if (file != settingsFile)
    {
        if (!CoreSettingsSetValue(SettingsID::Core_INPUT_Plugin, file.toStdString()) ||
            !CoreSettingsSave())
        {
            Utilities::QtMessageBox::Error(this, QStringLiteral("Failed to save the input plugin"));
            return false;
        }
    }

    const bool running = CoreIsEmulationRunning() || CoreIsEmulationPaused();
    if (running || file == this->loadedFile)
    {
        return true;
    }

    if (!CoreApplyPluginSettings())
    {
        Utilities::QtMessageBox::Error(this, QStringLiteral("Failed to load the input plugin"), QString::fromStdString(CoreGetError()));
        CoreSettingsSetValue(SettingsID::Core_INPUT_Plugin, this->loadedFile.toStdString());
        CoreSettingsSave();
        CoreApplyPluginSettings();

        const QSignalBlocker blocker(this->pluginComboBox);
        const int previousIndex = this->pluginComboBox->findData(this->loadedFile);
        if (previousIndex >= 0)
        {
            this->pluginComboBox->setCurrentIndex(previousIndex);
        }
        return false;
    }

    this->loadedFile = file;
    return true;
}

void InputPluginDialog::on_PluginComboBox_currentIndexChanged(int index)
{
    Q_UNUSED(index);
    this->applySelection();
    this->updateDetails();
}

void InputPluginDialog::on_ConfigureButton_clicked(void)
{
    if (!this->applySelection())
    {
        this->updateDetails();
        return;
    }

    if (!CorePluginsHasConfig(CorePluginType::Input))
    {
        this->updateDetails();
        return;
    }

    if (!CorePluginsOpenConfig(CorePluginType::Input, this))
    {
        Utilities::QtMessageBox::Error(this, QStringLiteral("Failed to open input settings"), QString::fromStdString(CoreGetError()));
    }
}
