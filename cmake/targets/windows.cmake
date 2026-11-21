# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        version.lib)

set(SUNSHINE_OPEN_VHF_SDK "${PROJECT_SOURCE_DIR}/third-party/libvirtualgamepad" CACHE PATH
    "Path to the libvirtualgamepad SDK. Set to an empty value to disable Open VHF.")
if(SUNSHINE_OPEN_VHF_SDK)
    if(NOT EXISTS "${SUNSHINE_OPEN_VHF_SDK}/client/CMakeLists.txt")
        message(FATAL_ERROR "SUNSHINE_OPEN_VHF_SDK must point to the libvirtualgamepad source tree")
    endif()
    add_subdirectory("${SUNSHINE_OPEN_VHF_SDK}/client" "${CMAKE_CURRENT_BINARY_DIR}/open-vhf-client")
    target_sources(sunshine PRIVATE "${PROJECT_SOURCE_DIR}/src/platform/windows/open_vhf_input.cpp")
    target_compile_definitions(sunshine PRIVATE SUNSHINE_OPEN_VHF)
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES libvirtualgamepad::client)
endif()
