/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "InstallUpdateDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QTextStream>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#ifdef __APPLE__
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#endif

#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Archive.hpp>
#include <RMG-Core/Error.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

#ifdef __APPLE__
namespace {

QString shellSingleQuote(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

bool isMupenApplicationBundle(const QString& path)
{
    return QFileInfo::exists(QDir(path).filePath(QStringLiteral("Contents/MacOS/Mupen-MPN")));
}

QString macOSBundlePath(QString* errorDetail)
{
    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.cdUp() || dir.dirName() != QStringLiteral("Contents") || !dir.cdUp())
    {
        *errorDetail = QStringLiteral("Mupen-MPN is not running from an application bundle, so it can't install a disk image update.");
        return QString();
    }

    const QFileInfo bundleInfo(dir.absolutePath());
    QString bundlePath = bundleInfo.canonicalFilePath();
    if (bundlePath.isEmpty())
    {
        bundlePath = bundleInfo.absoluteFilePath();
    }
    bundlePath = QDir::cleanPath(bundlePath);

    if (!bundlePath.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive) || !isMupenApplicationBundle(bundlePath))
    {
        *errorDetail = QStringLiteral("Mupen-MPN is not running from an application bundle, so it can't install a disk image update.");
        return QString();
    }
    if (bundlePath.contains(QStringLiteral("AppTranslocation")))
    {
        *errorDetail = QStringLiteral("macOS opened Mupen-MPN from a temporary location. Move Mupen-MPN.app to the Applications folder, open that copy, and check for updates again.");
        return QString();
    }
    if (bundlePath.startsWith(QStringLiteral("/Volumes/")))
    {
        *errorDetail = QStringLiteral("Mupen-MPN is running from a disk image. Copy Mupen-MPN.app to the Applications folder, open that copy, and check for updates again.");
        return QString();
    }

    return bundlePath;
}

struct ProcessResult
{
    bool finished = false;
    int exitCode = -1;
    QString output;
};

ProcessResult runProcess(const QString& program, const QStringList& arguments, int timeoutMs)
{
    ProcessResult result;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&process, &QProcess::finished, &loop, [&loop](int, QProcess::ExitStatus) {
        loop.quit();
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    process.start(program, arguments);
    if (!process.waitForStarted(15000))
    {
        result.output = QStringLiteral("Could not start ") + program;
        return result;
    }

    timer.start(timeoutMs);
    loop.exec();
    if (process.state() != QProcess::NotRunning)
    {
        process.kill();
        process.waitForFinished(5000);
        result.output = QStringLiteral("Timed out running ") + program;
        return result;
    }

    result.finished = true;
    result.exitCode = process.exitCode();
    result.output = QString::fromUtf8(process.readAll()).trimmed();
    return result;
}

void detachDiskImage(const QString& mountPoint)
{
    runProcess(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("detach"), mountPoint}, 20000);
    runProcess(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("detach"), QStringLiteral("-force"), mountPoint}, 20000);
}

QString findMupenBundle(const QString& root)
{
    if (isMupenApplicationBundle(root))
    {
        return root;
    }

    const QString named = QDir(root).filePath(QStringLiteral("Mupen-MPN.app"));
    if (isMupenApplicationBundle(named))
    {
        return named;
    }

    const QDir dir(root);
    const QStringList apps = dir.entryList({QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : apps)
    {
        const QString candidate = dir.filePath(name);
        if (isMupenApplicationBundle(candidate))
        {
            return candidate;
        }
    }

    const QStringList folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : folders)
    {
        const QDir nested(dir.filePath(name));
        const QString nestedNamed = nested.filePath(QStringLiteral("Mupen-MPN.app"));
        if (isMupenApplicationBundle(nestedNamed))
        {
            return nestedNamed;
        }
        const QStringList nestedApps = nested.entryList({QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& appName : nestedApps)
        {
            const QString candidate = nested.filePath(appName);
            if (isMupenApplicationBundle(candidate))
            {
                return candidate;
            }
        }
    }

    return QString();
}

} // namespace
#endif // __APPLE__

InstallUpdateDialog::InstallUpdateDialog(QWidget *parent, QString installationDirectory, QString temporaryDirectory, QString filename) : QDialog(parent)
{
    this->setupUi(this);

    this->installationDirectory = installationDirectory;
    this->temporaryDirectory = temporaryDirectory;
    this->filename = filename;
    this->startTimer(100);
}

InstallUpdateDialog::~InstallUpdateDialog(void)
{
}

void InstallUpdateDialog::install(void)
{
    QString fullFilePath;
    fullFilePath = this->temporaryDirectory;
    fullFilePath += "/" + this->filename;   

    QString appPath = QCoreApplication::applicationDirPath();
    QString appPid  = QString::number(QCoreApplication::applicationPid());
    QString logPath = QString::fromStdU32String(CoreGetUserCacheDirectory().u32string()) + "/updater.log";


    // convert paths to use the right path seperator
    this->temporaryDirectory = QDir::toNativeSeparators(this->temporaryDirectory);
    fullFilePath             = QDir::toNativeSeparators(fullFilePath);
    appPath                  = QDir::toNativeSeparators(appPath);
    logPath                  = QDir::toNativeSeparators(logPath);

    // remove log file when it exists
    QFile qLogFile(logPath);
    if (qLogFile.exists())
    {
        qLogFile.remove();
    }

    QString outputToLogLine = " >> \"" + logPath + "\" 2>&1";

#ifndef _WIN32
#ifdef __APPLE__
    if (this->filename.endsWith(QStringLiteral(".dmg"), Qt::CaseInsensitive))
    {
        if (!this->installMacOSDiskImage(fullFilePath, appPid, logPath))
        {
            this->reject();
            return;
        }
        this->accept();
        return;
    }
#endif

    if (this->filename.endsWith(QStringLiteral(".flatpak"), Qt::CaseInsensitive))
    {
        this->label->setText(QStringLiteral("Installing ") + this->filename + QStringLiteral("..."));
        this->progressBar->setValue(50);

        QStringList scriptLines =
        {
            QStringLiteral("#!/bin/sh"),
            QStringLiteral("("),
            QStringLiteral("   echo == Installing flatpak bundle") + outputToLogLine,
            QStringLiteral("   flatpak install --user -y --reinstall \"") + fullFilePath + QStringLiteral("\"") + outputToLogLine,
            QStringLiteral("   echo == Attempting to kill PID ") + appPid + outputToLogLine,
            QStringLiteral("   kill ") + appPid + outputToLogLine,
            QStringLiteral("   echo == Launching updated application") + outputToLogLine,
            QStringLiteral("   flatpak run org.mariopartynetplay.RMG-MPN &") + outputToLogLine,
            QStringLiteral(")"),
            QStringLiteral("rm -rf \"") + this->temporaryDirectory + QStringLiteral("\""),
        };
        this->writeAndRunScript(scriptLines);
        this->accept();
        return;
    }

    if (this->filename.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
    {
        this->label->setText(QStringLiteral("Extracting ") + this->filename + QStringLiteral("..."));
        this->progressBar->setValue(50);

        QDir dir(this->temporaryDirectory);
        if (!dir.mkdir(QStringLiteral("extract")))
        {
            QtMessageBox::Error(this, QStringLiteral("QDir::mkdir() Failed"), QString());
            this->reject();
            return;
        }

        QString extractDirectory = this->temporaryDirectory + QStringLiteral("/extract");
        if (!CoreUnzip(fullFilePath.toStdU32String(), extractDirectory.toStdU32String()))
        {
            QtMessageBox::Error(this, QStringLiteral("CoreUnzip() Failed"), QString::fromStdString(CoreGetError()));
            this->reject();
            return;
        }

        this->label->setText(QStringLiteral("Executing update script..."));
        this->progressBar->setValue(100);

        extractDirectory = QDir::toNativeSeparators(extractDirectory);
        const QString appBinary = appPath + QStringLiteral("/Mupen-MPN");

        QStringList scriptLines =
        {
            QStringLiteral("#!/bin/sh"),
            QStringLiteral("("),
            QStringLiteral("   echo == Attempting to remove '") + fullFilePath + QStringLiteral("'") + outputToLogLine,
            QStringLiteral("   rm -f \"") + fullFilePath + QStringLiteral("\"") + outputToLogLine,
            QStringLiteral("   echo == Attempting to kill PID ") + appPid + outputToLogLine,
            QStringLiteral("   kill ") + appPid + outputToLogLine,
            QStringLiteral("   echo == Attempting to copy '") + extractDirectory + QStringLiteral("' to '") + appPath + QStringLiteral("'") + outputToLogLine,
            QStringLiteral("   cp -a \"") + extractDirectory + QStringLiteral("/.\" \"") + appPath + QStringLiteral("/\"") + outputToLogLine,
            QStringLiteral("   echo == Attempting to start '") + appBinary + QStringLiteral("'") + outputToLogLine,
            QStringLiteral("   \"") + appBinary + QStringLiteral("\" &") + outputToLogLine,
            QStringLiteral(")"),
            QStringLiteral("rm -rf \"") + this->temporaryDirectory + QStringLiteral("\""),
        };
        this->writeAndRunScript(scriptLines);
        this->accept();
        return;
    }
#endif // _WIN32

    if (this->filename.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
    {
        this->label->setText("Executing " + this->filename + "...");
        QStringList scriptLines =
        {
            "@echo off",
            "(",
            "   echo == Attemping to kill PID " + appPid + outputToLogLine,
            "   taskkill /F /PID:"              + appPid + outputToLogLine,
            "   echo == Attemping to start \'" + fullFilePath + "\'"                                    + outputToLogLine,
            "   \"" + fullFilePath + "\" /CLOSEAPPLICATIONS /NOCANCEL /MERGETASKS=\"!desktopicon\"  /SILENT /DIR=\"" + appPath + "\"" + outputToLogLine,
            ")",
            "IF NOT ERRORLEVEL 0 (",
            "   start \"\" cmd /c \"echo Mupen MPN failed to update, check the updater.log file in the user cache directory for more information && pause\"",
            ")",
            // remove temporary directory at last
            "rmdir /S /Q \"" + this->temporaryDirectory + "\"",
        };
        this->writeAndRunScript(scriptLines);
        this->accept();
        return;
    }

    this->label->setText("Extracting " + this->filename + "...");
    this->progressBar->setValue(50);

    QDir dir(this->temporaryDirectory);
    if (!dir.mkdir("extract"))
    {
        QtMessageBox::Error(this, "QDir::mkdir() Failed", "");
        this->reject();
        return;
    }

    QString extractDirectory;
    extractDirectory = this->temporaryDirectory;
    extractDirectory += "/extract";

    if (!CoreUnzip(fullFilePath.toStdU32String(), extractDirectory.toStdU32String()))
    {
        QtMessageBox::Error(this, "CoreUnzip() Failed", QString::fromStdString(CoreGetError()));
        this->reject();
        return;
    }

    this->label->setText("Executing update script...");
    this->progressBar->setValue(100);

    extractDirectory = QDir::toNativeSeparators(extractDirectory);

    QStringList scriptLines = 
    {
        "@echo off",
        "(",
        "   echo == Attempting to remove \'" + fullFilePath + "\'" + outputToLogLine,
        "   del /F /Q \""                    + fullFilePath + "\"" + outputToLogLine,
        "   echo == Attemping to kill PID " + appPid               + outputToLogLine,
        "   taskkill /F /PID:"              + appPid               + outputToLogLine,
        "   echo == Attemping to copy \'" + extractDirectory + "\' to \'" + appPath + "\'"  + outputToLogLine,
        "   xcopy /S /Y /I \""            + extractDirectory + "\\*\" \"" + appPath + "\""  + outputToLogLine,
        "   echo == Attemping to start \'" + appPath + "\\Mupen-MPN.exe\'"           + outputToLogLine,
        "   start \"\" \""                 + appPath + "\\Mupen-MPN.exe\""           + outputToLogLine,
        ")",
        "IF NOT ERRORLEVEL 0 (",
        "   start \"\" cmd /c \"echo Mupen MPN failed to update, check the updater.log file in the user cache directory for more information && pause\"",
        ")",
        // remove temporary directory at last
        "rmdir /S /Q \"" + this->temporaryDirectory + "\"",
    };
    this->writeAndRunScript(scriptLines);
}

#ifdef __APPLE__
bool InstallUpdateDialog::installMacOSDiskImage(const QString& diskImagePath, const QString& appPid, const QString& logPath)
{
    QString errorDetail;
    const QString bundlePath = macOSBundlePath(&errorDetail);
    if (bundlePath.isEmpty())
    {
        QtMessageBox::Error(this, QStringLiteral("Can't install this update"), errorDetail);
        return false;
    }

    this->label->setText(QStringLiteral("Mounting ") + this->filename + QStringLiteral("..."));
    this->progressBar->setRange(0, 0);
    QCoreApplication::processEvents();

    const QString mountPoint = QDir(this->temporaryDirectory).filePath(QStringLiteral("mount"));
    QDir mountParent(this->temporaryDirectory);
    if (!mountParent.mkpath(QStringLiteral("mount")))
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to mount the disk image"), QStringLiteral("Couldn't create a mount directory."));
        return false;
    }

    QString attachError;
    bool attached = false;
    for (int attempt = 1; attempt <= 4; ++attempt)
    {
        const ProcessResult result = runProcess(
            QStringLiteral("/usr/bin/hdiutil"),
            {QStringLiteral("attach"), QStringLiteral("-nobrowse"), QStringLiteral("-readonly"),
             QStringLiteral("-mountpoint"), mountPoint, diskImagePath},
            60000);
        if (result.finished && result.exitCode == 0)
        {
            attached = true;
            break;
        }
        attachError = result.output;
        detachDiskImage(mountPoint);
        if (attempt < 4)
        {
            QEventLoop pause;
            QTimer::singleShot(attempt * 1000, &pause, &QEventLoop::quit);
            pause.exec();
        }
    }

    if (!attached)
    {
        QDir(mountPoint).removeRecursively();
        if (attachError.isEmpty())
        {
            attachError = QStringLiteral("hdiutil attach failed.");
        }
        QtMessageBox::Error(this, QStringLiteral("Failed to mount the disk image"), attachError);
        return false;
    }

    const QString sourceApp = findMupenBundle(mountPoint);
    if (sourceApp.isEmpty())
    {
        detachDiskImage(mountPoint);
        QDir(mountPoint).removeRecursively();
        QtMessageBox::Error(this, QStringLiteral("Failed to read the disk image"),
                            QStringLiteral("The disk image does not contain Mupen-MPN.app."));
        return false;
    }

    this->label->setText(QStringLiteral("Installing update..."));
    this->progressBar->setRange(0, 100);
    this->progressBar->setValue(100);

    const QString stashPath = QDir(this->temporaryDirectory).filePath(QStringLiteral("user-data"));
    const QString script =
        QStringLiteral("#!/bin/sh\n") +
        QStringLiteral("bundle=") + shellSingleQuote(bundlePath) + QLatin1Char('\n') +
        QStringLiteral("src=") + shellSingleQuote(sourceApp) + QLatin1Char('\n') +
        QStringLiteral("pid=") + shellSingleQuote(appPid) + QLatin1Char('\n') +
        QStringLiteral("stash=") + shellSingleQuote(stashPath) + QLatin1Char('\n') +
        QStringLiteral("mount=") + shellSingleQuote(mountPoint) + QLatin1Char('\n') +
        QStringLiteral("log=") + shellSingleQuote(logPath) + QLatin1Char('\n') +
        QStringLiteral("work=") + shellSingleQuote(this->temporaryDirectory) + QLatin1Char('\n') +
        QStringLiteral(R"sh(
cd /tmp || cd /

replace_bundle() {
    owner=$(/usr/bin/stat -f '%u:%g' "$bundle" 2>/dev/null || true)
    previous="$bundle.previous"
    /bin/rm -rf "$previous"
    echo "== Moving $bundle aside"
    if ! /bin/mv "$bundle" "$previous"; then
        echo "error: could not move the existing application"
        return 1
    fi
    echo "== Copying $src to $bundle"
    if ! /usr/bin/ditto "$src" "$bundle"; then
        echo "error: could not copy the new application"
        /bin/rm -rf "$bundle"
        /bin/mv "$previous" "$bundle"
        return 1
    fi

    new_macos="$bundle/Contents/MacOS"
    /bin/mkdir -p "$new_macos"
    for name in Config Save Cache Screenshots instances; do
        if [ -d "$stash/$name" ]; then
            echo "== Restoring $name"
            /bin/rm -rf "$new_macos/$name"
            /usr/bin/ditto "$stash/$name" "$new_macos/$name"
        fi
    done
    for name in portable.txt .env; do
        if [ -f "$stash/$name" ]; then
            echo "== Restoring $name"
            /bin/cp -p "$stash/$name" "$new_macos/$name"
        fi
    done
    if [ -d "$stash/Data" ]; then
        echo "== Restoring user files in Data"
        /bin/mkdir -p "$new_macos/Data"
        if [ -x /usr/bin/rsync ]; then
            /usr/bin/rsync -a --ignore-existing "$stash/Data/" "$new_macos/Data/" || echo "warning: could not restore every user file in Data"
        fi
    fi

    /usr/bin/xattr -dr com.apple.quarantine "$bundle" >/dev/null 2>&1 || true
    if [ -n "$owner" ]; then
        /usr/sbin/chown -R "$owner" "$bundle" >/dev/null 2>&1 || true
    fi
    /bin/rm -rf "$previous"
    return 0
}

stash_user_files() {
    macos="$bundle/Contents/MacOS"
    /bin/mkdir -p "$stash"
    if [ ! -d "$macos" ]; then
        return 0
    fi
    for name in Config Save Cache Screenshots instances; do
        if [ -d "$macos/$name" ]; then
            echo "== Stashing $name"
            /usr/bin/ditto "$macos/$name" "$stash/$name"
        fi
    done
    for name in portable.txt .env; do
        if [ -f "$macos/$name" ]; then
            echo "== Stashing $name"
            /bin/cp -p "$macos/$name" "$stash/$name"
        fi
    done
    if [ -d "$macos/Data" ]; then
        echo "== Stashing Data"
        /usr/bin/ditto "$macos/Data" "$stash/Data"
    fi
}

notify_failure() {
    /usr/bin/osascript -e 'display alert "Mupen MPN failed to update" message "Check the updater.log file in the user cache directory for more information."' >/dev/null 2>&1 || true
}

if [ "${1:-}" = "--as-root" ]; then
    replace_bundle >> "$log" 2>&1
    exit $?
fi

(
    exec >> "$log" 2>&1
    echo "== Waiting for Mupen-MPN to quit"
    i=0
    while /bin/kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -eq 45 ]; then
            echo "== Still running, sending TERM"
            /bin/kill "$pid" 2>/dev/null || true
        fi
        if [ "$i" -ge 60 ]; then
            echo "== Still running, sending KILL"
            /bin/kill -9 "$pid" 2>/dev/null || true
            /bin/sleep 1
            break
        fi
        /bin/sleep 1
    done

    if [ ! -d "$src" ] || [ ! -f "$src/Contents/MacOS/Mupen-MPN" ]; then
        echo "error: update image does not contain Mupen-MPN.app"
        notify_failure
        /usr/bin/open "$bundle" >/dev/null 2>&1 || true
        exit 1
    fi

    stash_user_files

    echo "== Installing update into $bundle"
    status=0
    if [ -w "$(/usr/bin/dirname "$bundle")" ]; then
        replace_bundle
        status=$?
    else
        echo "== Requesting administrator privileges"
        /usr/bin/osascript -e "do shell script \"/bin/sh \\\"$0\\\" --as-root\" with administrator privileges"
        status=$?
    fi

    if [ "$status" -ne 0 ]; then
        echo "error: update failed with status $status"
        notify_failure
        /usr/bin/open "$bundle" >/dev/null 2>&1 || true
        exit 1
    fi

    echo "== Launching $bundle"
    /usr/bin/open "$bundle"
)
if /usr/bin/hdiutil detach "$mount" >> "$log" 2>&1 || /usr/bin/hdiutil detach -force "$mount" >> "$log" 2>&1; then
    /bin/rm -rf "$work"
else
    echo "warning: disk image stayed mounted at $mount" >> "$log"
fi
)sh");

    if (!this->writeAndRunScript(script.split(QLatin1Char('\n'))))
    {
        detachDiskImage(mountPoint);
        return false;
    }
    return true;
}
#endif // __APPLE__

bool InstallUpdateDialog::writeAndRunScript(QStringList stringList)
{
    QString scriptPath;
    scriptPath = this->temporaryDirectory;
#ifdef _WIN32
    scriptPath += "/update.bat";
#else
    scriptPath += "/update.sh";
#endif

    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QtMessageBox::Error(this, "QFile::open() Failed", "");
        return false;
    }

    QTextStream textStream(&scriptFile);

    // write script to file
    for (const QString& str : stringList)
    {
        textStream << str << "\n";
    }

    scriptFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    scriptFile.close();

#ifdef _WIN32
    if (!this->launchProcess(scriptPath, {}))
#else
    if (!this->launchProcess(QStringLiteral("/bin/sh"), {scriptPath}))
#endif
    {
        QtMessageBox::Error(this, QStringLiteral("Failed to start the updater"), QString());
        return false;
    }
    return true;
}

bool InstallUpdateDialog::launchProcess(QString file, QStringList arguments)
{
    QProcess process;
    process.setProgram(file);
    process.setArguments(arguments);
#ifndef _WIN32
    process.setWorkingDirectory(QStringLiteral("/tmp"));
#endif
    return process.startDetached();
}

void InstallUpdateDialog::timerEvent(QTimerEvent *event)
{
    this->killTimer(event->timerId());
    this->install();
}
