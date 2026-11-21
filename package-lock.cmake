# CPM Package Lock
# This file should be committed to version control

# The first argument of CPMDeclarePackage can be freely chosen and is used as argument in CPMGetPackage.
# The NAME argument should be package name that would also be used in a find_package call.
# Ideally, both are the same, which might not always be possible: https://github.com/cpm-cmake/CPM.cmake/issues/603
# This is needed to support CPM_USE_LOCAL_PACKAGES

# Renovate-bot will update the versions and hashes in this file when a new version of a dependency is released.
# The comments above each dependency are used by renovate to identify the dependencies and extract the version numbers.
# See https://github.com/LizardByte/.github/blob/master/renovate/cmake.json5 for the configuration of renovate.
#
# Expected dependency structure for new entries:
# - Start each block with the dependency name, for example `# Example dependency`.
# - Follow it with consecutive renovate metadata comments.
# - The first metadata line must start with `# renovate:` and include `datasource=` and `depName=`.
# - Optional metadata keys are `versioning=`, `extractVersion=`, and `registryUrl=`.
# - Optional metadata may stay on the `# renovate:` line or continue on the next consecutive `#` lines.
# - Keep metadata keys in this order: `datasource`, `depName`, `versioning`, `extractVersion`, `registryUrl`.
# - After metadata, declare the tracked value with `set(NAME_VERSION ...)` or `set(NAME_TAG ...)`.
# - For `git-refs`, add a `# ref=<branch>` line and `set(NAME_COMMIT ...)`. Renovate reads the URL from
#   the literal `GIT_REPOSITORY` in the following `CPMDeclarePackage(...)`.
# - If the dependency also tracks a SHA256, keep `set(NAME_SHA256 ...)` immediately after the
#   matching `NAME_VERSION` or `NAME_TAG` line with no unrelated lines between them.
# - Keep `CPMDeclarePackage(...)` below the tracked values.
#
# Example layout:
# - `# Example dependency`
# - `# renovate: datasource=github-tags depName=owner/repo`
# - `# versioning=regex:^v(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)$`
# - `set(EXAMPLE_TAG v1.2.3)`
# - `set(EXAMPLE_SHA256 <sha256>)`
# - `CPMDeclarePackage(...)`

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

# Granite/PyroWave (no official releases/versioning, so the commit hash is tracked)
set(PYROWAVE_GRANITE_TAG fb178c8080d163419e8d20f10715c61c53c1ec9b)
CPMDeclarePackage(pyrowave_granite
        NAME pyrowave_granite
        GITHUB_REPOSITORY Themaister/Granite
        GIT_TAG ${PYROWAVE_GRANITE_TAG}
        GIT_SUBMODULES third_party/volk third_party/khronos/vulkan-headers
        GIT_SUBMODULES_RECURSE OFF
        DOWNLOAD_ONLY YES
)
