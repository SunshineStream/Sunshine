# windows specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="windows")

enable_language(RC)
set(CMAKE_RC_COMPILER windres)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

# gcc complains about misleading indentation in some mingw includes
list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-misleading-indentation)

# Disable warnings for Windows ARM64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-dll-attribute-on-redeclaration)  # Boost
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-unknown-warning-option)  # ViGEmClient
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-unused-variable)  # Boost

    # Qt's static qwindows plugin and MinGW's Windowsapp import library both provide the
    # UiaRaiseNotificationEvent import thunk on ARM64. Keep both required libraries while
    # permitting lld to coalesce their identical definitions.
    if(SUNSHINE_USE_STATIC_QT AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND SUNSHINE_LINK_OPTIONS -Wl,--allow-multiple-definition)
    endif()
endif()

# see gcc bug 98723
add_definitions(-DUSE_BOOST_REGEX)

# curl
add_definitions(-DCURL_STATICLIB)
include_directories(SYSTEM ${CURL_STATIC_INCLUDE_DIRS})
link_directories(${CURL_STATIC_LIBRARY_DIRS})

# miniupnpc
add_definitions(-DMINIUPNP_STATICLIB)

# extra tools/binaries for audio/display devices
add_subdirectory(tools)  # todo - this is temporary, only tools for Windows are needed, for now

# nvidia
CPMAddPackage(NAME nvapi DOWNLOAD_ONLY YES)
include_directories(SYSTEM "${nvapi_SOURCE_DIR}")
file(GLOB NVPREFS_FILES CONFIGURE_DEPENDS
        "${nvapi_SOURCE_DIR}/*.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.h")

# vigem
CPMAddPackage(NAME vigemclient DOWNLOAD_ONLY YES)
include_directories(SYSTEM "${vigemclient_SOURCE_DIR}/include")

# sunshine icon
if(NOT DEFINED SUNSHINE_ICON_PATH)
    set(SUNSHINE_ICON_PATH "${CMAKE_SOURCE_DIR}/sunshine.ico")
endif()

# Create a separate object library for the RC file with minimal includes
add_library(sunshine_rc_object OBJECT "${CMAKE_SOURCE_DIR}/src/platform/windows/windows.rc")

# Set minimal properties for RC compilation - only what's needed for the resource file
# Otherwise compilation can fail due to "line too long" errors
set_target_properties(sunshine_rc_object PROPERTIES
    COMPILE_DEFINITIONS "PROJECT_ICON_PATH=${SUNSHINE_ICON_PATH};PROJECT_NAME=${PROJECT_NAME};PROJECT_VENDOR=${SUNSHINE_PUBLISHER_NAME};PROJECT_VERSION=${PROJECT_VERSION};PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR};PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH};RC_VERSION_BUILD=${RC_VERSION_BUILD};RC_VERSION_REVISION=${RC_VERSION_REVISION}"  # cmake-lint: disable=C0301
    INCLUDE_DIRECTORIES ""
)

set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/windows/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/text_context.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_base.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_vram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_ram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_wgc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.h"
        "${vigemclient_SOURCE_DIR}/src/ViGEmClient.cpp"
        "${vigemclient_SOURCE_DIR}/include/ViGEm/Client.h"
        "${vigemclient_SOURCE_DIR}/include/ViGEm/Common.h"
        "${vigemclient_SOURCE_DIR}/include/ViGEm/Util.h"
        "${vigemclient_SOURCE_DIR}/include/ViGEm/km/BusShared.h"
        ${NVPREFS_FILES})

set(OPENSSL_LIBRARIES
        libssl.a
        libcrypto.a)

list(PREPEND PLATFORM_LIBRARIES
        ${CURL_STATIC_LIBRARIES}
        avrt
        d3d11
        D3DCompiler
        dwmapi
        dxgi
        iphlpapi
        ksuser
        libssp.a
        libstdc++.a
        libwinpthread.a
        minhook::minhook
        ntdll
        setupapi
        shlwapi
        synchronization.lib
        userenv
        ws2_32
        wsock32
)
