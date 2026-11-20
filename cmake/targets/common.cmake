# common target definitions
# this file will also load platform specific macros

if(APPLE AND NOT SUNSHINE_BUILD_HOMEBREW)
    add_executable(sunshine MACOSX_BUNDLE ${SUNSHINE_TARGET_FILES})
else()
    add_executable(sunshine ${SUNSHINE_TARGET_FILES})
endif()
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(sunshine ${dep})  # compile these before sunshine
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

target_link_libraries(sunshine ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sunshine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(sunshine PRIVATE ${SUNSHINE_LINK_OPTIONS})

# Homebrew build fails the vite build if we set these environment variables
if(${SUNSHINE_BUILD_HOMEBREW})
    set(NPM_SOURCE_ASSETS_DIR "")
    set(NPM_ASSETS_DIR "")
    set(NPM_BUILD_HOMEBREW "true")
else()
    set(NPM_SOURCE_ASSETS_DIR ${SUNSHINE_SOURCE_ASSETS_DIR})
    set(NPM_ASSETS_DIR ${CMAKE_BINARY_DIR})
    set(NPM_BUILD_HOMEBREW "")
endif()

#WebUI build
find_program(NPM npm REQUIRED)

if(WIN32)
    get_filename_component(NPM_DIRECTORY "${NPM}" DIRECTORY)
    find_program(NPM_NODE_EXECUTABLE NAMES node node.exe HINTS "${NPM_DIRECTORY}" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    string(CONCAT NPM_NODE_GNU_BINDING_CHECK
            "process.arch === 'x64' && "
            "(process.config.variables.shlib_suffix === 'dll.a' || "
            "process.config.variables.node_target_type === 'shared_library')")
    execute_process(
            COMMAND "${NPM_NODE_EXECUTABLE}" -p "${NPM_NODE_GNU_BINDING_CHECK}"
            OUTPUT_VARIABLE NPM_NODE_USES_GNU_BINDING
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NPM_NODE_USES_GNU_BINDING STREQUAL "true")
        find_program(NATIVE_NPM NAMES npm.cmd npm HINTS "$ENV{ProgramFiles}/nodejs" NO_DEFAULT_PATH NO_CACHE)
        if(NOT NATIVE_NPM)
            message(FATAL_ERROR
                    "The MSYS2 Node.js package is incompatible with Rolldown. "
                    "Install native Windows Node.js or set NPM to its npm.cmd path.")
        endif()

        set(NPM "${NATIVE_NPM}" CACHE FILEPATH "Path to the npm executable" FORCE)
        get_filename_component(NPM_DIRECTORY "${NPM}" DIRECTORY)
        message(STATUS "MSYS2 Node.js is incompatible with Rolldown; using native npm: ${NPM}")
    endif()

    # Ninja wraps custom commands in cmd.exe. `call` keeps a quoted npm.cmd path intact when it contains spaces.
    set(NPM_COMMAND cmd /C call)
    set(NPM_PATH "PATH=${NPM_DIRECTORY};$ENV{PATH}")
else()
    set(NPM_COMMAND)
    set(NPM_PATH "PATH=$ENV{PATH}")
endif()

set(NPM_INSTALL_FLAGS "--ignore-scripts")
if (NPM_OFFLINE)
    set(NPM_INSTALL_FLAGS "${NPM_INSTALL_FLAGS} --offline")
endif()

add_custom_target(web-ui ALL
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Installing NPM Dependencies and Building the Web UI"
        COMMAND "${CMAKE_COMMAND}" -E env "${NPM_PATH}" ${NPM_COMMAND} "${NPM}" ci ${NPM_INSTALL_FLAGS}
        COMMAND "${CMAKE_COMMAND}" -E env "${NPM_PATH}" "SUNSHINE_BUILD_HOMEBREW=${NPM_BUILD_HOMEBREW}" "SUNSHINE_SOURCE_ASSETS_DIR=${NPM_SOURCE_ASSETS_DIR}" "SUNSHINE_ASSETS_DIR=${NPM_ASSETS_DIR}" ${NPM_COMMAND} "${NPM}" run build  # cmake-lint: disable=C0301
        VERBATIM)

# docs
if(BUILD_DOCS)
    include(third-party/dockle/cmake/Dockle.cmake)
    dockle_add_docs(docs TARGETS docs)
endif()

# tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-missing-field-initializers ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        if (NOT BUILD_TESTS)
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}"
                    PROPERTIES COMPILE_FLAGS -O2)
        else()
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests"
                    PROPERTIES COMPILE_FLAGS -O2)
        endif()
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
