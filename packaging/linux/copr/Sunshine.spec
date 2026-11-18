%global build_timestamp %(date +"%Y%m%d")

# use sed to replace these values
%global build_version 0
%global branch 0
%global commit 0
%global sunshine_python_version 3.14

%undefine _hardened_build

# Define _metainfodir for openSUSE if not already defined
%if 0%{?suse_version}
%if !0%{?_metainfodir:1}
%global _metainfodir %{_datadir}/metainfo
%endif
%endif

Name: Sunshine
Version: %{build_version}
Release: 1%{?dist}
Summary: Self-hosted game stream host for Moonlight.
License: GPLv3-only
URL: https://github.com/SunshineStream/Sunshine
Source0: tarball.tar.gz

# Common BuildRequires
BuildRequires: cmake >= 3.25.0
BuildRequires: desktop-file-utils
BuildRequires: git
BuildRequires: libcap-devel
BuildRequires: libcurl-devel
BuildRequires: libdrm-devel
BuildRequires: libevdev-devel
BuildRequires: libva-devel
BuildRequires: libX11-devel
BuildRequires: libxcb-devel
BuildRequires: libXcursor-devel
BuildRequires: libXfixes-devel
BuildRequires: libXi-devel
BuildRequires: libXinerama-devel
BuildRequires: libXrandr-devel
BuildRequires: libXtst-devel
BuildRequires: openssl-devel
BuildRequires: pipewire-devel
BuildRequires: rpm-build
BuildRequires: systemd-rpm-macros
BuildRequires: wget
BuildRequires: which

%if 0%{?fedora}
# Fedora-specific BuildRequires
BuildRequires: appstream
BuildRequires: glslc
BuildRequires: libappstream-glib
BuildRequires: vulkan-loader-devel
%if 0%{fedora} > 43
BuildRequires: boost-devel >= 1.89.0
BuildRequires: boost-static >= 1.89.0
# needed for npm from nvm
BuildRequires: libatomic
BuildRequires: xz
%endif
BuildRequires: libgudev
BuildRequires: mesa-libGL-devel
BuildRequires: mesa-libgbm-devel
BuildRequires: miniupnpc-devel
%if 0%{?fedora} < 44
BuildRequires: nodejs-npm
%endif
BuildRequires: numactl-devel
BuildRequires: opus-devel
BuildRequires: pulseaudio-libs-devel
BuildRequires: qt6-qtbase-devel
BuildRequires: qt6-qtsvg-devel
BuildRequires: systemd-udev
BuildRequires: uv
%{?sysusers_requires_compat}
# for unit tests
BuildRequires: ImageMagick
BuildRequires: xorg-x11-server-Xvfb
%endif

%if 0%{?suse_version}
# openSUSE-specific BuildRequires
BuildRequires: AppStream
BuildRequires: appstream-glib
BuildRequires: libgudev-1_0-devel
BuildRequires: Mesa-libGL-devel
BuildRequires: libgbm-devel
BuildRequires: libminiupnpc-devel
BuildRequires: libnuma-devel
BuildRequires: libopus-devel
BuildRequires: libpulse-devel
BuildRequires: npm
BuildRequires: python313
BuildRequires: python313-Jinja2
BuildRequires: qt6-base-devel
BuildRequires: qt6-svg-devel
BuildRequires: shaderc
BuildRequires: udev
BuildRequires: vulkan-devel
BuildRequires: xz
# for unit tests
BuildRequires: ImageMagick
BuildRequires: xvfb-run
%endif

# Conditional BuildRequires for cuda-gcc based on distribution version
%if 0%{?fedora}
%if 0%{?fedora} <= 41
BuildRequires: gcc13
BuildRequires: gcc13-c++
%global gcc_version 13
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 42 && 0%{?fedora} <= 43
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 44
BuildRequires: gcc15
BuildRequires: gcc15-c++
%global gcc_version 15
%global cuda_version 13.1.1
%global cuda_build 590.48.01
%global cuda_redist_compiler_version 13.1.115
%global cuda_redist_runtime_version 13.1.80
%global cuda_use_redistributables 1
%endif
%endif

%if 0%{?suse_version}
BuildRequires: gcc15
BuildRequires: gcc15-c++
%global gcc_version 15
%global cuda_version 13.1.1
%global cuda_build 590.48.01
%global cuda_redist_compiler_version 13.1.115
%global cuda_redist_runtime_version 13.1.80
%global cuda_use_redistributables 1
%endif

%global cuda_dir %{_builddir}/cuda

# Common runtime requirements
Requires: miniupnpc >= 2.2.4
Requires: which >= 2.21

%if 0%{?fedora}
# Fedora runtime requirements
Requires: libcap >= 2.22
Requires: libcurl >= 7.0
Requires: libdrm > 2.4.97
Requires: libevdev >= 1.5.6
Requires: libopusenc >= 0.2.1
Requires: libva >= 2.14.0
Requires: libwayland-client >= 1.20.0
Requires: libX11 >= 1.7.3.1
Requires: numactl-libs >= 2.0.14
Requires: openssl >= 3.0.2
Requires: pulseaudio-libs >= 10.0
Requires: qt6-qtbase
Requires: qt6-qtsvg
Requires: vulkan-loader
%endif

%if 0%{?suse_version}
# openSUSE runtime requirements
Requires: libcap2
Requires: libcurl4
Requires: libdrm2
Requires: libevdev2
Requires: libopusenc0
Requires: libva2
Requires: libwayland-client0
Requires: libX11-6
Requires: libnuma1
Requires: libopenssl3
Requires: libpulse0
Requires: libQt6Svg6
Requires: libQt6Widgets6
Requires: libvulkan1
%endif

%description
Self-hosted game stream host for Moonlight.

%prep
# extract tarball to current directory
mkdir -p %{_builddir}/Sunshine
tar -xzf %{SOURCE0} -C %{_builddir}/Sunshine

# list directory
ls -a %{_builddir}/Sunshine

%build
# exit on error
set -e

# Detect the architecture and Fedora version
architecture=$(uname -m)

cuda_supported_architectures=("x86_64" "aarch64")

# prepare CMAKE args
cmake_args=(
  "-B=%{_builddir}/Sunshine/build"
  "-G=Unix Makefiles"
  "-S=."
  "-DBUILD_DOCS=OFF"
  "-DBUILD_WERROR=ON"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_PREFIX=%{_prefix}"
  "-DSUNSHINE_ASSETS_DIR=%{_datadir}/sunshine"
  "-DSUNSHINE_EXECUTABLE_PATH=%{_bindir}/sunshine"
  "-DSUNSHINE_ENABLE_DRM=ON"
  "-DSUNSHINE_ENABLE_KWIN=ON"
  "-DSUNSHINE_ENABLE_PORTAL=ON"
  "-DSUNSHINE_ENABLE_WAYLAND=ON"
  "-DSUNSHINE_ENABLE_X11=ON"
  "-DSUNSHINE_PUBLISHER_NAME=LizardByte"
  "-DSUNSHINE_PUBLISHER_WEBSITE=https://app.lizardbyte.dev"
  "-DSUNSHINE_PUBLISHER_ISSUE_URL=https://app.lizardbyte.dev/support"
)

%if 0%{?fedora}
# uv installs Python and glad's Python dependencies into .venv before CMake runs.
cmake_args+=("-DGLAD_SKIP_PIP_INSTALL=ON")
cmake_args+=("-DPython_EXECUTABLE=%{_builddir}/Sunshine/.venv/bin/python")
%endif

%if 0%{?suse_version}
# Use the Python interpreter that owns the python313-Jinja2 BuildRequires.
cmake_args+=("-DGLAD_SKIP_PIP_INSTALL=ON")
cmake_args+=("-DPython_EXECUTABLE=/usr/bin/python3.13")
%endif

export CC=gcc-%{gcc_version}
export CXX=g++-%{gcc_version}

%if 0%{?cuda_use_redistributables}
function install_cuda_from_redistributables() {
  local cuda_redist_arch="linux-x86_64"
  local cuda_target_arch="x86_64-linux"
  if [ "$architecture" == "aarch64" ]; then
    cuda_redist_arch="linux-sbsa"
    cuda_target_arch="sbsa-linux"
  fi

  local cuda_target_dir="%{cuda_dir}/targets/${cuda_target_arch}"
  mkdir -p "%{cuda_dir}" "${cuda_target_dir}"

  # NVIDIA's monolithic runfile installer requires libxml2.so.2, which current
  # distributions no longer provide. Use the official redistributable archives
  # for CUDA 13 builds so they share one installer-independent CUDA setup.
  local cuda_components=(
    "cuda_nvcc:%{cuda_redist_compiler_version}:root"
    "libnvvm:%{cuda_redist_compiler_version}:root"
    "cuda_cccl:%{cuda_redist_compiler_version}:target"
    "cuda_crt:%{cuda_redist_compiler_version}:target"
    "cuda_cudart:%{cuda_redist_runtime_version}:target"
    "cuda_culibos:%{cuda_redist_compiler_version}:target"
    "libnvptxcompiler:%{cuda_redist_compiler_version}:target"
  )

  local component_data
  for component_data in "${cuda_components[@]}"; do
    local component_name
    local component_version
    local component_destination
    IFS=: read -r component_name component_version component_destination <<< "${component_data}"

    local archive="${component_name}-${cuda_redist_arch}-${component_version}-archive.tar.xz"
    local url="https://developer.download.nvidia.com/compute/cuda/redist/${component_name}/${cuda_redist_arch}/${archive}"
    local extract_dir="${cuda_target_dir}"
    if [ "${component_destination}" == "root" ]; then
      extract_dir="%{cuda_dir}"
    fi

    echo "cuda component url: ${url}"
    wget \
      "${url}" \
      --progress=bar:force:noscroll \
      --retry-connrefused \
      --tries=3 \
      -q -O "%{_builddir}/${archive}"
    tar -xJf "%{_builddir}/${archive}" \
      --directory="${extract_dir}" \
      --strip-components=1
    rm "%{_builddir}/${archive}"
  done

  # nvcc expects this header in its target-specific include directory.
  mv "%{cuda_dir}/include/fatbinary_section.h" "${cuda_target_dir}/include/"
}
%endif

function install_cuda() {
  # check if we need to install cuda
  if [ -f "%{cuda_dir}/bin/nvcc" ]; then
    echo "cuda already installed"
    return
  fi

%if 0%{?cuda_use_redistributables}
  install_cuda_from_redistributables
%else
  local cuda_prefix="https://developer.download.nvidia.com/compute/cuda/"
  local cuda_suffix=""
  if [ "$architecture" == "aarch64" ]; then
    local cuda_suffix="_sbsa"
  fi

  local url="${cuda_prefix}%{cuda_version}/local_installers/cuda_%{cuda_version}_%{cuda_build}_linux${cuda_suffix}.run"
  echo "cuda url: ${url}"
  wget \
    "$url" \
    --progress=bar:force:noscroll \
    --retry-connrefused \
    --tries=3 \
    -q -O "%{_builddir}/cuda.run"
  chmod a+x "%{_builddir}/cuda.run"
  "%{_builddir}/cuda.run" \
    --no-drm \
    --no-man-page \
    --no-opengl-libs \
    --override \
    --silent \
    --toolkit \
    --toolkitpath="%{cuda_dir}"
  rm "%{_builddir}/cuda.run"
%endif

  # we need to patch math_functions.h depending on the CUDA major version
  # see https://forums.developer.nvidia.com/t/error-exception-specification-is-incompatible-for-cospi-sinpi-cospif-sinpif-with-glibc-2-41/323591/3
  local cuda_major
  cuda_major=$(echo "%{cuda_version}" | cut -d. -f1)
  local patch_file=""
  if [ "${cuda_major}" -eq 12 ]; then
    # CUDA 12.x: the extern declarations lack noexcept(true); add it to match glibc 2.41.
    patch_file="cuda-12-math_functions.patch"
  elif [ "${cuda_major}" -eq 13 ]; then
    # CUDA 13.x: the extern declarations already have noexcept(true), but the __func__()
    # macro invocations at the bottom still lack it, causing a redeclaration conflict.
    patch_file="cuda-13-math_functions.patch"
  else
    echo "Warning: no math_functions.h patch available for CUDA ${cuda_major}.x, skipping."
  fi

  if [ -n "${patch_file}" ]; then
    echo "Applying CUDA patch: ${patch_file}"
    patch -p2 \
      --backup \
      --directory="%{cuda_dir}" \
      --verbose \
      < "%{_builddir}/Sunshine/packaging/linux/patches/${architecture}/${patch_file}"
  fi
}

if [ -n "%{cuda_version}" ] && [[ " ${cuda_supported_architectures[@]} " =~ " ${architecture} " ]]; then
  install_cuda
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=ON")
  cmake_args+=("-DCMAKE_CUDA_COMPILER:PATH=%{cuda_dir}/bin/nvcc")
  cmake_args+=("-DCMAKE_CUDA_HOST_COMPILER=gcc-%{gcc_version}")
else
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=OFF")
fi

# Install and setup NVM for Fedora 44+
%if 0%{?fedora} > 43
echo "Installing NVM for Fedora 44+..."
export HOME=${HOME:-/builddir}
export NVM_DIR="$HOME/.nvm"

# Install NVM
if [ ! -d "$NVM_DIR" ]; then
  wget -qO- https://raw.githubusercontent.com/nvm-sh/nvm/master/install.sh | bash
fi

# Load NVM
export NVM_DIR="$HOME/.nvm"
[ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"

# Install and use Node.js
nvm install node
nvm use node

echo "Node.js version: $(node --version)"
echo "npm version: $(npm --version)"
echo "npm location: $(which npm)"
echo "node location: $(which node)"

# Add npm and node path to cmake args
NPM_PATH=$(which npm)
NODE_PATH=$(which node)
cmake_args+=("-DNPM=${NPM_PATH}")

# Add node bin directory to PATH for make
export PATH="$(dirname ${NODE_PATH}):${PATH}"
%endif

# setup the version
export BRANCH=%{branch}
export BUILD_VERSION=v%{build_version}
export COMMIT=%{commit}

# cmake
cd %{_builddir}/Sunshine
%if 0%{?fedora}
uv python install %{sunshine_python_version}
uv sync \
  --locked \
  --only-group glad \
  --python %{sunshine_python_version} \
  --no-install-project
%endif
echo "cmake args:"
echo "${cmake_args[@]}"
cmake "${cmake_args[@]}"
make -j$(nproc) -C "%{_builddir}/Sunshine/build"

%check
# validate the metainfo file
appstreamcli validate %{buildroot}%{_metainfodir}/*.metainfo.xml
appstream-util validate --nonet %{buildroot}%{_metainfodir}/*.metainfo.xml
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop

# run tests
cd %{_builddir}/Sunshine/build
xvfb-run ./tests/test_sunshine

%install
# Load NVM for Fedora 44+ so npm is available during make install
%if 0%{?fedora} > 43
export HOME=${HOME:-/builddir}
export NVM_DIR="$HOME/.nvm"
[ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"
nvm use node

# Add node bin directory to PATH for make install
NODE_PATH=$(which node)
export PATH="$(dirname ${NODE_PATH}):${PATH}"

echo "Node.js version: $(node --version)"
echo "npm version: $(npm --version)"
%endif

cd %{_builddir}/Sunshine/build
%make_install

%post
# Note: this is copied from the postinst script

# Load uhid for descriptor-driven gamepad emulation
echo "Loading uhid kernel module for gamepad emulation."
modprobe uhid

# Check if we're in an rpm-ostree environment
if [ ! -x "$(command -v rpm-ostree)" ]; then
  echo "Not in an rpm-ostree environment, proceeding with post install steps."

  # Reload the rules and reapply them to virtual gamepad child nodes.
  path_to_udevadm=$(which udevadm)
  if [ -x "$path_to_udevadm" ]; then
    echo "Reloading udev rules."
    $path_to_udevadm control --reload-rules
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uinput
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uhid
    $path_to_udevadm trigger --subsystem-match=hidraw
    $path_to_udevadm trigger --subsystem-match=input
    echo "Udev rules reloaded successfully."
  else
    echo "error: udevadm not found or not executable."
  fi
else
  echo "rpm-ostree environment detected, skipping post install steps. Restart to apply the changes."
fi

%files
# Executables
%caps(cap_sys_admin,cap_sys_nice+p) %{_bindir}/sunshine

# Systemd unit files for user services
%{_userunitdir}/*.service

# Udev rules
%{_udevrulesdir}/*-sunshine.rules

# Modules-load configuration
%{_modulesloaddir}/*-sunshine.conf

# Desktop entries
%{_datadir}/applications/*.desktop

# Icons
%{_datadir}/icons/hicolor/scalable/apps/*.Sunshine.svg

# Metainfo
%{_datadir}/metainfo/*.metainfo.xml

# Assets
%{_datadir}/sunshine/**

%changelog
