# Getting Started

The recommended method for running Sunshine is to use the [binaries](#binaries) included in the
[latest release][latest-release], unless otherwise specified.

[Pre-releases](https://github.com/SunshineStream/Sunshine/releases) are also available. These should be considered beta,
and release artifacts may be missing when merging changes on a faster cadence.

## Binaries

Binaries of Sunshine are created for each release. They are available for FreeBSD, Linux, macOS, and Windows.
Binaries can be found in the [latest release][latest-release].

> [!NOTE]
> Some third party packages also exist.
> See [Third Party Packages](third_party_packages.md) for more information.
> No support will be provided for third party packages!

## Install

### Docker

> [!WARNING]
> The Docker images are not recommended for most users.

Docker images are available on [Dockerhub.io](https://hub.docker.com/repository/docker/lizardbyte/sunshine)
and [ghcr.io](https://github.com/orgs/LizardByte/packages?repo_name=sunshine).

See [Docker](../DOCKER_README.md) for more information.

### FreeBSD

#### Install
1. Download the appropriate package for your architecture

   | Architecture  | Package                                                                                                                                |
   |---------------|----------------------------------------------------------------------------------------------------------------------------------------|
   | amd64/x86_64  | [Sunshine-FreeBSD-14.4-amd64.pkg](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-FreeBSD-14.3-amd64.pkg)     |
   | arm64/aarch64 | [Sunshine-FreeBSD-14.4-aarch64.pkg](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-FreeBSD-14.3-aarch64.pkg) |

2. Open terminal and run the following command.
   ```sh
   sudo pkg install ./Sunshine-FreeBSD-14.4-{arch}.pkg
   ```

#### Uninstall
```sh
sudo pkg delete Sunshine
```

### Linux

LizardByte publishes DEB and RPM packages to the
[stable Cloudsmith repository](https://cloudsmith.io/~lizardbyte/repos/stable/) for releases and the
[beta Cloudsmith repository](https://cloudsmith.io/~lizardbyte/repos/beta/) for prereleases. The packages also remain
available for manual download from each GitHub release.

**CUDA Compatibility**

CUDA is used for NVFBC capture and direct GPU-memory NVENC encoding.

> [!IMPORTANT]
> CUDA support is selected when Sunshine is compiled. If you build Sunshine for an NVIDIA GPU yourself, including
> through the AUR or for a third-party repository such as Omarchy, install the CUDA Toolkit before building Sunshine.
> Installing CUDA after Sunshine has been compiled does not add CUDA support; Sunshine must be rebuilt.
>
> This requirement does not apply when installing a prebuilt package supplied by LizardByte. In particular, the
> Arch Linux package from LizardByte's [pacman-repo](https://github.com/LizardByte/pacman-repo) is already built with
> CUDA support, so its users do not need to install the CUDA Toolkit.

> [!NOTE]
> See [CUDA GPUS](https://developer.nvidia.com/cuda-gpus) to cross-reference Compute Capability to your GPU.
> The table below applies to packages provided by LizardByte.

<table>
    <caption>CUDA Compatibility</caption>
    <tr>
        <th>CUDA Version</th>
        <th>Min Driver</th>
        <th>CUDA Compute Capabilities</th>
        <th>Package</th>
    </tr>
    <tr>
        <td rowspan="5">13.1.1</td>
        <td rowspan="5">590.48.01</td>
        <td rowspan="5">50;52;60;61;62;70;72;75;80;86;87;89;90;100;101;103;120;121</td>
        <td>Sunshine_{version}_{arch}.AppImage</td>
    </tr>
    <tr>
        <td>sunshine_{version}-1+{distro}{distro-version}_{arch}.deb</td>
    </tr>
    <tr>
        <td>sunshine_{arch}.flatpak</td>
    </tr>
    <tr>
        <td>Sunshine-{version}-1.{distro+version}.{arch}.rpm</td>
    </tr>
    <tr>
        <td>sunshine.pkg.tar.zst</td>
    </tr>
</table>

#### Alpine Linux

> [!IMPORTANT]
> The Alpine package is dynamically linked against Alpine 3.24 libraries. CUDA and NVFBC capture are not available in
> this package, but the other capture and encoding backends supported by the system remain enabled.

##### Install

1. Download `sunshine_{version}_alpine{distro-version}_{arch}.apk` from the [latest release][latest-release].
2. Install the package as root. Release APKs use a per-build signing key, so explicitly allow the downloaded package.

   ```sh
   apk add --allow-untrusted ./sunshine_{version}_alpine{distro-version}_{arch}.apk
   ```

##### Uninstall

```sh
apk del sunshine
```

#### AppImage

> [!CAUTION]
> Use distro-specific packages instead of the AppImage if they are available.
> AppImage does not support KMS capture.

> [!NOTE]
> The AppImage is built on Ubuntu 22.04, which requires `glibc 2.35` or newer and `libstdc++ 3.4.11` or newer.
> It also requires the host's libva core and DRM libraries (`libva2` and `libva-drm2` on Debian/Ubuntu).
> Install your GPU's VA-API driver to use VA-API encoding. Both libva libraries must come from the same system package version.

##### Install
1. Download `Sunshine_{version}_{arch}.AppImage`
   into your home directory.
   ```bash
   cd ~
   wget https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine_{version}_{arch}.AppImage
   ```
2. Open terminal and run the following command.
   ```bash
   ./Sunshine_{version}_{arch}.AppImage --install
   ```

##### Run
```bash
./Sunshine_{version}_{arch}.AppImage --install && ./Sunshine_{version}_{arch}.AppImage
```

##### Uninstall
```bash
./Sunshine_{version}_{arch}.AppImage --remove
```

#### ArchLinux

> [!CAUTION]
> Use AUR packages at your own risk.

> [!IMPORTANT]
> NVIDIA users installing Sunshine from LizardByte's pacman-repo do not need the CUDA Toolkit. If Sunshine is compiled
> locally from the AUR or by another package provider, such as Omarchy, the builder must install the CUDA Toolkit before
> compilation. Installing CUDA after the package was built cannot enable CUDA support in that package.

##### Install Prebuilt Packages
Follow the instructions at LizardByte's [pacman-repo](https://github.com/LizardByte/pacman-repo) to add
the repository. Then run the following command.
```bash
pacman -S sunshine
```

##### Install PKGBUILD Archive
Open terminal and run the following command.
```bash
wget https://github.com/SunshineStream/Sunshine/releases/latest/download/sunshine.pkg.tar.gz
tar -xvf sunshine.pkg.tar.gz
cd sunshine

# install optional dependencies
pacman -S cuda  # Nvidia GPU encoding support
pacman -S libva-mesa-driver  # AMD GPU encoding support

makepkg -si
```

##### Uninstall
```bash
pacman -R sunshine
```

#### Debian/Ubuntu

##### Install from Cloudsmith

Configure the `stable` repository for releases or the `beta` repository for prereleases. The setup script
automatically selects the appropriate Debian or Ubuntu release.

@tabs_grouped{release-channel|:|
  @tab{ Stable |:| ```bash
    curl -1sLf 'https://dl.cloudsmith.io/public/lizardbyte/stable/cfg/setup/bash.deb.sh' | sudo -E bash
    ```}
  @tab{ Beta |:| ```bash
    curl -1sLf 'https://dl.cloudsmith.io/public/lizardbyte/beta/cfg/setup/bash.deb.sh' | sudo -E bash
    ```}
}

Install Sunshine after configuring the repository.

```bash
sudo apt update
sudo apt install sunshine
```

##### Install from GitHub releases

Download `sunshine_{version}-1+{distro}{distro-version}_{arch}.deb` and run the following command.
```bash
sudo apt install ./sunshine_{version}-1+{distro}{distro-version}_{arch}.deb
```

> [!NOTE]
> The `{version}` is the Sunshine version. The `1+{distro}{distro-version}` suffix is the Debian package revision and
> identifies the distro used to build it. The `{arch}` is the architecture of your operating system.

> [!TIP]
> You can double-click the deb file to see details about the package and begin installation.

##### Uninstall
```bash
sudo apt remove sunshine
```

#### Fedora/OpenSUSE

> [!TIP]
> The package name is case-sensitive.

##### Install from Cloudsmith

Configure the `stable` repository for releases or the `beta` repository for prereleases. Cloudsmith's setup script
automatically selects Fedora or openSUSE and the appropriate release.

@tabs_grouped{release-channel|:|
  @tab{ Stable |:| ```bash
    curl -1sLf 'https://dl.cloudsmith.io/public/lizardbyte/stable/cfg/setup/bash.rpm.sh' | sudo -E bash
    ```}
  @tab{ Beta |:| ```bash
    curl -1sLf 'https://dl.cloudsmith.io/public/lizardbyte/beta/cfg/setup/bash.rpm.sh' | sudo -E bash
    ```}
}

Install Sunshine with your distribution's package manager.

@tabs_grouped{distribution|:|
  @tab{ Fedora |:| ```bash
    sudo dnf install Sunshine
    ```}
  @tab{ openSUSE |:| ```bash
    sudo zypper install Sunshine
    ```}
}

##### Install from GitHub releases

Download `Sunshine-{version}-1.{distro+version}.{arch}.rpm` and run the following command.

@tabs_grouped{distribution|:|
  @tab{ Fedora |:| ```bash
    sudo dnf install ./Sunshine-{version}-1.{distro+version}.{arch}.rpm
    ```}
  @tab{ openSUSE |:| ```bash
    sudo zypper install ./Sunshine-{version}-1.{distro+version}.{arch}.rpm
    ```}
}

> [!NOTE]
> The `{version}` is the Sunshine version. The `1` is the RPM package release. The `{distro+version}` is the distro and
> distro version of the distro we built the package on. The `{arch}` is the architecture of your operating system.

> [!TIP]
> You can double-click the rpm file to see details about the package and begin installation.

##### Uninstall

@tabs_grouped{distribution|:|
  @tab{ Fedora |:| ```bash
    sudo dnf remove Sunshine
    ```}
  @tab{ openSUSE |:| ```bash
    sudo zypper remove Sunshine
    ```}
}

##### Install (Copr)

> [!IMPORTANT]
> Stable builds are only available if the Sunshine release was made after the Fedora version release.
> Because of this, it is often recommended to use the beta copr; however, you do not need to regularly update.
> This could lead to annoyances in rare cases where there may be a breaking change.

1. Enable copr repository.
   ```bash
   sudo dnf copr enable lizardbyte/stable
   ```

   or
   ```bash
   sudo dnf copr enable lizardbyte/beta
   ```

2. Install the package.
   ```bash
   sudo dnf install Sunshine
   ```

##### Uninstall
```bash
sudo dnf remove Sunshine
```

#### Flatpak

> [!CAUTION]
> Use distro-specific packages instead of the Flatpak if they are available.
> Flatpak does not support KMS capture.

Using this package requires that you have [Flatpak](https://flatpak.org/setup) installed.

##### Download (local option)
1. Download `sunshine_{arch}.flatpak` and run the following command.

   > [!NOTE]
   > Replace `{arch}` with your system architecture.

##### Install (system level)
**Flathub**
```bash
flatpak install --system flathub dev.lizardbyte.app.Sunshine
```

**Local**
```bash
flatpak install --system ./sunshine_{arch}.flatpak
```

##### Install (user level)
**Flathub**
```bash
flatpak install --user flathub dev.lizardbyte.app.Sunshine
```

**Local**
```bash
flatpak install --user ./sunshine_{arch}.flatpak
```

##### Additional installation (required)
Run this command after installing or updating the Flatpak so the privileged host
udev rules stay synchronized with Sunshine:

```bash
flatpak run --command=additional-install.sh dev.lizardbyte.app.Sunshine
```

The script prompts for administrator authorization. If a step fails, resolve the
reported error and rerun the command before starting Sunshine.

##### Run with NVFBC capture (X11 Only) or XDG Portal (Wayland Only)
```bash
flatpak run dev.lizardbyte.app.Sunshine
```

##### Uninstall
```bash
flatpak run --command=remove-additional-install.sh dev.lizardbyte.app.Sunshine
flatpak uninstall --delete-data dev.lizardbyte.app.Sunshine
```

#### Homebrew

> [!IMPORTANT]
> The Homebrew package is experimental on Linux.

This package requires that you have [Homebrew](https://docs.brew.sh/Installation) installed.

##### Install
```bash
brew update
brew upgrade
brew tap LizardByte/homebrew
brew install sunshine
sudo "$(brew --prefix sunshine)/bin/postinst"
```

##### Uninstall
```bash
brew uninstall sunshine
```

> [!TIP]
> For beta you can replace `sunshine` with `sunshine-beta` in the above commands.

### macOS

> [!IMPORTANT]
> Virtual gamepads require the separately installed, licensed
> [Virtual HID Broker](https://github.com/LizardByte/libvirtualhid/releases/latest).

To use gamepads, download the macOS universal libvirtualhid DMG from the link above. Open it and run
**Install libvirtualhid.command** to install Virtual HID Broker. In **System Settings > Privacy & Security >
Device Control and Data Access**, add `/Applications/VirtualHIDBroker.app` and enable it. Restart the broker if
you changed this permission with `sudo launchctl kickstart -k system/dev.lizardbyte.app.libvirtualhid`.
Activate the machine license in Sunshine’s
**Troubleshooting > Virtual HID Broker License** section. In **Configuration > Input**, turn on **Enable Gamepad Input**
and choose the emulated gamepad. The Gamepad Backend setting can be set to **None** to disable gamepads without affecting
keyboard or mouse input. Sunshine’s menu bar **Virtual HID Broker** submenu shows the license status and links
for license management and downloads. Sunshine’s DMG does not contain the broker. Keyboard and mouse input use the
standard macOS synthetic-input permission path.

#### DMG

Prebuilt DMG packages require macOS 15.0 (Sequoia) or newer.

##### Install

1. Download and install based on your architecture:

   | Architecture          | Package                                                                                                                |
   |-----------------------|------------------------------------------------------------------------------------------------------------------------|
   | arm64 (Apple Silicon) | [Sunshine-macOS-arm64.dmg](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-macOS-arm64.dmg)   |
   | x86_64 (Intel)        | [Sunshine-macOS-x86_64.dmg](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-macOS-x86_64.dmg) |

2. Open the downloaded `.dmg` file.
3. Drag `Sunshine.app` into the `Applications` folder.
4. Eject the disk image.

##### Uninstall
1. Quit Sunshine if it is running.
2. Open `Finder`, navigate to `Applications`, and drag `Sunshine.app` to the Trash.

#### Homebrew
This package requires that you have [Homebrew](https://docs.brew.sh/Installation) installed.

##### Install
```bash
brew update
brew upgrade
brew tap LizardByte/homebrew
brew install sunshine
```

##### Uninstall
```bash
brew uninstall sunshine
```

> [!TIP]
> For beta you can replace `sunshine` with `sunshine-beta` in the above commands.

### Windows

> [!NOTE]
> Sunshine supports ARM64 on Windows; however, this should be considered experimental. This version does not properly
> support GPU scheduling and any hardware acceleration.

#### Installer (recommended)

> [!CAUTION]
> The msi installer is preferred moving forward. Before using a different type of installer, you should manually
> uninstall the previous installation.

1. Download and install based on your architecture:

   | Architecture          | Installer                                                                                                                                    |
   |-----------------------|----------------------------------------------------------------------------------------------------------------------------------------------|
   | AMD64/x64 (Intel/AMD) | [Sunshine-Windows-AMD64-installer.msi](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-installer.msi) |
   | ARM64                 | [Sunshine-Windows-ARM64-installer.msi](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-Windows-ARM64-installer.msi) |

> [!TIP]
> Installer logs can be found in the following directory:
> `%%TEMP%/Sunshine/logs/install/`

> [!CAUTION]
> You should carefully select or unselect the options you want to install. Do not blindly install or
> enable features.

To uninstall, find Sunshine in the list <a href="ms-settings:installed-apps">here</a> and select "Uninstall" from the
overflow menu. Different versions of Windows may provide slightly different steps for uninstall.

#### Standalone (lite version)

> [!WARNING]
> By using this package instead of the installer, performance will be reduced. This package is not
> recommended for most users. No support will be provided!

1. Download and extract based on your architecture:

   | Architecture          | Installer                                                                                                                          |
   |-----------------------|------------------------------------------------------------------------------------------------------------------------------------|
   | AMD64/x64 (Intel/AMD) | [Sunshine-Windows-AMD64-lite.zip](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-lite.zip) |
   | ARM64                 | [Sunshine-Windows-ARM64-lite.zip](https://github.com/SunshineStream/Sunshine/releases/latest/download/Sunshine-Windows-ARM64lite.zip)  |

2. Open command prompt as administrator
3. Firewall rules

   Install:
   ```bash
   cd /d {path to extracted directory}
   scripts/add-firewall-rule.bat
   ```

   Uninstall:
   ```bash
   cd /d {path to extracted directory}
   scripts/delete-firewall-rule.bat
   ```

4. Windows service

   Install:
   ```bash
   cd /d {path to extracted directory}
   scripts/install-service.bat
   scripts/autostart-service.bat
   ```

   Uninstall:
   ```bash
   cd /d {path to extracted directory}
   scripts/uninstall-service.bat
   ```

## Initial Setup
After installation, some initial setup is required.

### FreeBSD

#### Virtual Input Devices

> [!IMPORTANT]
> To use virtual input devices (keyboard, mouse, gamepads), you must add your user to the `input` group.

The installation process creates the `input` group and configures permissions for `/dev/uinput`.
To allow your user to create virtual input devices, run:

```bash
pw groupmod input -m $USER
```

After adding yourself to the group, log out and log back in for the changes to take effect.

### Linux

#### Services

**Start once**
```bash
systemctl --user start app-dev.lizardbyte.app.Sunshine
```

**Start on boot**
```bash
systemctl --user --now enable app-dev.lizardbyte.app.Sunshine
```

> [!NOTE]
> The service has been renamed to "app-dev.lizardbyte.app.Sunshine" in order to increase compatibility with
> XDG Desktop Portal, but it is also aliased to "sunshine.service" for convenience.

### macOS
On first launch, Sunshine requests Screen Recording and keyboard and mouse control when those features are enabled.
It also requests Microphone access if you configured a custom **Audio Sink**, and optional Notifications access when
the system tray is enabled. Open **Troubleshooting > Permissions** in the Web UI to review these permissions or
open their System Settings pages. Sunshine restarts once after missing required permissions are granted, including
when a permission was removed and later restored.
If macOS offers **Quit & Reopen** after you allow Screen Recording, choose it so the running process receives the new
access. Sunshine exits its tray and server threads before macOS reopens it.

On macOS 15 and newer, macOS requests Local Network access when Sunshine advertises itself with Bonjour. System Audio
Recording access is requested once at startup with a brief audio tap, when the first stream captures audio if startup
access was unavailable, or when you use its Web UI button. The button also opens Screen & System Audio Recording
settings. macOS does not offer Sunshine a passive status check for Local Network
or System Audio Recording, so the Web UI identifies them as permissions handled when used. The Local Network button
opens **Privacy & Security**; choose **Local Network** there to manage app access.
Sunshine uses libvirtualhid to send keyboard and mouse events through CoreGraphics. Virtual gamepads use the separately
installed Virtual HID Broker described in the macOS gamepad setup above. Sunshine shows the broker's availability and
license under **Troubleshooting > Virtual Input**.
If a macOS privacy switch is enabled but Sunshine still reports access denied, remove that Sunshine entry and add the
installed app again. Development builds should use a consistent Apple-issued signing identity so macOS can recognize
the app across updates.

Sunshine supports native system audio capture on macOS 14.0 (Sonoma) and newer via Apple’s Audio Tap API.
To use it, simply leave the **Audio Sink** setting blank.

If you prefer to manage your own loopback device, you can still use
[Soundflower](https://github.com/mattingalls/Soundflower) or
[BlackHole](https://github.com/ExistentialAudio/BlackHole)
and enter its device name in the [audio_sink](configuration.md#audio_sink) field.

> [!NOTE]
> Command Keys are not forwarded by Moonlight. Right Option-Key is mapped to CMD-Key.

> [!CAUTION]
> Gamepads are not currently supported.

### Windows
Sunshine supports two virtual gamepad backends on Windows. You can install the
[Virtual HID Broker](https://github.com/LizardByte/libvirtualhid/releases/latest) separately as an optional paid upgrade.
Its Windows package includes a broker service and user-mode driver for a Raw Input keyboard and mouse plus full virtual
gamepad support. ViGEmBus remains available as a limited alternative for Xbox 360 and DualShock 4 gamepads, but it has reached end of life.

When Virtual HID Broker is used, Sunshine requires libvirtualhid version `2026.914.1218.10` or newer.

Compared with the ViGEmBus fallback, Virtual HID Broker can create Xbox One, Xbox Series, DualSense, Nintendo Switch
Pro, and Generic gamepads in addition to Xbox 360 and DualShock 4. It can also expose controller-specific features such
as motion, touchpads, LEDs, and adaptive triggers when supported. Virtual HID Broker is actively developed and
supported by the LizardByte team.

With a compatible driver and active license, normal key transitions are exposed through a real HID keyboard so
applications using Raw Input can receive them. Unicode text input and keys outside the supported HID keyboard page
continue to use Windows input injection. When the driver-backed keyboard cannot be created because the driver,
broker, or license is unavailable, libvirtualhid retains its legacy SendInput fallback.

Relative mouse movement, buttons, and scrolling are exposed as a real HID
mouse so applications using Raw Input can receive them. Absolute mouse positioning continues to use Windows input
injection. When the driver-backed mouse cannot be created, libvirtualhid retains its legacy SendInput fallback.

The Virtual HID Broker requires an active paid machine license for driver-backed devices, including gamepads and the Raw
Input keyboard and mouse. Sunshine shows the current license status and actions on the Web UI Troubleshooting page and
in the **Virtual HID Broker** system tray submenu. In **Configuration > Input**, choose whether Sunshine may use **All
Available Drivers**, only **Virtual HID Broker**, or only **ViGEmBus**. Sunshine continues to show the selection prompt
until this setting is saved. If an active Virtual HID Broker license is already present, Sunshine selects **All Available
Drivers** automatically. That policy prefers Virtual HID Broker. When its license is not valid, Sunshine falls back to
ViGEmBus for Xbox 360 and DualShock 4 gamepads and to SendInput for keyboard and mouse. Selecting only **ViGEmBus**
suppresses Virtual HID Broker startup notifications and limits the available emulated gamepads to Xbox 360 and
DualShock 4. **None** disables all virtual gamepads and their startup notices without affecting keyboard or mouse
input. Sunshine recreates the shared keyboard and mouse after a successful license action, so switching between
the HID and SendInput paths does not require restarting Sunshine.

After installing or updating virtual input drivers, it is recommended to restart your computer.

## Usage

### Basic usage
If Sunshine is not installed/running as a service, then start Sunshine with the following command, unless a start
command is listed in the specified package [install](#install) instructions above.

> [!NOTE]
> A service is a process that runs in the background. This is the default when installing Sunshine from the
> Windows installer. Running multiple instances of Sunshine is not advised.

```bash
sunshine
```

### Specify config file
```bash
sunshine <directory of conf file>/sunshine.conf
```

> [!NOTE]
> This step is optional, you do not need to specify a config file.
> If no config file is entered, the default location will be used.
> The configuration file specified will be created if it doesn't exist.

### Start Sunshine over SSH (Linux/X11)
Assuming you are already logged into the host, you can use this command

```bash
ssh <user>@<ip_address> 'export DISPLAY=:0; sunshine'
```

If you are logged into the host with only a tty (teletypewriter), you can use `startx` to start the X server prior to
executing Sunshine. You nay need to add `sleep` between `startx` and `sunshine` to allow more time for the display to
be ready.

```bash
ssh <user>@<ip_address> 'startx &; export DISPLAY=:0; sunshine'
```

> [!TIP]
> You could also use the `~/.bash_profile` or `~/.bashrc` files to set up the `DISPLAY` variable.

@seealso{See [Remote SSH Headless Setup](https://app.lizardbyte.dev/2023-09-14-remote-ssh-headless-sunshine-setup)
on how to set up a headless streaming server without autologin and dummy plugs (X11 + NVidia GPUs)}

### Configuration

Sunshine is configured via the web ui, which is available on [https://localhost:47990](https://localhost:47990)
by default. You may replace *localhost* with your internal ip address.

> [!NOTE]
> Ignore any warning given by your browser about "insecure website". This is due to the SSL certificate
> being self-signed.

> [!CAUTION]
> If running for the first time, make sure to note the username and password that you created.

1. Change the web-ui to your desired theme, using the dropdown menu in the navbar.
   ![Theme Selection](images/split-themes.png)
2. Add games and applications.
   ![Applications](images/applications.png)
3. Adjust any configuration settings as needed. You can search for options in the search bar.
   ![Configuration](images/configuration-search.png)
4. Find Moonlight clients and other tools for Sunshine in the `Featured Apps` tab.
   ![Featured Apps](images/featured-apps.png)
5. In Moonlight, you may need to add the PC manually.
6. When Moonlight requests for you insert the pin:

   - Login to the web-ui
   - Go to "PIN" in the Navbar
   - Type in your PIN and press `Enter`, and enter a name of your choosing for the device.
     You should get a Success Message!
   - In Moonlight, select one of the Applications listed

7. If you run into issues, logs are available in the `Troubleshooting` tab.
   You can navigate through each warning/error message for clues to the issue.

   ![Logs](images/troubleshooting-logs.png)

### Arguments
To get a list of available arguments, run the following command.

@tabs_grouped{linux-package|:|
   @tab{ General |:| ```bash
      sunshine --help
      ```}
   @tab{ AppImage |:| ```bash
      ./Sunshine_{version}_{arch}.AppImage --help
      ```}
   @tab{ Flatpak |:| ```bash
      flatpak run --command=sunshine dev.lizardbyte.app.Sunshine --help
      ```}
}

### Shortcuts
All shortcuts start with `Ctrl+Alt+Shift`, just like Moonlight.

* `Ctrl+Alt+Shift+N`: Hide/Unhide the cursor (This may be useful for Remote Desktop Mode for Moonlight)
* `Ctrl+Alt+Shift+F1/F12`: Switch to different monitor for Streaming

### Application List
* Applications should be configured via the web UI
* A basic understanding of working directories and commands is required
* You can use Environment variables in place of values
* `$(HOME)` will be replaced by the value of `$HOME`
* `$$` will be replaced by `$`, e.g. `$$(HOME)` will be become `$(HOME)`
* `env` - Adds or overwrites Environment variables for the commands/applications run by Sunshine.
  This can only be changed by modifying the `apps.json` file directly.

### Considerations
* On Windows, Sunshine uses the Desktop Duplication API which only supports capturing from the GPU used for display.
  If you want to capture and encode on the eGPU, connect a display or HDMI dummy display dongle to it and run the games
  on that display.
* When an application is started, if there is an application already running, it will be terminated.
* If any of the prep-commands fail, starting the application is aborted.
* When the application has been shutdown, the stream shuts down as well.

  * For example, if you attempt to run `steam` as a `cmd` instead of `detached` the stream will immediately fail.
    This is due to the method in which the steam process is executed. Other applications may behave similarly.
  * This does not apply to `detached` applications.

* The "Desktop" app works the same as any other application except it has no commands. It does not start an application,
  instead it simply starts a stream. If you removed it and would like to get it back, just add a new application with
  the name "Desktop" and "desktop.png" as the image path.
* If inputs (mouse, keyboard, gamepads...) aren't working after connecting:

  * On FreeBSD/Linux, add the user running sunshine to the `input` group.

* The FreeBSD version of Sunshine is missing some features that are present on Linux.
  The following are known limitations.

  * Only X11 and Wayland capture are supported
  * Gamepads use libvirtualhid's uinput backend, so descriptor-driven features such as motion, touchpad input,
    battery state, RGB LEDs, adaptive triggers, and raw HID output reports are unavailable


### HDR Support
Streaming HDR content is officially supported on Windows hosts and experimentally supported for Linux hosts.

* General HDR support information and requirements:

  * HDR must be activated in the host OS, which may require an HDR-capable display or EDID emulator dongle
    connected to your host PC.
  * You must also enable the HDR option in your Moonlight client settings, otherwise the stream will be SDR
    (and probably overexposed if your host is HDR).
  * A good HDR experience relies on proper HDR display calibration both in the OS and in game. HDR calibration can
    differ significantly between client and host displays.
  * You may also need to tune the brightness slider or HDR calibration options in game to the different HDR brightness
    capabilities of your client's display.
  * Some GPUs video encoders can produce lower image quality or encoding performance when streaming in HDR compared
    to SDR.

Additional information:

@tabs_grouped{platform|:|
  @tab{ Windows |:|
  - HDR streaming is supported for Intel, AMD, and NVIDIA GPUs that support encoding HEVC Main 10 or AV1 10-bit profiles.
  - We recommend calibrating the display by streaming the Windows HDR Calibration app to your client device and saving an HDR calibration profile to use while streaming.
  - Older games that use NVIDIA-specific NVAPI HDR rather than native Windows HDR support may not display properly in HDR.
  }

@tab{ Linux |:|
  - HDR streaming is supported for Intel and AMD GPUs that support encoding HEVC Main 10 or AV1 10-bit profiles using VAAPI.
  - The KMS capture backend is required for HDR capture. Other capture methods, like NvFBC or X11, do not support HDR.
  - You will need a desktop environment with a compositor that supports HDR rendering, such as Gamescope or KDE Plasma 6.

  @seealso{[Arch wiki on HDR Support for Linux](https://wiki.archlinux.org/title/HDR_monitor_support) and
  [Reddit Guide for HDR Support for AMD GPUs](https://www.reddit.com/r/linux_gaming/comments/10m2gyx/guide_alpha_test_hdr_on_linux)}
  }
}

### Tutorials and Guides
Tutorial videos are available [here](https://www.youtube.com/playlist?list=PLMYr5_xSeuXAbhxYHz86hA1eCDugoxXY0).

Guides are available [here](guides.md).

@admonition{Community! |:|
Tutorials and Guides are community generated. Want to contribute? Reach out to us on our discord server.}


[latest-release]: https://github.com/SunshineStream/Sunshine/releases/latest
