# CPM Package Lock
# This file should be committed to version control

# The first argument of CPMDeclarePackage can be freely chosen and is used as argument in CPMGetPackage.
# The NAME argument should be package name that would also be used in a find_package call.
# Ideally, both are the same, which might not always be possible: https://github.com/cpm-cmake/CPM.cmake/issues/603
# This is needed to support CPM_USE_LOCAL_PACKAGES

# Renovate-bot will update the versions and hashes in this file when a new version of a dependency is released.
# The comments above each dependency are used by renovate to identify the dependencies and extract the version numbers.
# See https://github.com/LizardByte/.github/blob/master/renovate/cmake.json5 for the configuration of renovate.
# See https://github.com/LizardByte/.github/blob/master/renovate/README.md#cpm-package-lock for annotation instructions.

set(PATCH_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/patches")

# Boost
# renovate: datasource=github-release-attachments depName=boostorg/boost
# versioning=regex:^boost-(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)(?<prerelease>\.[A-Za-z0-9.-]+)?$
set(BOOST_TAG boost-1.92.0)
set(BOOST_SHA256 9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d)
string(REGEX REPLACE "^boost-" "" BOOST_VERSION "${BOOST_TAG}")
CPMDeclarePackage(Boost
        NAME Boost
        VERSION ${BOOST_VERSION}
        URL https://github.com/boostorg/boost/releases/download/${BOOST_TAG}/${BOOST_TAG}-cmake.tar.xz
        URL_HASH SHA256=${BOOST_SHA256}
        DOWNLOAD_ONLY YES
)

# Build-deps supplies the FFmpeg binaries and the NV codec and Vulkan header pins.
# renovate: datasource=github-releases depName=LizardByte/build-deps
set(BUILD_DEPS_TAG v2026.1017.131201)
CPMDeclarePackage(build_deps
        NAME build_deps
        GIT_REPOSITORY https://github.com/LizardByte/build-deps.git
        GIT_TAG ${BUILD_DEPS_TAG}
        GIT_SHALLOW TRUE
        # The pinned release still carries Vulkan Headers as a submodule.
        GIT_CONFIG "submodule.active=third-party/FFmpeg/Vulkan-Headers"
        DOWNLOAD_ONLY YES
)

# glad
# renovate: datasource=github-tags depName=Dav1dde/glad
set(GLAD_TAG v2.0.8)
CPMDeclarePackage(glad
        NAME glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG ${GLAD_TAG}
        GIT_SHALLOW TRUE
        DOWNLOAD_ONLY YES
)

# libdisplaydevice
# renovate: datasource=github-tags depName=LizardByte/libdisplaydevice
set(LIBDISPLAYDEVICE_TAG v2026.1008.124543)
CPMDeclarePackage(libdisplaydevice
        NAME libdisplaydevice
        GIT_REPOSITORY https://github.com/LizardByte/libdisplaydevice.git
        GIT_TAG ${LIBDISPLAYDEVICE_TAG}
        GIT_SHALLOW TRUE
        GIT_CONFIG "submodule.active=:(exclude)third-party"
)

# libvirtualhid
# renovate: datasource=git-refs depName=LizardByte/libvirtualhid
# ref=master
set(LIBVIRTUALHID_COMMIT dd8153df338c6fb399ea4a048fa97b4c536f12a0)
CPMDeclarePackage(libvirtualhid
        NAME libvirtualhid
        GIT_REPOSITORY https://github.com/LizardByte/libvirtualhid.git
        GIT_TAG ${LIBVIRTUALHID_COMMIT}
        # Windows compiles a source file from this submodule.
        GIT_SUBMODULES third-party/lizardbyte-common
        GIT_SUBMODULES_RECURSE FALSE
)

# moonlight-common-c
# renovate: datasource=git-refs depName=moonlight-stream/moonlight-common-c
# ref=master
set(MOONLIGHT_COMMON_C_COMMIT 94c65fd364d17b223f00a8763c9a5d94a66cb8dd)
CPMDeclarePackage(moonlight-common-c
        NAME moonlight-common-c
        GIT_REPOSITORY https://github.com/moonlight-stream/moonlight-common-c.git
        GIT_TAG ${MOONLIGHT_COMMON_C_COMMIT}
        GIT_SUBMODULES enet nanors
        DOWNLOAD_ONLY YES
)

# NVIDIA NVAPI
# renovate: datasource=git-refs depName=NVIDIA/nvapi
# ref=main
set(NVAPI_COMMIT 51e5aae4bc40c3ccd3fb7d4dfd94256e7bfc496e)
CPMDeclarePackage(nvapi
        NAME nvapi
        GIT_REPOSITORY https://github.com/NVIDIA/nvapi.git
        GIT_TAG ${NVAPI_COMMIT}
        DOWNLOAD_ONLY YES
)

# Plasma Wayland protocols
# renovate: datasource=github-tags depName=KDE/plasma-wayland-protocols
set(PLASMA_WAYLAND_PROTOCOLS_TAG v1.23.0)
CPMDeclarePackage(plasma_wayland_protocols
        NAME plasma_wayland_protocols
        GIT_REPOSITORY https://github.com/KDE/plasma-wayland-protocols.git
        GIT_TAG ${PLASMA_WAYLAND_PROTOCOLS_TAG}
        GIT_SHALLOW TRUE
        DOWNLOAD_ONLY YES
)

# Simple-Web-Server
# renovate: datasource=git-refs depName=LizardByte-infrastructure/Simple-Web-Server
# ref=master
set(SIMPLE_WEB_SERVER_COMMIT 546895a93a29062bb178367b46c7afb72da9881e)
CPMDeclarePackage(simple_web_server
        NAME simple_web_server
        GIT_REPOSITORY https://github.com/LizardByte-infrastructure/Simple-Web-Server.git
        GIT_TAG ${SIMPLE_WEB_SERVER_COMMIT}
)

# TPCircularBuffer
# renovate: datasource=git-refs depName=michaeltyson/TPCircularBuffer
# ref=master
set(TPCIRCULARBUFFER_COMMIT cc520397504bb72bc6df79ff03eb72988a6dc50d)
CPMDeclarePackage(tpcircularbuffer
        NAME tpcircularbuffer
        GIT_REPOSITORY https://github.com/michaeltyson/TPCircularBuffer.git
        GIT_TAG ${TPCIRCULARBUFFER_COMMIT}
        DOWNLOAD_ONLY YES
)

# System tray
# renovate: datasource=github-tags depName=LizardByte/tray
set(TRAY_TAG v2026.1117.200659)
CPMDeclarePackage(tray
        NAME tray
        GIT_REPOSITORY https://github.com/LizardByte/tray.git
        GIT_TAG ${TRAY_TAG}
        GIT_SHALLOW TRUE
        GIT_CONFIG "submodule.active=:(exclude)third-party"
)

# ViGEmClient
# renovate: datasource=git-refs depName=LizardByte/Virtual-Gamepad-Emulation-Client
# ref=master
set(VIGEMCLIENT_COMMIT 8d71f6740ffff4671cdadbca255ce528e3cd3fef)
CPMDeclarePackage(vigemclient
        NAME vigemclient
        GIT_REPOSITORY https://github.com/LizardByte/Virtual-Gamepad-Emulation-Client.git
        GIT_TAG ${VIGEMCLIENT_COMMIT}
        DOWNLOAD_ONLY YES
)

# Wayland protocols
# renovate: datasource=github-tags depName=wayland/wayland-protocols
set(WAYLAND_PROTOCOLS_TAG 1.50)
CPMDeclarePackage(wayland_protocols
        NAME wayland_protocols
        GIT_REPOSITORY https://github.com/LizardByte-infrastructure/wayland-protocols.git
        GIT_TAG ${WAYLAND_PROTOCOLS_TAG}
        GIT_SHALLOW TRUE
        DOWNLOAD_ONLY YES
)

# wlroots protocols
# renovate: datasource=git-refs depName=LizardByte-infrastructure/wlr-protocols
# ref=master
set(WLR_PROTOCOLS_COMMIT 1106a4a1c57562ddd438c547da4c0d6dc150d624)
CPMDeclarePackage(wlr_protocols
        NAME wlr_protocols
        GIT_REPOSITORY https://github.com/LizardByte-infrastructure/wlr-protocols.git
        GIT_TAG ${WLR_PROTOCOLS_COMMIT}
        DOWNLOAD_ONLY YES
)
