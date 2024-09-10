# Configuration
Sunshine will work with the default settings for most users. In some cases you may want to configure Sunshine further.

The default location for the configuration file is listed below. You can use another location if you
choose, by passing in the full configuration file path as the first argument when you start Sunshine.

**Example**
```bash
sunshine ~/sunshine_config.conf
```

The default location of the `apps.json` is the same as the configuration file. You can use a custom
location by modifying the configuration file.

**Default Config Directory**

| OS      | Location                                        |
|---------|-------------------------------------------------|
| Docker  | @code{}/config@endcode                          |
| Linux   | @code{}~/.config/sunshine@endcode               |
| macOS   | @code{}~/.config/sunshine@endcode               |
| Windows | @code{}%ProgramFiles%\\Sunshine\\config@endcode |

Although it is recommended to use the configuration UI, it is possible manually configure Sunshine by
editing the `conf` file in a text editor. Use the examples as reference.

## [General](https://localhost:47990/config/#general)

### [locale](https://localhost:47990/config/#locale)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The locale used for Sunshine's user interface.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            en
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            locale = en
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="13">Choices</td>
        <td>de</td>
        <td>German</td>
    </tr>
    <tr>
        <td>en</td>
        <td>English</td>
    </tr>
    <tr>
        <td>en_GB</td>
        <td>English (UK)</td>
    </tr>
    <tr>
        <td>en_US</td>
        <td>English (United States)</td>
    </tr>
    <tr>
        <td>es</td>
        <td>Spanish</td>
    </tr>
    <tr>
        <td>fr</td>
        <td>French</td>
    </tr>
    <tr>
        <td>it</td>
        <td>Italian</td>
    </tr>
    <tr>
        <td>ja</td>
        <td>Japanese</td>
    </tr>
    <tr>
        <td>pt</td>
        <td>Portuguese</td>
    </tr>
    <tr>
        <td>ru</td>
        <td>Russian</td>
    </tr>
    <tr>
        <td>sv</td>
        <td>Swedish</td>
    </tr>
    <tr>
        <td>tr</td>
        <td>Turkish</td>
    </tr>
    <tr>
        <td>zh</td>
        <td>Chinese (Simplified)</td>
    </tr>
</table>

### [sunshine_name](https://localhost:47990/config/#sunshine_name)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The name displayed by Moonlight.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">PC hostname</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            sunshine_name = Sunshine
            @endcode</td>
    </tr>
</table>

### [min_log_level](https://localhost:47990/config/#min_log_level)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The minimum log level printed to standard out.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            info
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            min_log_level = info
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="7">Choices</td>
        <td>verbose</td>
        <td>All logging message.
            @attention{This may negatively affect streaming performance.}</td>
    </tr>
    <tr>
        <td>debug</td>
        <td>Debug log messages and higher.
            @attention{This may negatively affect streaming performance.}</td>
    </tr>
    <tr>
        <td>info</td>
        <td>Informational log messages and higher.</td>
    </tr>
    <tr>
        <td>warning</td>
        <td>Warning log messages and higher.</td>
    </tr>
    <tr>
        <td>error</td>
        <td>Error log messages and higher.</td>
    </tr>
    <tr>
        <td>fatal</td>
        <td>Only fatal log messages.</td>
    </tr>
    <tr>
        <td>none</td>
        <td>No log messages.</td>
    </tr>
</table>

### [channels](https://localhost:47990/config/#channels)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sunshine can support multiple clients streaming simultaneously,
            at the cost of higher CPU and GPU usage.
            @note{All connected clients share control of the same streaming session.}
            @warning{Some hardware encoders may have limitations that reduce performance with multiple streams.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            1
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            channels = 1
            @endcode</td>
    </tr>
</table>

### [global_prep_cmd](https://localhost:47990/config/#global_prep_cmd)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            A list of commands to be run before/after all applications.
            If any of the prep-commands fail, starting the application is aborted.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            []
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            global_prep_cmd = [{"do":"nircmd.exe setdisplay 1280 720 32 144","undo":"nircmd.exe setdisplay 2560 1440 32 144"}]
            @endcode</td>
    </tr>
</table>

### [notify_pre_releases](https://localhost:47990/config/#notify_pre_releases)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Whether to be notified of new pre-release versions of Sunshine.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            notify_pre_releases = disabled
            @endcode</td>
    </tr>
</table>

## [Input](https://localhost:47990/config/#input)

### [controller](https://localhost:47990/config/#controller)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Whether to allow controller input from the client.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            controller = enabled
            @endcode</td>
    </tr>
</table>

### [gamepad](https://localhost:47990/config/#gamepad)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The type of gamepad to emulate on the host.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            auto
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            gamepad = auto
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="6">Choices</td>
        <td>ds4</td>
        <td>DualShock 4 controller (PS4)
            @note{This option applies to Windows only.}</td>
    </tr>
    <tr>
        <td>ds5</td>
        <td>DualShock 5 controller (PS5)
            @note{This option applies to Linux only.}</td>
    </tr>
    <tr>
        <td>switch</td>
        <td>Switch Pro controller
            @note{This option applies to Linux only.}</td>
    </tr>
    <tr>
        <td>x360</td>
        <td>Xbox 360 controller
            @note{This option applies to Windows only.}</td>
    </tr>
    <tr>
        <td>xone</td>
        <td>Xbox One controller
            @note{This option applies to Linux only.}</td>
    </tr>
</table>

### [ds4_back_as_touchpad_click](https://localhost:47990/config/#ds4_back_as_touchpad_click)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Allow Select/Back inputs to also trigger DS4 touchpad click. Useful for clients looking to
            emulate touchpad click on Xinput devices.
            @hint{Only applies when gamepad is set to ds4 manually. Unused in other gamepad modes.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            ds4_back_as_touchpad_click = enabled
            @endcode</td>
    </tr>
</table>

### [motion_as_ds4](https://localhost:47990/config/#motion_as_ds4)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            If a client reports that a connected gamepad has motion sensor support, emulate it on the
            host as a DS4 controller.
            <br>
            <br>
            When disabled, motion sensors will not be taken into account during gamepad type selection.
            @hint{Only applies when gamepad is set to auto.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            motion_as_ds4 = enabled
            @endcode</td>
    </tr>
</table>

### [touchpad_as_ds4](https://localhost:47990/config/#touchpad_as_ds4)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            If a client reports that a connected gamepad has a touchpad, emulate it on the host
            as a DS4 controller.
            <br>
            <br>
            When disabled, touchpad presence will not be taken into account during gamepad type selection.
            @hint{Only applies when gamepad is set to auto.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            touchpad_as_ds4 = enabled
            @endcode</td>
    </tr>
</table>

### [back_button_timeout](https://localhost:47990/config/#back_button_timeout)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            If the Back/Select button is held down for the specified number of milliseconds,
            a Home/Guide button press is emulated.
            @tip{If back_button_timeout < 0, then the Home/Guide button will not be emulated.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            -1
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            back_button_timeout = 2000
            @endcode</td>
    </tr>
</table>

### [keyboard](https://localhost:47990/config/#keyboard)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Whether to allow keyboard input from the client.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            keyboard = enabled
            @endcode</td>
    </tr>
</table>

### [key_repeat_delay](https://localhost:47990/config/#key_repeat_delay)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The initial delay, in milliseconds, before repeating keys. Controls how fast keys will
            repeat themselves.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            500
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            key_repeat_delay = 500
            @endcode</td>
    </tr>
</table>

### [key_repeat_frequency](https://localhost:47990/config/#key_repeat_frequency)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            How often keys repeat every second.
            @tip{This configurable option supports decimals.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            24.9
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            key_repeat_frequency = 24.9
            @endcode</td>
    </tr>
</table>

### [always_send_scancodes](https://localhost:47990/config/#always_send_scancodes)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sending scancodes enhances compatibility with games and apps but may result in incorrect keyboard input
            from certain clients that aren't using a US English keyboard layout.
            <br>
            <br>
            Enable if keyboard input is not working at all in certain applications.
            <br>
            <br>
            Disable if keys on the client are generating the wrong input on the host.
            @caution{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            always_send_scancodes = enabled
            @endcode</td>
    </tr>
</table>

### [key_rightalt_to_key_win](https://localhost:47990/config/#key_rightalt_to_key_win)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">It may be possible that you cannot send the Windows Key from Moonlight directly. In those cases it may be useful to
            make Sunshine think the Right Alt key is the Windows key.
            </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            key_rightalt_to_key_win = enabled
            @endcode</td>
    </tr>
</table>

### [mouse](https://localhost:47990/config/#mouse)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Whether to allow mouse input from the client.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            mouse = enabled
            @endcode</td>
    </tr>
</table>

### [high_resolution_scrolling](https://localhost:47990/config/#high_resolution_scrolling)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            When enabled, Sunshine will pass through high resolution scroll events from Moonlight clients.
            <br>
            This can be useful to disable for older applications that scroll too fast with high resolution scroll
            events.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            high_resolution_scrolling = enabled
            @endcode</td>
    </tr>
</table>

### [native_pen_touch](https://localhost:47990/config/#native_pen_touch)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            When enabled, Sunshine will pass through native pen/touch events from Moonlight clients.
            <br>
            This can be useful to disable for older applications without native pen/touch support.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            native_pen_touch = enabled
            @endcode</td>
    </tr>
</table>

### [native_pen_touch](https://localhost:47990/config/#native_pen_touch)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sometimes it may be useful to map keybindings. Wayland won't allow clients to capture the Win Key
            for example.
            @tip{See [virtual key codes](https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)}
            @hint{keybindings needs to have a multiple of two elements.}
            @note{This option is not available in the UI. A PR would be welcome.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            [
              0x10, 0xA0,
              0x11, 0xA2,
              0x12, 0xA4
            ]
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            keybindings = [
              0x10, 0xA0,
              0x11, 0xA2,
              0x12, 0xA4,
              0x4A, 0x4B
            ]
            @endcode</td>
    </tr>
</table>

## [Audio/Video](https://localhost:47990/config/#audio-video)

### [audio_sink](https://localhost:47990/config/#audio_sink)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The name of the audio sink used for audio loopback.
            @tip{To find the name of the audio sink follow these instructions.
            <br>
            <br>
            **Linux + pulseaudio:**
            <br>
            @code{}
            pacmd list-sinks | grep "name:"
            @endcode
            <br>
            <br>
            **Linux + pipewire:**
            <br>
            @code{}
            pactl info | grep Source
            # in some causes you'd need to use the `Sink` device, if `Source` doesn't work, so try:
            pactl info | grep Sink
            @endcode
            <br>
            <br>
            **macOS:**
            <br>
            Sunshine can only access microphones on macOS due to system limitations.
            To stream system audio use
            [Soundflower](https://github.com/mattingalls/Soundflower) or
            [BlackHole](https://github.com/ExistentialAudio/BlackHole).
            <br>
            <br>
            **Windows:**
            <br>
            Enter the following command in command prompt or PowerShell.
            @code{}
            %ProgramFiles%\Sunshine\tools\audio-info.exe
            @endcode
            If you have multiple audio devices with identical names, use the Device ID instead.
            }
            @attention{If you want to mute the host speakers, use
            [virtual_sink](#virtual_sinkhttpslocalhost47990configvirtual_sink) instead.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Sunshine will select the default audio device.</td>
    </tr>
    <tr>
        <td>Example (Linux)</td>
        <td colspan="2">@code{}
            audio_sink = alsa_output.pci-0000_09_00.3.analog-stereo
            @endcode</td>
    </tr>
    <tr>
        <td>Example (macOS)</td>
        <td colspan="2">@code{}
            audio_sink = BlackHole 2ch
            @endcode</td>
    </tr>
    <tr>
        <td>Example (Windows)</td>
        <td colspan="2">@code{}
            audio_sink = Speakers (High Definition Audio Device)
            @endcode</td>
    </tr>
</table>

### [virtual_sink](https://localhost:47990/config/#virtual_sink)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The audio device that's virtual, like Steam Streaming Speakers. This allows Sunshine to stream audio,
            while muting the speakers.
            @tip{See [audio_sink](#audio_sinkhttpslocalhost47990configaudio_sink)!}
            @tip{These are some options for virtual sound devices.
            * Stream Streaming Speakers (Linux, macOS, Windows)
              * Steam must be installed.
              * Enable [install_steam_audio_drivers](#install_steam_audio_drivershttpslocalhost47990configinstall_steam_audio_drivers)
                or use Steam Remote Play at least once to install the drivers.
            * [Virtual Audio Cable](https://vb-audio.com/Cable) (macOS, Windows)
            }
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">n/a</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            virtual_sink = Steam Streaming Speakers
            @endcode</td>
    </tr>
</table>

### [install_steam_audio_drivers](https://localhost:47990/config/#install_steam_audio_drivers)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Installs the Steam Streaming Speakers driver (if Steam is installed) to support surround sound and muting
            host audio.
            @note{This option is only supported on Windows.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            install_steam_audio_drivers = enabled
            @endcode</td>
    </tr>
</table>

### [adapter_name](https://localhost:47990/config/#adapter_name)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Select the video card you want to stream.
            @tip{To find the appropriate values follow these instructions.
            <br>
            <br>
            **Linux + VA-API:**
            <br>
            Unlike with *amdvce* and *nvenc*, it doesn't matter if video encoding is done on a different GPU.
            @code{}
            ls /dev/dri/renderD*  # to find all devices capable of VAAPI
            # replace ``renderD129`` with the device from above to list the name and capabilities of the device
            vainfo --display drm --device /dev/dri/renderD129 | \
              grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            @endcode
            To be supported by Sunshine, it needs to have at the very minimum:
            `VAProfileH264High   : VAEntrypointEncSlice`
            <br>
            <br>
            **Windows:**
            <br>
            Enter the following command in command prompt or PowerShell.
            @code{}
            %ProgramFiles%\Sunshine\tools\dxgi-info.exe
            @endcode
            For hybrid graphics systems, DXGI reports the outputs are connected to whichever graphics
            adapter that the application is configured to use, so it's not a reliable indicator of how the
            display is physically connected.
            }
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Sunshine will select the default video card.</td>
    </tr>
    <tr>
        <td>Example (Linux)</td>
        <td colspan="2">@code{}
            adapter_name = /dev/dri/renderD128
            @endcode</td>
    </tr>
    <tr>
        <td>Example (Windows)</td>
        <td colspan="2">@code{}
            adapter_name = Radeon RX 580 Series
            @endcode</td>
    </tr>
</table>

### [output_name](https://localhost:47990/config/#output_name)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Select the display number you want to stream.
            @tip{To find the appropriate values follow these instructions.
            <br>
            <br>
            **Linux:**
            <br>
            During Sunshine startup, you should see the list of detected displays:
            @code{}
            Info: Detecting displays
            Info: Detected display: DVI-D-0 (id: 0) connected: false
            Info: Detected display: HDMI-0 (id: 1) connected: true
            Info: Detected display: DP-0 (id: 2) connected: true
            Info: Detected display: DP-1 (id: 3) connected: false
            Info: Detected display: DVI-D-1 (id: 4) connected: false
            @endcode
            You need to use the id value inside the parenthesis, e.g. `1`.
            <br>
            <br>
            **macOS:**
            <br>
            During Sunshine startup, you should see the list of detected displays:
            @code{}
            Info: Detecting displays
            Info: Detected display: Monitor-0 (id: 3) connected: true
            Info: Detected display: Monitor-1 (id: 2) connected: true
            @endcode
            You need to use the id value inside the parenthesis, e.g. `3`.
            <br>
            <br>
            **Windows:**
            <br>
            Enter the following command in command prompt or PowerShell.
            @code{}
            %ProgramFiles%\Sunshine\tools\dxgi-info.exe
            @endcode
            }
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Sunshine will select the default display.</td>
    </tr>
    <tr>
        <td>Example (Linux)</td>
        <td colspan="2">@code{}
            output_name = 0
            @endcode</td>
    </tr>
    <tr>
        <td>Example (macOS)</td>
        <td colspan="2">@code{}
            output_name = 3
            @endcode</td>
    </tr>
    <tr>
        <td>Example (Windows)</td>
        <td colspan="2">@code{}
            output_name  = \\.\DISPLAY1
            @endcode</td>
    </tr>
</table>

### [min_fps_factor](https://localhost:47990/config/#min_fps_factor)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sunshine will use this factor to calculate the minimum time between frames. Increasing this value may help
            when streaming mostly static content.
            @warning{Higher values will consume more bandwidth.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            1
            @endcode</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colspan="2">1-3</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            min_fps_factor = 1
            @endcode</td>
    </tr>
</table>

## [Network](https://localhost:47990/config/#network)

### [upnp](https://localhost:47990/config/#upnp)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sunshine will attempt to open ports for streaming over the internet.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            upnp = enabled
            @endcode</td>
    </tr>
</table>

### [address_family](https://localhost:47990/config/#address_family)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Set the address family that Sunshine will use.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            ipv4
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            address_family = both
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="2">Choices</td>
        <td>ipv4</td>
        <td>IPv4 only</td>
    </tr>
    <tr>
        <td>both</td>
        <td>IPv4+IPv6</td>
    </tr>
</table>

### [port](https://localhost:47990/config/#port)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Set the family of ports used by Sunshine.
            Changing this value will offset other ports as shown in config UI.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            47989
            @endcode</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colspan="2">1029-65514</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            port = 47989
            @endcode</td>
    </tr>
</table>

### [origin_web_ui_allowed](https://localhost:47990/config/#origin_web_ui_allowed)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The origin of the remote endpoint address that is not denied for HTTPS Web UI.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            lan
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            origin_web_ui_allowed = lan
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>pc</td>
        <td>Only localhost may access the web ui</td>
    </tr>
    <tr>
        <td>lan</td>
        <td>Only LAN devices may access the web ui</td>
    </tr>
    <tr>
        <td>wan</td>
        <td>Anyone may access the web ui</td>
    </tr>
</table>

### [external_ip](https://localhost:47990/config/#external_ip)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            If no external IP address is given, Sunshine will attempt to automatically detect external ip-address.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Automatic</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            external_ip = 123.456.789.12
            @endcode</td>
    </tr>
</table>

### [lan_encryption_mode](https://localhost:47990/config/#lan_encryption_mode)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            This determines when encryption will be used when streaming over your local network.
            @warning{Encryption can reduce streaming performance, particularly on less powerful hosts and clients.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            0
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            lan_encryption_mode = 0
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>0</td>
        <td>encryption will not be used</td>
    </tr>
    <tr>
        <td>1</td>
        <td>encryption will be used if the client supports it</td>
    </tr>
    <tr>
        <td>2</td>
        <td>encryption is mandatory and unencrypted connections are rejected</td>
    </tr>
</table>

### [wan_encryption_mode](https://localhost:47990/config/#wan_encryption_mode)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            This determines when encryption will be used when streaming over the Internet.
            @warning{Encryption can reduce streaming performance, particularly on less powerful hosts and clients.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            1
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            wan_encryption_mode = 1
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>0</td>
        <td>encryption will not be used</td>
    </tr>
    <tr>
        <td>1</td>
        <td>encryption will be used if the client supports it</td>
    </tr>
    <tr>
        <td>2</td>
        <td>encryption is mandatory and unencrypted connections are rejected</td>
    </tr>
</table>

### [ping_timeout](https://localhost:47990/config/#ping_timeout)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            How long to wait, in milliseconds, for data from Moonlight before shutting down the stream.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            10000
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            ping_timeout = 10000
            @endcode</td>
    </tr>
</table>

## [Config Files](https://localhost:47990/config/#files)

### [file_apps](https://localhost:47990/config/#file_apps)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The application configuration file path. The file contains a JSON formatted list of applications that
            can be started by Moonlight.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            apps.json
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            file_apps = apps.json
            @endcode</td>
    </tr>
</table>

### [credentials_file](https://localhost:47990/config/#credentials_file)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The file where user credentials for the UI are stored.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            sunshine_state.json
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            credentials_file = sunshine_state.json
            @endcode</td>
    </tr>
</table>

### [log_path](https://localhost:47990/config/#log_path)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The path where the Sunshine log is stored.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            sunshine.log
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            log_path = sunshine.log
            @endcode</td>
    </tr>
</table>

### [pkey](https://localhost:47990/config/#pkey)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The private key used for the web UI and Moonlight client pairing. For best compatibility,
            this should be an RSA-2048 private key.
            @warning{Not all Moonlight clients support ECDSA keys or RSA key lengths other than 2048 bits.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            credentials/cakey.pem
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            pkey = /dir/pkey.pem
            @endcode</td>
    </tr>
</table>

### [cert](https://localhost:47990/config/#cert)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The certificate used for the web UI and Moonlight client pairing. For best compatibility,
            this should have an RSA-2048 public key.
            @warning{Not all Moonlight clients support ECDSA keys or RSA key lengths other than 2048 bits.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            credentials/cacert.pem
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            cert = /dir/cert.pem
            @endcode</td>
    </tr>
</table>

### [file_state](https://localhost:47990/config/#file_state)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The file where current state of Sunshine is stored.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            sunshine_state.json
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            file_state = sunshine_state.json
            @endcode</td>
    </tr>
</table>

## [Advanced](https://localhost:47990/config/#advanced)

### [fec_percentage](https://localhost:47990/config/#fec_percentage)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Percentage of error correcting packets per data packet in each video frame.
            @warning{Higher values can correct for more network packet loss,
            but at the cost of increasing bandwidth usage.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            20
            @endcode</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colspan="2">1-255</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            fec_percentage = 20
            @endcode</td>
    </tr>
</table>

### [qp](https://localhost:47990/config/#qp)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Quantization Parameter. Some devices don't support Constant Bit Rate. For those devices, QP is used instead.
            @warning{Higher value means more compression, but less quality.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            28
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            qp = 28
            @endcode</td>
    </tr>
</table>

### [min_threads](https://localhost:47990/config/#min_threads)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Minimum number of CPU threads used for encoding.
            @note{Increasing the value slightly reduces encoding efficiency, but the tradeoff is usually worth it to
            gain the use of more CPU cores for encoding. The ideal value is the lowest value that can reliably encode
            at your desired streaming settings on your hardware.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            2
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            min_threads = 2
            @endcode</td>
    </tr>
</table>

### [hevc_mode](https://localhost:47990/config/#hevc_mode)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Allows the client to request HEVC Main or HEVC Main10 video streams.
            @warning{HEVC is more CPU-intensive to encode, so enabling this may reduce performance when using software
            encoding.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            0
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            hevc_mode = 2
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="4">Choices</td>
        <td>0</td>
        <td>advertise support for HEVC based on encoder capabilities (recommended)</td>
    </tr>
    <tr>
        <td>1</td>
        <td>do not advertise support for HEVC</td>
    </tr>
    <tr>
        <td>2</td>
        <td>advertise support for HEVC Main profile</td>
    </tr>
    <tr>
        <td>3</td>
        <td>advertise support for HEVC Main and Main10 (HDR) profiles</td>
    </tr>
</table>

### [av1_mode](https://localhost:47990/config/#av1_mode)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Allows the client to request AV1 Main 8-bit or 10-bit video streams.
            @warning{AV1 is more CPU-intensive to encode, so enabling this may reduce performance when using software
            encoding.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            0
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            av1_mode = 2
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="4">Choices</td>
        <td>0</td>
        <td>advertise support for AV1 based on encoder capabilities (recommended)</td>
    </tr>
    <tr>
        <td>1</td>
        <td>do not advertise support for AV1</td>
    </tr>
    <tr>
        <td>2</td>
        <td>advertise support for AV1 Main 8-bit profile</td>
    </tr>
    <tr>
        <td>3</td>
        <td>advertise support for AV1 Main 8-bit and 10-bit (HDR) profiles</td>
    </tr>
</table>

### [capture](https://localhost:47990/config/#capture)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Force specific screen capture method.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Automatic.
            Sunshine will use the first capture method available in the order of the table above.</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            capture = kms
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="6">Choices</td>
        <td>nvfbc</td>
        <td>Use NVIDIA Frame Buffer Capture to capture direct to GPU memory. This is usually the fastest method for
            NVIDIA cards. NvFBC does not have native Wayland support and does not work with XWayland.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>wlr</td>
        <td>Capture for wlroots based Wayland compositors via DMA-BUF.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>kms</td>
        <td>DRM/KMS screen capture from the kernel. This requires that Sunshine has `cap_sys_admin` capability.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>x11</td>
        <td>Uses XCB. This is the slowest and most CPU intensive so should be avoided if possible.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>ddx</td>
        <td>Use DirectX Desktop Duplication API to capture the display. This is well-supported on Windows machines.
            @note{Applies to Windows only.}</td>
    </tr>
    <tr>
        <td>wgc</td>
        <td>(beta feature) Use Windows.Graphics.Capture to capture the display.
            @note{Applies to Windows only.}
            @attention{This capture method is not compatible with the Sunshine service.}</td>
    </tr>
</table>

### [encoder](https://localhost:47990/config/#encoder)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Force a specific encoder.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Sunshine will use the first encoder that is available.</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            encoder = nvenc
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="5">Choices</td>
        <td>nvenc</td>
        <td>For NVIDIA graphics cards</td>
    </tr>
    <tr>
        <td>quicksync</td>
        <td>For Intel graphics cards</td>
    </tr>
    <tr>
        <td>amdvce</td>
        <td>For AMD graphics cards</td>
    </tr>
    <tr>
        <td>vaapi</td>
        <td>Use Linux VA-API (AMD, Intel)</td>
    </tr>
    <tr>
        <td>software</td>
        <td>Encoding occurs on the CPU</td>
    </tr>
</table>

## [NVIDIA NVENC Encoder](https://localhost:47990/config/#nvidia-nvenc-encoder)

### [nvenc_preset](https://localhost:47990/config/#nvenc_preset)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            NVENC encoder performance preset.
            Higher numbers improve compression (quality at given bitrate) at the cost of increased encoding latency.
            Recommended to change only when limited by network or decoder, otherwise similar effect can be accomplished
            by increasing bitrate.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            1
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_preset = 1
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="7">Choices</td>
        <td>1</td>
        <td>P1 (fastest)</td>
    </tr>
    <tr>
        <td>2</td>
        <td>P2</td>
    </tr>
    <tr>
        <td>3</td>
        <td>P3</td>
    </tr>
    <tr>
        <td>4</td>
        <td>P4</td>
    </tr>
    <tr>
        <td>5</td>
        <td>P5</td>
    </tr>
    <tr>
        <td>6</td>
        <td>P6</td>
    </tr>
    <tr>
        <td>7</td>
        <td>P7 (slowest)</td>
    </tr>
</table>

### [nvenc_twopass](https://localhost:47990/config/#nvenc_twopass)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Enable two-pass mode in NVENC encoder.
            This allows to detect more motion vectors, better distribute bitrate across the frame and more strictly
            adhere to bitrate limits. Disabling it is not recommended since this can lead to occasional bitrate
            overshoot and subsequent packet loss.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            quarter_res
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_twopass = quarter_res
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>disabled</td>
        <td>One pass (fastest)</td>
    </tr>
    <tr>
        <td>quarter_res</td>
        <td>Two passes, first pass at quarter resolution (faster)</td>
    </tr>
    <tr>
        <td>full_res</td>
        <td>Two passes, first pass at full resolution (slower)</td>
    </tr>
</table>

### [nvenc_spatial_aq](https://localhost:47990/config/#nvenc_spatial_aq)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Assign higher QP values to flat regions of the video.
            Recommended to enable when streaming at lower bitrates.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
            @warning{Enabling this option may reduce performance.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_spatial_aq = disabled
            @endcode</td>
    </tr>
</table>

### [nvenc_vbv_increase](https://localhost:47990/config/#nvenc_vbv_increase)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Single-frame VBV/HRD percentage increase.
            By default Sunshine uses single-frame VBV/HRD, which means any encoded video frame size is not expected to
            exceed requested bitrate divided by requested frame rate. Relaxing this restriction can be beneficial and
            act as low-latency variable bitrate, but may also lead to packet loss if the network doesn't have buffer
            headroom to handle bitrate spikes. Maximum accepted value is 400, which corresponds to 5x increased
            encoded video frame upper size limit.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
            @warning{Can lead to network packet loss.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            0
            @endcode</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colspan="2">0-400</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_vbv_increase = 0
            @endcode</td>
    </tr>
</table>

### [nvenc_realtime_hags](https://localhost:47990/config/#nvenc_realtime_hags)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Use realtime gpu scheduling priority in NVENC when hardware accelerated gpu scheduling (HAGS) is enabled
            in Windows. Currently, NVIDIA drivers may freeze in encoder when HAGS is enabled, realtime priority is used
            and VRAM utilization is close to maximum. Disabling this option lowers the priority to high, sidestepping
            the freeze at the cost of reduced capture performance when the GPU is heavily loaded.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_realtime_hags = enabled
            @endcode</td>
    </tr>
</table>

### [nvenc_split_encode](https://localhost:47990/config/#nvenc_split_encode)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Split the encoding of each video frame over multiple NVENC hardware units.
            Significantly reduces encoding latency with a marginal compression efficiency penalty.
            This option is ignored if your GPU has a singular NVENC unit.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            driver_decides
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_split_encode = driver_decides
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>disabled</td>
        <td>Disabled</td>
    </tr>
    <tr>
        <td>driver_decides</td>
        <td>NVIDIA driver makes the decision whether to enable it</td>
    </tr>
    <tr>
        <td>enabled</td>
        <td>Enabled</td>
    </tr>
</table>

### [nvenc_latency_over_power](https://localhost:47990/config/#nvenc_latency_over_power)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Adaptive P-State algorithm which NVIDIA drivers employ doesn't work well with low latency streaming,
            so Sunshine requests high power mode explicitly.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
            @warning{Disabling this is not recommended since this can lead to significantly increased encoding latency.}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_latency_over_power = enabled
            @endcode</td>
    </tr>
</table>

### [nvenc_opengl_vulkan_on_dxgi](https://localhost:47990/config/#nvenc_opengl_vulkan_on_dxgi)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sunshine can't capture fullscreen OpenGL and Vulkan programs at full frame rate unless they present on
            top of DXGI. With this option enabled Sunshine changes global Vulkan/OpenGL present method to
            "Prefer layered on DXGI Swapchain". This is system-wide setting that is reverted on Sunshine program exit.
            @note{This option only applies when using NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_opengl_vulkan_on_dxgi = enabled
            @endcode</td>
    </tr>
</table>

### [nvenc_h264_cavlc](https://localhost:47990/config/#nvenc_h264_cavlc)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Prefer CAVLC entropy coding over CABAC in H.264 when using NVENC.
            CAVLC is outdated and needs around 10% more bitrate for same quality, but provides slightly faster
            decoding when using software decoder.
            @note{This option only applies when using H.264 format with the
            NVENC [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            nvenc_h264_cavlc = disabled
            @endcode</td>
    </tr>
</table>

## [Intel QuickSync Encoder](https://localhost:47990/config/#intel-quicksync-encoder)

### [qsv_preset](https://localhost:47990/config/#qsv_preset)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder preset to use.
            @note{This option only applies when using quicksync [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            medium
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            qsv_preset = medium
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="7">Choices</td>
        <td>veryfast</td>
        <td>fastest (lowest quality)</td>
    </tr>
    <tr>
        <td>faster</td>
        <td>faster (lower quality)</td>
    </tr>
    <tr>
        <td>fast</td>
        <td>fast (low quality)</td>
    </tr>
    <tr>
        <td>medium</td>
        <td>medium (default)</td>
    </tr>
    <tr>
        <td>slow</td>
        <td>slow (good quality)</td>
    </tr>
    <tr>
        <td>slower</td>
        <td>slower (better quality)</td>
    </tr>
    <tr>
        <td>veryslow</td>
        <td>slowest (best quality)</td>
    </tr>
</table>

### [qsv_coder](https://localhost:47990/config/#qsv_coder)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The entropy encoding to use.
            @note{This option only applies when using H.264 with the quicksync
            [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            auto
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            qsv_coder = auto
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>auto</td>
        <td>let ffmpeg decide</td>
    </tr>
    <tr>
        <td>cabac</td>
        <td>context adaptive binary arithmetic coding - higher quality</td>
    </tr>
    <tr>
        <td>cavlc</td>
        <td>context adaptive variable-length coding - faster decode</td>
    </tr>
</table>

### [qsv_slow_hevc](https://localhost:47990/config/#qsv_slow_hevc)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            This options enables use of HEVC on older Intel GPUs that only support low power encoding for H.264.
            @note{This option only applies when using quicksync [encoder](#encoderhttpslocalhost47990configencoder).}
            @caution{Streaming performance may be significantly reduced when this option is enabled.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            qsv_slow_hevc = disabled
            @endcode</td>
    </tr>
</table>

## [AMD AMF Encoder](https://localhost:47990/config/#amd-amf-encoder)

### [amd_usage](https://localhost:47990/config/#amd_usage)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder usage profile is used to set the base set of encoding parameters.
            @note{This option only applies when using amdvce [encoder](#encoderhttpslocalhost47990configencoder).}
            @note{The other AMF options that follow will override a subset of the settings applied by your usage
            profile, but there are hidden parameters set in usage profiles that cannot be overridden elsewhere.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            ultralowlatency
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_usage = ultralowlatency
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="5">Choices</td>
        <td>transcoding</td>
        <td>transcoding (slowest)</td>
    </tr>
    <tr>
        <td>webcam</td>
        <td>webcam (slow)</td>
    </tr>
    <tr>
        <td>lowlatency_high_quality</td>
        <td>low latency, high quality (fast)</td>
    </tr>
    <tr>
        <td>lowlatency</td>
        <td>low latency (faster)</td>
    </tr>
    <tr>
        <td>ultralowlatency</td>
        <td>ultra low latency (fastest)</td>
    </tr>
</table>

### [amd_rc](https://localhost:47990/config/#amd_rc)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder rate control.
            @note{This option only applies when using amdvce [encoder](#encoderhttpslocalhost47990configencoder).}
            @warning{The `vbr_latency` option generally works best, but some bitrate overshoots may still occur.
            Enabling HRD allows all bitrate based rate controls to better constrain peak bitrate, but may result in
            encoding artifacts depending on your card.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            vbr_latency
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_rc = vbr_latency
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="4">Choices</td>
        <td>cqp</td>
        <td>constant qp mode</td>
    </tr>
    <tr>
        <td>cbr</td>
        <td>constant bitrate</td>
    </tr>
    <tr>
        <td>vbr_latency</td>
        <td>variable bitrate, latency constrained</td>
    </tr>
    <tr>
        <td>vbr_peak</td>
        <td>variable bitrate, peak constrained</td>
    </tr>
</table>

### [amd_enforce_hrd](https://localhost:47990/config/#amd_enforce_hrd)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Enable Hypothetical Reference Decoder (HRD) enforcement to help constrain the target bitrate.
            @note{This option only applies when using amdvce [encoder](#encoderhttpslocalhost47990configencoder).}
            @warning{HRD is known to cause encoding artifacts or negatively affect encoding quality on certain cards.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_enforce_hrd = disabled
            @endcode</td>
    </tr>
</table>

### [amd_quality](https://localhost:47990/config/#amd_quality)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The quality profile controls the tradeoff between speed and quality of encoding.
            @note{This option only applies when using amdvce [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            balanced
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_quality = balanced
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>speed</td>
        <td>prefer speed</td>
    </tr>
    <tr>
        <td>balanced</td>
        <td>balanced</td>
    </tr>
    <tr>
        <td>quality</td>
        <td>prefer quality</td>
    </tr>
</table>

### [amd_preanalysis](https://localhost:47990/config/#amd_preanalysis)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Preanalysis can increase encoding quality at the cost of latency.
            @note{This option only applies when using amdvce [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            disabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_preanalysis = disabled
            @endcode</td>
    </tr>
</table>

### [amd_vbaq](https://localhost:47990/config/#amd_vbaq)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Variance Based Adaptive Quantization (VBAQ) can increase subjective visual quality by prioritizing
            allocation of more bits to smooth areas compared to more textured areas.
            @note{This option only applies when using amdvce [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_vbaq = enabled
            @endcode</td>
    </tr>
</table>

### [amd_coder](https://localhost:47990/config/#amd_coder)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The entropy encoding to use.
            @note{This option only applies when using H.264 with the amdvce
            [encoder](#encoderhttpslocalhost47990configencoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            auto
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_coder = auto
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>auto</td>
        <td>let ffmpeg decide</td>
    </tr>
    <tr>
        <td>cabac</td>
        <td>context adaptive binary arithmetic coding - faster decode</td>
    </tr>
    <tr>
        <td>cavlc</td>
        <td>context adaptive variable-length coding - higher quality</td>
    </tr>
</table>

## [VideoToolbox Encoder](https://localhost:47990/config/#videotoolbox-encoder)

### [vt_coder](https://localhost:47990/config/#vt_coder)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The entropy encoding to use.
            @note{This option only applies when using macOS.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            auto
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            vt_coder = auto
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>auto</td>
        <td>let ffmpeg decide</td>
    </tr>
    <tr>
        <td>cabac</td>
        <td>context adaptive binary arithmetic coding - faster decode</td>
    </tr>
    <tr>
        <td>cavlc</td>
        <td>context adaptive variable-length coding - higher quality</td>
    </tr>
</table>

### [vt_software](https://localhost:47990/config/#vt_software)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Force Video Toolbox to use software encoding.
            @note{This option only applies when using macOS.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            auto
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            vt_software = auto
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="4">Choices</td>
        <td>auto</td>
        <td>let ffmpeg decide</td>
    </tr>
    <tr>
        <td>disabled</td>
        <td>disable software encoding</td>
    </tr>
    <tr>
        <td>allowed</td>
        <td>allow software encoding</td>
    </tr>
    <tr>
        <td>forced</td>
        <td>force software encoding</td>
    </tr>
</table>

### [vt_realtime](https://localhost:47990/config/#vt_realtime)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Realtime encoding.
            @note{This option only applies when using macOS.}
            @warning{Disabling realtime encoding might result in a delayed frame encoding or frame drop.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            enabled
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            vt_realtime = enabled
            @endcode</td>
    </tr>
</table>

## [Software Encoder](https://localhost:47990/config/#software-encoder)

### [sw_preset](https://localhost:47990/config/#sw_preset)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder preset to use.
            @note{This option only applies when using software [encoder](#encoderhttpslocalhost47990configencoder).}
            @note{From [FFmpeg](https://trac.ffmpeg.org/wiki/Encode/H.264#preset).
            <br>
            <br>
            A preset is a collection of options that will provide a certain encoding speed to compression ratio. A slower
            preset will provide better compression (compression is quality per filesize). This means that, for example, if
            you target a certain file size or constant bit rate, you will achieve better quality with a slower preset.
            Similarly, for constant quality encoding, you will simply save bitrate by choosing a slower preset.
            <br>
            <br>
            Use the slowest preset that you have patience for.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            superfast
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            sw_preset = superfast
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="9">Choices</td>
        <td>ultrafast</td>
        <td>fastest</td>
    </tr>
    <tr>
        <td>superfast</td>
        <td></td>
    </tr>
    <tr>
        <td>veryfast</td>
        <td></td>
    </tr>
    <tr>
        <td>faster</td>
        <td></td>
    </tr>
    <tr>
        <td>fast</td>
        <td></td>
    </tr>
    <tr>
        <td>medium</td>
        <td></td>
    </tr>
    <tr>
        <td>slow</td>
        <td></td>
    </tr>
    <tr>
        <td>slower</td>
        <td></td>
    </tr>
    <tr>
        <td>veryslow</td>
        <td>slowest</td>
    </tr>
</table>

### [sw_tune](https://localhost:47990/config/#sw_tune)

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The tuning preset to use.
            @note{This option only applies when using software [encoder](#encoderhttpslocalhost47990configencoder).}
            @note{From [FFmpeg](https://trac.ffmpeg.org/wiki/Encode/H.264#preset).
            <br>
            <br>
            You can optionally use -tune to change settings based upon the specifics of your input.
            }
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            zerolatency
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            sw_tune = zerolatency
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="6">Choices</td>
        <td>film</td>
        <td>use for high quality movie content; lowers deblocking</td>
    </tr>
    <tr>
        <td>animation</td>
        <td>good for cartoons; uses higher deblocking and more reference frames</td>
    </tr>
    <tr>
        <td>grain</td>
        <td>preserves the grain structure in old, grainy film material</td>
    </tr>
    <tr>
        <td>stillimage</td>
        <td>good for slideshow-like content</td>
    </tr>
    <tr>
        <td>fastdecode</td>
        <td>allows faster decoding by disabling certain filters</td>
    </tr>
    <tr>
        <td>zerolatency</td>
        <td>good for fast encoding and low-latency streaming</td>
    </tr>
</table>

<div class="section_buttons">

| Previous          |                            Next |
|:------------------|--------------------------------:|
| [Legal](legal.md) | [App Examples](app_examples.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
