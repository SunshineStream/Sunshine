# macos specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="macos")

set(MACOS_LINK_DIRECTORIES
        /opt/homebrew/lib
        /opt/local/lib
        /usr/local/lib)

foreach(dir ${MACOS_LINK_DIRECTORIES})
    if(EXISTS ${dir})
        link_directories(${dir})
    endif()
endforeach()

ADD_DEFINITIONS(-DBOOST_LOG_DYN_LINK)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${APP_KIT_LIBRARY}
        ${APP_SERVICES_LIBRARY}
        ${AV_FOUNDATION_LIBRARY}
        ${CORE_MEDIA_LIBRARY}
        ${CORE_VIDEO_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${VIDEO_TOOLBOX_LIBRARY})

set(PLATFORM_INCLUDE_DIRS
        ${Boost_INCLUDE_DIR})

set(APPLE_PLIST_FILE "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/assets/Info.plist")

# todo - tray is not working on macos
set(SUNSHINE_TRAY 0)

set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/macos/av_audio.h"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/av_audio.m"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/av_img_t.h"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/av_video.h"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/av_video.m"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/display.mm"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/display_options.mm"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/microphone.mm"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/misc.mm"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/nv12_zero_device.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/nv12_zero_device.h"
        "${CMAKE_SOURCE_DIR}/src/platform/macos/publish.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/TPCircularBuffer/TPCircularBuffer.c"
        "${CMAKE_SOURCE_DIR}/third-party/TPCircularBuffer/TPCircularBuffer.h"
        ${APPLE_PLIST_FILE})

if(SUNSHINE_ENABLE_TRAY)
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
            ${COCOA})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/third-party/tray/tray_darwin.m")
endif()
