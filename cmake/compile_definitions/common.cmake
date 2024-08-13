# common compile definitions
# this file will also load platform specific definitions

list(APPEND SUNSHINE_COMPILE_OPTIONS -Wall -Wno-sign-compare)
# Wall - enable all warnings
# Werror - treat warnings as errors
# Wno-maybe-uninitialized/Wno-uninitialized - disable warnings for maybe uninitialized variables
# Wno-sign-compare - disable warnings for signed/unsigned comparisons
# Wno-restrict - disable warnings for memory overlap
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options

    # GCC 12 and higher will complain about maybe-uninitialized
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-maybe-uninitialized)

        # Disable the bogus warning that may prevent compilation (only for GCC 12).
        # See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
            list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-restrict)
        endif()
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang specific compile options

    # Clang doesn't actually complain about this this, so disabling for now
    # list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)
endif()
if(BUILD_WERROR)
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Werror)
endif()

# setup assets directory
if(NOT SUNSHINE_ASSETS_DIR)
    set(SUNSHINE_ASSETS_DIR "assets")
endif()

# platform specific compile definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/compile_definitions/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/compile_definitions/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/compile_definitions/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/compile_definitions/linux.cmake)
    endif()
endif()

include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nv-codec-headers/include")
file(GLOB NVENC_SOURCES CONFIGURE_DEPENDS "src/nvenc/*.cpp" "src/nvenc/*.h")
list(APPEND PLATFORM_TARGET_FILES ${NVENC_SOURCES})

configure_file("${CMAKE_SOURCE_DIR}/src/version.h.in" version.h @ONLY)
include_directories("${CMAKE_CURRENT_BINARY_DIR}")  # required for importing version.h

file(GLOB_RECURSE COMMON_SOURCES
        ${CMAKE_SOURCE_DIR}/src/**)
list(FILTER COMMON_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/platform/.*")

set(SUNSHINE_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Input.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Rtsp.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/RtspParser.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Video.h"
        "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray.h"
        ${COMMON_SOURCES}
        ${PLATFORM_TARGET_FILES})

if(NOT SUNSHINE_ASSETS_DIR_DEF)
    set(SUNSHINE_ASSETS_DIR_DEF "${SUNSHINE_ASSETS_DIR}")
endif()
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ASSETS_DIR="${SUNSHINE_ASSETS_DIR_DEF}")

list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_TRAY=${SUNSHINE_TRAY})

include_directories("${CMAKE_SOURCE_DIR}")

include_directories(
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl"
        ${FFMPEG_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}  # has to be the last, or we get runtime error on macOS ffmpeg encoder
)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${MINIUPNP_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        enet
        opus
        ${FFMPEG_LIBRARIES}
        ${Boost_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES})
