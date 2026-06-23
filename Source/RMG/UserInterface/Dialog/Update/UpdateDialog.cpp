/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "UpdateDialog.hpp"
#include "DownloadUpdateDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QNetworkAccessManager>
#include <QDesktopServices>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QPushButton>
#include <QJsonArray>
#include <QFileInfo>
#include <QProcess>
#include <QFile>

#include <RMG-Core/Settings.hpp>

namespace {

bool matchesCpuArchitecture(const QString& lowerFilename, const QString& osToken)
{
    const QString arch = QSysInfo::buildCpuArchitecture().toLower();

    if (lowerFilename.contains(osToken + "-" + arch)) {
        return true;
    }

    if (arch == QStringLiteral("x86_64")) {
        return lowerFilename.contains(osToken + QStringLiteral("64")) ||
               lowerFilename.contains(QStringLiteral("x86_64")) ||
               lowerFilename.contains(QStringLiteral("amd64"));
    }

    return false;
}

bool matchesWindowsUpdateAsset(const QString& filename, bool isWin32Setup)
{
    const QString lowerFilename = filename.toLower();

    if (!lowerFilename.contains(QStringLiteral("windows")) ||
        !matchesCpuArchitecture(lowerFilename, QStringLiteral("windows"))) {
        return false;
    }

    if (isWin32Setup) {
        return lowerFilename.contains(QStringLiteral("setup")) &&
               lowerFilename.endsWith(QStringLiteral(".exe"));
    }

    return lowerFilename.endsWith(QStringLiteral(".zip"));
}

bool matchesLinuxUpdateAsset(const QString& filename)
{
    const QString lowerFilename = filename.toLower();

    if (!lowerFilename.contains(QStringLiteral("linux")) ||
        !matchesCpuArchitecture(lowerFilename, QStringLiteral("linux"))) {
        return false;
    }

    return lowerFilename.endsWith(QStringLiteral(".appimage")) ||
           lowerFilename.endsWith(QStringLiteral(".zip"));
}

} // namespace

using namespace UserInterface::Dialog;
using namespace Utilities;

UpdateDialog::UpdateDialog(QWidget *parent, QJsonObject jsonObject, bool forced) : QDialog(parent)
{
    this->setupUi(this);

    this->jsonObject = jsonObject;

    this->label->setText(jsonObject.value("tag_name").toString() + " Available");
    this->textEdit->setText(jsonObject.value("body").toString());

    // change ok button text to 'Update'
    QPushButton* button = this->buttonBox->button(QDialogButtonBox::Ok);
    button->setText("Update");

    // don't show the 'Don't check for updates again' checkbox,
    // when the user requested we check for updates
    this->disableUpdateCheckCheckBox->setHidden(forced);
}

UpdateDialog::~UpdateDialog(void)
{
}

void UpdateDialog::on_disableUpdateCheckCheckBox_stateChanged(int state)
{
    CoreSettingsSetValue(SettingsID::GUI_CheckForUpdates, (state == Qt::Unchecked));
    CoreSettingsSave();
}

QString UpdateDialog::GetFileName(void)
{
    return this->filename;
}

QUrl UpdateDialog::GetUrl(void)
{
    return this->url;
}

void UpdateDialog::accept(void)
{
    QJsonArray jsonArray = jsonObject["assets"].toArray();
    QString filenameToDownload;
    QUrl urlToDownload;

    for (const QJsonValue& value : jsonArray)
    {
        QJsonObject object = value.toObject();

        QString filename = object.value("name").toString();
        QString url      = object.value("browser_download_url").toString();

#ifdef _WIN32
        const bool isWin32Setup =
            QFile::exists(QStringLiteral("unins000.exe")) &&
            QFile::exists(QStringLiteral("unins000.dat"));

        if (matchesWindowsUpdateAsset(filename, isWin32Setup))
        {
            filenameToDownload = filename;
            urlToDownload = QUrl(url);
            break;
        }
#else
        if (matchesLinuxUpdateAsset(filename))
        {
            filenameToDownload = filename;
            urlToDownload = QUrl(url);
            break;
        }
#endif // _WIN32
    }

    if (filenameToDownload.isEmpty())
    {
        QtMessageBox::Error(this, "Failed to find update file");
        QDialog::reject();
        return;
    }

    this->url = urlToDownload;
    this->filename = filenameToDownload;
    QDialog::accept();
}
