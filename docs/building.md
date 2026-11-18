# Building
Sunshine binaries are built using [CMake](https://cmake.org) and requires `cmake` > 3.25.

## Building Locally

### Compiler
It is recommended to use one of the following compilers:

| Compiler    | Version |
|:------------|:--------|
| GCC         | 14+     |
| Clang       | 17+     |
| Apple Clang | 15+     |

### Dependencies

Boost `1.89.0` or newer can be supplied as a system package. When a compatible system package is unavailable, CMake
downloads the project-locked Boost release through CPM. Static Boost libraries are preferred on every platform by
default; pass `-DBOOST_USE_STATIC=OFF` to use shared system libraries instead.

Sunshine's build and packaging automation uses packaged static Boost libraries on Arch Linux, Fedora 44 or newer,
Ubuntu 26.04 or newer, macOS with Homebrew, Windows with MSYS2, and FreeBSD. Older supported distributions continue
to use the CPM fallback because their repositories do not meet the minimum Boost version.

On Linux and FreeBSD, Sunshine and the prebuilt FFmpeg libraries use the system's shared libva core and backends.
These components share a private display-context ABI and must come from the same libva build. Sunshine also uses
the system libva headers so its compatibility shims match the installed API. Bundled libva archives and headers
are excluded from the build. The system provides libdrm, the GPU's VA-API driver, and libva's driver configuration.
AppImages use the same system libraries at runtime; they do not bundle libva.

#### Prebuilt Web UI

Sunshine releases publish the platform-independent Web UI as the `@lizardbyte/sunshine` package to npm and GitHub
Packages. The package contains the production assets under `build/assets/web`, so downstream packagers can install the
Web UI without running Node.js or Vite during the native Sunshine build.

#### FreeBSD
> [!CAUTION]
> Sunshine support for FreeBSD is experimental and may be incomplete or not work as expected

##### Install dependencies
```sh
pkg install -y \
  audio/opus \
  audio/pulseaudio \
  devel/cmake \
  devel/doxygen \
  devel/evdev-proto \
  devel/git \
  devel/libevdev \
  devel/llvm19 \
  devel/ninja \
  devel/pkgconf \
  devel/qt6-base \
  ftp/curl \
  graphics/graphviz \
  graphics/libdrm \
  graphics/qt6-svg \
  graphics/wayland \
  multimedia/libva \
  net/miniupnpc \
  ports-mgmt/pkg \
  security/openssl \
  shells/bash \
  www/npm-node22 \
  x11/libX11 \
  x11/libxcb \
  x11/libXfixes \
  x11/libXrandr \
  x11/libXtst
```

Use LLVM 19 when configuring a local FreeBSD build:

```sh
export CC=clang19
export CXX=clang++19
```

#### Linux
Dependencies vary depending on the distribution. You can reference our
[linux_build.sh](https://github.com/SunshineStream/Sunshine/blob/master/scripts/linux_build.sh) script for a list of
dependencies we use in Debian-based, Fedora-based and Arch-based distributions. Please submit a PR if you would like to extend the
script to support other distributions.

##### KMS Capture
If you are using KMS, patching the Sunshine binary with `setcap` is required. Some post-install scripts handle this. If building
from source and using the binary directly, this will also work:

```bash
sudo cp build/sunshine /tmp
sudo setcap cap_sys_admin,cap_sys_nice+p /tmp/sunshine
sudo getcap /tmp/sunshine
sudo mv /tmp/sunshine build/sunshine
```

##### CUDA Toolkit
When building Sunshine for a system with an NVIDIA GPU, install the CUDA Toolkit before configuring or compiling
Sunshine. CUDA support is selected at compile time and is used for NVFBC capture and direct GPU-memory NVENC encoding.
This applies to every source build, including builds made through the AUR or for third-party repositories such as
Omarchy. Installing the CUDA Toolkit after Sunshine was compiled does not enable CUDA support; Sunshine must be rebuilt.

> [!NOTE]
> Users installing a prebuilt package supplied by LizardByte do not need to install the CUDA Toolkit. This includes the
> Arch Linux package from LizardByte's [pacman-repo](https://github.com/LizardByte/pacman-repo), which is already built
> with CUDA support.

There are two caveats to CUDA:

1. The version installed depends on the version of GCC.
2. The version of CUDA you use will determine compatibility with various GPU generations.
   At the time of writing, the recommended version to use is CUDA ~13.1.
   See [CUDA compatibility](https://docs.nvidia.com/deploy/cuda-compatibility/index.html) for more info.

> [!NOTE]
> To install older versions, select the appropriate run file based on your desired CUDA version and architecture
> according to [CUDA Toolkit Archive](https://developer.nvidia.com/cuda-toolkit-archive)

#### macOS
You can either use [Homebrew](https://brew.sh) or [MacPorts](https://www.macports.org) to install dependencies.

##### Homebrew
```bash
dependencies=(
  "boost"  # Optional
  "cmake"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "icu4c"  # Optional, if boost is not installed
  "miniupnpc"
  "ninja"
  "node"
  "openssl@3"
  "opus"
  "pkg-config"
  "qtbase"
  "qtsvg"
)
brew install "${dependencies[@]}"
```

If there are issues with an SSL header that is not found:

@tabs_grouped{mac-architecture|:|
  @tab{ Intel |:| ```bash
    ln -s /usr/local/opt/openssl/include/openssl /usr/local/include/openssl
    ```}
  @tab{ Apple Silicon |:| ```bash
    ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl
    ```
  }
}

##### MacPorts
```bash
dependencies=(
  "cmake"
  "curl"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "libopus"
  "miniupnpc"
  "ninja"
  "npm9"
  "pkgconfig"
  "qt6-qtbase"
  "qt6-qtsvg"
)
sudo port install "${dependencies[@]}"
```

#### Windows

> [!WARNING]
> Cross-compilation is not supported on Windows. You must build on the target architecture.

First, you need to install [MSYS2](https://www.msys2.org).

For AMD64 startup "MSYS2 UCRT64" (or for ARM64 startup "MSYS2 CLANGARM64") then execute the following commands.

##### Update all packages
```bash
pacman -Syu
```

##### Set toolchain variable
For UCRT64:
```bash
export TOOLCHAIN="ucrt-x86_64"
```

For CLANGARM64:
```bash
export TOOLCHAIN="clang-aarch64"
```

##### Install dependencies
```bash
dependencies=(
  "git"
  "mingw-w64-${TOOLCHAIN}-boost"  # Optional
  "mingw-w64-${TOOLCHAIN}-cmake"
  "mingw-w64-${TOOLCHAIN}-cppwinrt"
  "mingw-w64-${TOOLCHAIN}-curl-winssl"
  "mingw-w64-${TOOLCHAIN}-doxygen"  # Optional, for docs
  "mingw-w64-${TOOLCHAIN}-graphviz"  # Optional, for docs
  "mingw-w64-${TOOLCHAIN}-miniupnpc"
  "mingw-w64-${TOOLCHAIN}-onevpl"
  "mingw-w64-${TOOLCHAIN}-openssl"
  "mingw-w64-${TOOLCHAIN}-opus"
  "mingw-w64-${TOOLCHAIN}-toolchain"
  "mingw-w64-${TOOLCHAIN}-qt6-static"
)
if [[ "${MSYSTEM}" == "UCRT64" ]]; then
  dependencies+=(
    "mingw-w64-${TOOLCHAIN}-MinHook"
    "mingw-w64-${TOOLCHAIN}-nsis"
  )
fi
pacman -S "${dependencies[@]}"
```

Static Qt is enabled by default on Windows. Sunshine automatically adds the MSYS2 static Qt prefix at
`${MINGW_PREFIX}/qt6-static` when that package is installed. If an IDE does not inherit `MINGW_PREFIX`, Sunshine
derives the same prefix from the selected compiler. If static Qt is installed in a custom location, specify it with
`-DCMAKE_PREFIX_PATH=/path/to/qt6-static`.

To use dynamic Qt instead, configure with `-DSUNSHINE_USE_STATIC_QT=OFF` and ensure the dynamic Qt package is
available through the normal toolchain prefix.

To create a WiX installer, you also need to install [.NET](https://dotnet.microsoft.com/download).

To build the frontend, install native [Node.js](https://nodejs.org/en/download) for the target architecture. The
MSYS2 Node.js package is not supported because its shared runtime cannot load the native binding required by Rolldown.

### Clone
Ensure [git](https://git-scm.com) is installed on your system, then clone the repository using the following command:

```bash
git clone https://github.com/SunshineStream/Sunshine.git --recurse-submodules
cd sunshine
mkdir build
```

### Build

```bash
cmake -B build -G Ninja -S .
ninja -C build
```

> [!TIP]
> Available build options can be found in
> [options.cmake](https://github.com/SunshineStream/Sunshine/blob/master/cmake/prep/options.cmake).

### Package

@tabs_grouped{platform|:|
  @tab{FreeBSD |:| @tabs{
    @tab{pkg |:| ```bash
      cpack -G FREEBSD --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{Linux |:| @tabs{
    @tab{deb |:| ```bash
      cpack -G DEB --config ./build/CPackConfig.cmake
      ```}
    @tab{rpm |:| ```bash
      cpack -G RPM --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{macOS |:| @tabs{
    @tab{DragNDrop |:| ```bash
      cpack -G DragNDrop --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{Windows |:| @tabs{
    @tab{NSIS Installer |:| ```bash
      cpack -G NSIS --config ./build/CPackConfig.cmake
      ```}
    @tab{WiX Installer |:| ```bash
      cpack -G WIX --config ./build/CPackConfig.cmake
      ```}
    @tab{Portable |:| ```bash
      cpack -G ZIP --config ./build/CPackConfig.cmake
      ```}
  }}
}

### Remote Build
It may be beneficial to build remotely in some cases. This will enable easier building on different operating systems.

1. Fork the project
2. Activate workflows
3. Trigger the *CI* workflow manually
4. Download the artifacts/binaries from the workflow run summary
