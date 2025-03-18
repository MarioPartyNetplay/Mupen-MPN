/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Directories.hpp"
#include "Settings.hpp"
#include "Config.hpp"
#include "Error.hpp"

#include "m64p/Api.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#elif __APPLE__
#include <sys/syslimits.h>
#include <unistd.h>
#endif // _WIN32

//
// Local Variables
//

static std::filesystem::path l_LibraryPathOverride;
static std::filesystem::path l_CorePathOverride;
static std::filesystem::path l_PluginPathOverride;
static std::filesystem::path l_SharedDataPathOverride;

//
// Local Functions
//

#ifdef PORTABLE_INSTALL
static std::filesystem::path get_exe_directory(void)
{
    static std::filesystem::path directory;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {0};
#elif __APPLE__
    char buffer[PATH_MAX];
#endif

    if (!directory.empty())
        return directory.make_preferred();

#ifdef _WIN32
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0)
    {
        MessageBoxA(nullptr, "get_exe_directory: GetModuleFileNameW() Failed!", "Error", MB_OK | MB_ICONERROR);
        std::terminate();
    }
#elif __APPLE__
    uint32_t size = PATH_MAX;
    if (_NSGetExecutablePath(buffer, &size) != 0)
    {
        std::cerr << "get_exe_directory: _NSGetExecutablePath() Failed!" << std::endl;
        std::terminate();
    }
#else
    try
    {
        directory = std::filesystem::canonical("/proc/self/exe").parent_path();
        return directory.make_preferred();
    }
    catch (...)
    {
        std::cerr << "get_exe_directory: Exception accessing /proc/self/exe" << std::endl;
        std::terminate();
    }
#endif

    directory = std::filesystem::path(buffer).parent_path();
    return directory.make_preferred();
}
#endif // PORTABLE_INSTALL

#ifdef _WIN32
static std::filesystem::path get_appdata_directory(std::filesystem::path directory)
{
    static std::filesystem::path appdataDirectory;
    if (appdataDirectory.empty())
    {
        wchar_t buffer[MAX_PATH];
        LPITEMIDLIST pidl;
        if (SHGetSpecialFolderLocation(nullptr, CSIDL_APPDATA, &pidl) != S_OK ||
            !SHGetPathFromIDListW(pidl, buffer))
        {
            appdataDirectory = get_exe_directory();
        }
        else
        {
            appdataDirectory = buffer;
        }
    }
    return appdataDirectory / "MupenMPN" / directory;
}
#else
static std::filesystem::path get_var_directory(const std::string& var, const std::string& append, 
                                              const std::string& fallbackVar, const std::string& fallbackAppend)
{
    const char* env = std::getenv(var.c_str());
    if (!env)
        env = std::getenv(fallbackVar.c_str());
    
    if (!env)
    {
        std::cerr << "get_var_directory: Missing environment variable: " << fallbackVar << std::endl;
        std::terminate();
    }
    
    return std::filesystem::path(env) / append;
}
#endif

//
// Exported Functions
//

bool CoreCreateDirectories(void)
{
    std::filesystem::path directories[] = 
    {
        CoreGetUserConfigDirectory(),
        CoreGetUserDataDirectory(),
        CoreGetUserCacheDirectory(),
        CoreGetSaveDirectory(),
        CoreGetSaveStateDirectory(),
        CoreGetScreenshotDirectory()
    };

    for (const auto& dir : directories)
    {
        try
        {
            std::filesystem::create_directories(dir);
        }
        catch (const std::exception& e)
        {
            CoreSetError("Failed to create directory '" + dir.string() + "': " + e.what());
            return false;
        }
    }
    return true;
}

bool CoreGetPortableDirectoryMode(void)
{
#ifdef PORTABLE_INSTALL
#if defined(__APPLE__)
    return false; // Force non-portable mode on macOS
#else
    static bool portable_set = false;
    static bool is_portable = false;

    if (portable_set)
        return is_portable;

    auto exe_dir = get_exe_directory();
    is_portable = std::filesystem::is_regular_file(exe_dir / "portable.txt") ||
                    std::filesystem::is_regular_file(exe_dir / "Config/mupen64plus.cfg");
    
    portable_set = true;
    return is_portable;
#endif
#else
    return false;
#endif
}

std::filesystem::path CoreGetLibraryDirectory(void)
{
    if (!l_LibraryPathOverride.empty())
        return l_LibraryPathOverride.make_preferred();

    return CORE_INSTALL_LIBDIR "/RMG";
}

std::filesystem::path CoreGetCoreDirectory(void)
{
    if (!l_CorePathOverride.empty())
        return l_CorePathOverride.make_preferred();

    return CoreGetLibraryDirectory() / "Core";
}

std::filesystem::path CoreGetPluginDirectory(void)
{
    if (!l_PluginPathOverride.empty())
        return l_PluginPathOverride.make_preferred();

    return CoreGetLibraryDirectory() / "Plugin";
}

std::filesystem::path CoreGetUserConfigDirectory(void)
{
#if defined(__APPLE__)
    return get_var_directory("XDG_CONFIG_HOME", "/Mupen-MPN", "HOME", "/Library/Preferences");
#else
    return get_var_directory("XDG_CONFIG_HOME", "/Mupen-MPN", "HOME", "/.config/Mupen-MPN");
#endif
}

std::filesystem::path CoreGetDefaultUserDataDirectory(void)
{
#if defined(__APPLE__)
    return get_var_directory("XDG_DATA_HOME", "/Mupen-MPN", "HOME", "/Library/Application Support");
#else
    return get_var_directory("XDG_DATA_HOME", "/Mupen-MPN", "HOME", "/.local/share/Mupen-MPN");
#endif
}

std::filesystem::path CoreGetSaveDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_SAVE_HOME", "/Mupen-MPN/saves", "HOME", "/Library/Application Support/Mupen-MPN/saves");
#else
    return get_var_directory("XDG_SAVE_HOME", "/Mupen-MPN/saves", "HOME", "/.local/share/Mupen-MPN/saves");
#endif
}

std::filesystem::path CoreGetUserDataDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_DATA_HOME", "/Mupen-MPN", "HOME", "/Library/Application Support/Mupen-MPN");
#else
    return get_var_directory("XDG_DATA_HOME", "/Mupen-MPN", "HOME", "/.local/share/Mupen-MPN");
#endif
}

std::filesystem::path CoreGetSaveStateDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_STATE_HOME", "/Mupen-MPN/savestates", "HOME", "/Library/Application Support/Mupen-MPN/savestates");
#else
    return get_var_directory("XDG_STATE_HOME", "/Mupen-MPN/savestates", "HOME", "/.local/share/Mupen-MPN/savestates");
#endif
}

std::filesystem::path CoreGetDefaultUserCacheDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_CACHE_HOME", "/Mupen-MPN/cache", "HOME", "/Library/Caches/Mupen-MPN");
#else
    return get_var_directory("XDG_CACHE_HOME", "/Mupen-MPN/cache", "HOME", "/.cache/Mupen-MPN");
#endif
}

std::filesystem::path CoreGetDefaultScreenshotDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_PICTURES_DIR", "/Mupen-MPN/screenshots", "HOME", "/Library/Pictures/Mupen-MPN");
#else
    return get_var_directory("XDG_PICTURES_DIR", "/Mupen-MPN/screenshots", "HOME", "/.local/share/Mupen-MPN/screenshots");
#endif
}

std::filesystem::path CoreGetUserCacheDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_CACHE_HOME", "/Mupen-MPN/cache", "HOME", "/Library/Caches/Mupen-MPN");
#else
    return get_var_directory("XDG_CACHE_HOME", "/Mupen-MPN/cache", "HOME", "/.cache/Mupen-MPN");
#endif
}

std::filesystem::path CoreGetScreenshotDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_PICTURES_DIR", "/Mupen-MPN/screenshots", "HOME", "/Library/Pictures/Mupen-MPN");
#else
    return get_var_directory("XDG_PICTURES_DIR", "/Mupen-MPN/screenshots", "HOME", "/.local/share/Mupen-MPN/screenshots");
#endif
}


std::filesystem::path CoreGetSharedDataDirectory() {
    if (!l_SharedDataPathOverride.empty())
        return l_SharedDataPathOverride.make_preferred();

    return CORE_INSTALL_DATADIR "/RMG";
}

std::filesystem::path CoreGetDefaultSaveDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_SAVE_HOME", "/Mupen-MPN/saves", "HOME", "/Library/Application Support/Mupen-MPN/saves");
#else
    return get_var_directory("XDG_SAVE_HOME", "/Mupen-MPN/saves", "HOME", "/.local/share/Mupen-MPN/saves");
#endif
}

std::filesystem::path CoreGetDefaultSaveStateDirectory() {
#if defined(__APPLE__)
    return get_var_directory("XDG_STATE_HOME", "/Mupen-MPN/savestates", "HOME", "/Library/Application Support/Mupen-MPN/savestates");
#else
    return get_var_directory("XDG_STATE_HOME", "/Mupen-MPN/savestates", "HOME", "/.local/share/Mupen-MPN/savestates");
#endif
}

