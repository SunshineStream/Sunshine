cmake_minimum_required(VERSION 3.24)
# `CMAKE_CUDA_ARCHITECTURES` requires 3.18
# `set_source_files_properties` requires 3.18
# `cmake_path(CONVERT ... TO_NATIVE_PATH_LIST ...)` requires 3.20
# `third-party/libvirtualhid` requires 3.24
# todo - set this conditionally

project(Sunshine VERSION 0.0.0
        DESCRIPTION "Self-hosted game stream host for Moonlight"
        HOMEPAGE_URL "https://sunshine.lizardbyte.dev")

# Minimum separately installed Virtual HID Broker version supported by this build.
set(LIBVIRTUALHID_MINIMUM_VERSION "2026.914.1218.10")
list(APPEND SUNSHINE_DEFINITIONS LIBVIRTUALHID_MINIMUM_VERSION="${LIBVIRTUALHID_MINIMUM_VERSION}")
if(APPLE)
    # macOS bundle versions have three components; the Windows driver uses four.
    if(LIBVIRTUALHID_MINIMUM_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)\\.[0-9]+$")
        set(LIBVIRTUALHID_MACOS_MINIMUM_VERSION "${CMAKE_MATCH_1}")
    else()
        set(LIBVIRTUALHID_MACOS_MINIMUM_VERSION "${LIBVIRTUALHID_MINIMUM_VERSION}")
    endif()
    list(APPEND SUNSHINE_DEFINITIONS LIBVIRTUALHID_MACOS_MINIMUM_VERSION="${LIBVIRTUALHID_MACOS_MINIMUM_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

set(PROJECT_LICENSE "GPL-3.0-only")

set(PROJECT_FQDN "dev.lizardbyte.app.Sunshine")

set(PROJECT_BRIEF_DESCRIPTION "GameStream host for Moonlight")  # must be <= 35 characters

set(PROJECT_LONG_DESCRIPTION "Offering low latency, cloud gaming server capabilities with support for AMD, Intel, \
and Nvidia GPUs for hardware encoding. Software encoding is also available. You can connect to Sunshine from any \
Moonlight client on a variety of devices. A web UI is provided to allow configuration, and client pairing, from \
your favorite web browser. Pair from the local server or any mobile device.")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(FREEBSD ON)
endif()

# set the module path, used for includes
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# CPM
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpm/CPM.cmake)
CPMUsePackageLock(package-lock.cmake)

# export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set version info for this build
include(${CMAKE_MODULE_PATH}/prep/build_version.cmake)

# cmake build flags
include(${CMAKE_MODULE_PATH}/prep/options.cmake)
include(${CMAKE_MODULE_PATH}/prep/static_qt.cmake)

# initial prep
include(${CMAKE_MODULE_PATH}/prep/init.cmake)

# configure special package files, such as sunshine.desktop, Flatpak manifest, Portfile , etc.
include(${CMAKE_MODULE_PATH}/prep/special_package_configuration.cmake)

# Exit early if END_BUILD is ON, i.e. when only generating package manifests
if(${END_BUILD})
    return()
endif()

# Build-deps owns the dependency versions shared with its FFmpeg builds.
# Include its lock only for full builds so manifest-only configurations do not
# require the build-deps submodule.
include(${CMAKE_CURRENT_SOURCE_DIR}/third-party/build-deps/package-lock.cmake)

# project constants
include(${CMAKE_MODULE_PATH}/prep/constants.cmake)

# load macros
include(${CMAKE_MODULE_PATH}/macros/common.cmake)

# load dependencies
include(${CMAKE_MODULE_PATH}/dependencies/common.cmake)

# setup compile definitions
include(${CMAKE_MODULE_PATH}/compile_definitions/common.cmake)

# target definitions
include(${CMAKE_MODULE_PATH}/targets/common.cmake)

# packaging
include(${CMAKE_MODULE_PATH}/packaging/common.cmake)
