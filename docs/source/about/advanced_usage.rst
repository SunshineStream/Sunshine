Advanced Usage
==============
Sunshine will work with the default settings for most users. In some cases you may want to configure Sunshine further.

Performance Tips
----------------

.. tab:: AMD

   In Windows, enabling `Enhanced Sync` in AMD's settings may help reduce the latency by an additional frame. This
   applies to `amfenc` and `libx264`.

.. tab:: NVIDIA

   Enabling `Fast Sync` in Nvidia settings may help reduce latency.

Configuration
-------------
The default location for the configuration file is listed below. You can use another location if you
choose, by passing in the full configuration file path as the first argument when you start Sunshine.

The default location of the ``apps.json`` is the same as the configuration file. You can use a custom
location by modifying the configuration file.

**Default File Location**

.. table::
   :widths: auto

   =========   ===========
   Value       Description
   =========   ===========
   Docker      /config/
   Linux       ~/.config/sunshine/
   macOS       ~/.config/sunshine/
   Windows     %ProgramFiles%\\Sunshine\\config
   =========   ===========

**Example**
   .. code-block:: bash

      sunshine ~/sunshine_config.conf

Although it is recommended to use the configuration UI, it is possible manually configure sunshine by
editing the `conf` file in a text editor. Use the examples as reference.

`General <https://localhost:47990/config/#general>`__
-----------------------------------------------------

`sunshine_name <https://localhost:47990/config/#sunshine_name>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The name displayed by Moonlight

**Default**
   PC hostname

**Example**
   .. code-block:: text

      sunshine_name = Sunshine

`min_log_level <https://localhost:47990/config/#min_log_level>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The minimum log level printed to standard out.

**Choices**

.. table::
   :widths: auto

   =======   ===========
   Value     Description
   =======   ===========
   verbose   verbose logging
   debug     debug logging
   info      info logging
   warning   warning logging
   error     error logging
   fatal     fatal logging
   none      no logging
   =======   ===========

**Default**
   ``info``

**Example**
   .. code-block:: text

      min_log_level = info

`global_prep_cmd <https://localhost:47990/config/#global_prep_cmd>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   A list of commands to be run before/after all applications. If any of the prep-commands fail, starting the application is aborted.

**Default**
   ``[]``

**Example**
   .. code-block:: text

      global_prep_cmd = [{"do":"nircmd.exe setdisplay 1280 720 32 144","undo":"nircmd.exe setdisplay 2560 1440 32 144"}]

`Files <https://localhost:47990/config/#files>`__
-------------------------------------------------

`file_apps <https://localhost:47990/config/#file_apps>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The application configuration file path. The file contains a json formatted list of applications that can be started
   by Moonlight.

**Default**
   OS and package dependent

**Example**
   .. code-block:: text

      file_apps = apps.json

`credentials_file <https://localhost:47990/config/#credentials_file>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The file where user credentials for the UI are stored.

**Default**
   ``sunshine_state.json``

**Example**
   .. code-block:: text

      credentials_file = sunshine_state.json

`log_path <https://localhost:47990/config/#log_path>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The path where the sunshine log is stored.

**Default**
   ``sunshine.log``

**Example**
   .. code-block:: text

      log_path = sunshine.log

`pkey <https://localhost:47990/config/#pkey>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The private key used for the web UI and Moonlight client pairing. For best compatibility, this should be an RSA-2048 private key.

   .. warning:: Not all Moonlight clients support ECDSA keys or RSA key lengths other than 2048 bits.

**Default**
   ``credentials/cakey.pem``

**Example**
   .. code-block:: text

      pkey = /dir/pkey.pem

`cert <https://localhost:47990/config/#cert>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The certificate used for the web UI and Moonlight client pairing. For best compatibility, this should have an RSA-2048 public key.

   .. warning:: Not all Moonlight clients support ECDSA keys or RSA key lengths other than 2048 bits.

**Default**
   ``credentials/cacert.pem``

**Example**
   .. code-block:: text

      cert = /dir/cert.pem

`file_state <https://localhost:47990/config/#file_state>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The file where current state of Sunshine is stored.

**Default**
   ``sunshine_state.json``

**Example**
   .. code-block:: text

      file_state = sunshine_state.json


`Input <https://localhost:47990/config/#input>`__
-------------------------------------------------

`controller <https://localhost:47990/config/#controller>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Whether to allow controller input from the client.

**Example**
   .. code-block:: text

      controller = enabled

`gamepad <https://localhost:47990/config/#gamepad>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The type of gamepad to emulate on the host.

   .. caution:: Applies to Windows only.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   auto      Selected based on information from client
   x360      Xbox 360 controller
   ds4       DualShock 4 controller (PS4)
   =====     ===========

**Default**
   ``auto``

**Example**
   .. code-block:: text

      gamepad = auto
      
`ds4_back_as_touchpad_click <https://localhost:47990/config/#ds4_back_as_touchpad_click>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   .. hint:: Only applies when gamepad is set to ds4 manually. Unused in other gamepad modes.

   Allow Select/Back inputs to also trigger DS4 touchpad click. Useful for clients looking to emulate touchpad click
   on Xinput devices.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      ds4_back_as_touchpad_click = enabled

`motion_as_ds4 <https://localhost:47990/config/#motion_as_ds4>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   .. hint:: Only applies when gamepad is set to auto.

   If a client reports that a connected gamepad has motion sensor support, emulate it on the host as a DS4 controller.

   When disabled, motion sensors will not be taken into account during gamepad type selection.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      motion_as_ds4 = enabled

`touchpad_as_ds4 <https://localhost:47990/config/#touchpad_as_ds4>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   .. hint:: Only applies when gamepad is set to auto.

   If a client reports that a connected gamepad has a touchpad, emulate it on the host as a DS4 controller.

   When disabled, touchpad presence will not be taken into account during gamepad type selection.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      touchpad_as_ds4 = enabled

`back_button_timeout <https://localhost:47990/config/#back_button_timeout>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   If the Back/Select button is held down for the specified number of milliseconds, a Home/Guide button press is emulated.

   .. tip:: If back_button_timeout < 0, then the Home/Guide button will not be emulated.

**Default**
   ``-1``

**Example**
   .. code-block:: text

      back_button_timeout = 2000

`keyboard <https://localhost:47990/config/#keyboard>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Whether to allow keyboard input from the client.

**Example**
   .. code-block:: text

      keyboard = enabled

`key_repeat_delay <https://localhost:47990/config/#key_repeat_delay>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The initial delay, in milliseconds, before repeating keys. Controls how fast keys will repeat themselves.

**Default**
   ``500``

**Example**
   .. code-block:: text

      key_repeat_delay = 500

`key_repeat_frequency <https://localhost:47990/config/#key_repeat_frequency>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   How often keys repeat every second.

   .. tip:: This configurable option supports decimals.

**Default**
   ``24.9``

**Example**
   .. code-block:: text

      key_repeat_frequency = 24.9

`always_send_scancodes <https://localhost:47990/config/#always_send_scancodes>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Sending scancodes enhances compatibility with games and apps but may result in incorrect keyboard input
   from certain clients that aren't using a US English keyboard layout.

   Enable if keyboard input is not working at all in certain applications.

   Disable if keys on the client are generating the wrong input on the host.

   .. caution:: Applies to Windows only.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      always_send_scancodes = enabled

`key_rightalt_to_key_win <https://localhost:47990/config/#key_rightalt_to_key_win>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   It may be possible that you cannot send the Windows Key from Moonlight directly. In those cases it may be useful to
   make Sunshine think the Right Alt key is the Windows key.

**Default**
   ``disabled``

**Example**
   .. code-block:: text

      key_rightalt_to_key_win = enabled

`mouse <https://localhost:47990/config/#mouse>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Whether to allow mouse input from the client.

**Example**
   .. code-block:: text

      mouse = enabled

`high_resolution_scrolling <https://localhost:47990/config/#high_resolution_scrolling>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   When enabled, Sunshine will pass through high resolution scroll events from Moonlight clients.

   This can be useful to disable for older applications that scroll too fast with high resolution scroll events.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      high_resolution_scrolling = enabled

`native_pen_touch <https://localhost:47990/config/#native_pen_touch>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   When enabled, Sunshine will pass through native pen/touch events from Moonlight clients. 

   This can be useful to disable for older applications without native pen/touch support.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      native_pen_touch = enabled

keybindings
^^^^^^^^^^^

**Description**
   Sometimes it may be useful to map keybindings. Wayland won't allow clients to capture the Win Key for example.

   .. tip:: See `virtual key codes <https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes>`__

   .. hint:: keybindings needs to have a multiple of two elements.

**Default**
   .. code-block:: text

      [
        0x10, 0xA0,
        0x11, 0xA2,
        0x12, 0xA4
      ]

**Example**
   .. code-block:: text

      keybindings = [
        0x10, 0xA0,
        0x11, 0xA2,
        0x12, 0xA4,
        0x4A, 0x4B
      ]

.. note:: This option is not available in the UI. A PR would be welcome.

`Audio/Video <https://localhost:47990/config/#audio-video>`__
-------------------------------------------------------------

`audio_sink <https://localhost:47990/config/#audio_sink>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The name of the audio sink used for audio loopback.

   .. tip:: To find the name of the audio sink follow these instructions.

      **Linux + pulseaudio**
         .. code-block:: bash

            pacmd list-sinks | grep "name:"

      **Linux + pipewire**
         .. code-block:: bash

            pactl info | grep Source
            # in some causes you'd need to use the `Sink` device, if `Source` doesn't work, so try:
            pactl info | grep Sink

      **macOS**
         Sunshine can only access microphones on macOS due to system limitations. To stream system audio use
         `Soundflower <https://github.com/mattingalls/Soundflower>`__ or
         `BlackHole <https://github.com/ExistentialAudio/BlackHole>`__.

      **Windows**
         .. code-block:: batch

            tools\audio-info.exe

         .. tip:: If you have multiple audio devices with identical names, use the Device ID instead.

   .. tip:: If you want to mute the host speakers, use `virtual_sink`_ instead.

**Default**
   Sunshine will select the default audio device.

**Examples**
   **Linux**
      .. code-block:: text

         audio_sink = alsa_output.pci-0000_09_00.3.analog-stereo

   **macOS**
      .. code-block:: text

         audio_sink = BlackHole 2ch

   **Windows**
      .. code-block:: text

         audio_sink = Speakers (High Definition Audio Device)

`virtual_sink <https://localhost:47990/config/#virtual_sink>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The audio device that's virtual, like Steam Streaming Speakers. This allows Sunshine to stream audio, while muting
   the speakers.

   .. tip:: See `audio_sink`_!

   .. tip:: These are some options for virtual sound devices.

      - Stream Streaming Speakers (Linux, macOS, Windows)

        - Steam must be installed.
        - Enable `install_steam_audio_drivers`_ or use Steam Remote Play at least once to install the drivers.

      - `Virtual Audio Cable <https://vb-audio.com/Cable/>`__ (macOS, Windows)

**Example**
   .. code-block:: text

      virtual_sink = Steam Streaming Speakers

`install_steam_audio_drivers <https://localhost:47990/config/#install_steam_audio_drivers>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Installs the Steam Streaming Speakers driver (if Steam is installed) to support surround sound and muting host audio.

   .. tip:: This option is only supported on Windows.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      install_steam_audio_drivers = enabled

`adapter_name <https://localhost:47990/config/#adapter_name>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Select the video card you want to stream.

   .. tip:: To find the name of the appropriate values follow these instructions.

      **Linux + VA-API**
         Unlike with `amdvce` and `nvenc`, it doesn't matter if video encoding is done on a different GPU.

         .. note:: This MUST be a render node (renderD*) not a primary node (card*).

         .. code-block:: bash

            ls /dev/dri/renderD*  # to find all devices capable of VAAPI

            # replace ``renderD129`` with the device from above to lists the name and capabilities of the device
            vainfo --display drm --device /dev/dri/renderD129 | \
              grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"

         To be supported by Sunshine, it needs to have at the very minimum:
         ``VAProfileH264High   : VAEntrypointEncSlice``

      .. todo:: macOS

      **Windows**
         .. code-block:: batch

            tools\dxgi-info.exe

         .. note:: For hybrid graphics systems, DXGI reports the outputs are connected to whichever graphics adapter
            that the application is configured to use, so it's not a reliable indicator of how the display is
            physically connected.

**Default**
   Sunshine will select the default video card.

**Examples**
   **Linux**
      .. code-block:: text

         adapter_name = /dev/dri/renderD128

   .. todo:: macOS

   **Windows**
      .. code-block:: text

         adapter_name = Radeon RX 580 Series

`output_name <https://localhost:47990/config/#output_name>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Select the display number you want to stream.

   .. tip:: To find the name of the appropriate values follow these instructions.

      **Linux**
         During Sunshine startup, you should see the list of detected monitors:

         .. code-block:: text

            Info: Detecting connected monitors
            Info: Detected monitor 0: DVI-D-0, connected: false
            Info: Detected monitor 1: HDMI-0, connected: true
            Info: Detected monitor 2: DP-0, connected: true
            Info: Detected monitor 3: DP-1, connected: false
            Info: Detected monitor 4: DVI-D-1, connected: false

         You need to use the value before the colon in the output, e.g. ``1``.

      .. todo:: macOS

      **Windows**
         .. code-block:: batch

            tools\dxgi-info.exe

**Default**
   Sunshine will select the default display.

**Examples**
   **Linux**
      .. code-block:: text

         output_name = 0

   .. todo:: macOS

   **Windows**
      .. code-block:: text

         output_name  = \\.\DISPLAY1

`resolutions <https://localhost:47990/config/#resolutions>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The resolutions advertised by Sunshine.

   .. note:: Some versions of Moonlight, such as Moonlight-nx (Switch), rely on this list to ensure that the requested
      resolution is supported.

**Default**
   .. code-block:: text

      [
        352x240,
        480x360,
        858x480,
        1280x720,
        1920x1080,
        2560x1080,
        3440x1440,
        1920x1200,
        3840x2160,
        3840x1600,
      ]

**Example**
   .. code-block:: text

      resolutions = [
        352x240,
        480x360,
        858x480,
        1280x720,
        1920x1080,
        2560x1080,
        3440x1440,
        1920x1200,
        3840x2160,
        3840x1600,
      ]

`fps <https://localhost:47990/config/#fps>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The fps modes advertised by Sunshine.

   .. note:: Some versions of Moonlight, such as Moonlight-nx (Switch), rely on this list to ensure that the requested
      fps is supported.

**Default**
   ``[10, 30, 60, 90, 120]``

**Example**
   .. code-block:: text

      fps = [10, 30, 60, 90, 120]

`Network <https://localhost:47990/config/#network>`__
-----------------------------------------------------

`address_family <https://localhost:47990/config/#address_family>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Set the address family that Sunshine will use.

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   ipv4      IPv4 only
   both      IPv4+IPv6
   =====     ===========

**Default**
   ``ipv4``

**Example**
   .. code-block:: text

      address_family = both

`port <https://localhost:47990/config/#port>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Set the family of ports used by Sunshine. Changing this value will offset other ports per the table below.

.. table::
   :widths: auto

   ================ ============ ===========================
   Port Description Default Port Difference from config port
   ================ ============ ===========================
   HTTPS            47984 TCP    -5
   HTTP             47989 TCP    0
   Web              47990 TCP    +1
   RTSP             48010 TCP    +21
   Video            47998 UDP    +9
   Control          47999 UDP    +10
   Audio            48000 UDP    +11
   Mic (unused)     48002 UDP    +13
   ================ ============ ===========================

.. attention:: Custom ports may not be supported by all Moonlight clients.

**Default**
   ``47989``

**Range**
   ``1029-65514``

**Example**
   .. code-block:: text

      port = 47989

`origin_web_ui_allowed <https://localhost:47990/config/#origin_web_ui_allowed>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The origin of the remote endpoint address that is not denied for HTTPS Web UI.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   pc        Only localhost may access the web ui
   lan       Only LAN devices may access the web ui
   wan       Anyone may access the web ui
   =====     ===========

**Default**
   ``lan``

**Example**
   .. code-block:: text

      origin_web_ui_allowed = lan

`upnp <https://localhost:47990/config/#upnp>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Sunshine will attempt to open ports for streaming over the internet.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   on        enable UPnP
   off       disable UPnP
   =====     ===========

**Default**
   ``disabled``

**Example**
   .. code-block:: text

      upnp = on

`external_ip <https://localhost:47990/config/#external_ip>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   If no external IP address is given, Sunshine will attempt to automatically detect external ip-address.

**Default**
   Automatic

**Example**
   .. code-block:: text

      external_ip = 123.456.789.12

`lan_encryption_mode <https://localhost:47990/config/#lan_encryption_mode>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   This determines when encryption will be used when streaming over your local network.

   .. warning:: Encryption can reduce streaming performance, particularly on less powerful hosts and clients.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   0         encryption will not be used
   1         encryption will be used if the client supports it
   2         encryption is mandatory and unencrypted connections are rejected
   =====     ===========

**Default**
   ``0``

**Example**
   .. code-block:: text

      lan_encryption_mode = 0

`wan_encryption_mode <https://localhost:47990/config/#wan_encryption_mode>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   This determines when encryption will be used when streaming over the Internet.

   .. warning:: Encryption can reduce streaming performance, particularly on less powerful hosts and clients.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   0         encryption will not be used
   1         encryption will be used if the client supports it
   2         encryption is mandatory and unencrypted connections are rejected
   =====     ===========

**Default**
   ``1``

**Example**
   .. code-block:: text

      wan_encryption_mode = 1

`ping_timeout <https://localhost:47990/config/#ping_timeout>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   How long to wait, in milliseconds, for data from Moonlight before shutting down the stream.

**Default**
   ``10000``

**Example**
   .. code-block:: text

      ping_timeout = 10000

`Advanced <https://localhost:47990/config/#advanced>`__
-------------------------------------------------------

`channels <https://localhost:47990/config/#channels>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   This will generate distinct video streams, unlike simply broadcasting to multiple Clients.

   When multicasting, it could be useful to have different configurations for each connected Client.

   For instance:

   - Clients connected through WAN and LAN have different bitrate constraints.
   - Decoders may require different settings for color.

   .. warning:: CPU usage increases for each distinct video stream generated.

**Default**
   ``1``

**Example**
   .. code-block:: text

      channels = 1

`fec_percentage <https://localhost:47990/config/#fec_percentage>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Percentage of error correcting packets per data packet in each video frame.

   .. warning:: Higher values can correct for more network packet loss, but at the cost of increasing bandwidth usage.

**Default**
   ``20``

**Range**
   ``1-255``

**Example**
   .. code-block:: text

      fec_percentage = 20

`qp <https://localhost:47990/config/#qp>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Quantization Parameter. Some devices don't support Constant Bit Rate. For those devices, QP is used instead.

   .. warning:: Higher value means more compression, but less quality.

**Default**
   ``28``

**Example**
   .. code-block:: text

      qp = 28

`min_threads <https://localhost:47990/config/#min_threads>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Minimum number of CPU threads used for encoding.

   .. note:: Increasing the value slightly reduces encoding efficiency, but the tradeoff is usually worth it to gain
      the use of more CPU cores for encoding. The ideal value is the lowest value that can reliably encode at your
      desired streaming settings on your hardware.

**Default**
   ``2``

**Example**
   .. code-block:: text

      min_threads = 2

`hevc_mode <https://localhost:47990/config/#hevc_mode>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Allows the client to request HEVC Main or HEVC Main10 video streams.

   .. warning:: HEVC is more CPU-intensive to encode, so enabling this may reduce performance when using software
      encoding.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   0         advertise support for HEVC based on encoder capabilities (recommended)
   1         do not advertise support for HEVC
   2         advertise support for HEVC Main profile
   3         advertise support for HEVC Main and Main10 (HDR) profiles
   =====     ===========

**Default**
   ``0``

**Example**
   .. code-block:: text

      hevc_mode = 2

`av1_mode <https://localhost:47990/config/#av1_mode>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Allows the client to request AV1 Main 8-bit or 10-bit video streams.

   .. warning:: AV1 is more CPU-intensive to encode, so enabling this may reduce performance when using software
      encoding.

**Choices**

.. table::
   :widths: auto

   =====     ===========
   Value     Description
   =====     ===========
   0         advertise support for AV1 based on encoder capabilities (recommended)
   1         do not advertise support for AV1
   2         advertise support for AV1 Main 8-bit profile
   3         advertise support for AV1 Main 8-bit and 10-bit (HDR) profiles
   =====     ===========

**Default**
   ``0``

**Example**
   .. code-block:: text

      av1_mode = 2

`capture <https://localhost:47990/config/#capture>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Force specific screen capture method.

   .. caution:: Applies to Linux only.

**Choices**

.. table::
   :widths: auto

   =========  ===========
   Value      Description
   =========  ===========
   nvfbc      Use NVIDIA Frame Buffer Capture to capture direct to GPU memory. This is usually the fastest method for
              NVIDIA cards. For GeForce cards it will only work with drivers patched with
              `nvidia-patch <https://github.com/keylase/nvidia-patch/>`__
              or `nvlax <https://github.com/illnyang/nvlax/>`__.
   wlr        Capture for wlroots based Wayland compositors via DMA-BUF.
   kms        DRM/KMS screen capture from the kernel. This requires that sunshine has cap_sys_admin capability.
              See :ref:`Linux Setup <about/setup:install>`.
   x11        Uses XCB. This is the slowest and most CPU intensive so should be avoided if possible.
   =========  ===========

**Default**
   Automatic. Sunshine will use the first capture method available in the order of the table above.

**Example**
   .. code-block:: text

      capture = kms

`encoder <https://localhost:47990/config/#encoder>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Force a specific encoder.

**Choices**

.. table::
   :widths: auto

   =========  ===========
   Value      Description
   =========  ===========
   nvenc      For NVIDIA graphics cards
   quicksync  For Intel graphics cards
   amdvce     For AMD graphics cards
   software   Encoding occurs on the CPU
   =========  ===========

**Default**
   Sunshine will use the first encoder that is available.

**Example**
   .. code-block:: text

      encoder = nvenc

`NVIDIA NVENC Encoder <https://localhost:47990/config/#nvidia-nvenc-encoder>`__
-------------------------------------------------------------------------------

`nvenc_preset <https://localhost:47990/config/#nvenc_preset>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   NVENC encoder performance preset.
   Higher numbers improve compression (quality at given bitrate) at the cost of increased encoding latency.
   Recommended to change only when limited by network or decoder, otherwise similar effect can be accomplished by increasing bitrate.

   .. note:: This option only applies when using NVENC `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   1          P1 (fastest)
   2          P2
   3          P3
   4          P4
   5          P5
   6          P6
   7          P7 (slowest)
   ========== ===========

**Default**
   ``1``

**Example**
   .. code-block:: text

      nvenc_preset = 1

`nvenc_twopass <https://localhost:47990/config/#nvenc_twopass>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Enable two-pass mode in NVENC encoder.
   This allows to detect more motion vectors, better distribute bitrate across the frame and more strictly adhere to bitrate limits.
   Disabling it is not recommended since this can lead to occasional bitrate overshoot and subsequent packet loss.

   .. note:: This option only applies when using NVENC `encoder`_.

**Choices**

.. table::
   :widths: auto

   =========== ===========
   Value       Description
   =========== ===========
   disabled    One pass (fastest)
   quarter_res Two passes, first pass at quarter resolution (faster)
   full_res    Two passes, first pass at full resolution (slower)
   =========== ===========

**Default**
   ``quarter_res``

**Example**
   .. code-block:: text

      nvenc_twopass = quarter_res

`nvenc_spatial_aq <https://localhost:47990/config/#nvenc_spatial_aq>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Assign higher QP values to flat regions of the video.
   Recommended to enable when streaming at lower bitrates.

   .. Note:: This option only applies when using NVENC `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   disabled   Don't enable Spatial AQ (faster)
   enabled    Enable Spatial AQ (slower)
   ========== ===========

**Default**
   ``disabled``

**Example**
   .. code-block:: text

      nvenc_spatial_aq = disabled

`nvenc_vbv_increase <https://localhost:47990/config/#nvenc_vbv_increase>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Single-frame VBV/HRD percentage increase.
   By default sunshine uses single-frame VBV/HRD, which means any encoded video frame size is not expected to exceed requested bitrate divided by requested frame rate.
   Relaxing this restriction can be beneficial and act as low-latency variable bitrate, but may also lead to packet loss if the network doesn't have buffer headroom to handle bitrate spikes.
   Maximum accepted value is 400, which corresponds to 5x increased encoded video frame upper size limit.

   .. Note:: This option only applies when using NVENC `encoder`_.

   .. Warning:: Can lead to network packet loss.

**Default**
   ``0``

**Range**
   ``0-400``

**Example**
   .. code-block:: text

      nvenc_vbv_increase = 0

`nvenc_realtime_hags <https://localhost:47990/config/#nvenc_realtime_hags>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Use realtime gpu scheduling priority in NVENC when hardware accelerated gpu scheduling (HAGS) is enabled in Windows.
   Currently NVIDIA drivers may freeze in encoder when HAGS is enabled, realtime priority is used and VRAM utilization is close to maximum.
   Disabling this option lowers the priority to high, sidestepping the freeze at the cost of reduced capture performance when the GPU is heavily loaded.

   .. note:: This option only applies when using NVENC `encoder`_.

   .. caution:: Applies to Windows only.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   disabled   Use high priority
   enabled    Use realtime priority
   ========== ===========

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      nvenc_realtime_hags = enabled

`nvenc_latency_over_power <https://localhost:47990/config/#nvenc_latency_over_power>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Adaptive P-State algorithm which NVIDIA drivers employ doesn't work well with low latency streaming, so sunshine requests high power mode explicitly.

   .. Note:: This option only applies when using NVENC `encoder`_.

   .. Warning:: Disabling it is not recommended since this can lead to significantly increased encoding latency.

   .. Caution:: Applies to Windows only.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   disabled   Sunshine doesn't change GPU power preferences (not recommended)
   enabled    Sunshine requests high power mode explicitly
   ========== ===========

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      nvenc_latency_over_power = enabled

`nvenc_opengl_vulkan_on_dxgi <https://localhost:47990/config/#nvenc_opengl_vulkan_on_dxgi>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Sunshine can't capture fullscreen OpenGL and Vulkan programs at full frame rate unless they present on top of DXGI.
   This is system-wide setting that is reverted on sunshine program exit.

   .. Note:: This option only applies when using NVENC `encoder`_.

   .. Caution:: Applies to Windows only.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   disabled   Sunshine leaves global Vulkan/OpenGL present method unchanged
   enabled    Sunshine changes global Vulkan/OpenGL present method to "Prefer layered on DXGI Swapchain"
   ========== ===========

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      nvenc_opengl_vulkan_on_dxgi = enabled

`nvenc_h264_cavlc <https://localhost:47990/config/#nvenc_h264_cavlc>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Prefer CAVLC entropy coding over CABAC in H.264 when using NVENC.
   CAVLC is outdated and needs around 10% more bitrate for same quality, but provides slightly faster decoding when using software decoder.

   .. note:: This option only applies when using H.264 format with NVENC `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   disabled   Prefer CABAC
   enabled    Prefer CAVLC
   ========== ===========

**Default**
   ``disabled``

**Example**
   .. code-block:: text

      nvenc_h264_cavlc = disabled

`Intel QuickSync Encoder <https://localhost:47990/config/#intel-quicksync-encoder>`__
-------------------------------------------------------------------------------------

`qsv_preset <https://localhost:47990/config/#qsv_preset>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The encoder preset to use.

   .. note:: This option only applies when using quicksync `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   veryfast   fastest (lowest quality)
   faster     faster (lower quality)
   fast       fast (low quality)
   medium     medium (default)
   slow       slow (good quality)
   slower     slower (better quality)
   veryslow   slowest (best quality)
   ========== ===========

**Default**
   ``medium``

**Example**
   .. code-block:: text

      qsv_preset = medium

`qsv_coder <https://localhost:47990/config/#qsv_coder>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The entropy encoding to use.

   .. note:: This option only applies when using H264 with quicksync `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   auto       let ffmpeg decide
   cabac      context adaptive binary arithmetic coding - higher quality
   cavlc      context adaptive variable-length coding - faster decode
   ========== ===========

**Default**
   ``auto``

**Example**
   .. code-block:: text

      qsv_coder = auto

`AMD AMF Encoder <https://localhost:47990/config/#amd-amf-encoder>`__
---------------------------------------------------------------------

`amd_quality <https://localhost:47990/config/#amd_quality>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The encoder preset to use.

   .. note:: This option only applies when using amdvce `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   speed      prefer speed
   balanced   balanced
   quality    prefer quality
   ========== ===========

**Default**
   ``balanced``

**Example**
   .. code-block:: text

      amd_quality = balanced

`amd_rc <https://localhost:47990/config/#amd_rc>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The encoder rate control.

   .. note:: This option only applies when using amdvce `encoder`_.

**Choices**

.. table::
   :widths: auto

   =========== ===========
   Value       Description
   =========== ===========
   cqp         constant qp mode
   cbr         constant bitrate
   vbr_latency variable bitrate, latency constrained
   vbr_peak    variable bitrate, peak constrained
   =========== ===========

**Default**
   ``vbr_latency``

**Example**
   .. code-block:: text

      amd_rc = vbr_latency

`amd_usage <https://localhost:47990/config/#amd_usage>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The encoder usage profile, used to balance latency with encoding quality.

   .. note:: This option only applies when using amdvce `encoder`_.

**Choices**

.. table::
   :widths: auto

   =============== ===========
   Value           Description
   =============== ===========
   transcoding     transcoding (slowest)
   webcam          webcam (slow)
   lowlatency      low latency (fast)
   ultralowlatency ultra low latency (fastest)
   =============== ===========

**Default**
   ``ultralowlatency``

**Example**
   .. code-block:: text

      amd_usage = ultralowlatency

`amd_preanalysis <https://localhost:47990/config/#amd_preanalysis>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Preanalysis can increase encoding quality at the cost of latency.

   .. note:: This option only applies when using amdvce `encoder`_.

**Default**
   ``disabled``

**Example**
   .. code-block:: text

      amd_preanalysis = disabled

`amd_vbaq <https://localhost:47990/config/#amd_vbaq>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Variance Based Adaptive Quantization (VBAQ) can increase subjective visual quality.

   .. note:: This option only applies when using amdvce `encoder`_.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      amd_vbaq = enabled

`amd_coder <https://localhost:47990/config/#amd_coder>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The entropy encoding to use.

   .. note:: This option only applies when using H264 with amdvce `encoder`_.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   auto       let ffmpeg decide
   cabac      context adaptive variable-length coding - higher quality
   cavlc      context adaptive binary arithmetic coding - faster decode
   ========== ===========

**Default**
   ``auto``

**Example**
   .. code-block:: text

      amd_coder = auto

`VideoToolbox Encoder <https://localhost:47990/config/#videotoolbox-encoder>`__
-------------------------------------------------------------------------------

`vt_coder <https://localhost:47990/config/#vt_coder>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The entropy encoding to use.

   .. note:: This option only applies when using macOS.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   auto       let ffmpeg decide
   cabac
   cavlc
   ========== ===========

**Default**
   ``auto``

**Example**
   .. code-block:: text

      vt_coder = auto

`vt_software <https://localhost:47990/config/#vt_software>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Force Video Toolbox to use software encoding.

   .. note:: This option only applies when using macOS.

**Choices**

.. table::
   :widths: auto

   ========== ===========
   Value      Description
   ========== ===========
   auto       let ffmpeg decide
   disabled   disable software encoding
   allowed    allow software encoding
   forced     force software encoding
   ========== ===========

**Default**
   ``auto``

**Example**
   .. code-block:: text

      vt_software = auto

`vt_realtime <https://localhost:47990/config/#vt_realtime>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   Realtime encoding.

   .. note:: This option only applies when using macOS.

   .. warning:: Disabling realtime encoding might result in a delayed frame encoding or frame drop.

**Default**
   ``enabled``

**Example**
   .. code-block:: text

      vt_realtime = enabled

`Software Encoder <https://localhost:47990/config/#software-encoder>`__
-----------------------------------------------------------------------

`sw_preset <https://localhost:47990/config/#sw_preset>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The encoder preset to use.

   .. note:: This option only applies when using software `encoder`_.

   .. note:: From `FFmpeg <https://trac.ffmpeg.org/wiki/Encode/H.264#preset>`__.

         A preset is a collection of options that will provide a certain encoding speed to compression ratio. A slower
         preset will provide better compression (compression is quality per filesize). This means that, for example, if
         you target a certain file size or constant bit rate, you will achieve better quality with a slower preset.
         Similarly, for constant quality encoding, you will simply save bitrate by choosing a slower preset.

         Use the slowest preset that you have patience for.

**Choices**

.. table::
   :widths: auto

   ========= ===========
   Value     Description
   ========= ===========
   ultrafast fastest
   superfast
   veryfast
   faster
   fast
   medium
   slow
   slower
   veryslow  slowest
   ========= ===========

**Default**
   ``superfast``

**Example**
   .. code-block:: text

      sw_preset = superfast

`sw_tune <https://localhost:47990/config/#sw_tune>`__
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Description**
   The tuning preset to use.

   .. note:: This option only applies when using software `encoder`_.

   .. note:: From `FFmpeg <https://trac.ffmpeg.org/wiki/Encode/H.264#preset>`__.

         You can optionally use -tune to change settings based upon the specifics of your input.

**Choices**

.. table::
   :widths: auto

   =========== ===========
   Value       Description
   =========== ===========
   film        use for high quality movie content; lowers deblocking
   animation   good for cartoons; uses higher deblocking and more reference frames
   grain       preserves the grain structure in old, grainy film material
   stillimage  good for slideshow-like content
   fastdecode  allows faster decoding by disabling certain filters
   zerolatency good for fast encoding and low-latency streaming
   =========== ===========

**Default**
   ``zerolatency``

**Example**
   .. code-block:: text

      sw_tune = zerolatency
