# CPM Package Lock
# This file should be committed to version control

# The first argument of CPMDeclarePackage can be freely chosen and is used as argument in CPMGetPackage.
# The NAME argument should be package name that would also be used in a find_package call.
# Ideally, both are the same, which might not always be possible: https://github.com/cpm-cmake/CPM.cmake/issues/603
# This is needed to support CPM_USE_LOCAL_PACKAGES

# Renovate-bot will update the versions and hashes in this file when a new version of a dependency is released.
# The comments above each dependency are used by renovate to identify the dependencies and extract the version numbers.
# See https://github.com/LizardByte/.github/blob/master/renovate/cmake.json5 for the configuration of renovate.
# See https://github.com/LizardByte/.github/blob/master/renovate/README.md#cpm-package-lock for annotation instructions.

set(PATCH_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/patches")

# Boost
# renovate: datasource=github-release-attachments depName=boostorg/boost
# versioning=regex:^boost-(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)(?<prerelease>\.[A-Za-z0-9.-]+)?$
set(BOOST_TAG boost-1.92.0)
set(BOOST_SHA256 9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d)
string(REGEX REPLACE "^boost-" "" BOOST_VERSION "${BOOST_TAG}")
CPMDeclarePackage(Boost
        NAME Boost
        VERSION ${BOOST_VERSION}
        URL https://github.com/boostorg/boost/releases/download/${BOOST_TAG}/${BOOST_TAG}-cmake.tar.xz
        URL_HASH SHA256=${BOOST_SHA256}
        DOWNLOAD_ONLY YES
)
