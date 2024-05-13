option(BUILD_TESTS "Build tests" ON)
option(TESTS_ENABLE_PYTHON_TESTS "Enable Python tests" ON)

# DirectX11 is not available in GitHub runners, so even software encoding fails
set(TESTS_SOFTWARE_ENCODER_UNAVAILABLE "fail"
        CACHE STRING "How to handle unavailable software encoders in tests. 'fail/skip'")

option(BUILD_WERROR "Enable -Werror flag." OFF)

# if this option is set, the build will exit after configuring special package configuration files
option(SUNSHINE_CONFIGURE_ONLY "Configure special files only, then exit." OFF)

option(SUNSHINE_ENABLE_TRAY "Enable system tray icon. This option will be ignored on macOS." ON)
option(SUNSHINE_REQUIRE_TRAY "Require system tray icon. Fail the build if tray requirements are not met." ON)

option(SUNSHINE_SYSTEM_NLOHMANN_JSON "Use system installation of nlohmann_json rather than the submodule." OFF)
option(SUNSHINE_SYSTEM_WAYLAND_PROTOCOLS "Use system installation of wayland-protocols rather than the submodule." OFF)

option(CUDA_INHERIT_COMPILE_OPTIONS
        "When building CUDA code, inherit compile options from the the main project. You may want to disable this if
        your IDE throws errors about unknown flags after running cmake." ON)

if(UNIX)
    # technically, the homebrew build could be on linux as well... no idea if it would actually work
    option(SUNSHINE_BUILD_HOMEBREW
            "Enable a Homebrew build." OFF)
endif ()

if(APPLE)
    option(SUNSHINE_CONFIGURE_HOMEBREW
            "Configure macOS Homebrew formula. Recommended to use with SUNSHINE_CONFIGURE_ONLY" OFF)
    option(SUNSHINE_CONFIGURE_PORTFILE
            "Configure macOS Portfile. Recommended to use with SUNSHINE_CONFIGURE_ONLY" OFF)
    option(SUNSHINE_PACKAGE_MACOS
            "Should only be used when creating a macOS package/dmg." OFF)
elseif(UNIX)  # Linux
    option(SUNSHINE_BUILD_APPIMAGE
            "Enable an AppImage build." OFF)
    option(SUNSHINE_BUILD_FLATPAK
            "Enable a Flatpak build." OFF)
    option(SUNSHINE_CONFIGURE_PKGBUILD
            "Configure files required for AUR. Recommended to use with SUNSHINE_CONFIGURE_ONLY" OFF)
    option(SUNSHINE_CONFIGURE_FLATPAK_MAN
            "Configure manifest file required for Flatpak build. Recommended to use with SUNSHINE_CONFIGURE_ONLY" OFF)

    # Linux capture methods
    option(SUNSHINE_ENABLE_CUDA
            "Enable cuda specific code." ON)
    option(SUNSHINE_ENABLE_DRM
            "Enable KMS grab if available." ON)
    option(SUNSHINE_ENABLE_VAAPI
            "Enable building vaapi specific code." ON)
    option(SUNSHINE_ENABLE_WAYLAND
            "Enable building wayland specific code." ON)
    option(SUNSHINE_ENABLE_X11
            "Enable X11 grab if available." ON)
endif()
