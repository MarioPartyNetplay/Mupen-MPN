#
# RMG-Core CMakeLists.txt
#
project(RMG-Core)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP minizip)
if (NOT MINIZIP_FOUND)
    pkg_check_modules(MINIZIP REQUIRED minizip-ng)
endif()

if (WIN32)
    pkg_check_modules(ICONV REQUIRED iconv)
endif(WIN32)

if (NOT PORTABLE_INSTALL)
    include(GNUInstallDirs)
endif(NOT PORTABLE_INSTALL)

configure_file(Config.hpp.in Config.hpp)

set(RMG_CORE_SOURCES
    m64p/Api.cpp
    m64p/CoreApi.cpp
    m64p/ConfigApi.cpp
    m64p/PluginApi.cpp
    Netplay/LockstepEngine.cpp
    Netplay/WebRTC/WebRTCDataChannel.cpp
    CachedRomHeaderAndSettings.cpp
    ConvertStringEncoding.cpp
    SpeedLimiter.cpp
    SpeedFactor.cpp
    RomSettings.cpp
    Directories.cpp
    MediaLoader.cpp
    Screenshot.cpp
    RomHeader.cpp
    Emulation.cpp
    SaveState.cpp
    Callback.cpp
    Settings.cpp
    Archive.cpp
    Library.cpp
    Netplay.cpp
    Plugins.cpp
    Version.cpp
    Cheats.cpp
    String.cpp
    Volume.cpp
    VidExt.cpp
    Video.cpp
    Error.cpp
    Discord.cpp
    Core.cpp
    File.cpp
    Key.cpp
    Rom.cpp
)

if (NETPLAY)
    add_definitions(-DNETPLAY -DWEBRTC_P2P)
endif(NETPLAY)

if (PORTABLE_INSTALL)
    add_definitions(-DPORTABLE_INSTALL)
endif(PORTABLE_INSTALL)

add_library(RMG-Core SHARED ${RMG_CORE_SOURCES})

if (NETPLAY)
    add_dependencies(RMG-Core libdatachannel libdatachannel_external)
endif(NETPLAY)

if(UNIX)
    target_link_libraries(RMG-Core dl)
endif(UNIX)

if(WIN32)
    target_link_libraries(RMG-Core wsock32 ws2_32 ${ICONV_LIBRARIES})
elseif(APPLE)
    target_link_libraries(RMG-Core iconv)
endif()

# Regenerate git commit hash on each build (execute_process only runs at configure time).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RMG_CORE_VERSION_HASH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/VersionHash.hpp")
add_custom_command(
    OUTPUT "${RMG_CORE_VERSION_HASH_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_version.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VersionHash.hpp.in"
            "${CMAKE_CURRENT_SOURCE_DIR}/make_version.py"
    VERBATIM
)
add_custom_target(RMG-Core-version-hash DEPENDS "${RMG_CORE_VERSION_HASH_HEADER}")
add_dependencies(RMG-Core RMG-Core-version-hash)

target_link_libraries(RMG-Core
    ${MINIZIP_LIBRARIES}
    lzma
    discord-rpc
)

target_include_directories(RMG-Core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdParty/discord-rpc/include
)

if (NETPLAY)
    if (LIBDATACHANNEL_EXTERNAL_TARGET)
        target_link_libraries(RMG-Core ${LIBDATACHANNEL_EXTERNAL_TARGET})
        target_include_directories(RMG-Core PRIVATE ${LIBDATACHANNEL_INCLUDE_DIR})
    elseif (TARGET libdatachannel::libdatachannel)
        target_link_libraries(RMG-Core libdatachannel::libdatachannel)
    elseif (TARGET LibDataChannel::LibDataChannel)
        target_link_libraries(RMG-Core LibDataChannel::LibDataChannel)
    endif()
endif(NETPLAY)

target_include_directories(RMG-Core PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${CMAKE_CURRENT_SOURCE_DIR}/../RMG/Netplay
    ${MINIZIP_INCLUDE_DIRS}
    ${MINIZIP_UNZIP_INCLUDE_DIR}
)

set_target_properties(RMG-Core PROPERTIES CXX_VISIBILITY_PRESET hidden)

if (APPLE AND PORTABLE_INSTALL)
    set_target_properties(RMG-Core PROPERTIES
        INSTALL_RPATH "@loader_path"
    )
endif()
