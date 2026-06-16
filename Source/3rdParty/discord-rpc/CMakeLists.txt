cmake_minimum_required (VERSION 3.2.0)
project (DiscordRPC)

include(GNUInstallDirs)

option(BUILD_EXAMPLES "Build example apps" ON)

# format
file(GLOB_RECURSE ALL_SOURCE_FILES
    examples/*.cpp examples/*.h examples/*.c
    include/*.h
    src/*.cpp src/*.h src/*.c
)

# Set CLANG_FORMAT_SUFFIX if you are using custom clang-format, e.g. clang-format-5.0
find_program(CLANG_FORMAT_CMD clang-format${CLANG_FORMAT_SUFFIX})

if (CLANG_FORMAT_CMD)
    add_custom_target(
        clangformat
        COMMAND ${CLANG_FORMAT_CMD}
        -i -style=file -fallback-style=none
        ${ALL_SOURCE_FILES}
        DEPENDS
        ${ALL_SOURCE_FILES}
    )
endif(CLANG_FORMAT_CMD)

# thirdparty stuff
execute_process(
    COMMAND mkdir ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ERROR_QUIET
)

find_path(RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rapidjson-1.1.0/include
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rapidjson/include
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    NO_DEFAULT_PATH
)
if (NOT RAPIDJSON_INCLUDE_DIR)
    message("no rapidjson, download")
    set(RJ_TAR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/v1.1.0.tar.gz)
    file(DOWNLOAD https://github.com/miloyip/rapidjson/archive/v1.1.0.tar.gz ${RJ_TAR_FILE})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${RJ_TAR_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    )
    file(REMOVE ${RJ_TAR_FILE})
    find_path(RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h
        PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rapidjson-1.1.0/include
        NO_DEFAULT_PATH
    )
endif(NOT RAPIDJSON_INCLUDE_DIR)

set(RAPIDJSON ${RAPIDJSON_INCLUDE_DIR})

add_library(rapidjson STATIC IMPORTED ${RAPIDJSON})

set(DISCORD_RPC_RAPIDJSON_SOURCE_DIR ${RAPIDJSON})
set(RAPIDJSON ${CMAKE_CURRENT_BINARY_DIR}/rapidjson)
file(COPY ${DISCORD_RPC_RAPIDJSON_SOURCE_DIR}/rapidjson DESTINATION ${RAPIDJSON}/include)

set(DISCORD_RPC_RAPIDJSON_DOCUMENT ${RAPIDJSON}/include/rapidjson/document.h)
file(READ ${DISCORD_RPC_RAPIDJSON_DOCUMENT} DISCORD_RPC_RAPIDJSON_DOCUMENT_CONTENT)
string(REPLACE "    GenericStringRef& operator=(const GenericStringRef& rhs) { s = rhs.s; length = rhs.length; }" "    GenericStringRef& operator=(const GenericStringRef& rhs) = delete;" DISCORD_RPC_RAPIDJSON_DOCUMENT_CONTENT "${DISCORD_RPC_RAPIDJSON_DOCUMENT_CONTENT}")
file(WRITE ${DISCORD_RPC_RAPIDJSON_DOCUMENT} "${DISCORD_RPC_RAPIDJSON_DOCUMENT_CONTENT}")

# add subdirs

add_subdirectory(src)
if (BUILD_EXAMPLES)
    add_subdirectory(examples/send-presence)
endif(BUILD_EXAMPLES)
