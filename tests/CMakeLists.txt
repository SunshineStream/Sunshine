cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_sunshine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/lizardbyte-common/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
if(NOT TARGET gtest)
    add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
endif()
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# coverage
# https://gcovr.com/en/stable/guide/compiling.html#compiler-options
option(SUNSHINE_LLVM_COVERAGE "Use LLVM source-based coverage" OFF)
if(SUNSHINE_LLVM_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -ggdb -O0)
    add_link_options(-fprofile-instr-generate)
else()
    set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
    set(CMAKE_C_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
endif()

set(SUNSHINE_TEST_GCOV_ROOT
        ""
        CACHE PATH "Relocated source and build root used to process gcov data")
if(SUNSHINE_TEST_GCOV_ROOT AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
            "-fprofile-prefix-map=${CMAKE_SOURCE_DIR}=${SUNSHINE_TEST_GCOV_ROOT}")
endif()

# Link the libgcov that belongs to the selected compiler. Asking GCC directly
# avoids selecting another installed GCC version through platform-specific
# library directory names (for example, the GCC required by CUDA on Arch Linux).
if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgcov.a
        OUTPUT_VARIABLE LIBGCOV_LIBRARY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE LIBGCOV_RESULT
    )

    if(LIBGCOV_RESULT EQUAL 0 AND EXISTS "${LIBGCOV_LIBRARY}")
        message(STATUS "Found libgcov: ${LIBGCOV_LIBRARY}")
        set(GCOV_LINK_LIBRARY "${LIBGCOV_LIBRARY}")
    else()
        message(WARNING "Could not find libgcov for ${CMAKE_C_COMPILER}")
        unset(GCOV_LINK_LIBRARY)
    endif()
endif()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

# modify SUNSHINE_DEFINITIONS
if (WIN32)
    list(APPEND
            SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
elseif (NOT APPLE)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/linux/assets/shaders/opengl")
endif ()

set(TEST_DEFINITIONS)  # list will be appended as needed
set(SUNSHINE_TEST_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}"
        CACHE PATH "Source fixture directory used by tests")
set(SUNSHINE_TEST_RUNTIME_DIR
        "${CMAKE_CURRENT_BINARY_DIR}"
        CACHE PATH "Writable runtime fixture directory used by tests")

# this indicates we're building tests in case sunshine needs to adjust some code or add private tests
list(APPEND TEST_DEFINITIONS SUNSHINE_TESTS)
list(APPEND TEST_DEFINITIONS SUNSHINE_SOURCE_DIR="${SUNSHINE_TEST_SOURCE_DIR}")
list(APPEND TEST_DEFINITIONS SUNSHINE_TEST_BIN_DIR="${SUNSHINE_TEST_RUNTIME_DIR}")
if(SUNSHINE_ENABLE_TRAY)
    list(APPEND TEST_DEFINITIONS TRAY_ENABLE_TEST_HOOKS)
endif()

# Override SUNSHINE_ASSETS_DIR to use a writable temp directory for tests
# Remove the existing definition from SUNSHINE_DEFINITIONS to avoid redefinition error
list(FILTER SUNSHINE_DEFINITIONS EXCLUDE REGEX "^SUNSHINE_ASSETS_DIR=")
list(APPEND TEST_DEFINITIONS SUNSHINE_ASSETS_DIR="${SUNSHINE_TEST_RUNTIME_DIR}/test_assets")

if(NOT WIN32)
    find_package(Udev 255)  # we need 255+ for udevadm verify
    message(STATUS "UDEV_FOUND: ${UDEV_FOUND}")
    if(UDEV_FOUND)
        list(APPEND TEST_DEFINITIONS UDEVADM_EXECUTABLE="${UDEVADM_EXECUTABLE}")
    endif()
endif()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)
if(SUNSHINE_ENABLE_TRAY AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    set(SUNSHINE_TRAY_TEST_ICON_SOURCE_DIR
            "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/images")
    set(SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/test_assets/web/images")
    file(MAKE_DIRECTORY "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}")

    configure_file(
            "${CMAKE_SOURCE_DIR}/sunshine.svg"
            "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}/logo-sunshine.svg"
            COPYONLY)

    if(WIN32 OR APPLE)
        configure_file(
                "${CMAKE_SOURCE_DIR}/third-party/libvirtualhid/libvirtualhid.svg"
                "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}/logo-libvirtualhid.svg"
                COPYONLY)
    endif()

    set(SUNSHINE_TRAY_TEST_ICONS
            sunshine-locked.svg
            sunshine-playing.svg
            sunshine-pausing.svg)

    foreach(icon IN LISTS SUNSHINE_TRAY_TEST_ICONS)
        configure_file(
                "${SUNSHINE_TRAY_TEST_ICON_SOURCE_DIR}/${icon}"
                "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}/${icon}"
                COPYONLY)
    endforeach()

    add_library(sunshine_tray_test_helpers STATIC
            ${CMAKE_SOURCE_DIR}/third-party/tray/tests/notification_utils.cpp
            ${CMAKE_SOURCE_DIR}/third-party/tray/tests/screenshot_utils.cpp)
    set_target_properties(sunshine_tray_test_helpers PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON)
    target_include_directories(sunshine_tray_test_helpers PRIVATE
            ${CMAKE_SOURCE_DIR}/third-party/tray)
    target_link_libraries(sunshine_tray_test_helpers PRIVATE
            lizardbyte::common)

    if(WIN32)
        target_link_libraries(sunshine_tray_test_helpers PRIVATE
                gdi32
                gdiplus)
    else()
        find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui)
        find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui)
        set(SUNSHINE_TRAY_TEST_QT_GUI_TARGET Qt${QT_VERSION_MAJOR}::Gui)

        target_link_libraries(sunshine_tray_test_helpers PRIVATE
                ${SUNSHINE_TRAY_TEST_QT_GUI_TARGET})
        if(APPLE)
            target_link_libraries(sunshine_tray_test_helpers PRIVATE
                    "-framework Cocoa")
        endif()
    endif()
endif()
# Add macOS-specific test files only when building tests for macOS
if (APPLE)
    file(GLOB_RECURSE MACOS_TEST_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/*.mm)
    list(APPEND TEST_SOURCES ${MACOS_TEST_SOURCES})
endif ()

set(SUNSHINE_SOURCES
        ${SUNSHINE_TARGET_FILES})

# remove main.cpp from the list of sources
list(REMOVE_ITEM SUNSHINE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(${PROJECT_NAME}
        ${TEST_SOURCES}
        ${SUNSHINE_SOURCES})
# TODO: Use proper gtest integration from GoogleTest module
add_test(NAME ${PROJECT_NAME}
        COMMAND ${PROJECT_NAME})

# Copy integration-test fixtures to the build directory.
# Preserve repository-relative paths for source and installed-test layouts.
# Using configure_file ensures files are copied when they change between builds
set(INTEGRATION_TEST_FILES
    "cmake/compile_definitions/macos.cmake"
    "cmake/dependencies/Boost_Sunshine.cmake"
    "cmake/macros/common.cmake"
    "cmake/prep/options.cmake"
    "package-lock.cmake"
    "packaging/linux/Arch/PKGBUILD"
    "packaging/linux/flatpak/modules/boost.json"
    "packaging/linux/flatpak/scripts/additional-install.sh"
    "scripts/linux_build.sh"
    "src/config.cpp"
    "src_assets/common/assets/web/Config.vue"
    "src_assets/common/assets/web/locale.js"
    "docs/building.md"
    "docs/configuration.md"
    "docs/getting_started.md"
    "docs/troubleshooting.md"
    "src_assets/common/assets/web/public/assets/locale/en.json"
    "src_assets/common/assets/web/configs/config_tabs.json"
    "src_assets/common/assets/web/configs/VirtualKeyCodeSelect.vue"
    "src_assets/common/assets/web/configs/virtual_key_codes.js"
    "src_assets/common/assets/web/configs/tabs/General.vue"
    "src_assets/common/assets/web/configs/tabs/Inputs.vue"
    "src_assets/linux/misc/60-sunshine.conf"
    "src_assets/linux/misc/60-sunshine.rules"
    "tests/scripts/test_additional_install.sh"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/packaging/linux/copr/Sunshine.spec")
    list(APPEND INTEGRATION_TEST_FILES "packaging/linux/copr/Sunshine.spec")
    list(APPEND TEST_DEFINITIONS SUNSHINE_COPR_SPEC_FIXTURE_AVAILABLE)
endif()

if(APPLE)
    list(APPEND INTEGRATION_TEST_FILES
        ".github/workflows/ci-macos.yml"
        "scripts/macos_build.sh"
    )
endif()

foreach(file ${INTEGRATION_TEST_FILES})
    configure_file(
        "${CMAKE_SOURCE_DIR}/${file}"
        "${CMAKE_CURRENT_BINARY_DIR}/${file}"
        COPYONLY
    )
endforeach()

# Copy all locale files for locale consistency tests
# Use a custom command to properly handle both adding and removing files
set(LOCALE_SRC_DIR "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/assets/locale")
set(LOCALE_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/src_assets/common/assets/web/public/assets/locale")
add_custom_target(sync_locale_files ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LOCALE_SRC_DIR}" "${LOCALE_DST_DIR}"
    COMMENT "Synchronizing locale files for tests"
    VERBATIM
)

foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(${PROJECT_NAME} ${dep})  # compile these before sunshine
endforeach()

# Ensure locale files are synchronized before building the test executable
add_dependencies(${PROJECT_NAME} sync_locale_files)

# Build the list of libraries to link
set(TEST_LINK_LIBRARIES
    ${SUNSHINE_EXTERNAL_LIBRARIES}
    lizardbyte::test_support
    ${PLATFORM_LIBRARIES}
)

if(SUNSHINE_ENABLE_TRAY AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    list(APPEND TEST_LINK_LIBRARIES
            sunshine_tray_test_helpers)
    if(SUNSHINE_TRAY_TEST_QT_GUI_TARGET)
        list(APPEND TEST_LINK_LIBRARIES
                ${SUNSHINE_TRAY_TEST_QT_GUI_TARGET})
    endif()
endif()

# Add the specific libgcov library if found
if(GCOV_LINK_LIBRARY)
    list(APPEND TEST_LINK_LIBRARIES ${GCOV_LINK_LIBRARY})
endif()

target_link_libraries(${PROJECT_NAME} ${TEST_LINK_LIBRARIES})
target_compile_definitions(${PROJECT_NAME} PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(${PROJECT_NAME} PRIVATE ${SUNSHINE_LINK_OPTIONS})

if (WIN32)
    # Compile the utility test with the SDK 13.1 implementation namespace and headers.
    set_source_files_properties(unit/test_nvenc_utils.cpp PROPERTIES
            COMPILE_DEFINITIONS "NVENC_NAMESPACE=nvenc_1301;NVENC_SDK_VERSION=1301"
            INCLUDE_DIRECTORIES "${NV_CODEC_HEADERS_13_1_INCLUDE_DIR}")

    # prefer static libraries since we're linking statically
    # this fixes libcurl linking errors when using non MSYS2 version of CMake
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_SEARCH_START_STATIC 1)

    # Copy minhook-detours DLL to test binary directory for ARM64
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_MINHOOK_DLL}"
                "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Copying minhook-detours DLL to test binary directory"
            VERBATIM
        )
    endif()
endif ()
