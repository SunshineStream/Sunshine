#
# Loads FFmpeg pre-compiled binaries from GitHub releases or a user-specified path
#
include_guard(GLOBAL)

# ffmpeg pre-compiled binaries
if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    # Set platform-specific libraries
    if(WIN32)
        set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
    elseif(FREEBSD)
        # numa is not available on FreeBSD
        set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
    endif()

    # Determine download location
    set(FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps")

    # Use the release pinned alongside the build-deps-owned codec headers.
    set(FFMPEG_GITHUB_REPO "LizardByte/build-deps")
    set(FFMPEG_RELEASE_URL "https://github.com/${FFMPEG_GITHUB_REPO}/releases/download/${BUILD_DEPS_TAG}")
    set(FFMPEG_VERSION_DIR "${FFMPEG_DOWNLOAD_DIR}/ffmpeg-${BUILD_DEPS_TAG}")
    message(STATUS "Using FFmpeg from build-deps tag: ${BUILD_DEPS_TAG}")

    # Set extraction directory and prepared binaries path
    set(FFMPEG_EXTRACT_DIR "${FFMPEG_VERSION_DIR}")
    set(FFMPEG_PREPARED_BINARIES "${FFMPEG_EXTRACT_DIR}/ffmpeg")

    # Set the archive filename based on architecture
    if(NOT DEFINED FFMPEG_RELEASE_ASSET_NAME)
        set(FFMPEG_ARCHIVE_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-ffmpeg.tar.gz")
    else()
        set(FFMPEG_ARCHIVE_NAME "${FFMPEG_RELEASE_ASSET_NAME}")
    endif()
    set(FFMPEG_ARCHIVE_PATH "${FFMPEG_VERSION_DIR}/${FFMPEG_ARCHIVE_NAME}")
    set(FFMPEG_DOWNLOAD_URL "${FFMPEG_RELEASE_URL}/${FFMPEG_ARCHIVE_NAME}")

    # Check if already downloaded and extracted
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
        # Check if we need to download the archive
        if(NOT EXISTS "${FFMPEG_ARCHIVE_PATH}")
            message(STATUS "Downloading FFmpeg binaries from ${FFMPEG_DOWNLOAD_URL}")

            # Download the archive
            file(DOWNLOAD
                "${FFMPEG_DOWNLOAD_URL}"
                "${FFMPEG_ARCHIVE_PATH}"
                SHOW_PROGRESS
                STATUS FFMPEG_DOWNLOAD_STATUS
                TIMEOUT 300
            )

            # Check download status
            list(GET FFMPEG_DOWNLOAD_STATUS 0 FFMPEG_DOWNLOAD_STATUS_CODE)
            list(GET FFMPEG_DOWNLOAD_STATUS 1 FFMPEG_DOWNLOAD_STATUS_MESSAGE)

            if(NOT FFMPEG_DOWNLOAD_STATUS_CODE EQUAL 0)
                message(FATAL_ERROR "Failed to download FFmpeg binaries: ${FFMPEG_DOWNLOAD_STATUS_MESSAGE}")
            endif()
        else()
            message(STATUS "Using cached FFmpeg archive at ${FFMPEG_ARCHIVE_PATH}")
        endif()

        # Extract the archive
        message(STATUS "Extracting FFmpeg binaries to ${FFMPEG_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT  # cmake-lint: disable=E1126
            INPUT "${FFMPEG_ARCHIVE_PATH}"
            DESTINATION "${FFMPEG_EXTRACT_DIR}"
        )

        # Verify extraction
        if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
            message(FATAL_ERROR "FFmpeg extraction failed or unexpected directory structure")
        endif()

        message(STATUS "FFmpeg binaries successfully downloaded and extracted")
    else()
        message(STATUS "Using existing FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")
    endif()

    # Set FFmpeg libraries
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
        ${HDR10_PLUS_LIBRARY}
        ${FFMPEG_PLATFORM_LIBRARIES}
    )
else()
    # User provided FFMPEG_PREPARED_BINARIES path
    message(STATUS "Using user-specified FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")

    # Set platform-specific libraries
    if(NOT DEFINED FFMPEG_PLATFORM_LIBRARIES)
        if(WIN32)
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
        elseif(FREEBSD)
            set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
        elseif(UNIX AND NOT APPLE)
            set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
        endif()
    endif()

    # Set base FFmpeg libraries (always required)
    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
    )

    # Add optional libraries if they exist (e.g., from prebuilt packages)
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    # Add platform libraries
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_PLATFORM_LIBRARIES})
endif()

# Shared packages supply the full static dependency list, including decoders.
# Older packages retain the library list above.
set(_sunshine_ffmpeg_pc "${FFMPEG_PREPARED_BINARIES}/lib/pkgconfig/libavcodec.pc")
set(_sunshine_ffmpeg_relocatable -1)
if(EXISTS "${_sunshine_ffmpeg_pc}")
    file(READ "${_sunshine_ffmpeg_pc}" _sunshine_ffmpeg_metadata LIMIT 512)
    string(FIND "${_sunshine_ffmpeg_metadata}" "prefix=\${pcfiledir}/../.." _sunshine_ffmpeg_relocatable)
endif()
if(_sunshine_ffmpeg_relocatable GREATER_EQUAL 0)
    # Refresh cached paths when the package or system library search policy changes.
    get_cmake_property(_sunshine_cache_variables CACHE_VARIABLES)
    foreach(variable IN LISTS _sunshine_cache_variables)
        if(variable MATCHES "^(pkgcfg_lib_|__pkg_config_checked_)?SUNSHINE_BUNDLED_(FFMPEG|OPUS)")
            unset(${variable} CACHE)
        endif()
    endforeach()
    foreach(variable PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR)
        if(DEFINED ENV{${variable}})
            set(_sunshine_saved_${variable} "$ENV{${variable}}")
        else()
            unset(_sunshine_saved_${variable})
        endif()
    endforeach()
    set(_sunshine_saved_pkg_config_argn "${PKG_CONFIG_ARGN}")
    # The package supplies static codec archives; system libraries keep their normal linkage.
    set(ENV{PKG_CONFIG_PATH} "${FFMPEG_PREPARED_BINARIES}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_LIBDIR} "$ENV{PKG_CONFIG_PATH}")
    unset(ENV{PKG_CONFIG_SYSROOT_DIR})
    set(PKG_CONFIG_ARGN --static)
    if(WIN32)
        set(_sunshine_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SUNSHINE_BUNDLED_FFMPEG REQUIRED IMPORTED_TARGET libcbs libavcodec libswscale libavutil)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Explicit shared compiler runtimes bypass -static-libstdc++/-static-libgcc.
        # Let the C++ driver apply Sunshine's runtime policy from targets/linux.cmake.
        get_target_property(_sunshine_ffmpeg_link_libraries
                PkgConfig::SUNSHINE_BUNDLED_FFMPEG INTERFACE_LINK_LIBRARIES)
        list(FILTER _sunshine_ffmpeg_link_libraries EXCLUDE
                REGEX "(^|/)(lib)?(stdc\\+\\+|gcc_s)(\\.[^/]*)?$")
        set_property(TARGET PkgConfig::SUNSHINE_BUNDLED_FFMPEG PROPERTY
                INTERFACE_LINK_LIBRARIES "${_sunshine_ffmpeg_link_libraries}")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libopus.a")
        pkg_check_modules(SUNSHINE_BUNDLED_OPUS REQUIRED IMPORTED_TARGET opus)
    endif()
    foreach(variable PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR)
        if(DEFINED _sunshine_saved_${variable})
            set(ENV{${variable}} "${_sunshine_saved_${variable}}")
        else()
            unset(ENV{${variable}})
        endif()
    endforeach()
    set(PKG_CONFIG_ARGN "${_sunshine_saved_pkg_config_argn}")
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES "${_sunshine_saved_library_suffixes}")
    endif()
    set(FFMPEG_LIBRARIES PkgConfig::SUNSHINE_BUNDLED_FFMPEG)
endif()

set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PREPARED_BINARIES}/include")

if(UNIX AND NOT APPLE)
    find_package(Libva REQUIRED)

    # Static FFmpeg metadata can pull bundled libva archives back into the link.
    # Resolve every VA backend to the host, including legacy library-name lists.
    set(_sunshine_saved_ignore_prefix_path "${CMAKE_IGNORE_PREFIX_PATH}")
    set(_sunshine_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "${FFMPEG_PREPARED_BINARIES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_sunshine_ffmpeg_library_lists FFMPEG_LIBRARIES)
    if(TARGET PkgConfig::SUNSHINE_BUNDLED_FFMPEG)
        get_target_property(_sunshine_ffmpeg_link_libraries
                PkgConfig::SUNSHINE_BUNDLED_FFMPEG INTERFACE_LINK_LIBRARIES)
        list(APPEND _sunshine_ffmpeg_library_lists _sunshine_ffmpeg_link_libraries)
    endif()
    foreach(library_list IN LISTS _sunshine_ffmpeg_library_lists)
        set(_sunshine_system_libraries)
        foreach(library IN LISTS ${library_list})
            get_filename_component(_sunshine_library_name "${library}" NAME)
            if(_sunshine_library_name MATCHES "^(-l|lib)?(va(-[a-z0-9]+)?)(\\.(a|so)(\\.[0-9]+)*)?$")
                set(_sunshine_va_name "${CMAKE_MATCH_2}")
                string(MAKE_C_IDENTIFIER "${_sunshine_va_name}" _sunshine_va_id)
                if(_sunshine_va_name STREQUAL "va")
                    list(APPEND _sunshine_system_libraries "${LIBVA_LIB}")
                elseif(_sunshine_va_name STREQUAL "va-drm")
                    list(APPEND _sunshine_system_libraries "${LIBVA_DRM_LIB}")
                else()
                    # Backends must come from the same installation as the core.
                    unset(_sunshine_system_${_sunshine_va_id} CACHE)
                    find_library(_sunshine_system_${_sunshine_va_id} NAMES "${_sunshine_va_name}"
                            PATHS "${_libva_library_dir}" NO_DEFAULT_PATH REQUIRED)
                    list(APPEND _sunshine_system_libraries "${_sunshine_system_${_sunshine_va_id}}")
                endif()
            else()
                list(APPEND _sunshine_system_libraries "${library}")
            endif()
        endforeach()
        set(${library_list} "${_sunshine_system_libraries}")
    endforeach()
    if(TARGET PkgConfig::SUNSHINE_BUNDLED_FFMPEG)
        set_property(TARGET PkgConfig::SUNSHINE_BUNDLED_FFMPEG PROPERTY
                INTERFACE_LINK_LIBRARIES "${_sunshine_ffmpeg_link_libraries}")
    endif()
    set(CMAKE_IGNORE_PREFIX_PATH "${_sunshine_saved_ignore_prefix_path}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_sunshine_saved_library_suffixes}")

    # /usr/include is a compiler default: putting it before the bundled include
    # directory cannot override bundled VA headers. Expose a header tree without
    # va/ so Sunshine's compatibility shims follow the host libva version.
    if(IS_DIRECTORY "${FFMPEG_INCLUDE_DIRS}/va")
        set(FFMPEG_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/_deps/ffmpeg-system-includes")
        file(COPY "${FFMPEG_PREPARED_BINARIES}/include/" DESTINATION "${FFMPEG_INCLUDE_DIRS}" PATTERN "va" EXCLUDE)
    endif()
    set(_sunshine_bundled_include "${FFMPEG_PREPARED_BINARIES}/include")
    if(NV_CODEC_HEADERS_13_1_INCLUDE_DIR STREQUAL _sunshine_bundled_include)
        set(NV_CODEC_HEADERS_13_1_INCLUDE_DIR "${FFMPEG_INCLUDE_DIRS}")
    endif()
    foreach(dependency FFMPEG OPUS)
        if(TARGET PkgConfig::SUNSHINE_BUNDLED_${dependency})
            get_target_property(_sunshine_include_dirs
                    PkgConfig::SUNSHINE_BUNDLED_${dependency} INTERFACE_INCLUDE_DIRECTORIES)
            set(_sunshine_system_include_dirs)
            foreach(_sunshine_include_dir IN LISTS _sunshine_include_dirs)  # cmake-lint: disable=C0103
                cmake_path(NORMAL_PATH _sunshine_include_dir)
                cmake_path(IS_PREFIX _sunshine_bundled_include "${_sunshine_include_dir}" NORMALIZE _sunshine_bundled)
                if(_sunshine_bundled)
                    cmake_path(RELATIVE_PATH _sunshine_include_dir BASE_DIRECTORY "${_sunshine_bundled_include}"
                            OUTPUT_VARIABLE _sunshine_include_suffix)
                    set(_sunshine_include_dir "${FFMPEG_INCLUDE_DIRS}/${_sunshine_include_suffix}")
                endif()
                list(APPEND _sunshine_system_include_dirs "${_sunshine_include_dir}")
            endforeach()
            set_property(TARGET PkgConfig::SUNSHINE_BUNDLED_${dependency} PROPERTY
                    INTERFACE_INCLUDE_DIRECTORIES "${_sunshine_system_include_dirs}")
        endif()
    endforeach()
endif()
