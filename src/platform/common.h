/**
 * @file src/platform/common.h
 * @brief Declarations for common platform specific utilities.
 */
#pragma once

// standard includes
#include <bitset>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

// lib includes
#include <boost/core/noncopyable.hpp>
#ifndef _WIN32
  #include <boost/asio.hpp>
#endif

// local includes
#ifndef _WIN32
  #include "src/compat/boost_process.h"
#endif
#include "src/config.h"
#include "src/logging.h"
#include "src/thread_safe.h"
#include "src/utility.h"
#include "src/video_colorspace.h"

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

using namespace std::literals;

struct sockaddr;
struct AVFrame;
struct AVBufferRef;
struct AVHWFramesContext;
struct AVCodecContext;
struct AVDictionary;

#ifdef _WIN32
struct ID3D11Device;

// Forward declarations of boost classes to avoid having to include boost headers
// here, which results in issues with Windows.h and WinSock2.h include order.
namespace boost {
  namespace asio {
    namespace ip {
      class address;
    }  // namespace ip
  }  // namespace asio

  namespace filesystem {
    class path;
  }

  namespace process::v1 {
    class child;
    class group;
    template<typename Char>
    class basic_environment;
    /**
     * @brief Map of environment variable names to values.
     */
    using environment = basic_environment<char>;
  }  // namespace process::v1
}  // namespace boost
#endif
namespace video {
  struct config_t;
}  // namespace video

namespace nvenc {
  class nvenc_encoder;
}

namespace platf {
  // Limited by bits in activeGamepadMask
  constexpr auto MAX_GAMEPADS = 16;  ///< Maximum number of simultaneously tracked gamepads.

  constexpr std::uint32_t DPAD_UP = 0x0001;  ///< Moonlight gamepad button mask bit for D-pad up.
  constexpr std::uint32_t DPAD_DOWN = 0x0002;  ///< Moonlight gamepad button mask bit for D-pad down.
  constexpr std::uint32_t DPAD_LEFT = 0x0004;  ///< Moonlight gamepad button mask bit for D-pad left.
  constexpr std::uint32_t DPAD_RIGHT = 0x0008;  ///< Moonlight gamepad button mask bit for D-pad right.
  constexpr std::uint32_t START = 0x0010;  ///< Moonlight gamepad button mask bit for Start.
  constexpr std::uint32_t BACK = 0x0020;  ///< Moonlight gamepad button mask bit for Back.
  constexpr std::uint32_t LEFT_STICK = 0x0040;  ///< Moonlight gamepad button mask bit for left stick press.
  constexpr std::uint32_t RIGHT_STICK = 0x0080;  ///< Moonlight gamepad button mask bit for right stick press.
  constexpr std::uint32_t LEFT_BUTTON = 0x0100;  ///< Moonlight gamepad button mask bit for left shoulder.
  constexpr std::uint32_t RIGHT_BUTTON = 0x0200;  ///< Moonlight gamepad button mask bit for right shoulder.
  constexpr std::uint32_t HOME = 0x0400;  ///< Moonlight gamepad button mask bit for Home.
  constexpr std::uint32_t A = 0x1000;  ///< Moonlight gamepad button mask bit for A.
  constexpr std::uint32_t B = 0x2000;  ///< Moonlight gamepad button mask bit for B.
  constexpr std::uint32_t X = 0x4000;  ///< Moonlight gamepad button mask bit for X.
  constexpr std::uint32_t Y = 0x8000;  ///< Moonlight gamepad button mask bit for Y.
  constexpr std::uint32_t PADDLE1 = 0x010000;  ///< Moonlight gamepad button mask bit for paddle 1.
  constexpr std::uint32_t PADDLE2 = 0x020000;  ///< Moonlight gamepad button mask bit for paddle 2.
  constexpr std::uint32_t PADDLE3 = 0x040000;  ///< Moonlight gamepad button mask bit for paddle 3.
  constexpr std::uint32_t PADDLE4 = 0x080000;  ///< Moonlight gamepad button mask bit for paddle 4.
  constexpr std::uint32_t TOUCHPAD_BUTTON = 0x100000;  ///< Moonlight gamepad button mask bit for touchpad click.
  constexpr std::uint32_t MISC_BUTTON = 0x200000;  ///< Moonlight gamepad button mask bit for the miscellaneous button.

  /**
   * @brief Gamepad type exposed to clients and why it may be disabled.
   */
  struct supported_gamepad_t {
    std::string name;  ///< Human-readable name for this item.
    bool is_enabled;  ///< Whether this gamepad type is currently available.
    std::string reason_disabled;  ///< Human-readable reason the gamepad type is disabled.
  };

  /**
   * @brief Enumerates supported gamepad feedback options.
   */
  enum class gamepad_feedback_e {
    rumble,  ///< Rumble
    rumble_triggers,  ///< Rumble triggers
    set_motion_event_state,  ///< Set motion event state
    set_rgb_led,  ///< Set RGB LED
    set_player_leds,  ///< Set player indicator LEDs
    set_adaptive_triggers,  ///< Set adaptive triggers
  };

  /**
   * @brief Feedback command sent from Sunshine to a virtual gamepad.
   */
  struct gamepad_feedback_msg_t {
    /**
     * @brief Create a rumble object or message.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @param lowfreq Low-frequency rumble motor intensity.
     * @param highfreq High-frequency rumble motor intensity.
     * @return Constructed rumble object.
     */
    static gamepad_feedback_msg_t make_rumble(std::uint16_t id, std::uint16_t lowfreq, std::uint16_t highfreq) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble;
      msg.id = id;
      msg.data.rumble = {lowfreq, highfreq};
      return msg;
    }

    /**
     * @brief Create rumble triggers.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @param left Left trigger or motor payload for the feedback command.
     * @param right Right trigger or motor payload for the feedback command.
     * @return Constructed rumble triggers object.
     */
    static gamepad_feedback_msg_t make_rumble_triggers(std::uint16_t id, std::uint16_t left, std::uint16_t right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble_triggers;
      msg.id = id;
      msg.data.rumble_triggers = {left, right};
      return msg;
    }

    /**
     * @brief Motion-event feedback command payload for a controller.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @param motion_type Motion type.
     * @param report_rate Report rate.
     * @return Constructed motion event state object.
     */
    static gamepad_feedback_msg_t make_motion_event_state(std::uint16_t id, std::uint8_t motion_type, std::uint16_t report_rate) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_motion_event_state;
      msg.id = id;
      msg.data.motion_event_state.motion_type = motion_type;
      msg.data.motion_event_state.report_rate = report_rate;
      return msg;
    }

    /**
     * @brief Create RGB led.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @param r Red color channel value.
     * @param g Green color channel value.
     * @param b Blue color channel value.
     * @return Constructed RGB led object.
     */
    static gamepad_feedback_msg_t make_rgb_led(std::uint16_t id, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_rgb_led;
      msg.id = id;
      msg.data.rgb_led = {r, g, b};
      return msg;
    }

    /**
     * @brief Create player indicator LED state.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @param solid Four-bit mask of solid player indicators.
     * @param flashing Four-bit mask of flashing player indicators.
     * @return Constructed player indicator LED object.
     */
    static gamepad_feedback_msg_t make_player_leds(std::uint16_t id, std::uint8_t solid, std::uint8_t flashing) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_player_leds;
      msg.id = id;
      msg.data.player_leds = {solid, flashing};
      return msg;
    }

    /**
     * @brief Create adaptive triggers.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @param event_flags Event flags.
     * @param type_left Type left.
     * @param type_right Type right.
     * @param left Left trigger or motor payload for the feedback command.
     * @param right Right trigger or motor payload for the feedback command.
     * @return Constructed adaptive triggers object.
     */
    static gamepad_feedback_msg_t make_adaptive_triggers(std::uint16_t id, uint8_t event_flags, uint8_t type_left, uint8_t type_right, const std::array<uint8_t, 10> &left, const std::array<uint8_t, 10> &right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_adaptive_triggers;
      msg.id = id;
      msg.data.adaptive_triggers = {.event_flags = event_flags, .type_left = type_left, .type_right = type_right, .left = left, .right = right};
      return msg;
    }

    gamepad_feedback_e type;  ///< Feedback command type stored in the union payload.
    std::uint16_t id;  ///< Controller identifier associated with this message.

    union {
      struct {
        std::uint16_t lowfreq;  ///< Low-frequency rumble motor intensity.
        std::uint16_t highfreq;  ///< High-frequency rumble motor intensity.
      } rumble;  ///< Main rumble-motor payload.

      struct {
        std::uint16_t left_trigger;  ///< Left-trigger rumble motor intensity.
        std::uint16_t right_trigger;  ///< Right-trigger rumble motor intensity.
      } rumble_triggers;  ///< Trigger-rumble payload.

      struct {
        std::uint16_t report_rate;  ///< Requested motion-sensor report rate.
        std::uint8_t motion_type;  ///< Motion-sensor type to configure.
      } motion_event_state;  ///< Motion-event configuration payload.

      struct {
        std::uint8_t r;  ///< Red LED channel intensity.
        std::uint8_t g;  ///< Green LED channel intensity.
        std::uint8_t b;  ///< Blue LED channel intensity.
      } rgb_led;  ///< RGB LED payload.

      struct {
        std::uint8_t solid;  ///< Bit mask of solid player indicators.
        std::uint8_t flashing;  ///< Bit mask of flashing player indicators.
      } player_leds;  ///< Player-indicator LED payload.

      struct {
        uint8_t event_flags;  ///< Flags describing which adaptive-trigger data is present.
        uint8_t type_left;  ///< Left adaptive-trigger effect type.
        uint8_t type_right;  ///< Right adaptive-trigger effect type.
        std::array<uint8_t, 10> left;  ///< Left adaptive-trigger effect parameters.
        std::array<uint8_t, 10> right;  ///< Right adaptive-trigger effect parameters.
      } adaptive_triggers;  ///< Adaptive-trigger effect payload.
    } data;  ///< Controller feedback payload for the selected feedback type.
  };

  /**
   * @brief Queue used to deliver controller feedback commands to the platform backend.
   */
  using feedback_queue_t = safe::mail_raw_t::queue_t<gamepad_feedback_msg_t>;

  namespace speaker {
    /**
     * @brief Enumerates supported speaker options.
     */
    enum speaker_e {
      FRONT_LEFT,  ///< Front left
      FRONT_RIGHT,  ///< Front right
      FRONT_CENTER,  ///< Front center
      LOW_FREQUENCY,  ///< Low frequency
      BACK_LEFT,  ///< Back left
      BACK_RIGHT,  ///< Back right
      SIDE_LEFT,  ///< Side left
      SIDE_RIGHT,  ///< Side right
      MAX_SPEAKERS,  ///< Maximum number of speakers
    };

    /**
     * @brief Moonlight speaker order for stereo audio.
     */
    constexpr std::array<std::uint8_t, 2> map_stereo {
      FRONT_LEFT,
      FRONT_RIGHT
    };
    /**
     * @brief Moonlight speaker order for 5.1 surround audio.
     */
    constexpr std::array<std::uint8_t, 6> map_surround51 {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
    };
    /**
     * @brief Moonlight speaker order for 7.1 surround audio.
     */
    constexpr std::array<std::uint8_t, 8> map_surround71 {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
      SIDE_LEFT,
      SIDE_RIGHT,
    };
  }  // namespace speaker

  /**
   * @brief Enumerates supported mem type options.
   */
  enum class mem_type_e {
    system,  ///< System memory
    vaapi,  ///< VAAPI
    dxgi,  ///< DXGI
    cuda,  ///< CUDA
    videotoolbox,  ///< VideoToolbox
    vulkan,  ///< Vulkan
    unknown  ///< Unknown
  };

  /**
   * @brief Enumerates supported pix fmt options.
   */
  enum class pix_fmt_e {
    yuv420p,  ///< YUV 4:2:0
    yuv420p10,  ///< YUV 4:2:0 10-bit
    nv12,  ///< NV12
    p010,  ///< P010
    ayuv,  ///< AYUV
    yuv444p16,  ///< Planar 10-bit (shifted to 16-bit) YUV 4:4:4
    yuv444p,  ///< Planar 8-bit YUV 4:4:4
    y410,  ///< Y410
    unknown  ///< Unknown
  };

  /**
   * @brief Convert a Sunshine pixel format enum to its string name.
   *
   * @param pix_fmt Sunshine pixel format to convert or allocate for.
   * @return Value converted from pix fmt.
   */
  inline std::string_view from_pix_fmt(pix_fmt_e pix_fmt) {
    using namespace std::literals;
#ifndef DOXYGEN
  #define _CONVERT(x) \
    case pix_fmt_e::x: \
      return #x##sv
#endif
    switch (pix_fmt) {
      _CONVERT(yuv420p);
      _CONVERT(yuv420p10);
      _CONVERT(nv12);
      _CONVERT(p010);
      _CONVERT(ayuv);
      _CONVERT(yuv444p16);
      _CONVERT(yuv444p);
      _CONVERT(y410);
      _CONVERT(unknown);
    }
#ifndef DOXYGEN
  #undef _CONVERT
#endif

    return "unknown"sv;
  }

  // Dimensions for touchscreen input
  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t {
    int offset_x;  ///< Horizontal offset in physical pixels.
    int offset_y;  ///< Vertical offset in physical pixels.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int logical_width;  ///< Logical width after display scaling.
    int logical_height;  ///< Logical height after display scaling.
    int env_offset_x {};  ///< Horizontal origin of the full input environment in native desktop coordinates.
    int env_offset_y {};  ///< Vertical origin of the full input environment in native desktop coordinates.
    std::string output_name {};  ///< Native connector name used to assign compositor-managed touch and pen devices.
  };

  // These values must match Limelight-internal.h's SS_FF_* constants!
  namespace platform_caps {
    /**
     * @brief Bitset containing platform capability flags.
     */
    using caps_t = uint32_t;

    /**
     * @brief Capability bit indicating native pen and touch support.
     */
    constexpr caps_t pen_touch = 0x01;  // Pen and touch events
    /**
     * @brief Capability bit indicating controller touchpad and motion support.
     */
    constexpr caps_t controller_touch = 0x02;  // Controller touch and motion events
  };  // namespace platform_caps

  /**
   * @brief Button and axis state received for a virtual gamepad.
   */
  struct gamepad_state_t {
    std::uint32_t buttonFlags;  ///< Moonlight button mask for the current gamepad state.
    std::uint8_t lt;  ///< Left trigger value.
    std::uint8_t rt;  ///< Right trigger value.
    std::int16_t lsX;  ///< Left stick X-axis value.
    std::int16_t lsY;  ///< Left stick Y-axis value.
    std::int16_t rsX;  ///< Right stick X-axis value.
    std::int16_t rsY;  ///< Right stick Y-axis value.
  };

  /**
   * @brief Global and client-relative identifiers for a virtual gamepad.
   */
  struct gamepad_id_t {
    // The global index is used when looking up gamepads in the platform's
    // gamepad array. It identifies gamepads uniquely among all clients.
    int globalIndex;  ///< Index unique across all connected clients.

    // The client-relative index is the controller number as reported by the
    // client. It must be used when communicating back to the client via
    // the input feedback queue.
    std::uint8_t clientRelativeIndex;  ///< Client relative index.
  };

  /**
   * @brief Capabilities reported when a controller is connected.
   */
  struct gamepad_arrival_t {
    std::uint8_t type;  ///< Protocol or controller type discriminator.
    std::uint16_t capabilities;  ///< Capability flags advertised by the controller.
    std::uint32_t supportedButtons;  ///< Button mask supported by the connected controller.
  };

  /**
   * @brief Touchpad contact data reported by a controller.
   */
  struct gamepad_touch_t {
    gamepad_id_t id;  ///< Gamepad identifier for the event.
    std::uint8_t eventType;  ///< Moonlight event type for the input packet.
    std::uint32_t pointerId;  ///< Client-provided pointer identifier for a touch contact.
    float x;  ///< Horizontal coordinate or vector component.
    float y;  ///< Vertical coordinate or vector component.
    float pressure;  ///< Contact pressure reported by the client.
  };

  /**
   * @brief Accelerometer or gyroscope sample from a controller.
   */
  struct gamepad_motion_t {
    gamepad_id_t id;  ///< Gamepad identifier for the event.
    std::uint8_t motionType;  ///< Motion type.

    // Accel: m/s^2
    // Gyro: deg/s
    float x;  ///< Horizontal coordinate or vector component.
    float y;  ///< Vertical coordinate or vector component.
    float z;  ///< Depth or Z-axis vector component.
  };

  /**
   * @brief Battery state reported by a virtual gamepad.
   */
  struct gamepad_battery_t {
    gamepad_id_t id;  ///< Gamepad identifier for the event.
    std::uint8_t state;  ///< Battery state reported by the client.
    std::uint8_t percentage;  ///< Battery charge percentage.
  };

  /**
   * @brief Absolute touchscreen event data from the client.
   */
  struct touch_input_t {
    std::uint8_t eventType;  ///< Moonlight event type for the input packet.
    std::uint16_t rotation;  ///< Degrees (0..360) or LI_ROT_UNKNOWN.
    std::uint32_t pointerId;  ///< Client-provided pointer identifier for a touch contact.
    float x;  ///< Horizontal coordinate or vector component.
    float y;  ///< Vertical coordinate or vector component.
    float pressureOrDistance;  ///< Distance for hover and pressure for contact.
    float contactAreaMajor;  ///< Major axis of the reported contact area.
    float contactAreaMinor;  ///< Minor axis of the reported contact area.
  };

  /**
   * @brief Pen tablet event data from the client.
   */
  struct pen_input_t {
    std::uint8_t eventType;  ///< Moonlight event type for the input packet.
    std::uint8_t toolType;  ///< Pen tool type reported by the client.
    std::uint8_t penButtons;  ///< Button mask for the active pen tool.
    std::uint8_t tilt;  ///< Degrees (0..90) or LI_TILT_UNKNOWN.
    std::uint16_t rotation;  ///< Degrees (0..360) or LI_ROT_UNKNOWN.
    float x;  ///< Horizontal coordinate or vector component.
    float y;  ///< Vertical coordinate or vector component.
    float pressureOrDistance;  ///< Distance for hover and pressure for contact.
    float contactAreaMajor;  ///< Major axis of the reported contact area.
    float contactAreaMinor;  ///< Minor axis of the reported contact area.
  };

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t {
  public:
    /**
     * @brief Destroy the deinitializer.
     */
    virtual ~deinit_t() = default;
  };

  /**
   * @brief Captured frame buffer shared between capture and encode stages.
   */
  struct img_t: std::enable_shared_from_this<img_t> {
  public:
    img_t() = default;

    img_t(img_t &&) = delete;
    img_t(const img_t &) = delete;
    img_t &operator=(img_t &&) = delete;
    img_t &operator=(const img_t &) = delete;

    std::uint8_t *data {};  ///< Pointer to the captured image buffer.
    std::int32_t width {};  ///< Image width in pixels.
    std::int32_t height {};  ///< Image height in pixels.
    std::int32_t pixel_pitch {};  ///< Bytes per pixel in the image buffer.
    std::int32_t row_pitch {};  ///< Bytes between consecutive image rows.

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< Capture timestamp associated with the frame.

    /**
     * @brief Destroy the image.
     */
    virtual ~img_t() = default;
  };

  /**
   * @brief Host and virtual audio sink names for audio routing.
   */
  struct sink_t {
    // Play on host PC
    std::string host;  ///< Host playback sink name.

    // On macOS and Windows, it is not possible to create a virtual sink
    // Therefore, it is optional
    /**
     * @brief Optional virtual sink names for each supported channel layout.
     */
    struct null_t {
      std::string stereo;  ///< Virtual sink name for stereo audio.
      std::string surround51;  ///< Virtual sink name for 5.1 surround audio.
      std::string surround71;  ///< Virtual sink name for 7.1 surround audio.
    };

    std::optional<null_t> null;  ///< Optional virtual sink names for monitor capture.
  };

  /**
   * @brief Base interface for hardware or software frame conversion.
   */
  struct encode_device_t {
    virtual ~encode_device_t() = default;

    /**
     * @brief Convert a captured image into the encoder input representation.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    virtual int convert(platf::img_t &img) = 0;

    video::sunshine_colorspace_t colorspace;  ///< Colorspace metadata expected by the encoder.
  };

  /**
   * @brief AVCodec-backed encode device and frame state.
   */
  struct avcodec_encode_device_t: encode_device_t {
    bool hardware = false;  ///< Whether this device supplies hardware frames instead of software conversion.
    /**
     * @brief Callback signature for backend-specific FFmpeg hardware device initialization.
     */
    using init_hw_device_fn_t = int (*)(avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);
    init_hw_device_fn_t init_hw_device = nullptr;  ///< Optional backend-specific hardware device initializer.
#ifdef _WIN32
    ID3D11Device *d3d11_device = nullptr;  ///< Native D3D11 device borrowed while the encode device is alive.
#endif
    AVFrame *frame {};  ///< FFmpeg frame currently owned by the encode device.

    /**
     * @brief Convert a captured image into the encoder input representation.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      return -1;
    }

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     */
    virtual void apply_colorspace() {
      // Backends without additional colorspace state need no update here.
    }

    /**
     * @brief Set the frame to be encoded.
     * @note Implementations must take ownership of 'new_frame'.
     *
     * @param new_frame Video or graphics frame being processed.
     * @param hw_frames_ctx FFmpeg hardware frames context associated with the frame.
     * @return Status from updating frame.
     */
    virtual int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) {
      BOOST_LOG(error) << "Illegal call to hwdevice_t::set_frame(). Did you forget to override it?";
      return -1;
    };

    /**
     * @brief Initialize the hwframes context.
     * @note Implementations may set parameters during initialization of the hwframes context.
     *
     * @param frames FFmpeg hardware frames context to initialize.
     */
    virtual void init_hwframes(AVHWFramesContext *frames) {
      // The base implementation leaves FFmpeg's hardware-frame parameters unchanged.
    };

    /**
     * @brief Allow platform-specific code to adjust codec options.
     * @note Implementations may set or modify codec options prior to codec initialization.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param options Request options or socket options to apply.
     */
    virtual void init_codec_options(AVCodecContext *ctx, AVDictionary **options) {
      // The base implementation requires no platform-specific codec options.
    };

    /**
     * @brief Prepare to derive a context.
     * @note Implementations may make modifications required before context derivation
     *
     * @param hw_device_type FFmpeg hardware device type requested for context derivation.
     * @return 0 when context derivation may continue; nonzero to abort.
     */
    virtual int prepare_to_derive_context(int hw_device_type) {
      return 0;
    };
  };

  /**
   * @brief NVENC-backed encode device state.
   */
  struct nvenc_encode_device_t: encode_device_t {
    /**
     * @brief Initialize the platform encoder for the client stream configuration.
     *
     * @param client_config Client stream configuration negotiated for this session.
     * @param colorspace Colorimetry information used for conversion or encoding.
     * @return True when the backend successfully completes the requested action.
     */
    virtual bool init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) = 0;

    nvenc::nvenc_encoder *nvenc = nullptr;  ///< NVENC encoder instance owned by the encode device.
  };

  /**
   * @brief A single encoded frame produced by a native VideoToolbox encoder.
   */
  struct vt_encoded_frame {
    std::vector<uint8_t> data;  ///< Annex-B elementary stream for exactly one frame.
    uint64_t frame_index = 0;  ///< Frame index the data corresponds to.
    bool idr = false;  ///< Whether the frame is an IDR frame.
  };

  /**
   * @brief Native VideoToolbox-backed encode device state.
   */
  struct vt_encode_device_t: encode_device_t {
    /**
     * @brief Initialize the platform encoder for the client stream configuration.
     *
     * @param client_config Client stream configuration negotiated for this session.
     * @param colorspace Colorimetry information used for conversion or encoding.
     * @return True when the backend successfully completes the requested action.
     */
    virtual bool init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) = 0;

    /**
     * @brief Encode the frame most recently supplied via convert().
     *
     * @param frame_index Frame index to associate with the encoded output.
     * @param force_idr Whether the frame must be encoded as an IDR frame.
     * @return Encoded frame, or one with empty data on failure.
     */
    virtual vt_encoded_frame encode_frame(uint64_t frame_index, bool force_idr) = 0;
  };

  /**
   * @brief Enumerates supported capture options.
   */
  enum class capture_e : int {
    ok,  ///< Success
    reinit,  ///< Need to reinitialize
    timeout,  ///< Timeout
    interrupted,  ///< Capture was interrupted
    capture_error  ///< Capture failed.
  };

  /**
   * @brief Abstract display capture backend used by the streaming pipeline.
   */
  class display_t {
  public:
    /**
     * @brief Callback for when a new image is ready.
     * When display has a new image ready or a timeout occurs, this callback will be called with the image.
     * If a frame was captured, frame_captured will be true. If a timeout occurred, it will be false.
     * @retval true On success
     * @retval false On break request
     */
    using push_captured_image_cb_t = std::function<bool(std::shared_ptr<img_t> &&img, bool frame_captured)>;

    /**
     * @brief Get free image from pool.
     * Calls must be synchronized.
     * Blocks until there is free image in the pool or capture is interrupted.
     * @retval true On success, img_out contains free image
     * @retval false When capture has been interrupted, img_out contains nullptr
     */
    using pull_free_image_cb_t = std::function<bool(std::shared_ptr<img_t> &img_out)>;

    display_t() noexcept = default;

    /**
     * @brief Capture a frame.
     * @param push_captured_image_cb The callback that is called with captured image,
     * must be called from the same thread as capture()
     * @param pull_free_image_cb Capture backends call this callback to get empty image from the pool.
     * If backend uses multiple threads, calls to this callback must be synchronized.
     * Calls to this callback and push_captured_image_cb must be synchronized as well.
     * @param cursor A pointer to the flag that indicates whether the cursor should be captured as well.
     * @retval capture_e::ok When stopping
     * @retval capture_e::capture_error On error
     * @retval capture_e::reinit When need of reinitialization
     */
    virtual capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) = 0;

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    virtual std::shared_ptr<img_t> alloc_img() = 0;

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    virtual int dummy_img(img_t *img) = 0;

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    virtual std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    /**
     * @brief Create NVENC encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed NVENC encode device object.
     */
    virtual std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    /**
     * @brief Create a native VideoToolbox encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed VideoToolbox encode device object.
     */
    virtual std::unique_ptr<vt_encode_device_t> make_vt_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    /**
     * @brief Report whether the active display mode is HDR.
     *
     * @return True when the active display mode is HDR.
     */
    virtual bool is_hdr() {
      return false;
    }

    /**
     * @brief Read HDR metadata for the active display mode.
     *
     * @param metadata Output structure populated with HDR metadata.
     * @return True when HDR metadata was written to `metadata`.
     */
    virtual bool get_hdr_metadata(SS_HDR_METADATA &metadata) {
      std::memset(&metadata, 0, sizeof(metadata));
      return false;
    }

    /**
     * @brief Check that a given codec is supported by the display device.
     * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
     * @param config The codec configuration.
     * @return `true` if supported, `false` otherwise.
     */
    virtual bool is_codec_supported(std::string_view name, const ::video::config_t &config) {
      return true;
    }

    virtual ~display_t() = default;

    // Offsets for when streaming a specific monitor. By default, they are 0.
    std::string output_name;  ///< Native connector name of the captured display for absolute input assignment.
    int offset_x {0};  ///< Horizontal capture offset in physical pixels.
    int offset_y {0};  ///< Vertical capture offset in physical pixels.
    int env_offset_x {0};  ///< Horizontal origin of the full capture environment in native desktop coordinates.
    int env_offset_y {0};  ///< Vertical origin of the full capture environment in native desktop coordinates.
    int env_width {0};  ///< Width of the full capture environment in physical pixels.
    int env_height {0};  ///< Height of the full capture environment in physical pixels.
    int env_logical_width {0};  ///< Width of the full capture environment after display scaling.
    int env_logical_height {0};  ///< Height of the full capture environment after display scaling.
    int width {0};  ///< Width of the captured display in physical pixels.
    int height {0};  ///< Height of the captured display in physical pixels.
    int logical_width {0};  ///< Width of the captured display after display scaling.
    int logical_height {0};  ///< Height of the captured display after display scaling.

  protected:
    // collect capture timing data (at loglevel debug)
    logging::time_delta_periodic_logger sleep_overshoot_logger = {debug, "Frame capture sleep overshoot"};  ///< Periodic logger for capture sleep overshoot measurements.
  };

  /**
   * @brief Audio capture source used by the streaming pipeline.
   */
  class mic_t {
  public:
    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param frame_buffer Destination for captured floating-point PCM samples.
     * @return Capture status reported to the streaming pipeline.
     */
    virtual capture_e sample(std::vector<float> &frame_buffer) = 0;

    virtual ~mic_t() = default;
  };

  /**
   * @brief Platform audio controller that manages sinks and microphone capture.
   */
  class audio_control_t {
  public:
    /**
     * @brief Update the sink value on the backend.
     *
     * @param sink Audio sink name to route or capture.
     * @return Status from updating sink.
     */
    virtual int set_sink(const std::string &sink) = 0;

    /**
     * @brief Create a microphone capture stream for the requested layout.
     *
     * @param mapping Opus channel mapping table for the requested layout.
     * @param channels Number of audio channels in the stream.
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param continuous Whether silent audio should continue to be emitted.
     * @param host_audio_enabled Whether host playback should remain enabled during capture.
     * @return Microphone capture object for the requested audio layout.
     */
    virtual std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, [[maybe_unused]] bool host_audio_enabled) = 0;

    /**
     * @brief Check if the audio sink is available in the system.
     * @param sink Sink to be checked.
     * @returns True if available, false otherwise.
     */
    virtual bool is_sink_available(const std::string &sink) = 0;

    /**
     * @brief Query host and virtual sink names available to Sunshine.
     *
     * @return Host and virtual sink names when the backend can report them.
     */
    virtual std::optional<sink_t> sink_info() = 0;

    /**
     * @brief Destroy the audio control.
     */
    virtual ~audio_control_t() = default;
  };

  /**
   * @brief Platform-specific input backend context.
   */
  struct input_raw_t;

  /**
   * @brief Release a platform input backend created by input().
   *
   * @param input Platform input backend to release.
   */
  void freeInput(input_raw_t *input);

  /**
   * @brief Owning pointer for a platform input backend.
   */
  using input_t = util::safe_ptr<input_raw_t, &freeInput>;

  std::filesystem::path appdata();

  std::filesystem::path get_xdg_restore_token_path();  ///< XDG restore token path.

  /**
   * @brief Return the hardware MAC address associated with a network address.
   *
   * @param address Network address being parsed or filtered.
   * @return Hardware MAC address string, or an empty string when it cannot be resolved.
   */
  std::string get_mac_address(const std::string_view &address);

  /**
   * @brief Convert a socket address to a printable IP address.
   *
   * @param ip_addr Socket address to format.
   * @return Printable address, or an empty string for an unsupported address family.
   */
  std::string from_sockaddr(const sockaddr *const);
  /**
   * @brief Convert a socket address to a port and printable IP address.
   *
   * @param ip_addr Socket address to format.
   * @return Port in network byte order and printable address, or zero and an empty string for an unsupported family.
   */
  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const);

  /**
   * @brief Create the platform audio controller.
   *
   * @return Platform audio controller, or nullptr when audio control is unavailable.
   */
  std::unique_ptr<audio_control_t> audio_control();

  /**
   * @brief Get the display_t instance for the given hwdevice_type.
   * If display_name is empty, use the first monitor that's compatible you can find
   * If you require to use this parameter in a separate thread, make a copy of it.
   * @param display_name The name of the monitor that SHOULD be displayed
   * @param config Stream configuration
   * @return The display_t instance based on hwdevice_type.
   */
  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  // A list of names of displays accepted as display_name with the mem_type_e
  /**
   * @brief List display names accepted by the selected capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @return Display names accepted by the selected capture backend.
   */
  std::vector<std::string> display_names(mem_type_e hwdevice_type);

  /**
   * @brief Check if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool needs_encoder_reenumeration();

  /**
   * @brief Launch a configured preparation or application command.
   *
   * @param elevated Whether the command should run with elevated privileges.
   * @param interactive Whether the command should run in an interactive session.
   * @param cmd Command line to execute or inspect.
   * @param working_dir Working directory for the child process.
   * @param env Environment variables for the child process.
   * @param file Optional stdio file handle connected to the child process.
   * @param ec Error code returned by the asynchronous operation.
   * @param group Process group used when launching the command.
   * @return Child process handle for the launched command.
   */
  boost::process::v1::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const boost::process::v1::environment &env, FILE *file, std::error_code &ec, boost::process::v1::group *group);

  /**
   * @brief Enumerates supported thread priority options.
   */
  enum class thread_priority_e : int {
    low,  ///< Low priority
    normal,  ///< Normal priority
    high,  ///< High priority
    critical  ///< Critical priority
  };
  /**
   * @brief Apply the requested scheduling priority to the current thread.
   *
   * @param priority Thread priority requested from the platform backend.
   */
  void adjust_thread_priority(thread_priority_e priority);

  /**
   * @brief Name the current thread for use with development tools.
   * @note On Linux this will be truncated after 15 characters.
   *
   * @param name Human-readable name to assign.
   */
  void set_thread_name(std::string_view name);

  void enable_mouse_keys();

  // Allow OS-specific actions to be taken to prepare for streaming
  void streaming_will_start();
  void streaming_will_stop();

  void restart();

  /**
   * @brief Platform buffer pointer and size for batched socket sends.
   */
  struct buffer_descriptor_t {
    const char *buffer;  ///< Pointer to the payload buffer.
    size_t size;  ///< Size of the buffer in bytes.

    // Constructors required for emplace_back() prior to C++20
    /**
     * @brief Describe a contiguous byte buffer for batched socket sending.
     *
     * @param buffer Serialized byte buffer to read from or write to.
     * @param size Number of bytes or elements requested.
     */
    buffer_descriptor_t(const char *buffer, size_t size):
        buffer(buffer),
        size(size) {
    }

    buffer_descriptor_t():
        buffer(nullptr),
        size(0) {
    }
  };

  /**
   * @brief Buffers and native metadata for one batched send operation.
   */
  struct batched_send_info_t {
    // Optional headers to be prepended to each packet
    const char *headers;  ///< Optional header bytes prepended to each payload.
    size_t header_size;  ///< Header size in bytes.

    // One or more data buffers to use for the payloads
    //
    // NB: Data buffers must be aligned to payload size!
    std::vector<buffer_descriptor_t> &payload_buffers;  ///< Payload buffers containing one or more packet blocks.
    size_t payload_size;  ///< Payload size in bytes for each packet block.

    // The offset (in header+payload message blocks) in the header and payload
    // buffers to begin sending messages from
    size_t block_offset;  ///< First packet block index to send.

    // The number of header+payload message blocks to send
    size_t block_count;  ///< Number of packet blocks to send.

    std::uintptr_t native_socket;  ///< Platform socket handle used for the send operation.
    boost::asio::ip::address &target_address;  ///< Destination IP address for outgoing packets.
    uint16_t target_port;  ///< Destination UDP port for outgoing packets.
    boost::asio::ip::address &source_address;  ///< Local source IP address for outgoing packets.

    /**
     * @brief Locate a payload region without changing the batch descriptor or its buffers.
     * @param offset The offset in the total payload data (bytes).
     * @return Buffer descriptor describing the region at the given offset.
     */
    buffer_descriptor_t buffer_for_payload_offset(ptrdiff_t offset) const {
      for (const auto &desc : payload_buffers) {
        if (offset < desc.size) {
          return {
            desc.buffer + offset,
            desc.size - offset,
          };
        } else {
          offset -= desc.size;
        }
      }
      return {};
    }
  };

  /**
   * @brief Send fixed-size UDP payload blocks without modifying the batch descriptor.
   *
   * @param send_info Socket addresses, buffers, and sizes for the send operation.
   * @return True when all requested packet blocks are submitted to the socket.
   */
  bool send_batch(const batched_send_info_t &send_info);

  /**
   * @brief Destination address and payload data for one UDP send.
   */
  struct send_info_t {
    const char *header;  ///< Optional header bytes prepended to the payload.
    size_t header_size;  ///< Header size in bytes.
    const char *payload;  ///< Payload bytes to send after the header.
    size_t payload_size;  ///< Payload size in bytes for each packet block.

    std::uintptr_t native_socket;  ///< Platform socket handle used for the send operation.
    boost::asio::ip::address &target_address;  ///< Destination IP address for outgoing packets.
    uint16_t target_port;  ///< Destination UDP port for outgoing packets.
    boost::asio::ip::address &source_address;  ///< Local source IP address for outgoing packets.
  };

  /**
   * @brief Send the serialized response over the active socket.
   *
   * @param send_info Socket addresses, buffers, and sizes for the send operation.
   * @return True when the packet is submitted to the socket.
   */
  bool send(send_info_t &send_info);

  /**
   * @brief Identifies traffic classes used for socket QoS tagging.
   */
  enum class qos_data_type_e : int {
    audio,  ///< Audio
    video  ///< Video
  };

  /**
   * @brief Enable QoS on the given socket for traffic to the specified destination.
   * @param native_socket The native socket handle.
   * @param address The destination address for traffic sent on this socket.
   * @param port The destination port for traffic sent on this socket.
   * @param data_type The type of traffic sent on this socket.
   * @param dscp_tagging Specifies whether to enable DSCP tagging on outgoing traffic.
   *
   * @return Cleanup handle that restores or releases QoS state when destroyed.
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging);

  /**
   * @brief Open a url in the default web browser.
   * @param url The url to open.
   */
  void open_url(const std::string &url);

  /**
   * @brief Attempt to gracefully terminate a process group.
   * @param native_handle The native handle of the process group.
   * @return `true` if termination was successfully requested.
   */
  bool request_process_group_exit(std::uintptr_t native_handle);

  /**
   * @brief Check if a process group still has running children.
   * @param native_handle The native handle of the process group.
   * @return `true` if processes are still running.
   */
  bool process_group_running(std::uintptr_t native_handle);

  /**
   * @brief Create the platform input backend for a stream.
   *
   * @return Platform-specific input backend for the active stream.
   */
  input_t input();
  /**
   * @brief Get the current mouse position for platform input tests.
   *
   * @param input The input_t instance to use.
   * @return Screen coordinates of the mouse, or `std::nullopt` when the platform cannot observe the cursor.
   *
   * @note This helper exists only so tests can observe virtual mouse movement. Production input paths should submit
   * mouse events through `move_mouse()` or `abs_mouse()` instead of reading the host cursor location.
   *
   * @examples
   * if (auto location = get_mouse_loc(input)) {
   *   auto [x, y] = *location;
   * }
   * @examples_end
   */
  std::optional<util::point_t> get_mouse_loc(input_t &input);
  /**
   * @brief Move mouse using the backend coordinate system.
   *
   * @param input Platform input backend that receives the event.
   * @param deltaX Delta x.
   * @param deltaY Delta y.
   */
  void move_mouse(input_t &input, int deltaX, int deltaY);
  /**
   * @brief Retarget the virtual mouse and move it relatively.
   *
   * @param input Platform input backend that receives the event.
   * @param touch_port Desktop and streamed-display bounds for the pointer.
   * @param deltaX Delta x.
   * @param deltaY Delta y.
   */
  void move_mouse(input_t &input, const touch_port_t &touch_port, int deltaX, int deltaY);
  /**
   * @brief Move the pointer to an absolute client-provided touch coordinate.
   *
   * @param input Platform input backend that receives the event.
   * @param touch_port Touch coordinate bounds used for scaling.
   * @param x Horizontal absolute coordinate from the client.
   * @param y Vertical absolute coordinate from the client.
   */
  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y);
  /**
   * @brief Press or release a virtual mouse button.
   *
   * @param input Platform input backend that receives the event.
   * @param button Mouse button identifier to press or release.
   * @param release Whether the key or button event is a release.
   */
  void button_mouse(input_t &input, int button, bool release);
  /**
   * @brief Apply a vertical scroll event to the virtual mouse.
   *
   * @param input Platform input backend that receives the event.
   * @param distance High-resolution scroll distance reported by the client.
   */
  void scroll(input_t &input, int distance);
  /**
   * @brief Apply a horizontal scroll event to the virtual mouse.
   *
   * @param input Platform input backend that receives the event.
   * @param distance High-resolution scroll distance reported by the client.
   */
  void hscroll(input_t &input, int distance);
  /**
   * @brief Press or release a virtual keyboard key.
   *
   * @param input Platform input backend that receives the event.
   * @param modcode Portable key code to update.
   * @param release Whether the key or button event is a release.
   * @param flags Bit flags that modify the requested operation.
   * @param extended Whether the client positively identified an extended key.
   */
  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags, bool extended = false);
  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state);
  /**
   * @brief Submit UTF-8 text input to the keyboard backend.
   *
   * @param input Platform input backend that receives the event.
   * @param utf8 UTF-8 text submitted by the client.
   * @param size Number of bytes or elements requested.
   */
  void unicode(input_t &input, const char *utf8, int size);

  /**
   * @brief Per-client input context allocated by a platform backend.
   */
  using client_input_t = deinit_t;

  /**
   * @brief Allocate a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input);

  /**
   * @brief Send a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief Send a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen);

  /**
   * @brief Send a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void gamepad_touch(input_t &input, const gamepad_touch_t &touch);

  /**
   * @brief Send a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void gamepad_motion(input_t &input, const gamepad_motion_t &motion);

  /**
   * @brief Send a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void gamepad_battery(input_t &input, const gamepad_battery_t &battery);

  /**
   * @brief Create a new virtual gamepad.
   * @param input The global input context.
   * @param id The gamepad ID.
   * @param metadata Controller metadata from client (empty if none provided).
   * @param feedback_queue The queue for posting messages back to the client.
   * @return 0 on success.
   */
  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);
  /**
   * @brief Rebind an existing virtual gamepad to a resumed client session.
   *
   * @param input Platform input backend that owns the virtual gamepad.
   * @param id Global and client-relative identifiers for the resumed controller.
   * @param feedback_queue Queue used to return gamepad feedback to the resumed client.
   * @return 0 when the gamepad exists and was rebound; otherwise -1.
   */
  int rebind_gamepad(input_t &input, const gamepad_id_t &id, feedback_queue_t feedback_queue);
  /**
   * @brief Release gamepad resources.
   *
   * @param input Platform input backend that receives the event.
   * @param nr Controller index assigned by the client.
   */
  void free_gamepad(input_t &input, int nr);

  /**
   * @brief Get the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities();

  constexpr auto SERVICE_NAME = "Sunshine";  ///< mDNS service instance name advertised for GameStream discovery.
  constexpr auto SERVICE_TYPE = "_nvstream._tcp";  ///< mDNS service type advertised for GameStream discovery.

  namespace publish {
    [[nodiscard]] std::unique_ptr<deinit_t> start();
  }

  /**
   * @brief Initialize the platform-specific high precision timer.
   *
   * @return Cleanup handle for initialized platform resources, or null if none are needed.
   */
  [[nodiscard]] std::unique_ptr<deinit_t> init();

  /**
   * @brief Returns the current computer name in UTF-8.
   * @return Computer name or a placeholder upon failure.
   */
  std::string get_host_name();

  /**
   * @brief Resolves the render device path to use for hardware encoding.
   * @details If `config::video.adapter_name` is set, returns that.
   *          Otherwise, auto-detects the GPU with a connected display via `find_render_node_with_display()`.
   *          Falls back to `/dev/dri/renderD128` if detection fails.
   * @return Resolved render device path (may be empty on non-Linux platforms).
   */
  std::string resolve_render_device();

  /**
   * @brief Gets the supported gamepads for this platform backend.
   * @details This may be called prior to `platf::input()`!
   * @param input Pointer to the platform's `input_t` or `nullptr`.
   * @return Vector of gamepad options and status.
   */
  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input);

  /**
   * @brief Platform timer object used for precise frame pacing.
   */
  struct high_precision_timer: private boost::noncopyable {
    virtual ~high_precision_timer() = default;

    /**
     * @brief Sleep for the duration
     * @param duration Sleep duration
     */
    virtual void sleep_for(const std::chrono::nanoseconds &duration) = 0;

    /**
     * @brief Explicitly check if platform-specific timer backend has been initialized successfully
     * @return `true` on success, `false` on error
     */
    virtual explicit operator bool() = 0;
  };

  /**
   * @brief Create platform-specific timer capable of high-precision sleep
   * @return A unique pointer to timer
   */
  std::unique_ptr<high_precision_timer> create_high_precision_timer();

  /**
   * @brief Check is the current process is running with elevated privileges (e.g. system admin/etc.)
   * @param all_caps Bool that specifies whether to check all caps or only CAP_SYS_ADMIN
   * @return True if capabilities specified to be checked are present.
   */
  bool has_elevated_privileges(bool all_caps);

  /**
   * @brief Drop elevated privileges (e.g. system admin/nice etc.)
   * @param all_caps Bool that specifies whether to drop all caps or only CAP_SYS_ADMIN
   */
  void drop_elevated_privileges(bool all_caps);

}  // namespace platf
