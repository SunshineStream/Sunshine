cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories(
        "${FFMPEG_INCLUDE_DIRS}"  # this is included only for logging
)

set(TOOL_SOURCES
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
)

add_executable(dxgi-info dxgi.cpp ${TOOL_SOURCES})
target_include_directories(dxgi-info BEFORE PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(dxgi-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${FFMPEG_LIBRARIES}  # this is included only for logging
        dxgi
        libdisplaydevice::display_device  # this is included only for logging
        ${PLATFORM_LIBRARIES}
)
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp ${TOOL_SOURCES})
target_include_directories(audio-info BEFORE PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${FFMPEG_LIBRARIES}  # this is included only for logging
        libdisplaydevice::display_device  # this is included only for logging
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
target_include_directories(sunshinesvc BEFORE PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
