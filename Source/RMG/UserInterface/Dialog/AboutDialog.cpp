/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "AboutDialog.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QTextEdit>

#include <RMG-Core/Core.hpp>
#include <RMG-Core/Version.hpp>

using namespace UserInterface::Dialog;

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
    this->setupUi(this);

    this->versionLabel->setText(QString::fromStdString(CoreGetVersion()));

    this->licenseText->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    this->licenseText->setLineWrapMode(QTextEdit::WidgetWidth);
    QFile licenseFile(QStringLiteral(":/LICENSE.txt"));
    if (licenseFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        this->licenseText->setPlainText(QString::fromUtf8(licenseFile.readAll()));
    }
}
