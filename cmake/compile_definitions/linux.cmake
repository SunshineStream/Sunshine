# linux specific compile definitions

if(FREEBSD)
    add_compile_definitions(SUNSHINE_PLATFORM="freebsd")
    # FreeBSD installs packages to /usr/local/lib, which is not in the default linker search path.
    # link_directories() is directory-scoped and propagates to all subdirectories (including tests/),
    # so all targets (sunshine, test_sunshine) can resolve libraries found via pkg_check_modules.
    link_directories(/usr/local/lib)
else()
    add_compile_definitions(SUNSHINE_PLATFORM="linux")
endif()

# AppImage
if(${SUNSHINE_BUILD_APPIMAGE})
    # use relative assets path for AppImage
    string(REPLACE "${CMAKE_INSTALL_PREFIX}" ".${CMAKE_INSTALL_PREFIX}" SUNSHINE_ASSETS_DIR_DEF ${SUNSHINE_ASSETS_DIR})
endif()

# cuda
set(CUDA_FOUND OFF)
if(${SUNSHINE_ENABLE_CUDA})
    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        set(CUDA_FOUND ON)
        enable_language(CUDA)

        message(STATUS "CUDA Compiler Version: ${CMAKE_CUDA_COMPILER_VERSION}")
        set(CMAKE_CUDA_ARCHITECTURES "")

        # https://docs.nvidia.com/cuda/archive/12.0.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 75 80 86 87 89 90)
        else()
            message(FATAL_ERROR
                    "Sunshine requires a minimum CUDA Compiler version of 12.0.
                    Found version: ${CMAKE_CUDA_COMPILER_VERSION}"
            )
        endif()

        # https://docs.nvidia.com/cuda/archive/12.8.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 100 101 120)
        endif()

        # https://docs.nvidia.com/cuda/archive/12.9.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.9)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 103 121)
        endif()

        # https://docs.nvidia.com/cuda/archive/13.0.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
            list(REMOVE_ITEM CMAKE_CUDA_ARCHITECTURES 101)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 110)
        else()
            list(APPEND CMAKE_CUDA_ARCHITECTURES 50 52 53 60 61 62 70 72)
        endif()

        # sort the architectures
        list(SORT CMAKE_CUDA_ARCHITECTURES COMPARE NATURAL)

        # message(STATUS "CUDA NVCC Flags: ${CUDA_NVCC_FLAGS}")
        message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    elseif(${CUDA_FAIL_ON_MISSING})
        message(FATAL_ERROR
                "CUDA not found.
                If this is intentional, set '-DSUNSHINE_ENABLE_CUDA=OFF' or '-DCUDA_FAIL_ON_MISSING=OFF'"
        )
    endif()
endif()
if(CUDA_FOUND)
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvfbc")
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda_color.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda_gl_textures.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cu"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/nvfbc/NvFBC.h")

    add_compile_definitions(SUNSHINE_BUILD_CUDA)
endif()

# libdrm headers are required for DRM (KMS), Wayland, Vulkan, and PipeWire
# (KWin, Portal). The library itself is linked for DRM (KMS) and for Wayland,
# where capture queries the render node's PCI identity to choose between
# zero-copy VRAM and the RAM bridge.
if(${SUNSHINE_ENABLE_DRM} OR ${SUNSHINE_ENABLE_WAYLAND} OR ${SUNSHINE_ENABLE_VULKAN}
   OR ${SUNSHINE_ENABLE_KWIN} OR ${SUNSHINE_ENABLE_PORTAL})
    find_package(LIBDRM REQUIRED)
else()
    set(LIBDRM_FOUND OFF)
endif()
if(LIBDRM_FOUND)
    include_directories(SYSTEM ${LIBDRM_INCLUDE_DIRS})
    if(${SUNSHINE_ENABLE_DRM} OR ${SUNSHINE_ENABLE_WAYLAND})
        list(APPEND PLATFORM_LIBRARIES ${LIBDRM_LIBRARIES})
    endif()
    if(${SUNSHINE_ENABLE_DRM})
        add_compile_definitions(SUNSHINE_BUILD_DRM)
        list(APPEND PLATFORM_TARGET_FILES
                "${CMAKE_SOURCE_DIR}/src/platform/linux/kmsgrab.cpp")
        list(APPEND SUNSHINE_DEFINITIONS EGL_NO_X11=1)
    endif()
endif()

# Capabilities
if(LINUX)
    find_package(LIBCAP REQUIRED)
    include_directories(SYSTEM ${LIBCAP_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBCAP_LIBRARIES})
endif()

# evdev
include(dependencies/libevdev_Sunshine)

# vaapi
# FFmpeg's static archives need these compatibility symbols even when Sunshine's
# VA-API encoder is disabled. The host's core and DRM libraries are always linked.
include_directories(SYSTEM ${LIBVA_INCLUDE_DIR})
list(APPEND PLATFORM_TARGET_FILES "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi_compat.cpp")
if(${SUNSHINE_ENABLE_VAAPI})
    find_package(Libva REQUIRED)
else()
    set(LIBVA_FOUND OFF)
endif()
if(LIBVA_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_VAAPI)
    include_directories(SYSTEM ${LIBVA_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${LIBVA_LIBRARIES} ${LIBVA_DRM_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.cpp")
endif()

# vulkan video encoding (via FFmpeg)
if(${SUNSHINE_ENABLE_VULKAN})
    if(SUNSHINE_SYSTEM_VULKAN_HEADERS)
        find_package(VulkanHeaders REQUIRED)
        get_target_property(VULKAN_HEADERS_DIR Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    elseif(EXISTS "${FFMPEG_PREPARED_BINARIES}/include/vulkan/vulkan.h")
        set(VULKAN_HEADERS_DIR "${FFMPEG_INCLUDE_DIRS}")
    else()
        include("${CMAKE_MODULE_PATH}/dependencies/build_deps.cmake")
        if(DEFINED CPM_DECLARATION_vulkan_headers)
            # Keep the package declaration owned by build-deps.
            set(SUNSHINE_CPM_PACKAGE_LOCK_ENABLED "${CPM_PACKAGE_LOCK_ENABLED}")
            set(CPM_PACKAGE_LOCK_ENABLED OFF)
            CPMAddPackage(NAME vulkan_headers DOWNLOAD_ONLY YES)
            set(CPM_PACKAGE_LOCK_ENABLED "${SUNSHINE_CPM_PACKAGE_LOCK_ENABLED}")
            unset(SUNSHINE_CPM_PACKAGE_LOCK_ENABLED)
            set(VULKAN_HEADERS_DIR "${vulkan_headers_SOURCE_DIR}/include")
        else()
            # Older build-deps releases pin Vulkan Headers as a Git submodule.
            set(VULKAN_HEADERS_DIR "${build_deps_SOURCE_DIR}/third-party/FFmpeg/Vulkan-Headers/include")
        endif()
    endif()

    if(NOT EXISTS "${VULKAN_HEADERS_DIR}/vulkan/vulkan.h")
        message(FATAL_ERROR "Vulkan headers not found in ${VULKAN_HEADERS_DIR}")
    endif()

    find_library(VULKAN_LIBRARY NAMES vulkan vulkan-1)
    if(NOT VULKAN_LIBRARY)
        message(FATAL_ERROR "libvulkan not found")
    endif()

    # prefer glslc, fall back to glslangValidator
    find_program(GLSLC_EXECUTABLE glslc)
    if(NOT GLSLC_EXECUTABLE)
        find_program(GLSLANG_EXECUTABLE glslangValidator)
    endif()
    if(NOT GLSLC_EXECUTABLE AND NOT GLSLANG_EXECUTABLE)
        message(FATAL_ERROR "Vulkan shader compiler not found (need glslc or glslangValidator)")
    endif()

    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_BUILD_VULKAN=1)
    include_directories(SYSTEM ${VULKAN_HEADERS_DIR})
    list(APPEND PLATFORM_LIBRARIES ${VULKAN_LIBRARY})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vulkan_encode.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vulkan_encode.cpp")

    # compile GLSL -> SPIR-V -> C include at build time
    set(VULKAN_SHADER_DIR "${CMAKE_BINARY_DIR}/generated-src/shaders")
    set(VULKAN_SHADER_SOURCE "${SUNSHINE_SOURCE_ASSETS_DIR}/linux/assets/shaders/vulkan/rgb2yuv.comp")
    set(VULKAN_SHADER_SPV "${VULKAN_SHADER_DIR}/rgb2yuv.spv")
    set(VULKAN_SHADER_DATA "${VULKAN_SHADER_DIR}/rgb2yuv.spv.inc")

    file(MAKE_DIRECTORY "${VULKAN_SHADER_DIR}")

    if(GLSLC_EXECUTABLE)
        add_custom_command(
                OUTPUT "${VULKAN_SHADER_SPV}"
                COMMAND ${GLSLC_EXECUTABLE} -O "${VULKAN_SHADER_SOURCE}" -o "${VULKAN_SHADER_SPV}"
                DEPENDS "${VULKAN_SHADER_SOURCE}"
                COMMENT "Compiling Vulkan shader rgb2yuv.comp (glslc)"
                VERBATIM)
    else()
        add_custom_command(
                OUTPUT "${VULKAN_SHADER_SPV}"
                COMMAND ${GLSLANG_EXECUTABLE} -V -o "${VULKAN_SHADER_SPV}" "${VULKAN_SHADER_SOURCE}"
                DEPENDS "${VULKAN_SHADER_SOURCE}"
                COMMENT "Compiling Vulkan shader rgb2yuv.comp (glslangValidator)"
                VERBATIM)
    endif()

    add_custom_command(
            OUTPUT "${VULKAN_SHADER_DATA}"
            COMMAND ${CMAKE_COMMAND} -DSPV_FILE=${VULKAN_SHADER_SPV} -DOUT_FILE=${VULKAN_SHADER_DATA}
                -P "${CMAKE_SOURCE_DIR}/cmake/scripts/binary_to_c.cmake"
            DEPENDS "${VULKAN_SHADER_SPV}"
            COMMENT "Generating C include from rgb2yuv.spv"
            VERBATIM)

    add_custom_target(vulkan_shaders
            DEPENDS "${VULKAN_SHADER_DATA}"
            COMMENT "Vulkan shader compilation")
    set(SUNSHINE_TARGET_DEPENDENCIES ${SUNSHINE_TARGET_DEPENDENCIES} vulkan_shaders)
endif()

# wayland
if(${SUNSHINE_ENABLE_WAYLAND})
    find_package(Wayland REQUIRED)
else()
    set(WAYLAND_FOUND OFF)
endif()
if(WAYLAND_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_WAYLAND)

    if(NOT SUNSHINE_SYSTEM_WAYLAND_PROTOCOLS)
        CPMAddPackage(NAME wayland_protocols DOWNLOAD_ONLY YES)
        set(WAYLAND_PROTOCOLS_DIR "${wayland_protocols_SOURCE_DIR}")
    else()
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols REQUIRED)
    endif()

    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/xdg-output" xdg-output-unstable-v1)
    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/linux-dmabuf" linux-dmabuf-unstable-v1)
    CPMAddPackage(NAME wlr_protocols DOWNLOAD_ONLY YES)
    GEN_WAYLAND("${wlr_protocols_SOURCE_DIR}" "unstable" wlr-screencopy-unstable-v1)

    include_directories(
            SYSTEM
            ${WAYLAND_INCLUDE_DIRS}
    )

    list(APPEND PLATFORM_LIBRARIES ${WAYLAND_LIBRARIES} gbm)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wlgrab.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.cpp")
endif()

# x11
if(${SUNSHINE_ENABLE_X11})
    find_package(X11 REQUIRED)
else()
    set(X11_FOUND OFF)
endif()
if(X11_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_X11)
    include_directories(SYSTEM ${X11_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${X11_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.cpp")
endif()

# GIO
pkg_check_modules(GIO gio-2.0 gio-unix-2.0 REQUIRED)
if(GIO_FOUND)
    include_directories(SYSTEM ${GIO_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${GIO_LIBRARIES})
endif()

# Pipewire
if(${SUNSHINE_ENABLE_KWIN} OR ${SUNSHINE_ENABLE_PORTAL})
    pkg_check_modules(PIPEWIRE libpipewire-0.3 REQUIRED)
else()
    set(PIPEWIRE_FOUND OFF)
endif()
if(PIPEWIRE_FOUND)
    include_directories(SYSTEM ${PIPEWIRE_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${PIPEWIRE_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/pipewire.cpp")
endif()

# XDG portal
set(PORTAL_FOUND OFF)
if(PIPEWIRE_FOUND AND GIO_FOUND AND ${SUNSHINE_ENABLE_PORTAL})
    set(PORTAL_FOUND ON)
    add_compile_definitions(SUNSHINE_BUILD_PORTAL)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/portalgrab.cpp")
endif()

# KWin ScreenCast (direct Wayland protocol, bypasses portal)
set(KWIN_FOUND OFF)
if(PIPEWIRE_FOUND AND WAYLAND_FOUND AND ${SUNSHINE_ENABLE_KWIN})
    set(KWIN_FOUND ON)
    add_compile_definitions(SUNSHINE_BUILD_KWIN)
    CPMAddPackage(NAME plasma_wayland_protocols DOWNLOAD_ONLY YES)
    GEN_WAYLAND("${plasma_wayland_protocols_SOURCE_DIR}/src/protocols" "" kde-output-order-v1)
    GEN_WAYLAND("${plasma_wayland_protocols_SOURCE_DIR}/src/protocols" "" zkde-screencast-unstable-v1)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/kwingrab.cpp")
elseif(${SUNSHINE_ENABLE_KWIN} AND NOT WAYLAND_FOUND)
    message(FATAL_ERROR "SUNSHINE_ENABLE_KWIN requires SUNSHINE_ENABLE_WAYLAND — KWin capture disabled")
endif()

if(NOT ${CUDA_FOUND}
        AND NOT ${LIBDRM_FOUND}
        AND NOT ${LIBVA_FOUND}
        AND NOT ${KWIN_FOUND}
        AND NOT ${PORTAL_FOUND}
        AND NOT ${WAYLAND_FOUND}
        AND NOT ${X11_FOUND})
    message(FATAL_ERROR "Couldn't find either cuda, libdrm, libva, kwin, pipewire, portal, wayland or x11")
endif()

# These need to be set before common.cmake adds the libvirtualhid subdirectory in order for them to be picked up
set(LIBEVDEV_CUSTOM_INCLUDE_DIR "${EVDEV_INCLUDE_DIR}")
set(LIBEVDEV_CUSTOM_LIBRARY "${EVDEV_LIBRARY}")

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/input/virtualhid.cpp")

# AppImage and Flatpak
if (${SUNSHINE_BUILD_APPIMAGE})
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_BUILD_APPIMAGE=1)
endif ()
if (${SUNSHINE_BUILD_FLATPAK})
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_BUILD_FLATPAK=1)
endif ()

include_directories(SYSTEM
        ${CMAKE_BINARY_DIR}/generated-src)

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/audio.cpp")

list(APPEND PLATFORM_LIBRARIES
        dl
        pulse
        pulse-simple)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES glad)
