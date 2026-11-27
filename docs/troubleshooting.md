# Troubleshooting

## General

### Forgotten Credentials
If you forgot your credentials to the web UI, try this.

@tabs_grouped{linux-package|:|
  @tab{General |:| ```bash
    sunshine --creds {new-username} {new-password}
    ```
  }
  @tab{AppImage |:| ```bash
    ./sunshine.AppImage --creds {new-username} {new-password}
    ```
  }
  @tab{Flatpak |:| ```bash
    flatpak run --command=sunshine dev.lizardbyte.app.Sunshine --creds {new-username} {new-password}
    ```
  }
}

> [!TIP]
> Remember to replace `{new-username}` and `{new-password}` with your new credentials.
> Do not include the curly braces.

### Unusual Mouse Behavior
If you experience unusual mouse behavior, try attaching a physical mouse to the Sunshine host.

For absolute-positioning problems, set **Configuration > Advanced > Log Level** to **Debug** and reproduce the issue.
Sunshine forwards libvirtualhid diagnostics with a `[libvirtualhid]` prefix, including the streamed display viewport and
full desktop bounds used for mouse coordinate mapping.

For native touch or pen input on KDE Wayland, Sunshine maps its virtual devices to the streamed display.
Use Plasma 6.5.1 or later, which fixes changing a device's assigned output. Display layouts with monitors
left of or above the desktop origin are supported. Other Wayland compositors retain their device mapping
settings; assign Sunshine's virtual touchscreen and pen tablet through the compositor's input configuration.

### Web UI Access
Can't access the web UI?

1. Check firewall rules.

### Controller works on Steam but not in games
One trick might be to change Steam settings and check or uncheck the configuration to support Xbox/PlayStation
controllers and leave only support for Generic controllers.

Also, if you have many controllers already directly connected to the host, it might help to disable them so that the
Sunshine-provided controller (connected to the guest) is the "first" one. In Linux this can be achieved on USB
devices by finding the device in `/sys/bus/usb/devices/` and writing `0` to the `authorized` file.

### Network performance test

For real-time game streaming the most important characteristic of the network
path between server and client is not pure bandwidth but rather stability and
consistency (low latency with low variance, minimal or no packet loss).

The network can be tested using the multi-platform tool [iPerf3](https://iperf.fr).

On the Sunshine host `iperf3` is started in server mode:

```bash
iperf3 -s
```

On the client device iperf3 is asked to perform a 60-second UDP test in a reverse
direction (from server to client) at a given bitrate (e.g. 50 Mbps):

```bash
iperf3 -c {HostIpAddress} -t 60 -u -R -b 50M
```

Watch the output on the client for packet loss and jitter values. Both should be
(very) low. Ideally, packet loss remains less than 5% and jitter below 1 ms.

For Android clients use
[PingMaster](https://play.google.com/store/apps/details?id=com.appplanex.pingmasternetworktools).

For iOS clients use [HE.NET Network Tools](https://apps.apple.com/us/app/he-net-network-tools/id858241710).

If you are testing a remote connection (over the internet), you will need to
forward the port 5201 (TCP and UDP) from your host.

### Packet loss (Buffer overrun)
If the host PC (running Sunshine) has a much faster connection to the network
than the slowest segment of the network path to the client device (running
Moonlight), massive packet loss can occur: Sunshine emits its stream in bursts
every 16 ms (for 60 fps), but those bursts can't be passed on fast enough to the
client and must be buffered by one of the network devices inbetween. If the
bitrate is high enough, these buffers will overflow and data will be discarded.

This can easily happen if e.g., the host has a 2.5 Gbit/s connection and the
client only 1 Gbit/s or Wi-Fi. Similarly, a 1 Gbps host may be too fast for a
client having only a 100 Mbps interface.

As a workaround the transmission speed of the host NIC can be reduced: 1 Gbps
instead of 2.5 or 100 Mbps instead of 1 Gbps. A technically more advanced
solution would be to configure traffic shaping rules at the OS level, so that
only Sunshine's traffic is slowed down.

Such a solution on Linux could look like that:

```bash
# 1) Remove existing qdisc (pfifo_fast)
sudo tc qdisc del dev <NIC> root

# 2) Add HTB root qdisc with default class 1:1
sudo tc qdisc add dev <NIC> root handle 1: htb default 1

# 3) Create class 1:1 for full 10 Gbit/s (all other traffic)
sudo tc class add dev <NIC> parent 1: classid 1:1 htb \
    rate 10000mbit ceil 10000mbit burst 32k

# 4) Create class 1:10 for Sunshine game stream at 1 Gbit/s
sudo tc class add dev <NIC> parent 1: classid 1:10 htb \
    rate 1000mbit ceil 1000mbit burst 32k

# 5) Filter UDP source port 47998 into class 1:10
sudo tc filter add dev <NIC> protocol ip parent 1: prio 1 \
    u32 match ip protocol 17 0xff \
    match ip sport 47998 0xffff flowid 1:10
```

In that way only the Sunshine traffic is limited by 1 Gbit. This is not persistent on reboots.
If you use a different port for the game stream, you need to adjust the last command.

Sunshine versions > 0.23.1 include improved networking code that should
alleviate or even solve this issue (without reducing the NIC speed).

### Packet loss (MTU)
Although unlikely, some guests might work better with a lower
[MTU](https://en.wikipedia.org/wiki/Maximum_transmission_unit) from the host.
For example, an LG TV was found to have 30–60% packet loss when the host had MTU
set to 1500 and 1472, but 0% packet loss with a MTU of 1428 set in the network card
serving the stream (a Linux PC). It's unclear how that helped precisely, so it's a last
resort suggestion.

## Linux

### Hardware Encoders throttle/drop FPS during high GPU load
Capture methods (`wlgrab`) or encoders (`nvenc`, `vaapi`) that utilize EGL contexts may exhibit FPS drops
in conjunction with a Sunshine installation that runs in a sandboxed or reduced permissions state
(Flatpak, AppImage packages) due to the lack of active CAP_SYS_NICE process permissions
needed to set up high priority EGL contexts.

To check if you are affected by this issue, look out for this message in your Sunshine log:
```
Warning: EGL: context priority set to HIGH but CAP_SYS_NICE capability is missing
```

> [!IMPORTANT]
> Switching to Vulkan encoding should resolve the issue for the majority of configurations, but refer to this
> table for recommended configurations (especially if Vulkan encoding is not supported on your system):
> | Desktop Environment | Vulkan Supported? | Recommended Sunshine Install Type | Recommended Capture & Encoder Configuration              |
> |:--------------------|-------------------|-----------------------------------|---------------------------------------------------------:|
> | KDE Plasma          | Yes               | Any                               | `portal` or `kwin` capture with `vulkan` encoding        |
> | KDE Plasma          | No                | Non-Sandboxed                     | `portal` or `kwin` capture with `vaapi`/`nvenc` encoding |
> | GNOME / other       | Yes               | Any                               | `portal` capture with `vulkan` encoding                  |
> | GNOME / other       | No                | Non-Sandboxed                     | `portal` capture with `vaapi`/`nvenc` encoding           |

### Hardware Encoding fails
Due to legal concerns, Mesa has disabled hardware decoding and encoding by default.

```txt
Error: Could not open codec [h264_vaapi]: Function not implemented
```

If you see the above error in the Sunshine logs, compiling *Mesa* manually may be required. See the official Mesa3D
[Compiling and Installing](https://docs.mesa3d.org/install.html) documentation for instructions.

> [!IMPORTANT]
> You must re-enable the disabled encoders. You can do so by passing the following argument to the build
> system. You may also want to enable decoders, however, that is not required for Sunshine and is not covered here.
> ```bash
> -Dvideo-codecs=h264enc,h265enc
> ```

> [!NOTE]
> Other build options are listed in the
> [meson options](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson_options.txt) file.

### XDG Portal Capture Permissions

Portal capture requires you to approve Remote Desktop permissions using an on-screen prompt on the host. Sunshine saves the
resulting restore token so the XDG Desktop Portal can reauthorize capture automatically on subsequent starts.

If a restore token doesn't yet exist or becomes stale (for example, after a Sunshine crash, switching desktop environments,
changing portal implementations, or changing the monitor associated with the Portal session), Sunshine will attempt to run
in fallback capture mode. While in this mode, Sunshine will temporarily select another working capture method so that you
can interact with the Portal Remote Desktop dialog. Once a new restore token is negotiated and saved, Sunshine will
automatically restart and switch back to Portal capture.

> [!IMPORTANT]
> If a fallback session cannot be created because no other capture methods are available, you will need to directly
> interact with the host or use an alternative Remote Desktop connection (e.g. VNC, RDP) to complete setup.

If the saved Portal token is valid but you want to change the monitor associated with Portal capture, open the Web UI,
open **Troubleshooting**, and select **Reset XDG Portal Capture**. Sunshine deletes the saved token and restarts.
Approve the Remote Desktop prompt again and select the display to capture when it appears.

> [!TIP]
> If Sunshine uses a custom configuration directory, you may need to delete the `portal_token` file (which may
> include a suffix such as .kde or .gnome) from the custom directory and restart Sunshine manually in lieu of the Web UI.

Users of the KDE Plasma desktop can bypass manual permission setup either by switching to `kwin` capture or setting
the following configuration to enable permanent capture authorization for Sunshine via Portal capture:

```bash
flatpak permission-set kde-authorized remote-desktop dev.lizardbyte.app.Sunshine yes
```
> [!NOTE]
> Although this configuration is plumbed through Flatpak, it will work with any supported Sunshine installation type.

### Input not working
After installation, the `udev` rules need to be reloaded. Our post-install script tries to do this for you
automatically, but if it fails, you may need to restart your system.

Sunshine recreates virtual gamepad device nodes for each streaming session. Manual `chmod` or `setfacl`
changes therefore disappear when the client reconnects. Confirm that the installed Sunshine rule contains the
parent-property import and `libvirtualhid/uhid/*` match, then reload it and reapply it to existing gamepad nodes:

```bash
grep -R -E 'IMPORT\{parent\}="HID_\*"|ENV\{HID_PHYS\}=="libvirtualhid/uhid/\*"' \
  /etc/udev/rules.d /usr/lib/udev/rules.d /lib/udev/rules.d 2>/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw
sudo udevadm trigger --subsystem-match=input
```

If the input is still not working, you may need to add your user to the `input` group.

```bash
sudo usermod -aG input $USER
```

#### Multiseat

If you run multiple concurrent Wayland sessions on separate logind seats (e.g. `seat0`, `seat1`),
your compositor may ignore injected input unless Sunshine's virtual devices are assigned to the correct seat.

Sunshine determines its target seat from `XDG_SEAT`, which is typically set automatically by your display manager.
If needed, you can override it manually in your systemd service file or shell environment before starting Sunshine.

When the seat is not `seat0`, Sunshine appends the seat name to its virtual device names, for example:

- libvirtualhid Keyboard (seat1)
- libvirtualhid Mouse (seat1)
- libvirtualhid Mouse (seat1) (Absolute)
- Sunshine (libvirtualhid) X-Box 360 Controller (seat1)

Sunshine creates two mouse devices: a relative one and an absolute one.

> [!NOTE]
> PlayStation gamepads always use Sony's native product name (`Wireless Controller`) so that Steam recognizes them,
> so they do not carry a seat suffix.

To assign Sunshine's virtual devices to the correct seat, create this udev rules file
(/etc/udev/rules.d/72-sunshine-virtual-seat.rules):
```udev
SUBSYSTEM=="input", KERNEL=="input*", ATTR{name}=="*(seat1)*", TAG+="seat", ENV{ID_SEAT}="seat1"
```

Then reload udev:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger -s input
```

### KMS Streaming fails
KMS screencasting requires elevated privileges which are not allowed for Flatpak or AppImage packages.
This means that you must install Sunshine using the native package format of your distribution, if available.
KMS capture will soon be phased out in favour of XDG Portal Capture (which works with all package types).

### KMS Streaming; some windows flicker/disappear on KDE Plasma 6.5+
KWin's overlay support interferes with KMS capture. As of KWin 6.5 this is not yet set by default, but
for future versions that enables this by default, you may be able to disable again via a special
[environment variable](https://invent.kde.org/plasma/kwin/-/wikis/Environment-Variables#kwin_use_overlays):

```bash
export KWIN_USE_OVERLAYS=0
```

> [!NOTE]
> Disabling overlays will reduce KWin's rendering efficiency. Consider using XDG Portal Capture instead.

### KMS streaming fails on Nvidia GPUs
If KMS screen capture results in a black screen being streamed, you may need to
set the parameter `modeset=1` for Nvidia's kernel module. This can be done by
adding the following directive to the kernel command line:

```bash
nvidia_drm.modeset=1
```

Consult your distribution's documentation for details on how to do this. (Most
often grub is used to load the kernel and set its command line.)

### AMD encoding latency issues
If you notice unexpectedly high encoding latencies (e.g., in Moonlight's
performance overlay) or strong fluctuations thereof, your system's Mesa
libraries are outdated (<24.2). This is particularly problematic at higher
resolutions (4K).

Starting with Mesa-24.2, applications can request a
[low-latency mode](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30039)
by running them with a special
[environment variable](https://docs.mesa3d.org/envvars.html#envvar-AMD_DEBUG):
```bash
export AMD_DEBUG=lowlatencyenc
```
Sunshine sets this variable automatically, no manual
configuration is needed.

To check whether low-latency mode is being used, one can watch the VCLK and DCLK
frequencies in amdgpu_top. Without this encoder tuning both clock frequencies
will fluctuate strongly, whereas with active low-latency encoding they will stay
high as long as the encoder is used.

### Gamescope compatibility
Some users have reported stuttering issues when streaming games running within Gamescope.

## macOS

### No gamepad detected
Confirm **Enable Gamepad Input** is on under **Configuration > Input**. Install
[Virtual HID Broker](https://github.com/LizardByte/libvirtualhid/releases/latest) separately from Sunshine,
using the macOS installer in its universal DMG. Confirm that `/Applications/VirtualHIDBroker.app` is allowed in
**System Settings > Privacy & Security > Device Control and Data Access**, and activate a machine license under
**Troubleshooting > Virtual HID Broker License** in Sunshine. The menu bar **Virtual HID Broker** submenu also
shows license status and links for license management and downloads. If the broker or license is unavailable, the
Troubleshooting page shows the broker response. Sunshine can show a startup notice when gamepad input is enabled
and the broker or license is unavailable. Choose **None** under **Configuration > Input > Gamepad Backend** to disable
virtual gamepads and that notice without affecting keyboard or mouse input. Reconnect the Moonlight session after choosing
a different emulated gamepad profile.

### Dynamic session lookup failed
If you get this error:

> Dynamic session lookup supported but failed: launchd did not provide a socket path, verify that
> org.freedesktop.dbus-session.plist is loaded!

Try this.
```bash
launchctl load -w /Library/LaunchAgents/org.freedesktop.dbus-session.plist
```

### Stream stutters at low bitrates with VideoToolbox
With the VideoToolbox encoder, a bitrate that is too low for the stream's resolution and content can make the encoder
drop frames. Sunshine then recreates the encoder, so every frame is sent as a key frame and the stream stutters at
reduced frame rate. The log shows repeated lines like:

```
Error: VideoToolbox dropped a frame
Info: Creating encoder [hevc_vt]
```

This happens with a low fixed bitrate as well as with [adaptive bitrate](configuration.md#bitrate_mode). For example,
on an Apple M5 streaming 1080p60 high-motion video, it happened below about 5 Mbps.

* With a fixed bitrate, raise the bitrate in Moonlight or lower the resolution or frame rate.
* With adaptive bitrate, set [abr_min_kbps](configuration.md#abr_min_kbps) above that level, e.g.
  `abr_min_kbps = 6000` for 1080p60.

## Windows

### No gamepad detected
Sunshine supports two virtual gamepad backends on Windows. You can install the
[Virtual HID Broker](https://github.com/LizardByte/libvirtualhid/releases/latest) separately as an optional paid upgrade.
Its Windows package includes a broker service and user-mode driver for a Raw Input keyboard and mouse plus full virtual
gamepad support. ViGEmBus is a limited alternative for Xbox 360 and DualShock 4 support that has reached end of life. If you use the
[ViGEmBus fallback](https://github.com/nefarius/ViGEmBus/releases/latest), you must use version 1.17 or newer.

When Virtual HID Broker is used, Sunshine requires libvirtualhid version `2026.914.1218.10` or newer.

Virtual HID Broker adds Xbox One, Xbox Series, DualSense, Nintendo Switch Pro, and Generic gamepads, plus advanced
controller features such as motion, touchpads, LEDs, and adaptive triggers when supported. Unlike the discontinued
ViGEmBus project, Virtual HID Broker is actively developed and supported by the LizardByte team.

An active paid Virtual HID Broker machine license is required before Sunshine can create driver-backed libvirtualhid
devices, including gamepads and the Raw Input keyboard and mouse. Use the message on the Web UI home page, the startup
tray notification, or **Get/Manage License** in the **Virtual HID Broker** tray submenu to open the license section on
the Troubleshooting page. In **Configuration > Input**, select **All Available Drivers**, only **Virtual HID Broker**, or
only **ViGEmBus**. Sunshine keeps prompting until this setting is saved, but automatically selects **All Available
Drivers** when it detects an existing active Virtual HID Broker license. Whenever the Virtual HID Broker license is not
valid, **All Available Drivers** falls back to a compatible ViGEmBus installation for Xbox 360 and DualShock 4 gamepads
and to SendInput for keyboard and mouse. Selecting only **ViGEmBus** suppresses Virtual HID Broker startup notifications
and hides its status and license details from the Troubleshooting page. Selecting **None** disables all virtual
gamepads and suppresses broker and driver choice notices without affecting keyboard or mouse input.

After installation, it is recommended to restart your computer.

### Games do not detect keyboard input
With a compatible Virtual HID Broker and active license, Sunshine sends normal key transitions through a real HID
keyboard so games using Raw Input can receive them. Unicode text input and keys outside the supported HID keyboard
page continue to use Windows input injection. When the driver-backed keyboard cannot be created because the driver,
broker, or license is unavailable, libvirtualhid falls back to SendInput.

Check the libvirtualhid driver version and Virtual HID Broker license on the Web UI Troubleshooting page. Sunshine recreates the
shared keyboard and mouse after a successful license activation, validation, or deactivation, so you do not need to
restart Sunshine merely to switch between the HID and SendInput paths.

### Games do not detect mouse input
With a compatible Virtual HID Broker and active license, Sunshine sends relative mouse movement, buttons, and scrolling
through a real HID device so games using Raw Input can receive them. Absolute positioning still uses Windows input
injection. When the driver-backed mouse cannot be created, libvirtualhid falls back to SendInput; the Windows cursor may
still move even though a game that listens only for Raw Input receives nothing.

Check the libvirtualhid driver version and Virtual HID Broker license on the Web UI Troubleshooting page even when controller input
is disabled. The same live refresh used by the keyboard path also switches the mouse between HID and SendInput without
requiring a Sunshine restart.

### Permission denied
Since Sunshine runs as a service on Windows, it may not have the same level of access that your regular user account
has. You may get permission denied errors when attempting to launch a game or application from a non-system drive.

You will need to modify the security permissions on your disk. Ensure that user/principal SYSTEM has full
permissions on the disk.

### Stuttering
If you experience stuttering using NVIDIA, try disabling `vsync:fast` in the NVIDIA Control Panel.
