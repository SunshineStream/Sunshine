# unix specific compile definitions
# put anything here that applies to both linux and macos

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        Boost::log
        ${CURL_LIBRARIES})
set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/httpcommon_curl.cpp")

# add install prefix to assets path if not already there
if(NOT SUNSHINE_ASSETS_DIR MATCHES "^${CMAKE_INSTALL_PREFIX}")
    set(SUNSHINE_ASSETS_DIR "${CMAKE_INSTALL_PREFIX}/${SUNSHINE_ASSETS_DIR}")
endif()
