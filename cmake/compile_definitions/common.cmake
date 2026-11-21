# common compile definitions
# this file will also load platform specific definitions

list(APPEND SUNSHINE_COMPILE_OPTIONS -Wall -Wno-sign-compare)
# Wall - enable all warnings
# Werror - treat warnings as errors
# Wno-maybe-uninitialized/Wno-uninitialized - disable warnings for maybe uninitialized variables
# Wno-sign-compare - disable warnings for signed/unsigned comparisons
# Wno-restrict - disable warnings for memory overlap
# Wmissing-field-initializers - enable warnings for missing field initializers
# Wno-missing-designated-field-initializers - disable warning for missing designated initializers
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

    # GCC 15 will complain about uninitialized variables in some cases (Simple-Web-Server)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    # Clang specific compile options

    # Clang doesn't actually complain about this this, so disabling for now
    # list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)

    # Warn for missing positional field initializers but not designated initializers
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wmissing-field-initializers)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-missing-designated-field-initializers)
    endif()

    # Some libc++ versions on Apple and FreeBSD guard std::jthread behind this flag.
    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        list(APPEND SUNSHINE_COMPILE_OPTIONS -fexperimental-library)
        list(APPEND SUNSHINE_LINK_OPTIONS -fexperimental-library)
    endif()
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

# libvirtualhid
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libvirtualhid")
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES libvirtualhid::libvirtualhid)
list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/virtualhid_input.h"
        "${CMAKE_SOURCE_DIR}/src/platform/virtualhid_input.cpp")

# build libevdev before the libvirtualhid target when using the ExternalProject fallback
if(EXTERNAL_PROJECT_LIBEVDEV_USED AND TARGET libvirtualhid)
    add_dependencies(libvirtualhid libevdev)
endif()

set(NVENC_PUBLIC_SOURCES
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_config.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_d3d11_interface.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_dynamic_factory.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_dynamic_factory.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_dynamic_factory_versions.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_encoded_frame.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_encoder.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_shared_dll.h"
        "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_version.h"
)
set(NVENC_SOURCES ${NVENC_PUBLIC_SOURCES})

if(WIN32)
    set(NVENC_IMPLEMENTATION_SOURCES
            "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_base.cpp"
            "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_d3d11.cpp"
            "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_d3d11_native.cpp"
            "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_d3d11_on_cuda.cpp"
            "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_dynamic_factory_impl.cpp"
            "${CMAKE_SOURCE_DIR}/src/nvenc/nvenc_utils.cpp"
    )

    # Add a version-isolated NVENC implementation object library.
    # add_nvenc_sdk_implementation: args = `target_name`, `sdk_version`, `sdk_include_dir`
    function(add_nvenc_sdk_implementation target_name sdk_version sdk_include_dir)
        add_library(${target_name} OBJECT ${NVENC_IMPLEMENTATION_SOURCES})
        target_link_libraries(${target_name} PRIVATE sunshine_pyrowave)
        target_include_directories(${target_name} BEFORE PRIVATE "${sdk_include_dir}")
        target_compile_definitions(${target_name} PRIVATE
                NVENC_FACTORY_SUFFIX=${sdk_version}
                NVENC_NAMESPACE=nvenc_${sdk_version}
                NVENC_SDK_VERSION=${sdk_version}
        )
        target_compile_options(${target_name} PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
    endfunction()

    add_nvenc_sdk_implementation(nvenc_sdk_1100 1100 "${NV_CODEC_HEADERS_11_INCLUDE_DIR}")
    add_nvenc_sdk_implementation(nvenc_sdk_1200 1200 "${NV_CODEC_HEADERS_12_INCLUDE_DIR}")
    add_nvenc_sdk_implementation(nvenc_sdk_1300 1300 "${NV_CODEC_HEADERS_13_INCLUDE_DIR}")
    add_nvenc_sdk_implementation(nvenc_sdk_1301 1301 "${NV_CODEC_HEADERS_13_1_INCLUDE_DIR}")

    list(APPEND NVENC_SOURCES
            $<TARGET_OBJECTS:nvenc_sdk_1100>
            $<TARGET_OBJECTS:nvenc_sdk_1200>
            $<TARGET_OBJECTS:nvenc_sdk_1300>
            $<TARGET_OBJECTS:nvenc_sdk_1301>
    )
endif()

list(APPEND PLATFORM_TARGET_FILES ${NVENC_SOURCES})

set(SUNSHINE_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Input.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Rtsp.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/RtspParser.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Video.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/nanors/deps/obl/oblas_common.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/nanors/deps/obl/oblas_lite.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/nanors/rs.c"
        "${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        "${CMAKE_SOURCE_DIR}/src/upnp.h"
        "${CMAKE_SOURCE_DIR}/src/cbs.cpp"
        "${CMAKE_SOURCE_DIR}/src/utility.h"
        "${CMAKE_SOURCE_DIR}/src/uuid.h"
        "${CMAKE_SOURCE_DIR}/src/config.h"
        "${CMAKE_SOURCE_DIR}/src/config.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device.h"
        "${CMAKE_SOURCE_DIR}/src/display_device.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.h"
        "${CMAKE_SOURCE_DIR}/src/file_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/file_handler.h"
        "${CMAKE_SOURCE_DIR}/src/globals.cpp"
        "${CMAKE_SOURCE_DIR}/src/globals.h"
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/logging.h"
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_SOURCE_DIR}/src/main.h"
        "${CMAKE_SOURCE_DIR}/src/crypto.cpp"
        "${CMAKE_SOURCE_DIR}/src/crypto.h"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.h"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.cpp"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.h"
        "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/confighttp.h"
        "${CMAKE_SOURCE_DIR}/src/compat/moonlight.h"
        "${CMAKE_SOURCE_DIR}/src/rtsp.cpp"
        "${CMAKE_SOURCE_DIR}/src/rtsp.h"
        "${CMAKE_SOURCE_DIR}/src/stream.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream.h"
        "${CMAKE_SOURCE_DIR}/src/video.cpp"
        "${CMAKE_SOURCE_DIR}/src/video.h"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.cpp"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.h"
        "${CMAKE_SOURCE_DIR}/src/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/input.h"
        "${CMAKE_SOURCE_DIR}/src/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio.h"
        "${CMAKE_SOURCE_DIR}/src/platform/common.h"
        "${CMAKE_SOURCE_DIR}/src/platform/permissions.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/permissions.h"
        "${CMAKE_SOURCE_DIR}/src/process.cpp"
        "${CMAKE_SOURCE_DIR}/src/process.h"
        "${CMAKE_SOURCE_DIR}/src/network.cpp"
        "${CMAKE_SOURCE_DIR}/src/network.h"
        "${CMAKE_SOURCE_DIR}/src/move_by_copy.h"
        "${CMAKE_SOURCE_DIR}/src/system_tray.cpp"
        "${CMAKE_SOURCE_DIR}/src/system_tray.h"
        "${CMAKE_SOURCE_DIR}/src/task_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_safe.h"
        "${CMAKE_SOURCE_DIR}/src/sync.h"
        "${CMAKE_SOURCE_DIR}/src/round_robin.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.cpp"
        ${PLATFORM_TARGET_FILES})

if(NOT SUNSHINE_ASSETS_DIR_DEF)
    set(SUNSHINE_ASSETS_DIR_DEF "${SUNSHINE_ASSETS_DIR}")
endif()
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ASSETS_DIR="${SUNSHINE_ASSETS_DIR_DEF}")

# Publisher metadata
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_NAME="${SUNSHINE_PUBLISHER_NAME}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_WEBSITE="${SUNSHINE_PUBLISHER_WEBSITE}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_ISSUE_URL="${SUNSHINE_PUBLISHER_ISSUE_URL}")

include_directories(BEFORE "${CMAKE_SOURCE_DIR}")

include_directories(
        BEFORE
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/nanors/deps/obl"
        ${OPENSSL_INCLUDE_DIR}
        ${Opus_INCLUDE_DIR}
        ${FFMPEG_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}  # has to be the last, or we get runtime error on macOS ffmpeg encoder
)

if(NOT APPLE)
    include_directories(BEFORE SYSTEM "${NV_CODEC_HEADERS_13_1_INCLUDE_DIR}")
endif()

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${MINIUPNP_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        enet
        libdisplaydevice::display_device
        lizardbyte::common
        nlohmann_json::nlohmann_json
        ${Opus_LIBRARY}
        ${FFMPEG_LIBRARIES}
        ${Boost_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES})

# tray icon
if(SUNSHINE_ENABLE_TRAY)
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES tray::tray)
else()
    set(SUNSHINE_TRAY 0)
    message(STATUS "Tray icon disabled")
endif()
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_TRAY=${SUNSHINE_TRAY})
