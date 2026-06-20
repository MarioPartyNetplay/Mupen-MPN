#
# 3rdParty CMakeLists.txt
#
include(ExternalProject)
find_package(Git REQUIRED)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(BUILD_EXAMPLES OFF CACHE BOOL "Build example apps" FORCE)
add_subdirectory(discord-rpc)

set(M64P_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-core)
if(WIN32)
    set(SO_EXT "dll")
    set(GCA_LIB "mupen64plus_input_gca.${SO_EXT}")
    set(CORE_FILE "mupen64plus.dll")
elseif(APPLE)
    set(SO_EXT "dylib")
    set(GCA_LIB "libmupen64plus_input_gca.${SO_EXT}")
    set(CORE_FILE "libmupen64plus.dylib")
else()
    set(SO_EXT "so")
    set(GCA_LIB "libmupen64plus_input_gca.${SO_EXT}")
    set(CORE_FILE "libmupen64plus.so")
endif()

set(BASE_CMAKE_CONFIGURE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} 
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS} -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS})
if (APPLE)
    if (CMAKE_OSX_ARCHITECTURES)
        list(APPEND BASE_CMAKE_CONFIGURE_ARGS -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
    endif()
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND BASE_CMAKE_CONFIGURE_ARGS -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif()
    if (CMAKE_PREFIX_PATH)
        list(APPEND BASE_CMAKE_CONFIGURE_ARGS -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH})
    endif()
    if (CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(MAKE_HOST_CPU "x86_64")
    elseif (CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(MAKE_HOST_CPU "arm64")
    endif()
endif()
if (MAKE_HOST_CPU)
    set(MAKE_HOST_CPU_ARG "HOST_CPU=${MAKE_HOST_CPU}")
else()
    set(MAKE_HOST_CPU_ARG "")
endif()
if (USE_CCACHE)
    set(CMAKE_CONFIGURE_ARGS ${BASE_CMAKE_CONFIGURE_ARGS} -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
    set(MAKE_CC_COMPILER "ccache ${CMAKE_C_COMPILER}")
    set(MAKE_CXX_COMPILER "ccache ${CMAKE_CXX_COMPILER}")
else(USE_CCACHE)
    set(CMAKE_CONFIGURE_ARGS ${BASE_CMAKE_CONFIGURE_ARGS})
    set(MAKE_CC_COMPILER "${CMAKE_C_COMPILER}")
    set(MAKE_CXX_COMPILER "${CMAKE_CXX_COMPILER}")
endif(USE_CCACHE)

# use sub-make when available
if ("${CMAKE_GENERATOR}" STREQUAL "Unix Makefiles" OR
    "${CMAKE_GENERATOR}" STREQUAL "MSYS Makefiles")
    set(MAKE_CMD "$(MAKE)")
else()
    set(MAKE_CMD "make")
endif()

if ("${CMAKE_C_FLAGS}" STREQUAL "")
    set(MAKE_OPTFLAGS "-O3")
else()
    set(MAKE_OPTFLAGS "${CMAKE_C_FLAGS}")
endif()

if ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    set(MAKE_DEBUG "0")
    set(MAKE_OPTFLAGS "${MAKE_OPTFLAGS} -g")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    set(MAKE_DEBUG "1")
else()
    set(MAKE_DEBUG "0")
endif()

# TODO: when SDL3_net has made a release,
# remove the vendored SDL3_net
set(SDL3NET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL_net)
set(SDL3NET_SOURCES
    ${SDL3NET_DIR}/src/SDL_net.c
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)
add_library(SDL3_net STATIC ${SDL3NET_SOURCES})
target_include_directories(SDL3_net PRIVATE
    ${SDL3NET_DIR}/include
    ${SDL3_INCLUDE_DIRS}
)
set(SDL3NET_CFLAGS "-DUSE_SDL3NET -I${SDL3NET_DIR}/include ${SDL3_CFLAGS}")
# for some reason MingW needs SDL3 to be linked after SDL3_net,
# else we get a lot of missing symbol errors, should be harmless
# on other platforms
set(SDL3NET_LDLIBS "$<TARGET_FILE:SDL3_net> -l${SDL3_LIBRARIES}")
if (WIN32)
    set(SDL3NET_LDLIBS "${SDL3NET_LDLIBS} -liphlpapi -lws2_32")
endif (WIN32)

ExternalProject_Add(mupen64plus-core
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-core/

    CONFIGURE_COMMAND ""
    INSTALL_COMMAND ""

    BUILD_COMMAND ${MAKE_CMD} all -f ${M64P_CORE_DIR}/projects/unix/Makefile 
        SRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-core/src 
        SUBDIR=${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-core/subprojects 
        OSD=0 NEW_DYNAREC=1 NO_ASM=$<BOOL:${NO_ASM}> 
        KEYBINDINGS=0 ACCURATE_FPU=0 VULKAN=0 NETPLAY=$<BOOL:${NETPLAY}> 
        TARGET=${CORE_FILE} DEBUG=${MAKE_DEBUG}
        CC=${MAKE_CC_COMPILER} CXX=${MAKE_CXX_COMPILER}
        OPTFLAGS=${MAKE_OPTFLAGS} ${MAKE_HOST_CPU_ARG}
        # TODO: when SDL3_net has made a release,
        # remove the vendored SDL3_net
        SDLNET_CFLAGS=${SDL3NET_CFLAGS} SDLNET_LDLIBS=${SDL3NET_LDLIBS}
    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-core

    BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-core/${CORE_FILE}
    DEPENDS SDL3_net
)

set(APIDIR "${CMAKE_SOURCE_DIR}/Source/3rdParty/mupen64plus-core/src/api")

ExternalProject_Add(mupen64plus-rsp-cxd4
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-cxd4/

    CONFIGURE_COMMAND ""
    INSTALL_COMMAND ""

    BUILD_COMMAND ${MAKE_CMD} all -f ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-cxd4/projects/unix/Makefile
        SRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-cxd4
        APIDIR=${APIDIR} DEBUG=${MAKE_DEBUG} POSTFIX= 
        CC=${MAKE_CC_COMPILER} CXX=${MAKE_CXX_COMPILER}
        OPTFLAGS=${MAKE_OPTFLAGS} ${MAKE_HOST_CPU_ARG}
    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-rsp-cxd4

    BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-rsp-cxd4/mupen64plus-rsp-cxd4.${SO_EXT}
)

ExternalProject_Add(mupen64plus-rsp-hle
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-hle/

    CONFIGURE_COMMAND ""
    INSTALL_COMMAND ""

    BUILD_COMMAND ${MAKE_CMD} all -f ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-hle/projects/unix/Makefile
        SRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-hle/src
        APIDIR=${APIDIR} DEBUG=${MAKE_DEBUG}
        CC=${MAKE_CC_COMPILER} CXX=${MAKE_CXX_COMPILER}
        OPTFLAGS=${MAKE_OPTFLAGS} ${MAKE_HOST_CPU_ARG}
    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-rsp-hle

    BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-rsp-hle/mupen64plus-rsp-hle.${SO_EXT}
)

ExternalProject_Add(mupen64plus-rsp-parallel
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-rsp-parallel/

    INSTALL_COMMAND ""

    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    CMAKE_ARGS -DUSE_IPO=OFF ${CMAKE_CONFIGURE_ARGS}
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-rsp-parallel

    BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-rsp-parallel/mupen64plus-rsp-parallel.${SO_EXT}
)

ExternalProject_Add(mupen64plus-input-raphnetraw
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-input-raphnetraw/

    CONFIGURE_COMMAND ""
    INSTALL_COMMAND ""

    BUILD_COMMAND ${MAKE_CMD} all -f ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-input-raphnetraw/projects/unix/Makefile
        SRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-input-raphnetraw/src
        APIDIR=${APIDIR} DEBUG=${MAKE_DEBUG}
        CC=${MAKE_CC_COMPILER} CXX=${MAKE_CXX_COMPILER}
        OPTFLAGS=${MAKE_OPTFLAGS} ${MAKE_HOST_CPU_ARG}
    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-input-raphnetraw

    BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-input-raphnetraw/mupen64plus-input-raphnetraw.${SO_EXT}
)

set(GLIDEN64_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-video-GLideN64")
set(GLIDEN64_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-video-GLideN64")
set(GLIDEN64_CUSTOM_INI "${GLIDEN64_DIR}/ini/GLideN64.custom.ini")
set(GLIDEN64_CMAKE_ARGS -DMUPENPLUSAPI=ON -DMUPENPLUSAPI_GLIDENUI=ON -DGLIDENUI_QT6=ON -DUSE_IPO=OFF -DUSE_SYSTEM_LIBS=ON -DNO_OSD=ON ${CMAKE_CONFIGURE_ARGS})

if (APPLE)
    add_subdirectory(angle-project)

    list(APPEND GLIDEN64_CMAKE_ARGS
        -DEGL=ON
        -DANGLE_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/angle/include
        -DANGLE_STATIC_LIBRARY=${CMAKE_CURRENT_BINARY_DIR}/angle-project/angle-cmake/libangle_static.a
        -DANGLE_SPIRV_TOOLS_LIBRARY=${CMAKE_CURRENT_BINARY_DIR}/angle-project/angle-cmake/spirv-tools/source/libSPIRV-Tools.a
    )

    set(ANGLE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/angle/include" PARENT_SCOPE)
endif()
# taken from GLideN64's CMakeLists.txt
if("${CMAKE_BUILD_TYPE}" STREQUAL "Release" OR
    "${CMAKE_BUILD_TYPE}" STREQUAL "MinSizeRel" OR
    "${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    set(GLIDEN64_BUILD_TYPE Release)
elseif("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    set(GLIDEN64_BUILD_TYPE Debug)
endif()

if (USE_ANGRYLION)
    ExternalProject_Add(mupen64plus-video-angrylion-plus
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-video-angrylion-plus

        INSTALL_COMMAND ""

        BUILD_IN_SOURCE False
        BUILD_ALWAYS True
        CMAKE_ARGS -DBUILD_MUPEN64PLUS=ON -DBUILD_PROJECT64=OFF -DUSE_QT5=OFF -DUSE_IPO=OFF ${CMAKE_CONFIGURE_ARGS}
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-video-angrylion-plus

        BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-video-angrylion-plus/mupen64plus-video-angrylion-plus.${SO_EXT}
    )
endif(USE_ANGRYLION)

if (APPLE)
    set(GLIDEN64_EXTERNAL_DEPENDS angle_static)
else()
    set(GLIDEN64_EXTERNAL_DEPENDS "")
endif()

ExternalProject_Add(mupen64plus-video-GLideN64
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-video-GLideN64

    INSTALL_COMMAND ""

    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    CMAKE_ARGS ${GLIDEN64_CMAKE_ARGS}
    DEPENDS ${GLIDEN64_EXTERNAL_DEPENDS}

    SOURCE_SUBDIR ./src/
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-video-GLideN64

    BUILD_BYPRODUCTS ${GLIDEN64_BUILD_DIR}/plugin/${GLIDEN64_BUILD_TYPE}/mupen64plus-video-GLideN64.${SO_EXT}
)

ExternalProject_Add(mupen64plus-video-parallel
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mupen64plus-video-parallel

    INSTALL_COMMAND ""

    BUILD_IN_SOURCE False
    BUILD_ALWAYS True
    CMAKE_ARGS -DCONFIG_GUI=ON -DUSE_IPO=OFF ${CMAKE_CONFIGURE_ARGS}
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-video-parallel

    BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/mupen64plus-video-parallel/mupen64plus-video-parallel.${SO_EXT}
)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp 
    ${IMGUI_DIR}/imgui_widgets.cpp 
    ${IMGUI_DIR}/imgui.cpp 
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.h 
    ${IMGUI_DIR}/backends/imgui_impl_opengl3_loader.h 
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PRIVATE ${IMGUI_DIR})

set(SDL_GAMECONTROLLERDB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL_GameControllerDB)
set(SDL_GAMECONTROLLERDB_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/SDL_GameControllerDB)
set(SDL_GAMECONTROLLERDB_SOURCES_IN
    ${SDL_GAMECONTROLLERDB_DIR}/gamecontrollerdb.txt
)
set(SDL_GAMECONTROLLERDB_SOURCES_OUT
    ${SDL_GAMECONTROLLERDB_BUILD_DIR}/gamecontrollerdb.txt
)
ExternalProject_Add(SDL_GameControllerDB
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL_GameControllerDB

    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E make_directory ${SDL_GAMECONTROLLERDB_BUILD_DIR}
    BUILD_COMMAND ${CMAKE_COMMAND} -E copy ${SDL_GAMECONTROLLERDB_SOURCES_IN} ${SDL_GAMECONTROLLERDB_SOURCES_OUT}
    INSTALL_COMMAND ""

    BUILD_BYPRODUCTS ${SDL_GAMECONTROLLERDB_SOURCES_OUT}
)

# libdatachannel integration (for WebRTC P2P netplay)
if (NETPLAY)
    set(LIBDATACHANNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libdatachannel")
    
    # MinGW-specific flags for libdatachannel
    set(LIBDATACHANNEL_CMAKE_ARGS ${CMAKE_CONFIGURE_ARGS})
    
    if (WIN32 AND MINGW)
        # MinGW on Windows needs special handling
        list(APPEND LIBDATACHANNEL_CMAKE_ARGS
            -DCMAKE_SYSTEM_NAME=Windows
            -DCMAKE_RC_COMPILER="${CMAKE_RC_COMPILER}"
        )
    endif()
    
    # Always prefer system OpenSSL, nlohmann-json, and libSRTP
    # Disable examples/tests to avoid unnecessary example linking failures
    list(APPEND LIBDATACHANNEL_CMAKE_ARGS
        -DUSE_GNUTLS=OFF
        -DUSE_MBEDTLS=OFF
        -DUSE_OPENSSL=ON
        -DUSE_SYSTEM_SRTP=ON
        -DNO_EXAMPLES=ON
        -DNO_TESTS=ON
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    )

    # libdatachannel defaults to shared libs; RMG links the static archive on Unix
    # and MSVC. MinGW keeps shared so we get libdatachannel.dll + libdatachannel.dll.a.
    if (NOT (WIN32 AND MINGW))
        list(APPEND LIBDATACHANNEL_CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF)
    endif()
    
    ExternalProject_Add(libdatachannel
        SOURCE_DIR ${LIBDATACHANNEL_DIR}
        
        INSTALL_COMMAND ""
        
        BUILD_IN_SOURCE False
        BUILD_ALWAYS False
        CMAKE_ARGS ${LIBDATACHANNEL_CMAKE_ARGS}
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel
        
        BUILD_BYPRODUCTS 
            ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.a
            ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.lib
            ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.dll.a
            ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.dll
    )
    
    # Create imported library target for linking
    add_library(libdatachannel_external STATIC IMPORTED GLOBAL)
    add_dependencies(libdatachannel_external libdatachannel)
    
    # Handle platform-specific library naming
    if (WIN32 AND MINGW)
        # MinGW uses .dll.a for import libraries
        set_target_properties(libdatachannel_external PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.dll.a"
        )
    elseif (WIN32)
        # MSVC uses .lib
        set_target_properties(libdatachannel_external PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.lib"
        )
    else()
        # Unix-like systems use .a
        set_target_properties(libdatachannel_external PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.a"
        )
    endif()
    
    # Set include directories
    set_target_properties(libdatachannel_external PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBDATACHANNEL_DIR}/include"
    )

    # Static libdatachannel does not carry bundled deps or OpenSSL/libSRTP; propagate them.
    if (NOT (WIN32 AND MINGW))
        list(APPEND CMAKE_MODULE_PATH "${LIBDATACHANNEL_DIR}/cmake/Modules")
        find_package(OpenSSL REQUIRED)
        find_package(Threads REQUIRED)
        find_package(libSRTP REQUIRED)
        if (NOT TARGET libSRTP::srtp2)
            add_library(libSRTP::srtp2 UNKNOWN IMPORTED)
            set_target_properties(libSRTP::srtp2 PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SRTP_INCLUDE_DIRS}"
                IMPORTED_LINK_INTERFACE_LANGUAGES C
                IMPORTED_LOCATION "${SRTP_LIBRARIES}")
        endif()
        set(_LIBDATACHANNEL_DEPS
            OpenSSL::SSL
            libSRTP::srtp2
            Threads::Threads
            "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/deps/libjuice/libjuice.a"
            "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/deps/usrsctp/usrsctplib/libusrsctp.a"
        )
        set_target_properties(libdatachannel_external PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_LIBDATACHANNEL_DEPS}"
        )
    endif()
    
endif(NETPLAY)

ExternalProject_Get_property(mupen64plus-core BUILD_BYPRODUCTS)
set(MUPEN64PLUSCORE_LIB ${BUILD_BYPRODUCTS} PARENT_SCOPE)
set(MUPEN64PLUSCORE_INI ${M64P_CORE_DIR}/data/mupen64plus.ini PARENT_SCOPE)

ExternalProject_Get_property(mupen64plus-rsp-cxd4 BUILD_BYPRODUCTS)
set(MUPEN64PLUS_PLUGIN_RSP_CXD4 ${BUILD_BYPRODUCTS} PARENT_SCOPE)

ExternalProject_Get_property(mupen64plus-rsp-hle BUILD_BYPRODUCTS)
set(MUPEN64PLUS_PLUGIN_RSP_HLE ${BUILD_BYPRODUCTS} PARENT_SCOPE)

ExternalProject_Get_property(mupen64plus-rsp-parallel BUILD_BYPRODUCTS)
set(MUPEN64PLUS_PLUGIN_RSP_PARALLEL ${BUILD_BYPRODUCTS} PARENT_SCOPE)

ExternalProject_Get_property(mupen64plus-input-raphnetraw BUILD_BYPRODUCTS)
set(MUPEN64PLUS_PLUGIN_INPUT_RAPHNET ${BUILD_BYPRODUCTS} PARENT_SCOPE)

if (USE_ANGRYLION)
    ExternalProject_Get_property(mupen64plus-video-angrylion-plus BUILD_BYPRODUCTS)
    set(MUPEN64PLUS_PLUGIN_GFX_ANGRYLION ${BUILD_BYPRODUCTS} PARENT_SCOPE)
endif(USE_ANGRYLION)

ExternalProject_Get_property(mupen64plus-video-GLideN64 BUILD_BYPRODUCTS)
set(MUPEN64PLUS_PLUGIN_GFX_GLIDEN64 ${BUILD_BYPRODUCTS} PARENT_SCOPE)
set(MUPEN64PLUS_PLUGIN_GFX_GLIDEN64_DATA ${GLIDEN64_CUSTOM_INI} PARENT_SCOPE)

ExternalProject_Get_property(mupen64plus-video-parallel BUILD_BYPRODUCTS)
set(MUPEN64PLUS_PLUGIN_GFX_PARALLEL ${BUILD_BYPRODUCTS} PARENT_SCOPE)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui PARENT_SCOPE)

ExternalProject_Get_property(SDL_GameControllerDB BUILD_BYPRODUCTS)
set(SDL_GAMECONTROLLERDB ${BUILD_BYPRODUCTS} PARENT_SCOPE)

# libdatachannel exports
if (NETPLAY)
    set(LIBDATACHANNEL_EXTERNAL_TARGET libdatachannel_external PARENT_SCOPE)
    set(LIBDATACHANNEL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libdatachannel/include" PARENT_SCOPE)
    
    if (WIN32 AND MINGW)
        set(LIBDATACHANNEL_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.dll.a" PARENT_SCOPE)
    elseif (WIN32)
        set(LIBDATACHANNEL_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.lib" PARENT_SCOPE)
    else()
        set(LIBDATACHANNEL_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel/libdatachannel.a" PARENT_SCOPE)
    endif()
endif(NETPLAY)
