#
# CMake project wrapper for building ANGLE (Metal backend) on macOS.
# Uses the angle-cmake sources with the google/angle submodule.
#
cmake_minimum_required(VERSION 3.16)

if(NOT APPLE)
    return()
endif()

set(ANGLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../angle")
set(ANGLE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/../angle-cmake")
set(ANGLE_BUILD_SUBDIR "${CMAKE_CURRENT_BINARY_DIR}/angle-cmake")

if(NOT EXISTS "${ANGLE_ROOT}/include/EGL/egl.h")
    message(FATAL_ERROR "ANGLE submodule not found at ${ANGLE_ROOT}")
endif()

set(ANGLE_BACKEND Metal CACHE STRING "ANGLE GPU backend")
set(ANGLE_SKIP_VK_HASH_CHECK ON CACHE BOOL "Skip Vulkan vk_cache_utils hash check")
set(CHROMIUM_ZLIB_ROOT "${ANGLE_ROOT}/third_party/zlib")
set(ANGLE_SPIRV_HEADERS_ROOT "${ANGLE_ROOT}/third_party/spirv-headers/src")
set(ANGLE_SPIRV_TOOLS_ROOT "${ANGLE_ROOT}/third_party/spirv-tools/src")
set(ANGLE_GLSLANG_ROOT "${ANGLE_ROOT}/third_party/glslang/src")
set(ANGLE_SPIRV_CROSS_ROOT "${ANGLE_ROOT}/third_party/spirv-cross/src")

if(NOT EXISTS "${CHROMIUM_ZLIB_ROOT}/adler32.c"
    OR NOT EXISTS "${ANGLE_GLSLANG_ROOT}/glslang/Public/ShaderLang.h"
    OR NOT EXISTS "${ANGLE_SPIRV_HEADERS_ROOT}/include/spirv/unified1/spirv.h"
    OR NOT EXISTS "${ANGLE_SPIRV_TOOLS_ROOT}/source/CMakeLists.txt"
    OR NOT EXISTS "${ANGLE_SPIRV_CROSS_ROOT}/spirv_cross.hpp")
    message(STATUS "ANGLE: fetching Metal-only third_party dependencies")
    execute_process(
        COMMAND "${CMAKE_SOURCE_DIR}/Source/Script/SyncANGLEDeps.sh" "${ANGLE_ROOT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _angle_sync_result
    )
    if(NOT _angle_sync_result EQUAL 0
        OR NOT EXISTS "${CHROMIUM_ZLIB_ROOT}/adler32.c"
        OR NOT EXISTS "${ANGLE_GLSLANG_ROOT}/glslang/Public/ShaderLang.h"
        OR NOT EXISTS "${ANGLE_SPIRV_HEADERS_ROOT}/include/spirv/unified1/spirv.h"
        OR NOT EXISTS "${ANGLE_SPIRV_TOOLS_ROOT}/source/CMakeLists.txt"
        OR NOT EXISTS "${ANGLE_SPIRV_CROSS_ROOT}/spirv_cross.hpp")
        message(FATAL_ERROR "ANGLE Metal dependency sync failed (${_angle_sync_result})")
    endif()
endif()

add_subdirectory("${ANGLE_CMAKE_DIR}" "${ANGLE_BUILD_SUBDIR}")

set(SPIRV-Headers_SOURCE_DIR "${ANGLE_SPIRV_HEADERS_ROOT}")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${ANGLE_SPIRV_TOOLS_ROOT}" "${ANGLE_BUILD_SUBDIR}/spirv-tools")

target_compile_options(angle_static PRIVATE
    -Wno-nullability-completeness
    -Wno-unguarded-availability
)

target_link_libraries(angle_static PUBLIC
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework IOSurface"
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework AppKit"
)

add_library(ANGLE::Static ALIAS angle_static)

set(ANGLE_INCLUDE_DIR "${ANGLE_ROOT}/include" PARENT_SCOPE)
set(ANGLE_STATIC_LIBRARY "$<TARGET_FILE:angle_static>" PARENT_SCOPE)
set(ANGLE_STATIC_TARGET angle_static PARENT_SCOPE)
