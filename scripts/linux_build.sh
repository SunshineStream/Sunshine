#!/bin/bash
set -e

# Version requirements - centralized for easy maintenance
cmake_min="4.0.0"
target_cmake_version="4.3.0"
doxygen_min="1.10.0"
_doxygen_min="${doxygen_min//\./_}"  # Convert dots to underscores for URL
doxygen_max="1.12.0"
default_cuda_version="13.5.0"
default_cuda_system_minor_version="13.1"

# Default value for arguments
appimage_build=0
cuda_version="$default_cuda_version"
cuda_patches=0
cuda_system_package=0
cuda_system_package_name=""
force_cuda_runfile=0
num_processors=$(nproc)
publisher_name="Third Party Publisher"
publisher_website=""
publisher_issue_url="https://app.lizardbyte.dev/support"
skip_cleanup=0
skip_cuda=0
skip_libva=0
skip_package=0
sudo_cmd="sudo"
ubuntu_test_repo=0
step="all"

# constants
AARCH64="aarch64"
DOXYGEN="doxygen"
readonly DISTRO_ARCH="arch"
readonly DISTRO_DEBIAN="debian"
readonly DISTRO_FEDORA="fedora"
readonly DISTRO_UBUNTU="ubuntu"
readonly UBUNTU_APPSTREAM_GLIB_REMOVAL_VERSION="26.10"

function setup_cuda_system_package_environment() {
  if [[ "$cuda_system_package" == 1 ]]; then
    # Ubuntu CUDA 13 packages install nvcc here but do not add it to PATH.
    local cuda_bin_path
    cuda_bin_path="$(cuda_system_toolkit_path)/bin"
    if [[ ":${PATH}:" != *":${cuda_bin_path}:"* ]]; then
      export PATH="${cuda_bin_path}:${PATH}"
    fi
  fi
  return 0
}

function cuda_system_toolkit_path() {
  local cuda_minor_version="$default_cuda_system_minor_version"
  if [[ "$cuda_system_package_name" =~ ^cuda-toolkit-([0-9]+)-([0-9]+)$ ]]; then
    cuda_minor_version="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}"
  fi
  echo "/usr/local/cuda-${cuda_minor_version}"
}

function cuda_target_dir() {
  if [[ "$architecture" == "${AARCH64}" ]]; then
    echo "sbsa-linux"
  else
    echo "x86_64-linux"
  fi
}

function cuda_math_functions_patch_applied() {
  local cuda_toolkit_path=$1
  local math_functions_file
  math_functions_file="${cuda_toolkit_path}/targets/$(cuda_target_dir)/include/crt/math_functions.h"

  if [[ ! -f "$math_functions_file" ]]; then
    return 1
  fi

  # Ubuntu's CUDA packages provide the same glibc fix through this specifier macro.
  local exception_specifier='(noexcept[[:space:]]*\(true\)|_NV_RSQRT_SPECIFIER)'
  grep -Eq "rsqrt\(double x\) ${exception_specifier};" "$math_functions_file" && \
    grep -Eq "rsqrtf\(float x\) ${exception_specifier};" "$math_functions_file" && \
    grep -Eq "__func__\(double rsqrt\(double a\) ${exception_specifier}\);" "$math_functions_file" && \
    grep -Eq "__func__\(float rsqrtf\(float a\) ${exception_specifier}\);" "$math_functions_file"
}

function apply_cuda_patches() {
  local cuda_toolkit_path=$1

  if [[ "$cuda_patches" != 1 ]]; then
    return 0
  fi

  if [[ ! -d "$cuda_toolkit_path" ]]; then
    echo "CUDA toolkit path not found: $cuda_toolkit_path"
    return 1
  fi

  if cuda_math_functions_patch_applied "$cuda_toolkit_path"; then
    echo "CUDA math_functions.h patch already applied"
    return 0
  fi

  echo "Applying CUDA patches"
  local patch_dir="${script_dir}/../packaging/linux/patches/${architecture}"
  local patch_file=""

  # Select the patch based on the CUDA major version, not the distro version.
  # see https://forums.developer.nvidia.com/t/error-exception-specification-is-incompatible-for-cospi-sinpi-cospif-sinpif-with-glibc-2-41/323591/3
  local cuda_major="${cuda_version%%.*}"
  if [[ "${cuda_major}" -eq 12 ]]; then
    # CUDA 12.x: the extern declarations lack noexcept(true); add it to match glibc 2.41.
    patch_file="${patch_dir}/cuda-12-math_functions.patch"
  elif [[ "${cuda_major}" -eq 13 ]]; then
    # CUDA 13.x: the extern declarations already have noexcept(true), but the __func__()
    # macro invocations at the bottom still lack it, causing a redeclaration conflict.
    patch_file="${patch_dir}/cuda-13-math_functions.patch"
  else
    echo "Warning: no math_functions.h patch available for CUDA ${cuda_major}.x, skipping."
  fi

  if [[ -n "$patch_file" ]]; then
    if [[ -f "$patch_file" ]]; then
      echo "Applying patch: $patch_file"
      ${sudo_cmd} patch -p2 \
        --backup \
        --directory="$cuda_toolkit_path" \
        --verbose \
        < "$patch_file"
    else
      echo "Patch file not found: $patch_file"
    fi
  else
    echo "No CUDA patch required for ${distro} ${version}"
  fi
}

# Reusable function to detect nvcc path
function detect_nvcc_path() {
  local nvcc_path=""

  # First check for system-installed CUDA
  nvcc_path=$(command -v nvcc 2>/dev/null) || true
  if [[ -n "$nvcc_path" ]]; then
    echo "$nvcc_path"
    return 0
  fi

  # Then check for locally installed CUDA in build directory
  if [[ -f "${build_dir}/cuda/bin/nvcc" ]]; then
    echo "${build_dir}/cuda/bin/nvcc"
    return 0
  fi

  # No CUDA found
  return 1
}

# Reusable function to setup NVM environment
function setup_nvm_environment() {
  # Only setup NVM if it should be used for this distro
  if [[ "$nvm_node" == 1 ]]; then
    # Check if NVM is installed and source it
    if [[ -f "$HOME/.nvm/nvm.sh" ]]; then
      # shellcheck source=/dev/null
      source "$HOME/.nvm/nvm.sh"
      # Use the default node version installed by NVM
      nvm use default 2>/dev/null || nvm use node 2>/dev/null || true
      echo "Using NVM Node.js version: $(node --version 2>/dev/null || echo 'not available')"
      echo "Using NVM npm version: $(npm --version 2>/dev/null || echo 'not available')"
    else
      echo "NVM not found, using system Node.js if available"
    fi
  fi
  return 0
}

function _usage() {
  local exit_code=$1

  cat <<EOF
This script installs the dependencies and builds the project.
The script is intended to be run on a Debian-based or Fedora-based system.

Usage:
  $0 [options]

Options:
  -h, --help               Display this help message.
  -s, --sudo-off           Disable sudo command.
  --appimage-build         Compile for AppImage, this will not create the AppImage, just the executable.
  --cuda-patches           Apply cuda patches. Enabled automatically on Ubuntu 26.04.
  --cuda-runfile           Force CUDA installation from the NVIDIA runfile.
  --cuda-system-package=*  The CUDA package to install when system CUDA is enabled.
                           Defaults: cuda-toolkit-13-1 on Ubuntu 26.04, cuda-toolkit-13-3 on 26.10.
  --num-processors         The number of processors to use for compilation. Default is the value of 'nproc'.
  --publisher-name         The name of the publisher (not developer) of the application.
  --publisher-website      The URL of the publisher's website.
  --publisher-issue-url    The URL of the publisher's support site or issue tracker.
                           If you provide a modified version of Sunshine, we kindly request that you use your own url.
  --skip-cleanup           Do not restore the original gcc alternatives, or the math-vector.h file.
  --skip-cuda              Skip CUDA installation.
  --skip-libva             Skip libva installation.
  --skip-package           Skip creating DEB, or RPM package.
  --ubuntu-test-repo       Install ppa:ubuntu-toolchain-r/test repo on Ubuntu.
  --step                   Which step(s) to run: deps, cmake, validation, build, package, cleanup, or all (default: all)

Steps:
  deps                     Install dependencies only
  cmake                    Run cmake configure only
  validation               Run validation commands only
  build                    Build the project only
  package                  Create packages only
  cleanup                  Cleanup alternatives and backups only
  all                      Run all steps (default)
EOF

  exit "$exit_code"
}

# Parse named arguments
while getopts ":hs-:" opt; do
  case ${opt} in
    h ) _usage 0 ;;
    s ) sudo_cmd="" ;;
    - )
      case "${OPTARG}" in
        help) _usage 0 ;;
        appimage-build)
          appimage_build=1
          ;;
        cuda-patches)
          cuda_patches=1
          ;;
        cuda-runfile)
          force_cuda_runfile=1
          ;;
        cuda-system-package=*)
          cuda_system_package_name="${OPTARG#*=}"
          ;;
        num-processors=*)
          num_processors="${OPTARG#*=}"
          ;;
        publisher-name=*)
          publisher_name="${OPTARG#*=}"
          ;;
        publisher-website=*)
          publisher_website="${OPTARG#*=}"
          ;;
        publisher-issue-url=*)
          publisher_issue_url="${OPTARG#*=}"
          ;;
        skip-cleanup) skip_cleanup=1 ;;
        skip-cuda) skip_cuda=1 ;;
        skip-libva) skip_libva=1 ;;
        skip-package) skip_package=1 ;;
        sudo-off) sudo_cmd="" ;;
        ubuntu-test-repo) ubuntu_test_repo=1 ;;
        step=*)
          step="${OPTARG#*=}"
          ;;
        *)
          echo "Invalid option: --${OPTARG}" 1>&2
          _usage 1
          ;;
      esac
      ;;
    \? )
      echo "Invalid option: -${OPTARG}" 1>&2
      _usage 1
      ;;
    *)
      echo "Invalid option: -${OPTARG}" 1>&2
      _usage 1
      ;;
  esac
done
shift $((OPTIND -1))

# dependencies array to build out
dependencies=()

function add_arch_deps() {
  dependencies+=(
    'appstream-glib'
    'avahi'
    'base-devel'
    'boost'
    'boost-libs'
    'cmake'
    'curl'
    'doxygen'
    "gcc${gcc_version}"
    "gcc${gcc_version}-libs"
    'git'
    'graphviz'
    'libcap'
    'libdrm'
    'libevdev'
    'libmfx'
    'libpulse'
    'libva'
    'libx11'
    'libxcb'
    'libxfixes'
    'libxrandr'
    'libxtst'
    'miniupnpc'
    'ninja'
    'nodejs'
    'npm'
    'numactl'
    'openssl'
    'opus'
    'python-jinja'  # glad OpenGL/EGL loader generator
    'qt6-base'
    'qt6-svg'
    'shaderc'
    'udev'
    'vulkan-icd-loader'
    'wayland'
  )

  if [[ "$skip_libva" == 0 ]]; then
    dependencies+=(
      "libva"  # VA-API
    )
  fi

  if [[ "$skip_cuda" == 0 ]]; then
    dependencies+=(
      "cuda"  # VA-API
    )
  fi
  return 0
}

function add_debian_based_deps() {
  dependencies+=(
    "appstream"
    "bison"  # required if we need to compile doxygen
    "build-essential"
    "cmake"
    "desktop-file-utils"
    "${DOXYGEN}"
    "file"
    "flex"  # required if we need to compile doxygen
    "gcc-${gcc_version}"
    "g++-${gcc_version}"
    "git"
    "glslang-tools"  # Vulkan shader compiler
    "graphviz"
    "imagemagick"  # necessary for system tray screenshots in unit tests
    "libcap-dev"  # KMS
    "libcurl4-openssl-dev"
    "libdrm-dev"  # KMS
    "libevdev-dev"
    "libgbm-dev"
    "libminiupnpc-dev"
    "libnuma-dev"
    "libopus-dev"
    "libpipewire-0.3-dev"
    "libpulse-dev"
    "libssl-dev"
    "libsystemd-dev"
    "libudev-dev"
    "libwayland-dev"  # Wayland
    "libx11-dev"  # X11
    "libxcb-shm0-dev"  # X11
    "libxcb-xfixes0-dev"  # X11
    "libxcb1-dev"  # X11
    "libxfixes-dev"  # X11
    "libxrandr-dev"  # X11
    "libxtst-dev"  # X11
    "libvulkan-dev"  # Vulkan
    "ninja-build"
    "npm"  # web-ui
    "python3-jinja2"  # glad OpenGL/EGL loader generator
    "qt6-base-dev"
    "systemd"
    "udev"
    "wget"  # necessary for cuda install with `run` file
    "xvfb"  # necessary for headless unit testing
  )

  # appstream-glib was removed in Ubuntu 26.10; appstreamcli remains available.
  if [[ "$distro" != "$DISTRO_UBUNTU" ]] || [[ "$version" < "$UBUNTU_APPSTREAM_GLIB_REMOVAL_VERSION" ]]; then
    dependencies+=("appstream-util")
  fi

  # Ubuntu 22.04 uses a different package name for Qt6 SVG
  if [[ "$distro" == "$DISTRO_UBUNTU" ]] && [[ "$version" == "22.04" ]]; then
    dependencies+=(
      "libgl-dev"  # OpenGL development headers, needed for qt6-svg
      "libqt6svg6-dev"
    )
  else
    dependencies+=("qt6-svg-dev")
  fi

  if [[ "$skip_libva" == 0 ]]; then
    dependencies+=(
      "libva-dev"  # VA-API
    )
  fi
  return 0
}

function add_test_ppa() {
  if [[ "$ubuntu_test_repo" == 1 ]]; then
    $package_install_command "software-properties-common"
    ${sudo_cmd} add-apt-repository ppa:ubuntu-toolchain-r/test -y
  fi
  return 0
}

function add_debian_deps() {
  add_test_ppa
  add_debian_based_deps
  dependencies+=(
    "systemd-dev"
  )
  return 0
}

function add_ubuntu_deps() {
  # Enable universe; qt6-base-dev and several other required packages live there.
  $package_install_command "software-properties-common"
  ${sudo_cmd} add-apt-repository universe -y
  add_test_ppa
  add_debian_based_deps

  # Ubuntu 26.04+ provides compatible Boost static libraries in the component development packages.
  if [[ "$(printf '%s\n' "$version" "26.04" | sort -V | head -n1)" == "26.04" ]]; then
    dependencies+=(
      "libboost-filesystem-dev"
      "libboost-locale-dev"
      "libboost-log-dev"
      "libboost-program-options-dev"
      "libicu-dev"
    )
  fi

  if [[ "$skip_cuda" == 0 ]] && [[ "$cuda_system_package" == 1 ]]; then
    if [[ -z "$cuda_system_package_name" ]]; then
      echo "CUDA system package was requested, but no package name was configured."
      return 1
    fi

    echo "Using CUDA system package: $cuda_system_package_name"
    dependencies+=(
      "$cuda_system_package_name"
    )
  fi

  if [[ "$(printf '%s\n' "$version" "24.04" | sort -V | head -n1)" == "24.04" ]]; then
    dependencies+=(
      "systemd-dev"
    )
  fi
  return 0
}

function add_fedora_deps() {
  dependencies+=(
    "appstream"
    "cmake"
    "desktop-file-utils"
    "${DOXYGEN}"
    "gcc${gcc_version}"
    "gcc${gcc_version}-c++"
    "git"
    "glslc"
    "graphviz"
    "ImageMagick"  # necessary for system tray screenshots in unit tests
    "libappstream-glib"
    "libcap-devel"
    "libcurl-devel"
    "libdrm-devel"
    "libevdev-devel"
    "libX11-devel"  # X11
    "libxcb-devel"  # X11
    "libXcursor-devel"  # X11
    "libXfixes-devel"  # X11
    "libXi-devel"  # X11
    "libXinerama-devel"  # X11
    "libXrandr-devel"  # X11
    "libXtst-devel"  # X11
    "mesa-libGL-devel"
    "mesa-libgbm-devel"
    "miniupnpc-devel"
    "ninja-build"
    "npm"
    "numactl-devel"
    "openssl-devel"
    "opus-devel"
    "pipewire-devel"
    "pulseaudio-libs-devel"
    "python3-jinja2"  # glad OpenGL/EGL loader generator
    "qt6-qtbase-devel"
    "qt6-qtsvg-devel"
    "rpm-build"  # if you want to build an RPM binary package
    "vulkan-loader-devel"
    "wget"  # necessary for cuda install with `run` file
    "which"  # necessary for cuda install with `run` file
    "xorg-x11-server-Xvfb"  # necessary for headless unit testing
  )

  # Fedora 44+ provides Boost 1.90 or newer and packages its static libraries separately.
  if [[ "$version" =~ ^[0-9]+$ ]] && (( version >= 44 )); then
    dependencies+=(
      "boost-devel"
      "boost-static"
    )
  fi

  if [[ "$skip_libva" == 0 ]]; then
    dependencies+=(
      "libva-devel"  # VA-API
    )
  fi
  return 0
}

function install_cuda() {
  setup_cuda_system_package_environment

  # Check if CUDA is already available
  if [[ "$force_cuda_runfile" == 1 ]] && [[ -f "${build_dir}/cuda/bin/nvcc" ]]; then
    apply_cuda_patches "${build_dir}/cuda"
    return
  elif [[ "$force_cuda_runfile" == 0 ]] && detect_nvcc_path > /dev/null 2>&1; then
    if [[ "$cuda_system_package" == 1 ]]; then
      apply_cuda_patches "$(cuda_system_toolkit_path)"
    fi
    return
  fi

  if [[ "$cuda_system_package" == 1 ]]; then
    echo "CUDA system package '$cuda_system_package_name' was requested, but nvcc was not found after dependency installation."
    return 1
  fi

  local cuda_override_arg=""
  if [[ "$distro" == "$DISTRO_FEDORA" ]]; then
    cuda_override_arg="--override"
  fi

  local cuda_prefix="https://developer.download.nvidia.com/compute/cuda/"
  local cuda_suffix=""
  if [[ "$architecture" == "${AARCH64}" ]]; then
    local cuda_suffix="_sbsa"
  fi

  if [[ "$architecture" == "${AARCH64}" ]]; then
    # we need to patch the math-vector.h file for aarch64 fedora
    # back up /usr/include/bits/math-vector.h
    math_vector_file=""
    if [[ "$distro" == "$DISTRO_UBUNTU" ]] || [[ "$version" == "24.04" ]]; then
      math_vector_file="/usr/include/aarch64-linux-gnu/bits/math-vector.h"
    elif [[ "$distro" == "$DISTRO_FEDORA" ]]; then
      math_vector_file="/usr/include/bits/math-vector.h"
    fi

    if [[ -n "$math_vector_file" ]]; then
      # patch headers https://bugs.launchpad.net/ubuntu/+source/mumax3/+bug/2032624
      ${sudo_cmd} cp "$math_vector_file" "$math_vector_file.bak"
      ${sudo_cmd} sed -i 's/__Float32x4_t/int/g' "$math_vector_file"
      ${sudo_cmd} sed -i 's/__Float64x2_t/int/g' "$math_vector_file"
      ${sudo_cmd} sed -i 's/__SVFloat32_t/float/g' "$math_vector_file"
      ${sudo_cmd} sed -i 's/__SVFloat64_t/float/g' "$math_vector_file"
      ${sudo_cmd} sed -i 's/__SVBool_t/int/g' "$math_vector_file"
    fi
  fi

  local url="${cuda_prefix}${cuda_version}/local_installers/cuda_${cuda_version}_linux${cuda_suffix}.run"
  echo "cuda url: ${url}"
  wget "$url" --max-redirect=0 --progress=bar:force:noscroll -q --show-progress -O "${build_dir}/cuda.run"
  chmod a+x "${build_dir}/cuda.run"
  "${build_dir}/cuda.run" --silent --toolkit --toolkitpath="${build_dir}/cuda" --no-man-page "$cuda_override_arg"
  rm "${build_dir}/cuda.run"

  apply_cuda_patches "${build_dir}/cuda"
  return 0
}

function check_version() {
  local package_name=$1
  local min_version=$2
  local max_version=$3
  local installed_version

  echo "Checking if $package_name is installed and at least version $min_version"

  if [[ "$distro" == "$DISTRO_DEBIAN" ]] || [[ "$distro" == "$DISTRO_UBUNTU" ]]; then
    installed_version=$(dpkg -s "$package_name" 2>/dev/null | grep '^Version:' | awk '{print $2}')
  elif [[ "$distro" == "$DISTRO_FEDORA" ]]; then
    installed_version=$(rpm -q --queryformat '%{VERSION}' "$package_name" 2>/dev/null)
  elif [[ "$distro" == "$DISTRO_ARCH" ]]; then
    installed_version=$(pacman -Q "$package_name" | awk '{print $2}' )
  else
    echo "Unsupported Distro"
    return 1
  fi

  if [[ -z "$installed_version" ]]; then
    echo "Package not installed"
    return 1
  fi

if [[ "$(printf '%s\n' "$installed_version" "$min_version" | sort -V | head -n1)" = "$min_version" ]] && \
   [[ "$(printf '%s\n' "$installed_version" "$max_version" | sort -V | head -n1)" = "$installed_version" ]]; then
    echo "Installed version is within range"
    return 0
  else
    echo "$package_name version $installed_version is out of range"
    return 1
  fi
}

function run_step_deps() {
  echo "Running step: Install dependencies"

  # Update the package list
  $package_update_command

  if [[ "$distro" == "$DISTRO_ARCH" ]]; then
    add_arch_deps
  elif [[ "$distro" == "$DISTRO_DEBIAN" ]]; then
    add_debian_deps
  elif [[ "$distro" == "$DISTRO_UBUNTU" ]]; then
    add_ubuntu_deps
  elif [[ "$distro" == "$DISTRO_FEDORA" ]]; then
    add_fedora_deps
    ${sudo_cmd} dnf group install "development-tools" -y
  fi

  # Install the dependencies
  $package_install_command "${dependencies[@]}"

  # reload the environment
  # shellcheck source=/dev/null
  source ~/.bashrc

  #set gcc version based on distros
  export CC=gcc-${gcc_version}
  export CXX=g++-${gcc_version}

  # compile cmake if the version is too low
  if ! check_version "cmake" "$cmake_min" "inf"; then
    cmake_prefix="https://github.com/Kitware/CMake/releases/download/v"
    if [[ "$architecture" == "x86_64" ]]; then
      cmake_arch="x86_64"
    elif [[ "$architecture" == "${AARCH64}" ]]; then
      cmake_arch="${AARCH64}"
    fi
    url="${cmake_prefix}${target_cmake_version}/cmake-${target_cmake_version}-linux-${cmake_arch}.sh"
    echo "cmake url: ${url}"
    wget "$url" --progress=bar:force:noscroll -q --show-progress -O "${build_dir}/cmake.sh"
    ${sudo_cmd} sh "${build_dir}/cmake.sh" --skip-license --prefix=/usr/local
    echo "cmake installed, version:"
    cmake --version
  fi

  # compile doxygen if version is too low
  if ! check_version "${DOXYGEN}" "$doxygen_min" "$doxygen_max"; then
    if [[ "${SUNSHINE_COMPILE_DOXYGEN}" == "true" ]]; then
      echo "Compiling doxygen"
      doxygen_url="https://github.com/doxygen/doxygen/releases/download/Release_${_doxygen_min}/${DOXYGEN}-${doxygen_min}.src.tar.gz"
      echo "${DOXYGEN} url: ${doxygen_url}"
      pushd "${build_dir}"
        local -a doxygen_download_args=(
          "$doxygen_url"
          --max-redirect=1
          --progress=bar:force:noscroll
          -q
          --show-progress
          -O "${DOXYGEN}.tar.gz"
        )
        # GitHub release downloads require one redirect to the release asset host.
        wget "${doxygen_download_args[@]}"  # NOSONAR(shell:S6506)
        tar -xzf "${DOXYGEN}.tar.gz"
        cd "${DOXYGEN}-${doxygen_min}"
        cmake -DCMAKE_BUILD_TYPE=Release -G="Ninja" -B="build" -S="."
        ninja -C "build" -j"${num_processors}"
        ${sudo_cmd} ninja -C "build" install
      popd
    else
      echo "${DOXYGEN} version not in range, skipping docs"
      # Note: cmake_args will be set in cmake step
    fi
  fi

  # install node from nvm
  if [[ "$nvm_node" == 1 ]]; then
    nvm_url="https://raw.githubusercontent.com/nvm-sh/nvm/master/install.sh"
    echo "nvm url: ${nvm_url}"
    wget --max-redirect=0 -qO- ${nvm_url} | bash

    # shellcheck source=/dev/null  # we don't care that shellcheck cannot find nvm.sh
    source "$HOME/.nvm/nvm.sh"
    nvm install node
    nvm use node
  fi

  # run the cuda install
  if [[ "$skip_cuda" == 0 ]]; then
    install_cuda
  fi
  return 0
}

function run_step_cmake() {
  echo "Running step: CMake configure"

  # Setup NVM environment if needed (for web UI builds)
  setup_nvm_environment
  setup_cuda_system_package_environment
  if [[ "$skip_cuda" == 0 ]] && [[ "$cuda_system_package" == 1 ]]; then
    apply_cuda_patches "$(cuda_system_toolkit_path)"
  fi

  # Detect CUDA path using the reusable function
  nvcc_path=""
  if [[ "$skip_cuda" == 0 ]]; then
    if [[ "$force_cuda_runfile" == 1 ]] && [[ -f "${build_dir}/cuda/bin/nvcc" ]]; then
      nvcc_path="${build_dir}/cuda/bin/nvcc"
    else
      nvcc_path=$(detect_nvcc_path)
    fi
  fi

  #set gcc version based on distros
  export CC=gcc-${gcc_version}
  export CXX=g++-${gcc_version}

  # prepare CMAKE args
  cmake_args=(
    "-B=build"
    "-G=Ninja"
    "-S=."
    "-DBUILD_WERROR=ON"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_INSTALL_PREFIX=/usr"
    "-DSUNSHINE_ASSETS_DIR=share/sunshine"
    "-DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine"
    "-DSUNSHINE_ENABLE_DRM=ON"
    "-DSUNSHINE_ENABLE_KWIN=ON"
    "-DSUNSHINE_ENABLE_PORTAL=ON"
    "-DSUNSHINE_ENABLE_WAYLAND=ON"
    "-DSUNSHINE_ENABLE_X11=ON"
  )

  if [[ "$appimage_build" == 1 ]]; then
    cmake_args+=("-DSUNSHINE_BUILD_APPIMAGE=ON")
  fi

  # Publisher metadata
  if [[ -n "$publisher_name" ]]; then
    cmake_args+=("-DSUNSHINE_PUBLISHER_NAME='${publisher_name}'")
  fi
  if [[ -n "$publisher_website" ]]; then
    cmake_args+=("-DSUNSHINE_PUBLISHER_WEBSITE='${publisher_website}'")
  fi
  if [[ -n "$publisher_issue_url" ]]; then
    cmake_args+=("-DSUNSHINE_PUBLISHER_ISSUE_URL='${publisher_issue_url}'")
  fi

  # Handle doxygen docs flag
  if ! check_version "${DOXYGEN}" "$doxygen_min" "$doxygen_max" && [[ "${SUNSHINE_COMPILE_DOXYGEN}" != "true" ]]; then
    cmake_args+=("-DBUILD_DOCS=OFF")
  fi

  # Handle CUDA
  if [[ "$skip_cuda" == 0 ]]; then
    cmake_args+=("-DSUNSHINE_ENABLE_CUDA=ON")
    if [[ -n "$nvcc_path" ]]; then
      cmake_args+=("-DCMAKE_CUDA_COMPILER:PATH=$nvcc_path")
      cmake_args+=("-DCMAKE_CUDA_HOST_COMPILER=gcc-${gcc_version}")
    fi
  else
    cmake_args+=("-DSUNSHINE_ENABLE_CUDA=OFF")
  fi

  # Cmake stuff here
  mkdir -p "build"
  echo "cmake args:"
  echo "${cmake_args[@]}"
  cmake "${cmake_args[@]}"
  return 0
}

function run_step_validation() {
  echo "Running step: Validation"

  # Run appstream validation, etc.
  appstreamcli validate "build/dev.lizardbyte.app.Sunshine.metainfo.xml"
  if [[ "$distro" != "$DISTRO_UBUNTU" ]] || [[ "$version" < "$UBUNTU_APPSTREAM_GLIB_REMOVAL_VERSION" ]]; then
    appstream-util validate "build/dev.lizardbyte.app.Sunshine.metainfo.xml"
  fi
  desktop-file-validate "build/dev.lizardbyte.app.Sunshine.desktop"
  if [[ "$appimage_build" == 0 ]]; then
    desktop-file-validate "build/dev.lizardbyte.app.Sunshine.terminal.desktop"
  fi
  return 0
}

function run_step_build() {
  echo "Running step: Build"

  # Setup NVM environment if needed (for web UI builds)
  setup_nvm_environment

  # Build the project
  ninja -C "build"
  return 0
}

function run_step_package() {
  echo "Running step: Package"

  # Create the package
  if [[ "$skip_package" == 0 ]]; then
    if [[ "$distro" == "$DISTRO_DEBIAN" ]] || [[ "$distro" == "$DISTRO_UBUNTU" ]]; then
      cpack -G DEB --config ./build/CPackConfig.cmake
    elif [[ "$distro" == "$DISTRO_FEDORA" ]]; then
      cpack -G RPM --config ./build/CPackConfig.cmake
    fi
  fi
  return 0
}

function run_step_cleanup() {
  echo "Running step: Cleanup"

  # restore the math-vector.h file
  if [[ "$skip_cleanup" == 0 ]] && [[ "$architecture" == "${AARCH64}" ]] && [[ -n "$math_vector_file" ]]; then
    ${sudo_cmd} mv -f "$math_vector_file.bak" "$math_vector_file"
  fi
  return 0
}

function run_install() {
  case "$step" in
    deps)
      run_step_deps
      ;;
    cmake)
      run_step_cmake
      ;;
    validation)
      run_step_validation
      ;;
    build)
      run_step_build
      ;;
    package)
      run_step_package
      ;;
    cleanup)
      run_step_cleanup
      ;;
    all)
      run_step_deps
      run_step_cmake
      run_step_validation
      run_step_build
      run_step_package
      run_step_cleanup
      ;;
    *)
      echo "Invalid step: $step"
      echo "Valid steps are: deps, cmake, validation, build, package, cleanup, all"
      exit 1
      ;;
  esac
  return 0
}

# Determine the OS and call the appropriate function
cat /etc/os-release

# shellcheck disable=SC1091  # trusted system file is unavailable to static analysis
source /etc/os-release
distro="$ID"
version="${VERSION_ID:-}"

case "${distro}:${version}" in
  "${DISTRO_ARCH}":*)
    version=""
    package_update_command="${sudo_cmd} pacman -Syu --noconfirm"
    package_install_command="${sudo_cmd} pacman -Sy --needed"
    nvm_node=0
    gcc_version="15"
    ;;
  "${DISTRO_DEBIAN}":12)
    package_update_command="${sudo_cmd} apt-get update"
    package_install_command="${sudo_cmd} apt-get install -y"
    gcc_version="13"
    nvm_node=0
    ;;
  "${DISTRO_DEBIAN}":13)
    package_update_command="${sudo_cmd} apt-get update"
    package_install_command="${sudo_cmd} apt-get install -y"
    gcc_version="14"
    nvm_node=0
    ;;
  "${DISTRO_FEDORA}":42|"${DISTRO_FEDORA}":43|"${DISTRO_FEDORA}":44)
    package_update_command="${sudo_cmd} dnf update -y"
    package_install_command="${sudo_cmd} dnf install -y"
    gcc_version="14"
    nvm_node=0
    ;;
  "${DISTRO_FEDORA}":45)
    package_update_command="${sudo_cmd} dnf update -y"
    package_install_command="${sudo_cmd} dnf install -y"
    gcc_version="15"
    nvm_node=0
    ;;
  "${DISTRO_UBUNTU}":22.04|"${DISTRO_UBUNTU}":24.04)
    package_update_command="${sudo_cmd} apt-get update"
    package_install_command="${sudo_cmd} apt-get install -y"
    gcc_version="14"
    nvm_node=1
    ;;
  "${DISTRO_UBUNTU}":25.04|"${DISTRO_UBUNTU}":25.10)
    package_update_command="${sudo_cmd} apt-get update"
    package_install_command="${sudo_cmd} apt-get install -y"
    gcc_version="14"
    nvm_node=0
    ;;
  "${DISTRO_UBUNTU}":26.04)
    package_update_command="${sudo_cmd} apt-get update"
    package_install_command="${sudo_cmd} apt-get install -y"
    cuda_patches=1
    if [[ "$force_cuda_runfile" == 0 ]]; then
      cuda_system_package=1
      if [[ -z "$cuda_system_package_name" ]]; then
        cuda_system_package_name="cuda-toolkit-13-1"
      fi
    fi
    gcc_version="14"
    nvm_node=0
    ;;
  "${DISTRO_UBUNTU}":26.10)
    package_update_command="${sudo_cmd} apt-get update"
    package_install_command="${sudo_cmd} apt-get install -y"
    if [[ "$force_cuda_runfile" == 0 ]]; then
      cuda_system_package=1
      if [[ -z "$cuda_system_package_name" ]]; then
        cuda_system_package_name="cuda-toolkit-13-3"
      fi
    fi
    gcc_version="14"
    nvm_node=0
    ;;
  *)
    echo "Unsupported Distro or Version"
    exit 1
    ;;
esac

architecture=$(uname -m)

echo "Detected Distro: $distro"
echo "Detected Version: $version"
echo "Detected Architecture: $architecture"

if [[ "$architecture" != "x86_64" ]] && [[ "$architecture" != "${AARCH64}" ]]; then
  echo "Unsupported Architecture"
  exit 1
fi

# export variables for github actions ci
if [[ -f "$GITHUB_ENV" ]]; then
  {
    echo "CC=gcc-${gcc_version}"
    echo "CXX=g++-${gcc_version}"
    echo "GCC_VERSION=${gcc_version}"
  } >> "$GITHUB_ENV"
fi

# get directory of this script
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
build_dir=$(readlink -f "$script_dir/../build")
echo "Script Directory: $script_dir"
echo "Build Directory: $build_dir"
mkdir -p "$build_dir"

run_install
