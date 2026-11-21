set(SUNSHINE_PREPARED_NV_CODEC_HEADERS "${FFMPEG_PREPARED_BINARIES}/include/ffnvcodec/nvEncodeAPI.h")

if(WIN32 OR (NOT APPLE AND NOT EXISTS "${SUNSHINE_PREPARED_NV_CODEC_HEADERS}"))
    include("${CMAKE_MODULE_PATH}/dependencies/build_deps.cmake")
endif()

# Keep build-deps-owned declarations out of Sunshine's generated package lock.
set(SUNSHINE_CPM_PACKAGE_LOCK_ENABLED "${CPM_PACKAGE_LOCK_ENABLED}")
set(CPM_PACKAGE_LOCK_ENABLED OFF)

if(NOT APPLE)
    if(EXISTS "${SUNSHINE_PREPARED_NV_CODEC_HEADERS}")
        # Prepared FFmpeg bundles include the NV codec headers used to build them.
        set(NV_CODEC_HEADERS_13_1_INCLUDE_DIR "${FFMPEG_PREPARED_BINARIES}/include")
        message(STATUS "Using NV codec headers from prepared FFmpeg binaries")
    else()
        CPMGetPackage(nv_codec_headers_13_1)
        set(NV_CODEC_HEADERS_13_1_INCLUDE_DIR "${nv_codec_headers_13_1_SOURCE_DIR}/include")
    endif()
endif()
unset(SUNSHINE_PREPARED_NV_CODEC_HEADERS)

if(WIN32)
    CPMGetPackage(nv_codec_headers_13)
    CPMGetPackage(nv_codec_headers_11)
    CPMGetPackage(nv_codec_headers_12)

    set(NV_CODEC_HEADERS_11_INCLUDE_DIR "${nv_codec_headers_11_SOURCE_DIR}/include")
    set(NV_CODEC_HEADERS_12_INCLUDE_DIR "${nv_codec_headers_12_SOURCE_DIR}/include")
    set(NV_CODEC_HEADERS_13_INCLUDE_DIR "${nv_codec_headers_13_SOURCE_DIR}/include")
endif()

set(CPM_PACKAGE_LOCK_ENABLED "${SUNSHINE_CPM_PACKAGE_LOCK_ENABLED}")
unset(SUNSHINE_CPM_PACKAGE_LOCK_ENABLED)
