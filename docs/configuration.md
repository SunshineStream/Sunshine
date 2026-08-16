# Configuration

@admonition{ Host authority |:| @htmlonly
By providing the host authority (URI + port), you can easily open each configuration option in the config UI.
<br>
<script src="configuration.js"></script>
<strong>Host authority: </strong> <input type="text" id="host-authority" value="localhost:47990">
@endhtmlonly
}

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
| FreeBSD | @code{}~/.config/sunshine@endcode               |
| Linux   | @code{}~/.config/sunshine@endcode               |
| macOS   | @code{}~/.config/sunshine@endcode               |
| Windows | @code{}%ProgramFiles%\\Sunshine\\config@endcode |

Although it is recommended to use the configuration UI, it is possible manually configure Sunshine by
editing the `conf` file in a text editor. Use the examples as reference.

The web UI groups these settings into the sidebar categories documented below. Encoder categories are shown only when
supported on the current platform.

## Permissions on every platform

Open **Troubleshooting > Permissions** in the Web UI to see required and optional access for the current platform.
The Home page flags verifiable required access that is missing. Sunshine checks for access granted while it is running
and restarts once after all verifiable required access is available. On Unix, adding a user to a group takes effect only
after a new login session; Sunshine cannot detect the new group membership in the existing process.

On Linux and FreeBSD, virtual keyboard, mouse, and gamepad input need read and write access to `/dev/uinput` (Linux
also checks `/dev/input/uinput`). The Web UI shows setup steps if that access is missing. On Windows, Sunshine checks
whether its account can list and create files in the `config` directory beside the executable. Windows provides no
consent prompt for directory ACLs, so the Web UI shows setup steps for correcting access.

## General

### locale

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
        <td rowspan="20">Choices</td>
        <td>bg</td>
        <td>Bulgarian</td>
    </tr>
    <tr>
        <td>cs</td>
        <td>Czech</td>
    </tr>
    <tr>
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
        <td>ko</td>
        <td>Korean</td>
    </tr>
    <tr>
        <td>pl</td>
        <td>Polish</td>
    </tr>
    <tr>
        <td>pt</td>
        <td>Portuguese</td>
    </tr>
    <tr>
        <td>pt_BR</td>
        <td>Portuguese (Brazilian)</td>
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
        <td>uk</td>
        <td>Ukranian</td>
    </tr>
    <tr>
        <td>zh</td>
        <td>Chinese (Simplified)</td>
    </tr>
    <tr>
        <td>zh_TW</td>
        <td>Chinese (Traditional)</td>
    </tr>
</table>

### sunshine_name

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

### min_log_level

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

### global_prep_cmd

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
            global_prep_cmd = [{"do":"nircmd.exe setdisplay 1280 720 32 144","elevated":true,"undo":"nircmd.exe setdisplay 2560 1440 32 144"}]
            @endcode</td>
    </tr>
</table>

### notify_pre_releases

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

### system_tray

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Show icon in system tray and display desktop notifications
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
            system_tray = enabled
            @endcode</td>
    </tr>
</table>

## Input

### controller

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

### gamepad_driver

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Controls which virtual gamepad backend Sunshine may use on Windows and macOS. On Windows, the Web UI and
            startup notification request a choice while this option is not set. If Sunshine detects an active Virtual
            HID Broker license on Windows, it automatically sets this option to `all` when it is missing.
            @warning{ViGEmBus has limited gamepad features, supports only Xbox 360 and DualShock 4 emulation, and has
            reached end of life. Selecting `vigembus` also suppresses Virtual HID Broker startup notifications.}
            @note{`all` and `vigembus` apply only to Windows. On macOS, an unset value uses Virtual HID Broker.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            not set
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            gamepad_driver = all
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="4">Choices</td>
        <td>all</td>
        <td>Windows only: prefer Virtual HID Broker when it is available and licensed, with ViGEmBus as a limited fallback.</td>
    </tr>
    <tr>
        <td>virtualhid</td>
        <td>Use Virtual HID Broker for gamepads. An active paid license is required; Windows ViGEmBus fallback is disabled.</td>
    </tr>
    <tr>
        <td>vigembus</td>
        <td>Windows only: use ViGEmBus for gamepads and hide Virtual HID Broker status and licensing details.</td>
    </tr>
    <tr>
        <td>none</td>
        <td>Disable virtual gamepads on Windows and macOS without changing keyboard or mouse input. Suppress broker and driver choice notices.</td>
    </tr>
</table>

### gamepad

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The type of gamepad to emulate on the host. Automatic selection uses the controller type
            reported by the client. If the type is unknown, Sunshine can select a PlayStation-style
            controller from reported motion or touchpad support; otherwise it uses an Xbox-style controller.
            @note{When gamepad_driver is `vigembus` on Windows, only auto, x360, and ds4 are available.}
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
        <td rowspan="7">Choices</td>
        <td>generic</td>
        <td>Generic HID gamepad</td>
    </tr>
    <tr>
        <td>ds4</td>
        <td>DualShock 4 controller (PS4)</td>
    </tr>
    <tr>
        <td>ds5</td>
        <td>DualShock 5 controller (PS5)</td>
    </tr>
    <tr>
        <td>switch</td>
        <td>Switch Pro controller</td>
    </tr>
    <tr>
        <td>x360</td>
        <td>Xbox 360 controller</td>
    </tr>
    <tr>
        <td>xone</td>
        <td>Xbox One controller</td>
    </tr>
    <tr>
        <td>xseries</td>
        <td>Xbox Series controller</td>
    </tr>
</table>

### ds4_back_as_touchpad_click

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Allow Select/Back inputs to also trigger a PlayStation-style gamepad touchpad click. Useful
            for clients looking to emulate touchpad click on XInput devices.
            @hint{Applies to ds4, ds5, and automatically selected PlayStation-style gamepads.}
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

### motion_as_ds4

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            If a client reports that a connected gamepad has motion sensor support, emulate it on the
            host as a PlayStation-style controller.
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

### touchpad_as_ds4

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            If a client reports that a connected gamepad has a touchpad, emulate it on the host as a
            PlayStation-style controller.
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

### virtualhid_randomize_mac

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Randomize the MAC address for PlayStation-style virtual controllers created by libvirtualhid.
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
            virtualhid_randomize_mac = enabled
            @endcode</td>
    </tr>
</table>

### back_button_timeout

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

### keyboard

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

### key_repeat_delay

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

### key_repeat_frequency

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

### always_send_scancodes

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

### key_rightalt_to_key_win

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

### mouse

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

### high_resolution_scrolling

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

### native_pen_touch

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

### keybindings

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sometimes it may be useful to map keybindings. Wayland won't allow clients to capture the Win Key
            for example.
            @tip{See [virtual key codes](https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)}
            @hint{keybindings needs to have a multiple of two elements.}
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

## Audio/Video

### audio_sink

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The name of the audio sink used for audio loopback.
            @tip{To find the name of the audio sink follow these instructions.
            <br>
            <br>
            **FreeBSD/Linux + pulseaudio:**
            <br>
            @code{}
            pacmd list-sinks | grep "name:"
            @endcode
            <br>
            <br>
            **FreeBSD/Linux + pipewire:**
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
            [virtual_sink](#virtual_sink) instead.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Sunshine will select the default audio device.</td>
    </tr>
    <tr>
        <td>Example (FreeBSD/Linux)</td>
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

### virtual_sink

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The audio device that's virtual, like Steam Streaming Speakers. This allows Sunshine to stream audio,
            while muting the speakers.
            @tip{See [audio_sink](#audio_sink)!}
            @tip{These are some options for virtual sound devices.
            * Stream Streaming Speakers (Linux, macOS, Windows)
              * Steam must be installed.
              * Enable [install_steam_audio_drivers](#install_steam_audio_drivers)
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

### stream_audio

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Whether to stream audio or not. Disabling this can be useful for streaming headless displays as second monitors.
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
            stream_audio = disabled
            @endcode</td>
    </tr>
</table>

### install_steam_audio_drivers

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

### external_audio

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Enables capture-only operation when an external audio router manages application playback.
            An explicit [audio_sink](#audio_sink) is required; use an endpoint ID when names are ambiguous.
            Sunshine captures that endpoint directly without changing or restoring Windows default devices
            or endpoint formats.
            <br>
            <br>
            In this mode, [virtual_sink](#virtual_sink), automatic Steam audio driver installation,
            and Moonlight's host-playback toggle do not affect routing. Configure local playback and the audio
            sent to the selected endpoint in your external mixer. If the endpoint is unavailable, audio capture
            fails rather than falling back to another endpoint. Video streaming can continue without audio.
            @note{This option is only supported on Windows. Other platforms retain their existing behavior.}
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
            external_audio = enabled
            audio_sink = My Streaming Mix
            @endcode</td>
    </tr>
</table>

### adapter_name

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Select the video card you want to stream.
            @tip{To find the appropriate values follow these instructions.
            <br>
            <br>
            **FreeBSD/Linux + VA-API:**
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
        <td>Example (FreeBSD/Linux)</td>
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

### output_name

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Select the display number you want to stream.
            @tip{To find the appropriate values follow these instructions.
            <br>
            <br>
            **FreeBSD/Linux:**
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
            It is recommended to use the stable display connector name (text right before the parenthesis, e.g. DP-0) for this value.
            For wlgrab/x11grab and kmsgrab the numeric id value can also be used.
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
            During Sunshine startup, you should see the list of detected displays:
            @code{}
            Info: Currently available display devices:
            [
              {
                "device_id": "{64243705-4020-5895-b923-adc862c3457e}",
                "display_name": "",
                "friendly_name": "IDD HDR",
                "info": null
              },
              {
                "device_id": "{77f67f3e-754f-5d31-af64-ee037e18100a}",
                "display_name": "",
                "friendly_name": "SunshineHDR",
                "info": null
              },
              {
                "device_id": "{daeac860-f4db-5208-b1f5-cf59444fb768}",
                "display_name": "\\\\.\\DISPLAY1",
                "friendly_name": "ROG PG279Q",
                "info": {
                  "hdr_state": null,
                  "origin_point": {
                    "x": 0,
                    "y": 0
                  },
                  "primary": true,
                  "refresh_rate": {
                    "type": "rational",
                    "value": {
                      "denominator": 1000,
                      "numerator": 119998
                    }
                  },
                  "resolution": {
                    "height": 1440,
                    "width": 2560
                  },
                  "resolution_scale": {
                    "type": "rational",
                    "value": {
                      "denominator": 100,
                      "numerator": 100
                    }
                  }
                }
              }
            ]
            @endcode
            You need to use the `device_id` value.
            }
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">Sunshine will select the default display.</td>
    </tr>
    <tr>
        <td>Example (FreeBSD/Linux)</td>
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
            output_name = {daeac860-f4db-5208-b1f5-cf59444fb768}
            @endcode</td>
    </tr>
</table>

### dd_configuration_option

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Perform mandatory verification and additional configuration for the display device.
            @note{Applies to Windows only.}
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
            dd_configuration_option = ensure_only_display
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="5">Choices</td>
        <td>disabled</td>
        <td>Perform no additional configuration (disables all `dd_` configuration options).</td>
    </tr>
    <tr>
        <td>verify_only</td>
        <td>Verify that display is active only (this is a mandatory step without any extra steps to verify display state).</td>
    </tr>
    <tr>
        <td>ensure_active</td>
        <td>Activate the display if it's currently inactive.</td>
    </tr>
    <tr>
        <td>ensure_primary</td>
        <td>Activate the display if it's currently inactive and make it primary.</td>
    </tr>
    <tr>
        <td>ensure_only_display</td>
        <td>Activate the display if it's currently inactive and disable all others.</td>
    </tr>
</table>

### dd_resolution_option

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Perform additional resolution configuration for the display device.
            @note{"Optimize game settings" must be enabled in Moonlight for this option to work.}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}auto@endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_resolution_option = manual
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>disabled</td>
        <td>Perform no additional configuration.</td>
    </tr>
    <tr>
        <td>auto</td>
        <td>Change resolution to the requested resolution from the client.</td>
    </tr>
    <tr>
        <td>manual</td>
        <td>Change resolution to the user specified one (set via [dd_manual_resolution](#dd_manual_resolution)).</td>
    </tr>
</table>

### dd_manual_resolution

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Specify manual resolution to be used.
            @note{[dd_resolution_option](#dd_resolution_option) must be set to `manual`}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">n/a</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_manual_resolution = 1920x1080
            @endcode</td>
    </tr>
</table>

### dd_refresh_rate_option

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Perform additional refresh rate configuration for the display device.
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}auto@endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_refresh_rate_option = manual
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>disabled</td>
        <td>Perform no additional configuration.</td>
    </tr>
    <tr>
        <td>auto</td>
        <td>Change refresh rate to the requested FPS value from the client.</td>
    </tr>
    <tr>
        <td>manual</td>
        <td>Change refresh rate to the user specified one (set via [dd_manual_refresh_rate](#dd_manual_refresh_rate)).</td>
    </tr>
</table>

### dd_manual_refresh_rate

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Specify manual refresh rate to be used.
            @note{[dd_refresh_rate_option](#dd_refresh_rate_option) must be set to `manual`}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">n/a</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_manual_resolution = 120
            dd_manual_resolution = 59.95
            @endcode</td>
    </tr>
</table>

### dd_hdr_option

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Perform additional HDR configuration for the display device.
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}auto@endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_hdr_option = disabled
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="2">Choices</td>
        <td>disabled</td>
        <td>Perform no additional configuration.</td>
    </tr>
    <tr>
        <td>auto</td>
        <td>Change HDR to the requested state from the client if the display supports it.</td>
    </tr>
</table>

### dd_wa_hdr_toggle_delay

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            When using virtual display device (VDD) for streaming, it might incorrectly display HDR color. Sunshine can try to mitigate this issue, by turning HDR off and then on again.<br>
            If the value is set to 0, the workaround is disabled (default). If the value is between 0 and 3000 milliseconds, Sunshine will turn off HDR, wait for the specified amount of time and then turn HDR on again. The recommended delay time is around 500 milliseconds in most cases.<br>
            DO NOT use this workaround unless you actually have issues with HDR as it directly impacts stream start time!
            @note{This option works independently of [dd_hdr_option](#dd_hdr_option)}
            @note{Applies to Windows only.}
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
            dd_wa_hdr_toggle_delay = 500
            @endcode</td>
    </tr>
</table>

### dd_config_revert_delay

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Additional delay in milliseconds to wait before reverting configuration when the app has been closed or the last session terminated.
            Main purpose is to provide a smoother transition when quickly switching between apps.
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}3000@endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_config_revert_delay = 1500
            @endcode</td>
    </tr>
</table>


### dd_config_revert_on_disconnect

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            When enabled, display configuration is reverted upon disconnect of all clients instead of app close or last session termination.
            This can be useful for returning to physical usage of the host machine without closing the active app.
            @warning{Some applications may not function properly when display configuration is changed while active.}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}disabled@endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_config_revert_on_disconnect = enabled
            @endcode</td>
    </tr>
</table>

### dd_mode_remapping

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Remap the requested resolution and FPS to another display mode.<br>
            Depending on the [dd_resolution_option](#dd_resolution_option) and
            [dd_refresh_rate_option](#dd_refresh_rate_option) values, the following mapping
            groups are available:
            <ul>
                <li>`mixed` - both options are set to `auto`.</li>
                <li>
                  `resolution_only` - only [dd_resolution_option](#dd_resolution_option) is set to `auto`.
                </li>
                <li>
                  `refresh_rate_only` - only [dd_refresh_rate_option](#dd_refresh_rate_option) is set to `auto`.
                </li>
            </ul>
            For each of those groups, a list of fields can be configured to perform remapping:
            <ul>
                <li>
                  `requested_resolution` - resolution that needs to be matched in order to use this remapping entry.
                </li>
                <li>`requested_fps` - FPS that needs to be matched in order to use this remapping entry.</li>
                <li>`final_resolution` - resolution value to be used if the entry was matched.</li>
                <li>`final_refresh_rate` - refresh rate value to be used if the entry was matched.</li>
            </ul>
            If `requested_*` field is left empty, it will match <b>everything</b>.<br>
            If `final_*` field is left empty, the original value will not be remapped and either a requested, manual
            or current value is used. However, at least one `final_*` must be set, otherwise the entry is considered
            invalid.<br>
            @note{"Optimize game settings" must be enabled on client side for ANY entry with `resolution`
            field to be considered.}
            @note{First entry to be matched in the list is the one that will be used.}
            @tip{`requested_resolution` and `final_resolution` can be omitted for `refresh_rate_only` group.}
            @tip{`requested_fps` and `final_refresh_rate` can be omitted for `resolution_only` group.}
            @note{Applies to Windows only.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            dd_mode_remapping = {
              "mixed": [],
              "resolution_only": [],
              "refresh_rate_only": []
            }
            @endcode
        </td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            dd_mode_remapping = {
              "mixed": [
                {
                  "requested_fps": "60",
                  "final_refresh_rate": "119.95",
                  "requested_resolution": "1920x1080",
                  "final_resolution": "2560x1440"
                },
                {
                  "requested_fps": "60",
                  "final_refresh_rate": "120",
                  "requested_resolution": "",
                  "final_resolution": ""
                }
              ],
              "resolution_only": [
                {
                  "requested_resolution": "1920x1080",
                  "final_resolution": "2560x1440"
                }
              ],
              "refresh_rate_only": [
                {
                  "requested_fps": "60",
                  "final_refresh_rate": "119.95"
                }
              ]
            }@endcode
        </td>
    </tr>
</table>

### max_bitrate

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The maximum bitrate (in Kbps) that Sunshine will encode the stream at. If set to 0, it will always use the bitrate requested by Moonlight.
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
            max_bitrate = 5000
            @endcode</td>
    </tr>
</table>

### max_stream_resolution

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The maximum resolution (formatted as WIDTHxHEIGHT) that Sunshine will stream at. Larger client requests
            are downscaled, preserving aspect ratio. Lowering this reduces encoding latency on hosts where the
            encoder cannot keep up with the requested resolution. If empty, the resolution requested by Moonlight
            is used.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            @endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            max_stream_resolution = 2560x1440
            @endcode</td>
    </tr>
</table>

### minimum_fps_target

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sunshine tries to save bandwidth when content on screen is static or a low framerate. Because many clients expect a constant stream of video frames, a certain amount of duplicate frames are sent when this happens. This setting controls the lowest effective framerate a stream can reach.
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}
            0
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="3">Choices</td>
        <td>0</td>
        <td>Use half the stream's FPS as the minimum target.</td>
    </tr>
    <tr>
        <td>1-1000</td>
        <td>Specify your own value. The real minimum may differ from this value.</td>
    </tr>
</table>

## Network

### upnp

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

### address_family

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

### bind_address

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Set the IP address to bind Sunshine to. This is useful when you have multiple network interfaces
            and want to restrict Sunshine to a specific one. If not set, Sunshine will bind to all available
            interfaces (0.0.0.0 for IPv4 or :: for IPv6).
            <br><br>
            <strong>Note:</strong> The address must exist on the host and be compatible with address_family.
            An IPv4 address works with either "ipv4" or "both"; when used with "both", Sunshine listens only
            on that IPv4 address. An IPv6 address requires address_family to be set to "both".
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">
            Empty, binds to all interfaces
            </td>
    </tr>
    <tr>
        <td>Example (IPv4)</td>
        <td colspan="2">@code{}
            bind_address = 192.168.1.100
            @endcode</td>
    </tr>
    <tr>
        <td>Example (IPv6)</td>
        <td colspan="2">@code{}
            bind_address = 2001:db8::1
            @endcode</td>
    </tr>
    <tr>
        <td>Example (Loopback)</td>
        <td colspan="2">@code{}
            bind_address = 127.0.0.1
            @endcode</td>
    </tr>
</table>

### port

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

### origin_web_ui_allowed

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

### csrf_allowed_origins

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Comma-separated list of additional allowed origins for CSRF protection. These origins will be
            appended to the default allowed origins (localhost variants and the configured web UI port).
            Requests from allowed origins can access state-changing API endpoints without CSRF tokens.
            <br><br>
            @attention{Only add origins you trust. Each origin must be a complete URL prefix
            including protocol and host (e.g., https://example.com). Port numbers are optional.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">
            Empty, uses built-in defaults: https://localhost, https://127.0.0.1, https://[::1],
            with configured UI port variants
            </td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            csrf_allowed_origins = https://myapp.local,https://custom.domain.com
            @endcode</td>
    </tr>
</table>

### external_ip

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

### lan_encryption_mode

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

### wan_encryption_mode

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

### ping_timeout

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

### packetsize

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Limit the packetsize to avoid fragmentation on a low MTU link.
            @note{This helps avoid packet loss and micro-stutter on a layer 2 VPN with
            clients that cannot configure this value, e.g. Moonlight for Android/iOS.
            }
            @tip{To discover the optimal value:
            <ul>
                <li>Send ping to the server with don't fragment flag (DF)</li>
                <li>Find the size of the largest replay, and subtract 16</li>
                <li>Monitor the traffic to ensure no fragmentation</li>
            </ul>
            If using a VPN tunnel:
            <ul>
                <li>Set MTU on the TUN/TAP interface, and</li>
                <li>Ensure no fragmentation both inside and outside the tunnel</li>
                <li>Max UDP size = MTU size - 28</li>
                <li>`packetsize` = max UDP size - 16</li>
                <li>Monitor the traffic to ensure no fragmentation</li>
            </ul>
            Sample calculation for OpenVPN layer 2, using IPv4:
            <ul>
                <li>1428 bytes for max ICMP/UDP size outside the tunnel</li>
                <li>Subtract the OpenVPN overhead: 24 bytes (may vary)</li>
                <li>1404 bytes for Ethernet inside the tunnel</li>
                <li>Subtract the Ethernet header: 14 bytes</li>
                <li>1390 bytes for MTU inside the tunnel</li>
                <li>Subtract the IPv4 header: 20 bytes</li>
                <li>Subtract the UDP header: 8 bytes</li>
                <li>1362 bytes for UDP payload</li>
                <li>Subtract: 16 bytes</li>
                <li>1346 bytes for `packetsize`</li>
            </ul>
            }
            @warning{Reduce the bitrate when using low values.
            Values larger than 1456 require jumbo frames.
            }
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
        <td colspan="2">0, 200-65535</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            packetsize = 1346
            @endcode</td>
    </tr>
</table>

### dscp_tagging

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Mark audio and video packets with DSCP priority tags when requested by the client.
            @tip{Disabling this may help if audio crackles or stutters on Wi-Fi while the network otherwise appears
            healthy. Some routers handle tagged traffic poorly.}
            @note{On Windows, disabling this also removes local traffic prioritization.}
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
            dscp_tagging = disabled
            @endcode</td>
    </tr>
</table>

## Config Files

### file_apps

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

### credentials_file

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

### log_path

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The path where the current Sunshine log is stored. Each time Sunshine starts, up to five previous
            logs are retained by appending <code>.1</code> through <code>.5</code> to this path.
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

### pkey

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

### cert

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

### file_state

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

## Advanced

### fec_percentage

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Percentage of error correcting packets per data packet in each video frame.
            Set to <code>0</code> to disable forward error correction.
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
        <td colspan="2">0-100</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            fec_percentage = 20
            @endcode</td>
    </tr>
</table>

### qp

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

### min_threads

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

### hevc_mode

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

### av1_mode

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

### capture

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
        <td rowspan="8">Choices</td>
        <td>nvfbc</td>
        <td>Use NVIDIA Frame Buffer Capture to capture direct to GPU memory. This is usually the fastest method for
            NVIDIA cards. NvFBC does not have native Wayland support and does not work with XWayland.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>wlr</td>
        <td>Capture for wlroots based Wayland compositors via wlr-screencopy-unstable-v1. It is possible to capture
            virtual displays in e.g. Hyprland using this method. On an output that refreshes at least 1.5 times
            as fast as the stream, frames are captured when the compositor has a new one instead of on a clock of
            Sunshine's own, which avoids repeated frames; a virtual display at twice the stream's frame rate
            qualifies.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>kms</td>
        <td>DRM/KMS screen capture from the kernel. This requires that Sunshine has `cap_sys_admin` capability.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>kwin</td>
        <td>Capture with KDE/KWin Wayland compositor directly via KDE screencasting.
            @note{Applies to Linux only.}
            @warning{Flatpak users currently shouldn't use this capture method and use `portal` instead. KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 is necessary to make this capture method work with Flatpak but that has security implications.}</td>
    </tr>
    <tr>
        <td>portal</td>
        <td>Capture via XDG Desktop Portal.
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>x11</td>
        <td>Uses XCB. This is the slowest and most CPU intensive so should be avoided if possible.
            @note{Applies to FreeBSD and Linux only.}</td>
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

### fp16_sdr_gamma_encoded

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Treat FP16 captures of SDR displays as already sRGB gamma-encoded and skip the sRGB curve when converting
            them. Enable this if the stream looks washed out or its highlights are blown out while the display is in SDR
            mode and the desktop is captured in FP16 format (the log shows `Desktop format [DXGI_FORMAT_R16G16B16A16_FLOAT]`).
            Leave this disabled otherwise, because it darkens the stream when the FP16 capture is linear scRGB.
            This option has no effect when the display is in HDR mode.
            @note{Applies to Windows only.}
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
            fp16_sdr_gamma_encoded = enabled
            @endcode</td>
    </tr>
</table>

### encoder

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
        <td rowspan="7">Choices</td>
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
        <td>Use VA-API (AMD, Intel)</td>
    </tr>
    <tr>
        <td>vulkan</td>
        <td>Use Vulkan encoder (AMD, Intel, NVIDIA).
            @note{Applies to Linux only.}</td>
    </tr>
    <tr>
        <td>videotoolbox</td>
        <td>Use VideoToolbox.
            @note{Applies to macOS only.}</td>
    </tr>
    <tr>
        <td>software</td>
        <td>Encoding occurs on the CPU</td>
    </tr>
</table>

## NVIDIA NVENC Encoder

### nvenc_preset

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            NVENC encoder performance preset.
            Higher numbers improve compression (quality at given bitrate) at the cost of increased encoding latency.
            Recommended to change only when limited by network or decoder, otherwise similar effect can be accomplished
            by increasing bitrate.
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_twopass

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Enable two-pass mode in NVENC encoder.
            This allows to detect more motion vectors, better distribute bitrate across the frame and more strictly
            adhere to bitrate limits. Disabling it is not recommended since this can lead to occasional bitrate
            overshoot and subsequent packet loss.
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_spatial_aq

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Assign higher QP values to flat regions of the video.
            Recommended to enable when streaming at lower bitrates.
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_vbv_increase

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
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_realtime_hags

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Use realtime gpu scheduling priority in NVENC when hardware accelerated gpu scheduling (HAGS) is enabled
            in Windows. Currently, NVIDIA drivers may freeze in encoder when HAGS is enabled, realtime priority is used
            and VRAM utilization is close to maximum. Disabling this option lowers the priority to high, sidestepping
            the freeze at the cost of reduced capture performance when the GPU is heavily loaded.
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_split_encode

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Split the encoding of each video frame over multiple NVENC hardware units.
            Significantly reduces encoding latency with a marginal compression efficiency penalty.
            This option is ignored if your GPU has a singular NVENC unit.
            @note{This option only applies when using NVENC [encoder](#encoder) with HEVC or AV1.}
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
        <td>The NVIDIA driver will automatically enable split frame encoding when the following conditions are met: 2+ NVENC units, resolution is at least 4K, and the preset is P1-P4.</td>
    </tr>
    <tr>
        <td>enabled</td>
        <td>Enabled</td>
    </tr>
</table>

### nvenc_latency_over_power

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Adaptive P-State algorithm which NVIDIA drivers employ doesn't work well with low latency streaming,
            so Sunshine requests high power mode explicitly.
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_opengl_vulkan_on_dxgi

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Sunshine can't capture fullscreen OpenGL and Vulkan programs at full frame rate unless they present on
            top of DXGI. With this option enabled Sunshine changes global Vulkan/OpenGL present method to
            "Prefer layered on DXGI Swapchain". This is system-wide setting that is reverted on Sunshine program exit.
            @note{This option only applies when using NVENC [encoder](#encoder).}
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

### nvenc_h264_cavlc

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Prefer CAVLC entropy coding over CABAC in H.264 when using NVENC.
            CAVLC is outdated and needs around 10% more bitrate for same quality, but provides slightly faster
            decoding when using software decoder.
            @note{This option only applies when using H.264 format with the
            NVENC [encoder](#encoder).}
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

## Intel QuickSync Encoder

### qsv_preset

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder preset to use.
            @note{This option only applies when using quicksync [encoder](#encoder).}
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

### qsv_coder

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The entropy encoding to use.
            @note{This option only applies when using H.264 with the quicksync
            [encoder](#encoder).}
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

### qsv_slow_hevc

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            This options enables use of HEVC on older Intel GPUs that only support low power encoding for H.264.
            @note{This option only applies when using quicksync [encoder](#encoder).}
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

## AMD AMF Encoder

### amd_usage

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder usage profile is used to set the base set of encoding parameters.
            @note{This option only applies when using amdvce [encoder](#encoder).}
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

### amd_rc

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder rate control.
            @note{This option only applies when using amdvce [encoder](#encoder).}
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

### amd_enforce_hrd

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Enable Hypothetical Reference Decoder (HRD) enforcement to help constrain the target bitrate.
            @note{This option only applies when using amdvce [encoder](#encoder).}
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

### amd_max_au_size

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Maximum Access Unit (frame) size for rate control, in bits. This is a last-resort
            control for capping oversized encoded frames that would otherwise exceed Sunshine's
            four-block FEC capacity and be sent without FEC protection.
            @note{This option only applies to H.264 and HEVC when using amdvce [encoder](#encoder).
            AV1 does not support this option.}
            @note{The FEC ceiling depends on the negotiated packet size and [fec_percentage](#fec_percentage),
            not the average bitrate or frame rate. The approximate encoded-payload ceiling is
            `4 * floor(25500 / (100 + fec_percentage)) * (packet_size - 16) * 8` bits, before
            allowing for the short frame header or codec-header replacements. With 20% FEC and
            1024-byte packets, 6400000 bits provides conservative headroom.}
            @warning{Setting this unnecessarily low can reduce image quality, especially for
            keyframes. Leave it empty unless oversized frames are actually causing FEC to be skipped.}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">
            Empty, uses encoder default.
            </td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            amd_max_au_size = 6400000
            @endcode</td>
    </tr>
</table>

### amd_quality

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The quality profile controls the tradeoff between speed and quality of encoding.
            @note{This option only applies when using amdvce [encoder](#encoder).}
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

### amd_preanalysis

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Preanalysis can increase encoding quality at the cost of latency.
            @note{This option only applies when using amdvce [encoder](#encoder).}
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

### amd_vbaq

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Variance Based Adaptive Quantization (VBAQ) can increase subjective visual quality by prioritizing
            allocation of more bits to smooth areas compared to more textured areas.
            @note{This option only applies when using amdvce [encoder](#encoder).}
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

### amd_coder

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The entropy encoding to use.
            @note{This option only applies when using H.264 with the amdvce
            [encoder](#encoder).}
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

## VideoToolbox Encoder

### vt_coder

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The entropy encoding to use for H.264.
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
        <td>let VideoToolbox decide</td>
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

### vt_realtime

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

## VA-API Encoder

### vaapi_blbrc

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Block level based bitrate control (BLBRC) can assign different bitrate on a per-block basis. May improve quality on supported devices.
            @note{This option only applies when using the VA-API [encoder](#encoder).}
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
            vaapi_blbrc = enabled
            @endcode</td>
    </tr>
</table>

### vaapi_quality

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The quality profile controls the tradeoff between speed and quality of encoding.
            @note{This option only applies when using the VA-API [encoder](#encoder).}
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
            vaapi_quality = auto
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="4">Choices</td>
        <td>auto</td>
        <td>driver default quality</td>
    </tr>
    <tr>
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

### vaapi_rc

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder rate control.
            @note{This option only applies when using the VA-API [encoder](#encoder).}
            @warning{The automatic setting may override the driver-default rate control method to VBR and force [vaapi_strict_rc_buffer](#vaapi_strict_rc_buffer) enabled on certain configurations. Selecting another rate control manually will override this behaviour.}
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
            vaapi_rc = vbr
            @endcode</td>
    </tr>
    <tr>
        <td rowspan="7">Choices</td>
        <td>auto</td>
        <td>driver default (or whitelisted override)</td>
    </tr>
        <tr>
        <td>avbr</td>
        <td>average variable bitrate</td>
    </tr>
    <tr>
        <td>cbr</td>
        <td>constant bitrate</td>
    </tr>
        <tr>
        <td>cqp</td>
        <td>constant qp mode</td>
    </tr>
    <tr>
        <td>icq</td>
        <td>intelligent qp mode</td>
    </tr>
    <tr>
        <td>qvbr</td>
        <td>quality-defined variable bitrate</td>
    </tr>
        <tr>
        <td>vbr</td>
        <td>variable bitrate</td>
    </tr>
</table>

### vaapi_strict_rc_buffer

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Enabling this option can avoid dropped frames over the network during scene changes, but video quality may
            be reduced during motion.
            @note{This option only applies for H.264 and HEVC when using VA-API [encoder](#encoder) on AMD GPUs (or when overriding the default rate control on other devices).}
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
            vaapi_strict_rc_buffer = enabled
            @endcode</td>
    </tr>
</table>

## Vulkan Encoder

### vk_tune

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Encoder tuning preset. Low latency modes reduce encoding delay at the cost of quality.
            @note{This option only applies when using Vulkan [encoder](#encoder).}
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
            vk_tune = 1
            @endcode</td>
    </tr>
    <tr>
        <td>Options</td>
        <td>0 (default)</td>
        <td>Let the driver decide</td>
    </tr>
    <tr>
        <td></td>
        <td>1 (hq)</td>
        <td>High Quality</td>
    </tr>
    <tr>
        <td></td>
        <td>2 (ll)</td>
        <td>Low Latency</td>
    </tr>
    <tr>
        <td></td>
        <td>3 (ull)</td>
        <td>Ultra Low Latency</td>
    </tr>
    <tr>
        <td></td>
        <td>4 (lossless)</td>
        <td>Lossless</td>
    </tr>
</table>

### vk_rc_mode

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Rate control mode for encoding. Auto lets the driver decide.
            @note{This option only applies when using Vulkan [encoder](#encoder).}
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
            vk_rc_mode = 4
            @endcode</td>
    </tr>
    <tr>
        <td>Options</td>
        <td>0</td>
        <td>Auto (driver decides)</td>
    </tr>
    <tr>
        <td></td>
        <td>1</td>
        <td>CQP (Constant QP)</td>
    </tr>
    <tr>
        <td></td>
        <td>2</td>
        <td>CBR (Constant Bitrate)</td>
    </tr>
    <tr>
        <td></td>
        <td>4</td>
        <td>VBR (Variable Bitrate)</td>
    </tr>
</table>

### vk_quality

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            Determines encoder tradeoff between quality and speed.
            @note{This option only applies when using Vulkan [encoder](#encoder).}
        </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colspan="2">@code{}balanced@endcode</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colspan="2">@code{}
            vk_quality = quality
            @endcode</td>
    </tr>
    <tr>
        <td>Options</td>
        <td>speed</td>
        <td>Speed (prefer speed)</td>
    </tr>
    <tr>
        <td></td>
        <td>balanced</td>
        <td>Balanced (default)</td>
    </tr>
    <tr>
        <td></td>
        <td>quality</td>
        <td>Quality (prefer quality)</td>
    </tr>
</table>

## Software Encoder

### sw_preset

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The encoder preset to use.
            @note{This option only applies when using software [encoder](#encoder).}
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

### sw_tune

<table>
    <tr>
        <td>Description</td>
        <td colspan="2">
            The tuning preset to use.
            @note{This option only applies when using software [encoder](#encoder).}
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
