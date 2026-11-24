include_guard(GLOBAL)

CPMAddPackage(NAME build_deps DOWNLOAD_ONLY YES)
include("${build_deps_SOURCE_DIR}/package-lock.cmake")
