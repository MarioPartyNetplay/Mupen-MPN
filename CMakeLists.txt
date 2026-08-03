#
# Mupen MPN CMakeLists.txt
#
cmake_minimum_required(VERSION 3.15)

# On Apple Silicon: use x86_64 (Rosetta) when x86_64 Homebrew exists, else native arm64.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
    if(EXISTS "/usr/local/bin/brew")
        set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "macOS target architectures")
    else()
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architectures")
    endif()
endif()

# std::format (used in RMG-Core) requires std::to_chars, available from macOS 13.3+.
if(APPLE AND (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.3"))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS deployment version" FORCE)
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version" FORCE)
option(PORTABLE_INSTALL "Portable Installation" ON)
option(UPDATER          "Enables updater" ${WIN32})
option(APPIMAGE_UPDATER "Enables AppImage updater" OFF)
option(KCA_DRAG_DROP    "Enables drag and drop using KCoreAddons" OFF) # https://bugreports.qt.io/browse/QTBUG-91357
option(NETPLAY          "Enables netplay" ON)
option(VRU              "Enables VRU support in RMG-Input" ON)
option(USE_CCACHE       "Enables usage of ccache when ccache has been found" ON)
option(NO_ASM           "Disables the usage of assembly in the mupen64plus-core" OFF)
option(USE_ANGRYLION    "Enables building angrylion-rdp-plus which uses a non-GPL compliant license" OFF)
option(DISCORD_RPC      "Enables Discord Rich Presence support" ON)
option(ENABLE_ASAN      "Build with AddressSanitizer (catches double-free/use-after-free)" OFF)
option(ENABLE_TSAN      "Build with ThreadSanitizer (catches data races)" OFF)

project(RMG)

# Sanitizer builds: instrument every target defined below. Use to capture the
# exact allocation/free (ASan) or racing accesses (TSan) behind heap-corruption
# crashes. ASan and TSan are mutually exclusive.
if (ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot be enabled at the same time")
endif()
if (ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
elseif (ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

if(APPLE)
    enable_language(OBJC OBJCXX)
    set(VRU OFF CACHE BOOL "Enables VRU support in RMG-Input" FORCE)
endif()

find_package(Git)
if (GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE GIT_RESULT
        OUTPUT_VARIABLE GIT_VERSION
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif(GIT_FOUND)
if (NOT GIT_FOUND OR NOT ${GIT_RESULT} EQUAL 0)
    if (NOT ${GIT_RESULT} EQUAL 0)
        message(WARNING "git failed to retrieve the current revision, falling back to VERSION file!")
    endif()
    file(STRINGS "VERSION" GIT_VERSION)
endif()

if (USE_CCACHE)
    find_program(CCACHE_FOUND ccache)
    if (CCACHE_FOUND)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    else(CCACHE_FOUND)
        set(USE_CCACHE OFF)
    endif(CCACHE_FOUND)
endif(USE_CCACHE)

if (NOT PORTABLE_INSTALL AND WIN32)
    message(WARNING "Not-Portable build not supported on windows, re-enabling PORTABLE_INSTALL!")
    set(PORTABLE_INSTALL ON)
endif()

if (NOT PORTABLE_INSTALL AND UPDATER AND NOT APPIMAGE_UPDATER)
    message(WARNING "Not-Portable builds don't support the updater, disabling UPDATER")
    set(UPDATER OFF)
endif()

if (APPIMAGE_UPDATER AND WIN32)
    message(WARNING "AppImage updater not supported on windows, disabling APPIMAGE_UPDATER!")
    set(APPIMAGE_UPDATER OFF)
endif()

if (UPDATER AND NOT WIN32 AND NOT APPIMAGE_UPDATER)
    message(WARNING "Only AppImage updater is supported on linux, disabling UPDATER!")
    set(UPDATER OFF)
endif()

if (FORCE_XCB AND WIN32)
    message(WARNING "XCB Qt platform not supported on windows, disabling FORCE_XCB!")
    set(FORCE_XCB OFF)
endif()

if (PORTABLE_INSTALL)
    set(CMAKE_INSTALL_PREFIX "")
    set(INSTALL_PATH "Bin/${CMAKE_BUILD_TYPE}")
    if (APPLE)
        set(RMG_BUNDLE_NAME "Mupen-MPN.app")
        set(RMG_BUNDLE_DIR "${INSTALL_PATH}/${RMG_BUNDLE_NAME}")
        set(RMG_MACOS_DIR "${RMG_BUNDLE_DIR}/Contents/MacOS")
        set(RMG_INSTALL_PATH "${INSTALL_PATH}")
        set(SYSTEM_LIB_INSTALL_PATH "${RMG_MACOS_DIR}")
        set(LIB_INSTALL_PATH "${RMG_MACOS_DIR}")
        set(CORE_INSTALL_PATH "${RMG_MACOS_DIR}/Core")
        set(PLUGIN_INSTALL_PATH "${RMG_MACOS_DIR}/Plugin")
        set(DATA_INSTALL_PATH "${RMG_MACOS_DIR}/Data")
    else()
        set(RMG_INSTALL_PATH "${INSTALL_PATH}")
        set(SYSTEM_LIB_INSTALL_PATH "${INSTALL_PATH}")
        set(LIB_INSTALL_PATH "${INSTALL_PATH}")
        set(CORE_INSTALL_PATH "${INSTALL_PATH}/Core")
        set(PLUGIN_INSTALL_PATH "${INSTALL_PATH}/Plugin")
        set(DATA_INSTALL_PATH "${INSTALL_PATH}/Data")
    endif()
    set(INSTALL_DESKTOP_FILE OFF)
else()
    include(GNUInstallDirs)
    set(RMG_INSTALL_PATH "${CMAKE_INSTALL_BINDIR}")
    set(SYSTEM_LIB_INSTALL_PATH "${CMAKE_INSTALL_LIBDIR}")
    set(LIB_INSTALL_PATH "${SYSTEM_LIB_INSTALL_PATH}/RMG")
    set(CORE_INSTALL_PATH "${SYSTEM_LIB_INSTALL_PATH}/RMG/Core")
    set(PLUGIN_INSTALL_PATH "${SYSTEM_LIB_INSTALL_PATH}/RMG/Plugin")
    set(DATA_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/RMG")
    set(INSTALL_DESKTOP_FILE ON)
    set(METAINFO_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/metainfo/")
    set(DESKTOP_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/applications")
    set(ICON_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps/")
endif()

add_subdirectory(Source/3rdParty)
add_subdirectory(Source/3rdParty/lzma)
if (VRU)
    add_subdirectory(Source/3rdParty/vosk-api)
endif(VRU)

add_subdirectory(Source/RMG-Core)
add_subdirectory(Source/RMG)
add_subdirectory(Source/RMG-Audio)
add_subdirectory(Source/RMG-Input)
add_subdirectory(Source/RMG-Input-GCA)
install(TARGETS RMG-Core
    DESTINATION ${SYSTEM_LIB_INSTALL_PATH}
)
install(TARGETS RMG
    DESTINATION ${RMG_INSTALL_PATH}
)
install(TARGETS RMG-Audio
    DESTINATION ${PLUGIN_INSTALL_PATH}/Audio
)
install(TARGETS RMG-Input RMG-Input-GCA
    DESTINATION ${PLUGIN_INSTALL_PATH}/Input
)

if (WIN32)
    add_subdirectory(Source/Installer)

    add_custom_target(bundle_dependencies
        COMMAND bash "${CMAKE_SOURCE_DIR}/Source/Script/BundleDependencies.sh" "${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}/Mupen-MPN.exe" "${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}"
	)
elseif(APPLE)
    add_custom_target(bundle_dependencies
        COMMAND bash "${CMAKE_SOURCE_DIR}/Source/Script/BundleDependenciesMacOS.sh" "${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}"
    )
endif()

set(LIB_PERMISSIONS OWNER_READ OWNER_EXECUTE OWNER_WRITE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

install(FILES ${MUPEN64PLUSCORE_LIB}
    DESTINATION ${CORE_INSTALL_PATH}
    PERMISSIONS ${LIB_PERMISSIONS}
)
install(FILES ${MUPEN64PLUSCORE_INI} Data/font.ttf
    DESTINATION ${DATA_INSTALL_PATH}
)
if (VRU)
    install(FILES ${VOSK_API_LIBRARY}
        DESTINATION ${LIB_INSTALL_PATH}
        PERMISSIONS ${LIB_PERMISSIONS}
    )
    install(FILES ${VOSK_API_MODEL}
        DESTINATION ${DATA_INSTALL_PATH}
    )
endif(VRU)
install(FILES ${SDL_GAMECONTROLLERDB} ${CMAKE_SOURCE_DIR}/Data/InputProfileDB.json
    DESTINATION ${DATA_INSTALL_PATH}
)
if (WIN32)
    install(FILES ${CMAKE_SOURCE_DIR}/Data/pp64-cli/partyplanner64-cli-win.exe
        DESTINATION ${DATA_INSTALL_PATH}/pp64-cli
        PERMISSIONS ${LIB_PERMISSIONS}
    )
elseif(APPLE)
    if (EXISTS "${CMAKE_SOURCE_DIR}/Data/pp64-cli/partyplanner64-cli-macos")
        install(FILES ${CMAKE_SOURCE_DIR}/Data/pp64-cli/partyplanner64-cli-macos
            DESTINATION ${DATA_INSTALL_PATH}/pp64-cli
            PERMISSIONS ${LIB_PERMISSIONS}
        )
    else()
        install(FILES ${CMAKE_SOURCE_DIR}/Data/pp64-cli/partyplanner64-cli-linux
            DESTINATION ${DATA_INSTALL_PATH}/pp64-cli
            PERMISSIONS ${LIB_PERMISSIONS}
        )
    endif()
else()
    install(FILES ${CMAKE_SOURCE_DIR}/Data/pp64-cli/partyplanner64-cli-linux
        DESTINATION ${DATA_INSTALL_PATH}/pp64-cli
        PERMISSIONS ${LIB_PERMISSIONS}
    )
endif()
file(GLOB GENERATED_CHEAT_FILES ${CMAKE_SOURCE_DIR}/Data/Cheats/Generated/*.cht)
file(GLOB CUSTOM_CHEAT_FILES ${CMAKE_SOURCE_DIR}/Data/Cheats/Custom/*.cht)
install(FILES ${GENERATED_CHEAT_FILES} ${CUSTOM_CHEAT_FILES}
    DESTINATION ${DATA_INSTALL_PATH}/Cheats
)

install(FILES ${MUPEN64PLUS_PLUGIN_RSP_HLE}
    DESTINATION ${PLUGIN_INSTALL_PATH}/RSP
    PERMISSIONS ${LIB_PERMISSIONS}
)

install(FILES ${MUPEN64PLUS_PLUGIN_RSP_PARALLEL}
    DESTINATION ${PLUGIN_INSTALL_PATH}/RSP
    PERMISSIONS ${LIB_PERMISSIONS}
)

install(FILES ${MUPEN64PLUS_PLUGIN_INPUT_RAPHNET}
    DESTINATION ${PLUGIN_INSTALL_PATH}/Input
    PERMISSIONS ${LIB_PERMISSIONS}
)

install(FILES ${MUPEN64PLUS_PLUGIN_GFX_GLIDEN64}
    DESTINATION ${PLUGIN_INSTALL_PATH}/GFX
    PERMISSIONS ${LIB_PERMISSIONS}
)

install(FILES ${MUPEN64PLUS_PLUGIN_GFX_GLIDEN64_DATA}
    DESTINATION ${DATA_INSTALL_PATH}
)

if (INSTALL_DESKTOP_FILE)
    install(FILES Package/com.github.Rosalie241.RMG.desktop
        DESTINATION ${DESKTOP_INSTALL_PATH}
        RENAME org.marioparty.mupen.desktop
    )
    install(FILES Package/com.github.Rosalie241.RMG.svg
        DESTINATION ${ICON_INSTALL_PATH}
        RENAME org.marioparty.mupen.svg
    )
    install(FILES Package/com.github.Rosalie241.RMG.metainfo.xml
        DESTINATION ${METAINFO_INSTALL_PATH}
        RENAME org.marioparty.mupen.metainfo.xml
    )
endif(INSTALL_DESKTOP_FILE)

if (APPLE AND PORTABLE_INSTALL)
    install(CODE "
        message(STATUS \"Rewriting Qt framework paths in ${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}/Mupen-MPN.app\")
        execute_process(
            COMMAND bash \"${CMAKE_SOURCE_DIR}/Source/Script/BundleDependenciesMacOS.sh\"
                    \"${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}\"
                    --rewrite-only
            RESULT_VARIABLE _mupen_qt_rewrite_result
        )
        if(_mupen_qt_rewrite_result)
            message(WARNING \"Qt path rewrite failed with code \${_mupen_qt_rewrite_result}\")
        endif()
    ")
endif()
