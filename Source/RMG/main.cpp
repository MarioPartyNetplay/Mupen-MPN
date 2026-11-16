/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <UserInterface/MainWindow.hpp>

#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QCoreApplication>
#include <QApplication>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QLockFile>

#include <iostream>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include <Windows.h>
#include <Dbghelp.h>
#include <wchar.h>
#else
#include <signal.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <vector>
#include <climits>
#endif

#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Version.hpp>

//
// Local Functions
//

static void loadEnvironmentFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        if (line.startsWith(QStringLiteral("export "))) {
            line = line.mid(7).trimmed();
        }

        const int equalsIndex = line.indexOf('=');
        if (equalsIndex <= 0) {
            continue;
        }

        const QString key = line.left(equalsIndex).trimmed();
        QString value = line.mid(equalsIndex + 1).trimmed();

        if ((value.startsWith('"') && value.endsWith('"')) ||
            (value.startsWith('\'') && value.endsWith('\''))) {
            value = value.mid(1, value.size() - 2);
        }

        if (!key.isEmpty() && !qEnvironmentVariableIsSet(key.toUtf8().constData())) {
            qputenv(key.toUtf8(), value.toUtf8());
        }
    }
}

static void message_handler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // personally I think that there should be a better
    // way to silence these warnings because I'd rather
    // not change the names of all custom signal/slot implementations,
    // it'd cause the code to be very inconsistent naming-wise
    if (type == QtWarningMsg && msg.startsWith("QMetaObject::connectSlotsByName:"))
    {
        return;
    }

    std::cerr << msg.toStdString() << std::endl;
}

#ifdef _WIN32
typedef BOOL (WINAPI* ptr_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                             PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION,
                                             PMINIDUMP_CALLBACK_INFORMATION);
static void show_error(const char* error)
{
    MessageBoxA(nullptr, error, "RMG Crash Handler", MB_OK | MB_ICONERROR);
}

static LONG WINAPI exception_handler(_EXCEPTION_POINTERS* ExceptionInfo)
{
    HMODULE dbgHelpModule = LoadLibraryA("Dbghelp.dll");
    if (dbgHelpModule == nullptr)
    {
        show_error("Failed to find Dbghelp.dll!");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    ptr_MiniDumpWriteDump miniDumpWriteDump = 
        (ptr_MiniDumpWriteDump)GetProcAddress(dbgHelpModule, "MiniDumpWriteDump");
    if (miniDumpWriteDump == nullptr)
    {
        show_error("Failed to retrieve MiniDumpWriteDump from Dbghelp.dll!");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    wchar_t dump_filename[MAX_PATH+1] = {0};
    DWORD filename_len = GetModuleFileNameW(nullptr, dump_filename, MAX_PATH);
    if (filename_len <= 0)
    {
        show_error("Failed to retrieve path of current process!");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    if ((filename_len + 4) > MAX_PATH)
    { // replace .exe with .dmp
        wcscpy((dump_filename + (filename_len - 4)), L".dmp");
    }
    else
    { // append .dmp to .exe
        wcscpy((dump_filename + filename_len), L".dmp");
    }

    HANDLE hDumpFile = CreateFileW(dump_filename, GENERIC_WRITE, FILE_SHARE_WRITE, 
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hDumpFile != INVALID_HANDLE_VALUE)
    {
        _MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = ExceptionInfo;
        exceptionInfo.ClientPointers = FALSE;

        BOOL ret = miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), 
                                     hDumpFile, MiniDumpNormal, &exceptionInfo, 
                                     nullptr, nullptr);
        if (!ret)
        {
            show_error("Failed to save minidump file!");
        }
    }

    // we've saved the minidump,
    // now we can crash safely
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void signal_handler(int sig)
{
    QGuiApplication::quit();
}
#endif // _WIN32

#ifdef PORTABLE_INSTALL
static std::unique_ptr<QLockFile> l_PortableInstanceLock;

static void configurePortableMultiInstanceProfile(QCoreApplication& app)
{
    if (!CoreGetPortableDirectoryMode())
    {
        return;
    }

    const QByteArray profileRootEnv = qgetenv("RMG_USER_DATA_DIR");
    if (!profileRootEnv.isEmpty())
    {
        CoreSetPortableProfileRoot(profileRootEnv.toStdString());
        std::cerr << "Using portable profile root from RMG_USER_DATA_DIR: "
                  << profileRootEnv.constData() << std::endl;
        return;
    }

    const QString lockPath =
        QDir(app.applicationDirPath()).filePath(QStringLiteral(".rmg-instance.lock"));
    l_PortableInstanceLock = std::make_unique<QLockFile>(lockPath);
    l_PortableInstanceLock->setStaleLockTime(0);

    if (l_PortableInstanceLock->tryLock(0))
    {
        return;
    }

    const QString isolatedProfile =
        QStringLiteral("instances/%1").arg(QCoreApplication::applicationPid());
    CoreSetPortableProfileRoot(isolatedProfile.toStdString());
    std::cerr << "Portable install: another instance is using the default profile; "
              << "using isolated profile '" << isolatedProfile.toStdString()
              << "'" << std::endl;
}
#endif // PORTABLE_INSTALL

#ifdef __APPLE__
static QString macOSExecutableDirectory(void)
{
    std::vector<char> buffer(PATH_MAX);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            return QString();
        }
    }

    return QFileInfo(QString::fromUtf8(buffer.data())).absolutePath();
}

static void configureMoltenVK(void)
{
    if (qEnvironmentVariableIsSet("QT_VULKAN_LIB"))
    {
        return;
    }

    const QString exeDir = macOSExecutableDirectory();
    const QString bundledMoltenVk = exeDir + QStringLiteral("/libMoltenVK.dylib");
    if (QFile::exists(bundledMoltenVk))
    {
        qputenv("QT_VULKAN_LIB", bundledMoltenVk.toUtf8());
        qputenv("GRANITE_VULKAN_LIBRARY", bundledMoltenVk.toUtf8());
        return;
    }

    static const char* moltenVkPrefixes[] = {
        "/opt/local",
        "/usr/local/opt/molten-vk",
        "/opt/homebrew/opt/molten-vk"
    };
    for (const char* prefix : moltenVkPrefixes)
    {
        const QString moltenVk = QString::fromUtf8(prefix) + QStringLiteral("/lib/libMoltenVK.dylib");
        if (QFile::exists(moltenVk))
        {
            qputenv("QT_VULKAN_LIB", moltenVk.toUtf8());
            qputenv("GRANITE_VULKAN_LIBRARY", moltenVk.toUtf8());
            return;
        }
    }

    qputenv("QT_VULKAN_LIB", "libMoltenVK.dylib");
}
#endif // __APPLE__

//
// Exported Functions
//

static bool isCliBoolToken(const QString& token)
{
    const QString value = token.trimmed().toLower();
    return value == QStringLiteral("on") || value == QStringLiteral("off") ||
           value == QStringLiteral("true") || value == QStringLiteral("false") ||
           value == QStringLiteral("yes") || value == QStringLiteral("no") ||
           value == QStringLiteral("1") || value == QStringLiteral("0");
}

static bool parseCliBool(const QString& text, bool* ok)
{
    const QString value = text.trimmed().toLower();
    if (value == QStringLiteral("on") || value == QStringLiteral("true") ||
        value == QStringLiteral("yes") || value == QStringLiteral("1"))
    {
        *ok = true;
        return true;
    }
    if (value == QStringLiteral("off") || value == QStringLiteral("false") ||
        value == QStringLiteral("no") || value == QStringLiteral("0"))
    {
        *ok = true;
        return false;
    }

    *ok = false;
    return false;
}

// Bare --netplay-host, --upnp, and --lobby keep their defaults
// (traversal, on, on) without swallowing the following ROM path.
static QStringList normalizeNetplayArguments(const QStringList& arguments)
{
    QStringList normalized;
    normalized.reserve(arguments.size() + 3);
    bool endOfOptions = false;

    for (int i = 0; i < arguments.size(); ++i)
    {
        const QString argument = arguments.at(i);
        if (!endOfOptions && argument == QStringLiteral("--"))
        {
            endOfOptions = true;
            normalized << argument;
            continue;
        }
        if (endOfOptions)
        {
            normalized << argument;
            continue;
        }

        const auto rewriteBare = [&](const QString& name, const QString& fallback, auto acceptsValue) -> bool {
            if (argument == name)
            {
                const bool hasValue = (i + 1) < arguments.size() && acceptsValue(arguments.at(i + 1));
                normalized << (name + QLatin1Char('=') + (hasValue ? arguments.at(i + 1) : fallback));
                if (hasValue)
                {
                    ++i;
                }
                return true;
            }

            const QString prefix = name + QLatin1Char('=');
            if (argument.startsWith(prefix) && argument.mid(prefix.size()).trimmed().isEmpty())
            {
                normalized << (prefix + fallback);
                return true;
            }
            return false;
        };

        if (rewriteBare(QStringLiteral("--netplay-host"), QStringLiteral("traversal"), [](const QString& token) {
                const QString value = token.trimmed().toLower();
                return value == QStringLiteral("direct") || value == QStringLiteral("traversal");
            }) ||
            rewriteBare(QStringLiteral("--upnp"), QStringLiteral("on"), [](const QString& token) {
                return isCliBoolToken(token);
            }) ||
            rewriteBare(QStringLiteral("--lobby"), QStringLiteral("on"), [](const QString& token) {
                return isCliBoolToken(token);
            }))
        {
            continue;
        }

        normalized << argument;
    }

    return normalized;
}

int main(int argc, char **argv)
{
    loadEnvironmentFile(QDir::current().filePath(QStringLiteral(".env")));

    // install message handler
    qInstallMessageHandler(message_handler);

#ifdef _WIN32
    // on Windows, to aid with crash debugging
    // we'll install a crash handler and
    // write out a minidump there
    SetUnhandledExceptionFilter(exception_handler);
#else
    // on Linux we need to install signal handlers,
    // so we can exit cleanly when the user presses
    // i.e control+c
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
#endif // _WIN32

#ifdef __linux__
    // on Linux, wayland works only on some compositors,
    // it works on KDE plasma and sway (on 2023-07-26),
    // but i.e doesn't work on GNOME wayland or labwc, 
    // so to compromise the situation, we'll force xwayland
    // unless RMG_ALLOW_WAYLAND is set to 1, which'll allow wayland
    // as qt platform, so users can experiment with the
    // wayland support themselves
    const char* allow_wayland = std::getenv("RMG_ALLOW_WAYLAND");
    if (allow_wayland == nullptr ||
        std::string(allow_wayland) != "1")
    {
        setenv("QT_QPA_PLATFORM", "xcb", 1);
    }

    // on Linux, Qt on some distributions
    // fails to load libvulkan, so to fix that
    // we'll help out and tell it about libvulkan.so.1,
    // but we wont overwrite the variable value if it
    // already contains one, the user might set it
    // to a custom value themselves
    setenv("QT_VULKAN_LIB", "libvulkan.so.1", 0);

    // ensure the default OpenGL format
    // doesn't have vsync enabled by default,
    // only needed for linux (for now)
    // and ensure OpenGL 3.3 is specified by default (for wayland)
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(0);
    format.setMajorVersion(3);
    format.setMinorVersion(3);
    QSurfaceFormat::setDefaultFormat(format);

    // ensure that the desktop file is correctly
    // specified, else the window icon will be
    // the generic wayland icon on wayland
    QGuiApplication::setDesktopFileName("org.marioparty.mupen");
#elif defined(__APPLE__)
    configureMoltenVK();

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(0);
    format.setMajorVersion(3);
    format.setMinorVersion(2);
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
#endif // __linux__

    QApplication app(argc, argv);

    loadEnvironmentFile(QDir(app.applicationDirPath()).filePath(QStringLiteral(".env")));

    UserInterface::MainWindow window;

#ifdef PORTABLE_INSTALL
    // only change current directory
    // when we're in portable directory mode
    if (CoreGetPortableDirectoryMode())
    {
        QDir::setCurrent(app.applicationDirPath());
    }
#endif

    QCoreApplication::setApplicationName("Mupen MPN");
    QCoreApplication::setApplicationVersion(QString::fromStdString(CoreGetVersion()));

    // setup commandline parser
    QCommandLineParser parser;
    // default options
    parser.addHelpOption();
    parser.addVersionOption();
    // custom options
#ifndef PORTABLE_INSTALL
    QCommandLineOption libPathOption("lib-path", "Changes the path where the libraries are stored", "path");
    QCommandLineOption corePathOption("core-path", "Changes the path where the core library is stored", "path");
    QCommandLineOption pluginPathOption("plugin-path", "Changes the path where the plugins are stored", "path");
    QCommandLineOption sharedDataPathOption("shared-data-path", "Changes the path where the shared data is stored", "path");
    libPathOption.setFlags(QCommandLineOption::HiddenFromHelp);
    corePathOption.setFlags(QCommandLineOption::HiddenFromHelp);
    pluginPathOption.setFlags(QCommandLineOption::HiddenFromHelp);
    sharedDataPathOption.setFlags(QCommandLineOption::HiddenFromHelp);
#endif // PORTABLE_INSTALL
    QCommandLineOption debugMessagesOption({"d", "debug-messages"}, "Prints debug callback messages to stdout");
    QCommandLineOption fullscreenOption({"f", "fullscreen"}, "Launches ROM in fullscreen mode");
    QCommandLineOption noGuiOption({"n", "nogui"}, "Hides GUI elements (menubar, toolbar, statusbar)");
    QCommandLineOption quitAfterEmulationOption({"q", "quit-after-emulation"}, "Quits RMG when emulation has finished");
    QCommandLineOption loadStateSlot("load-state-slot", "Loads save state slot when launching the ROM", "Slot Number");
    QCommandLineOption diskOption("disk", "64DD Disk to open ROM in combination with", "64DD Disk");
#ifdef NETPLAY
    QCommandLineOption netplayHostOption(
        "netplay-host",
        "Host a netplay room for the ROM and open the netplay window. "
        "Mode is traversal (default) or direct.",
        "traversal|direct");
    QCommandLineOption netplayPortOption(
        "netplay-port",
        "Port to listen on when hosting. Defaults to 2626.",
        "port");
    QCommandLineOption upnpOption(
        "upnp",
        "Enable UPnP port mapping when hosting. Defaults to off. Value: on or off.",
        "on|off");
    QCommandLineOption lobbyOption(
        "lobby",
        "List the hosted room in the public browser. Defaults to on. Value: on or off.",
        "on|off");
    QCommandLineOption netplayJoinOption(
        "netplay-join",
        "Join a netplay room and open the netplay window. Accepts a traversal code or ip:port.",
        "code|ip:port");
#endif // NETPLAY
#ifdef PORTABLE_INSTALL
    QCommandLineOption userDataDirOption(
        "user-data-dir",
        "Use an isolated portable profile directory (Config/Data/Save/Cache). "
        "Useful when running multiple instances from the same install.",
        "path");
#endif // PORTABLE_INSTALL

#ifndef PORTABLE_INSTALL
    parser.addOption(libPathOption);
    parser.addOption(corePathOption);
    parser.addOption(pluginPathOption);
    parser.addOption(sharedDataPathOption);
#endif // PORTABLE_INSTALL
    parser.addOption(debugMessagesOption);
    parser.addOption(fullscreenOption);
    parser.addOption(noGuiOption);
    parser.addOption(quitAfterEmulationOption);
    parser.addOption(loadStateSlot);
    parser.addOption(diskOption);
#ifdef NETPLAY
    parser.addOption(netplayHostOption);
    parser.addOption(netplayPortOption);
    parser.addOption(upnpOption);
    parser.addOption(lobbyOption);
    parser.addOption(netplayJoinOption);
#endif // NETPLAY
#ifdef PORTABLE_INSTALL
    parser.addOption(userDataDirOption);
#endif // PORTABLE_INSTALL
    parser.addPositionalArgument("ROM", "ROM to open");

    // parse arguments
#ifdef NETPLAY
    parser.process(normalizeNetplayArguments(QCoreApplication::arguments()));
#else
    parser.process(app);
#endif // NETPLAY

#ifdef NETPLAY
    UserInterface::MainWindow::NetplayCliRequest netplayCli;
    if (parser.isSet(netplayHostOption) && parser.isSet(netplayJoinOption))
    {
        std::cerr << "Use either --netplay-host or --netplay-join." << std::endl;
        return 1;
    }

    if (parser.isSet(netplayHostOption))
    {
        const QString hostMode = parser.value(netplayHostOption).trimmed().toLower();
        if (hostMode != QStringLiteral("direct") && hostMode != QStringLiteral("traversal"))
        {
            std::cerr << "--netplay-host must be 'direct' or 'traversal'." << std::endl;
            return 1;
        }
        netplayCli.host = true;
        netplayCli.traversal = hostMode != QStringLiteral("direct");
    }

    if (parser.isSet(netplayPortOption))
    {
        bool portOk = false;
        const int port = parser.value(netplayPortOption).toInt(&portOk);
        if (!portOk || port < 1024 || port > 65535)
        {
            std::cerr << "--netplay-port must be between 1024 and 65535." << std::endl;
            return 1;
        }
        netplayCli.port = port;
    }

    if (parser.isSet(upnpOption))
    {
        bool upnpOk = false;
        netplayCli.upnp = parseCliBool(parser.value(upnpOption), &upnpOk);
        if (!upnpOk)
        {
            std::cerr << "--upnp must be on or off." << std::endl;
            return 1;
        }
    }

    if (parser.isSet(lobbyOption))
    {
        bool lobbyOk = false;
        netplayCli.showInBrowser = parseCliBool(parser.value(lobbyOption), &lobbyOk);
        if (!lobbyOk)
        {
            std::cerr << "--lobby must be on or off." << std::endl;
            return 1;
        }
    }

    if (parser.isSet(netplayJoinOption))
    {
        netplayCli.joinTarget = parser.value(netplayJoinOption).trimmed();
        if (netplayCli.joinTarget.isEmpty())
        {
            std::cerr << "--netplay-join requires a traversal code or ip:port." << std::endl;
            return 1;
        }
    }

    const bool launchNetplay = netplayCli.host || !netplayCli.joinTarget.isEmpty();
    if (netplayCli.host && parser.positionalArguments().isEmpty())
    {
        std::cerr << "--netplay-host requires a ROM path." << std::endl;
        return 1;
    }
#else
    const bool launchNetplay = false;
#endif // NETPLAY

#ifdef PORTABLE_INSTALL
    configurePortableMultiInstanceProfile(app);
    if (parser.isSet(userDataDirOption))
    {
        CoreSetPortableProfileRoot(parser.value(userDataDirOption).toStdString());
        std::cerr << "Using portable profile root from --user-data-dir: "
                  << parser.value(userDataDirOption).toStdString() << std::endl;
    }
#endif // PORTABLE_INSTALL

#ifndef PORTABLE_INSTALL
    // set path overrides before initializing
    QString libPathOverride        = parser.value(libPathOption);
    QString corePathOveride        = parser.value(corePathOption);
    QString pluginPathOverride     = parser.value(pluginPathOption);
    QString sharedDataPathOverride = parser.value(sharedDataPathOption);
    if (!libPathOverride.isEmpty())
    {
        CoreSetLibraryPathOverride(libPathOverride.toStdString());
    }
    if (!corePathOveride.isEmpty())
    {
        CoreSetCorePathOverride(corePathOveride.toStdString());
    }
    if (!pluginPathOverride.isEmpty())
    {
        CoreSetPluginPathOverride(pluginPathOverride.toStdString());
    }
    if (!sharedDataPathOverride.isEmpty())
    {
        CoreSetSharedDataPathOverride(sharedDataPathOverride.toStdString());
    }
#endif // PORTABLE_INSTALL

    // print debug callbacks to stdout if needed
    CoreSetPrintDebugCallback(parser.isSet(debugMessagesOption));

    // specified ROM path to launch
    QStringList args = parser.positionalArguments();

    CoreAddCallbackMessage(CoreDebugMessageType::Info, 
            "Initializing on " + QGuiApplication::platformName().toStdString());

    // initialize window
    if (!window.Init(&app, !parser.isSet(noGuiOption), !args.empty()))
    {
        return 1;
    }

    bool parsedNumber = false;
    int saveStateSlot = parser.value(loadStateSlot).toInt(&parsedNumber);
    if (parser.value(loadStateSlot).isEmpty() || !parsedNumber ||
        saveStateSlot < 0 || saveStateSlot > 9)
    {
        saveStateSlot = -1;
    }

    if (!launchNetplay && !args.empty())
    {
        window.OpenROM(args.at(0), parser.value(diskOption), parser.isSet(fullscreenOption), parser.isSet(quitAfterEmulationOption), saveStateSlot);
    }

    // show window
    window.show();

#ifdef NETPLAY
    if (launchNetplay)
    {
        netplayCli.romFile = args.isEmpty() ? QString() : args.at(0);
        netplayCli.diskFile = parser.value(diskOption);
        netplayCli.fullscreen = parser.isSet(fullscreenOption);
        netplayCli.quitAfterEmulation = parser.isSet(quitAfterEmulationOption);
        netplayCli.stateSlot = saveStateSlot;
        window.launchNetplayFromCli(netplayCli);
    }
#endif // NETPLAY

    return app.exec();
}
