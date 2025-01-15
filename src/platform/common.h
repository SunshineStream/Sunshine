/**
 * @file src/platform/common.h
 * @brief Declarations for common platform specific utilities.
 */
#pragma once

#include <bitset>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include <boost/core/noncopyable.hpp>
#ifndef _WIN32
  #include <boost/asio.hpp>
  #include <boost/process.hpp>
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
  namespace process::inline v1 {
    class child;
    class group;
    template <typename Char>
    class basic_environment;
    typedef basic_environment<char> environment;
  }  // namespace process::inline v1
}  // namespace boost
#endif
namespace video {
  struct config_t;
}  // namespace video
namespace nvenc {
  class nvenc_base;
}

namespace platf {
  // Limited by bits in activeGamepadMask
  constexpr auto MAX_GAMEPADS = 16;

  constexpr std::uint32_t DPAD_UP = 0x0001;
  constexpr std::uint32_t DPAD_DOWN = 0x0002;
  constexpr std::uint32_t DPAD_LEFT = 0x0004;
  constexpr std::uint32_t DPAD_RIGHT = 0x0008;
  constexpr std::uint32_t START = 0x0010;
  constexpr std::uint32_t BACK = 0x0020;
  constexpr std::uint32_t LEFT_STICK = 0x0040;
  constexpr std::uint32_t RIGHT_STICK = 0x0080;
  constexpr std::uint32_t LEFT_BUTTON = 0x0100;
  constexpr std::uint32_t RIGHT_BUTTON = 0x0200;
  constexpr std::uint32_t HOME = 0x0400;
  constexpr std::uint32_t A = 0x1000;
  constexpr std::uint32_t B = 0x2000;
  constexpr std::uint32_t X = 0x4000;
  constexpr std::uint32_t Y = 0x8000;
  constexpr std::uint32_t PADDLE1 = 0x010000;
  constexpr std::uint32_t PADDLE2 = 0x020000;
  constexpr std::uint32_t PADDLE3 = 0x040000;
  constexpr std::uint32_t PADDLE4 = 0x080000;
  constexpr std::uint32_t TOUCHPAD_BUTTON = 0x100000;
  constexpr std::uint32_t MISC_BUTTON = 0x200000;

  struct supported_gamepad_t {
    std::string name;
    bool is_enabled;
    std::string reason_disabled;
  };

  enum class gamepad_feedback_e {
    rumble,  ///< Rumble
    rumble_triggers,  ///< Rumble triggers
    set_motion_event_state,  ///< Set motion event state
    set_rgb_led,  ///< Set RGB LED
  };

  struct gamepad_feedback_msg_t {
    static gamepad_feedback_msg_t
    make_rumble(std::uint16_t id, std::uint16_t lowfreq, std::uint16_t highfreq) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble;
      msg.id = id;
      msg.data.rumble = { lowfreq, highfreq };
      return msg;
    }

    static gamepad_feedback_msg_t
    make_rumble_triggers(std::uint16_t id, std::uint16_t left, std::uint16_t right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble_triggers;
      msg.id = id;
      msg.data.rumble_triggers = { left, right };
      return msg;
    }

    static gamepad_feedback_msg_t
    make_motion_event_state(std::uint16_t id, std::uint8_t motion_type, std::uint16_t report_rate) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_motion_event_state;
      msg.id = id;
      msg.data.motion_event_state.motion_type = motion_type;
      msg.data.motion_event_state.report_rate = report_rate;
      return msg;
    }

    static gamepad_feedback_msg_t
    make_rgb_led(std::uint16_t id, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_rgb_led;
      msg.id = id;
      msg.data.rgb_led = { r, g, b };
      return msg;
    }

    gamepad_feedback_e type;
    std::uint16_t id;
    union {
      struct {
        std::uint16_t lowfreq;
        std::uint16_t highfreq;
      } rumble;
      struct {
        std::uint16_t left_trigger;
        std::uint16_t right_trigger;
      } rumble_triggers;
      struct {
        std::uint16_t report_rate;
        std::uint8_t motion_type;
      } motion_event_state;
      struct {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
      } rgb_led;
    } data;
  };

  using feedback_queue_t = safe::mail_raw_t::queue_t<gamepad_feedback_msg_t>;

  namespace speaker {
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

    constexpr std::uint8_t map_stereo[] {
      FRONT_LEFT, FRONT_RIGHT
    };
    constexpr std::uint8_t map_surround51[] {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
    };
    constexpr std::uint8_t map_surround71[] {
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

  enum class mem_type_e {
    system,  ///< System memory
    vaapi,  ///< VAAPI
    dxgi,  ///< DXGI
    cuda,  ///< CUDA
    videotoolbox,  ///< VideoToolbox
    unknown  ///< Unknown
  };

  enum class pix_fmt_e {
    yuv420p,  ///< YUV 4:2:0
    yuv420p10,  ///< YUV 4:2:0 10-bit
    nv12,  ///< NV12
    p010,  ///< P010
    ayuv,  ///< AYUV
    yuv444p16,  ///< Planar 10-bit (shifted to 16-bit) YUV 4:4:4
    y410,  ///< Y410
    unknown  ///< Unknown
  };

  inline std::string_view
  from_pix_fmt(pix_fmt_e pix_fmt) {
    using namespace std::literals;
#define _CONVERT(x)  \
  case pix_fmt_e::x: \
    return #x##sv
    switch (pix_fmt) {
      _CONVERT(yuv420p);
      _CONVERT(yuv420p10);
      _CONVERT(nv12);
      _CONVERT(p010);
      _CONVERT(ayuv);
      _CONVERT(yuv444p16);
      _CONVERT(y410);
      _CONVERT(unknown);
    }
#undef _CONVERT

    return "unknown"sv;
  }

  // Dimensions for touchscreen input
  struct touch_port_t {
    int offset_x, offset_y;
    int width, height;
  };

  // These values must match Limelight-internal.h's SS_FF_* constants!
  namespace platform_caps {
    typedef uint32_t caps_t;

    constexpr caps_t pen_touch = 0x01;  // Pen and touch events
    constexpr caps_t controller_touch = 0x02;  // Controller touch events
  };  // namespace platform_caps

  struct gamepad_state_t {
    std::uint32_t buttonFlags;
    std::uint8_t lt;
    std::uint8_t rt;
    std::int16_t lsX;
    std::int16_t lsY;
    std::int16_t rsX;
    std::int16_t rsY;
  };

  struct gamepad_id_t {
    // The global index is used when looking up gamepads in the platform's
    // gamepad array. It identifies gamepads uniquely among all clients.
    int globalIndex;

    // The client-relative index is the controller number as reported by the
    // client. It must be used when communicating back to the client via
    // the input feedback queue.
    std::uint8_t clientRelativeIndex;
  };

  struct gamepad_arrival_t {
    std::uint8_t type;
    std::uint16_t capabilities;
    std::uint32_t supportedButtons;
  };

  struct gamepad_touch_t {
    gamepad_id_t id;
    std::uint8_t eventType;
    std::uint32_t pointerId;
    float x;
    float y;
    float pressure;
  };

  struct gamepad_motion_t {
    gamepad_id_t id;
    std::uint8_t motionType;

    // Accel: m/s^2
    // Gyro: deg/s
    float x;
    float y;
    float z;
  };

  struct gamepad_battery_t {
    gamepad_id_t id;
    std::uint8_t state;
    std::uint8_t percentage;
  };

  struct touch_input_t {
    std::uint8_t eventType;
    std::uint16_t rotation;  // Degrees (0..360) or LI_ROT_UNKNOWN
    std::uint32_t pointerId;
    float x;
    float y;
    float pressureOrDistance;  // Distance for hover and pressure for contact
    float contactAreaMajor;
    float contactAreaMinor;
  };

  struct pen_input_t {
    std::uint8_t eventType;
    std::uint8_t toolType;
    std::uint8_t penButtons;
    std::uint8_t tilt;  // Degrees (0..90) or LI_TILT_UNKNOWN
    std::uint16_t rotation;  // Degrees (0..360) or LI_ROT_UNKNOWN
    float x;
    float y;
    float pressureOrDistance;  // Distance for hover and pressure for contact
    float contactAreaMajor;
    float contactAreaMinor;
  };

  class deinit_t {
  public:
    virtual ~deinit_t() = default;
  };

  struct img_t: std::enable_shared_from_this<img_t> {
  public:
    img_t() = default;

    img_t(img_t &&) = delete;
    img_t(const img_t &) = delete;
    img_t &
    operator=(img_t &&) = delete;
    img_t &
    operator=(const img_t &) = delete;

    std::uint8_t *data {};
    std::int32_t width {};
    std::int32_t height {};
    std::int32_t pixel_pitch {};
    std::int32_t row_pitch {};

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

    virtual ~img_t() = default;
  };

  struct sink_t {
    // Play on host PC
    std::string host;

    // On macOS and Windows, it is not possible to create a virtual sink
    // Therefore, it is optional
    struct null_t {
      std::string stereo;
      std::string surround51;
      std::string surround71;
    };
    std::optional<null_t> null;
  };

  struct encode_device_t {
    virtual ~encode_device_t() = default;

    virtual int
    convert(platf::img_t &img) = 0;

    video::sunshine_colorspace_t colorspace;
  };

  struct avcodec_encode_device_t: encode_device_t {
    void *data {};
    AVFrame *frame {};

    int
    convert(platf::img_t &img) override {
      return -1;
    }

    virtual void
    apply_colorspace() {
    }

    /**
     * @brief Set the frame to be encoded.
     * @note Implementations must take ownership of 'frame'.
     */
    virtual int
    set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
      BOOST_LOG(error) << "Illegal call to hwdevice_t::set_frame(). Did you forget to override it?";
      return -1;
    };

    /**
     * @brief Initialize the hwframes context.
     * @note Implementations may set parameters during initialization of the hwframes context.
     */
    virtual void
    init_hwframes(AVHWFramesContext *frames) {};

    /**
     * @brief Provides a hook for allow platform-specific code to adjust codec options.
     * @note Implementations may set or modify codec options prior to codec initialization.
     */
    virtual void
    init_codec_options(AVCodecContext *ctx, AVDictionary **options) {};

    /**
     * @brief Prepare to derive a context.
     * @note Implementations may make modifications required before context derivation
     */
    virtual int
    prepare_to_derive_context(int hw_device_type) {
      return 0;
    };
  };

  struct nvenc_encode_device_t: encode_device_t {
    virtual bool
    init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) = 0;

    nvenc::nvenc_base *nvenc = nullptr;
  };

  enum class capture_e : int {
    ok,  ///< Success
    reinit,  ///< Need to reinitialize
    timeout,  ///< Timeout
    interrupted,  ///< Capture was interrupted
    error  ///< Error
  };

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

    display_t() noexcept:
        offset_x { 0 }, offset_y { 0 } {}

    /**
     * @brief Capture a frame.
     * @param push_captured_image_cb The callback that is called with captured image,
     * must be called from the same thread as capture()
     * @param pull_free_image_cb Capture backends call this callback to get empty image from the pool.
     * If backend uses multiple threads, calls to this callback must be synchronized.
     * Calls to this callback and push_captured_image_cb must be synchronized as well.
     * @param cursor A pointer to the flag that indicates whether the cursor should be captured as well.
     * @retval capture_e::ok When stopping
     * @retval capture_e::error On error
     * @retval capture_e::reinit When need of reinitialization
     */
    virtual capture_e
    capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) = 0;

    virtual std::shared_ptr<img_t>
    alloc_img() = 0;

    virtual int
    dummy_img(img_t *img) = 0;

    virtual std::unique_ptr<avcodec_encode_device_t>
    make_avcodec_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    virtual std::unique_ptr<nvenc_encode_device_t>
    make_nvenc_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    virtual bool
    is_hdr() {
      return false;
    }

    virtual bool
    get_hdr_metadata(SS_HDR_METADATA &metadata) {
      std::memset(&metadata, 0, sizeof(metadata));
      return false;
    }

    /**
     * @brief Check that a given codec is supported by the display device.
     * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
     * @param config The codec configuration.
     * @return `true` if supported, `false` otherwise.
     */
    virtual bool
    is_codec_supported(std::string_view name, const ::video::config_t &config) {
      return true;
    }

    virtual ~display_t() = default;

    // Offsets for when streaming a specific monitor. By default, they are 0.
    int offset_x, offset_y;
    int env_width, env_height;

    int width, height;

  protected:
    // collect capture timing data (at loglevel debug)
    logging::time_delta_periodic_logger sleep_overshoot_logger = { debug, "Frame capture sleep overshoot" };
  };

  class mic_t {
  public:
    virtual capture_e
    sample(std::vector<float> &frame_buffer, std::chrono::steady_clock::time_point &capture_timestamp_out) = 0;

    virtual ~mic_t() = default;
  };

  class audio_control_t {
  public:
    virtual int
    set_sink(const std::string &sink) = 0;

    virtual std::unique_ptr<mic_t>
    microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) = 0;

    /**
     * @brief Check if the audio sink is available in the system.
     * @param sink Sink to be checked.
     * @returns True if available, false otherwise.
     */
    virtual bool
    is_sink_available(const std::string &sink) = 0;

    virtual std::optional<sink_t>
    sink_info() = 0;

    virtual ~audio_control_t() = default;
  };

  void
  freeInput(void *);

  using input_t = util::safe_ptr<void, freeInput>;

  std::filesystem::path
  appdata();

  std::string
  get_mac_address(const std::string_view &address);

  std::string
  from_sockaddr(const sockaddr *const);
  std::pair<std::uint16_t, std::string>
  from_sockaddr_ex(const sockaddr *const);

  std::unique_ptr<audio_control_t>
  audio_control();

  /**
   * @brief Get the display_t instance for the given hwdevice_type.
   * If display_name is empty, use the first monitor that's compatible you can find
   * If you require to use this parameter in a separate thread, make a copy of it.
   * @param display_name The name of the monitor that SHOULD be displayed
   * @param config Stream configuration
   * @return The display_t instance based on hwdevice_type.
   */
  std::shared_ptr<display_t>
  display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  // A list of names of displays accepted as display_name with the mem_type_e
  std::vector<std::string>
  display_names(mem_type_e hwdevice_type);

  /**
   * @brief Check if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool
  needs_encoder_reenumeration();

  boost::process::v1::child
  run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const boost::process::v1::environment &env, FILE *file, std::error_code &ec, boost::process::v1::group *group);

  enum class thread_priority_e : int {
    low,  ///< Low priority
    normal,  ///< Normal priority
    high,  ///< High priority
    critical  ///< Critical priority
  };
  void
  adjust_thread_priority(thread_priority_e priority);

  // Allow OS-specific actions to be taken to prepare for streaming
  void
  streaming_will_start();
  void
  streaming_will_stop();

  void
  restart();

  /**
   * @brief Set an environment variable.
   * @param name The name of the environment variable.
   * @param value The value to set the environment variable to.
   * @return 0 on success, non-zero on failure.
   */
  int
  set_env(const std::string &name, const std::string &value);

  /**
   * @brief Unset an environment variable.
   * @param name The name of the environment variable.
   * @return 0 on success, non-zero on failure.
   */
  int
  unset_env(const std::string &name);

  struct buffer_descriptor_t {
    const char *buffer;
    size_t size;

    // Constructors required for emplace_back() prior to C++20
    buffer_descriptor_t(const char *buffer, size_t size):
        buffer(buffer), size(size) {}
    buffer_descriptor_t():
        buffer(nullptr), size(0) {}
  };

  struct batched_send_info_t {
    // Optional headers to be prepended to each packet
    const char *headers;
    size_t header_size;

    // One or more data buffers to use for the payloads
    //
    // NB: Data buffers must be aligned to payload size!
    std::vector<buffer_descriptor_t> &payload_buffers;
    size_t payload_size;

    // The offset (in header+payload message blocks) in the header and payload
    // buffers to begin sending messages from
    size_t block_offset;

    // The number of header+payload message blocks to send
    size_t block_count;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    boost::asio::ip::address &source_address;

    /**
     * @brief Returns a payload buffer descriptor for the given payload offset.
     * @param offset The offset in the total payload data (bytes).
     * @return Buffer descriptor describing the region at the given offset.
     */
    buffer_descriptor_t
    buffer_for_payload_offset(ptrdiff_t offset) {
      for (const auto &desc : payload_buffers) {
        if (offset < desc.size) {
          return {
            desc.buffer + offset,
            desc.size - offset,
          };
        }
        else {
          offset -= desc.size;
        }
      }
      return {};
    }
  };
  bool
  send_batch(batched_send_info_t &send_info);

  struct send_info_t {
    const char *header;
    size_t header_size;
    const char *payload;
    size_t payload_size;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    boost::asio::ip::address &source_address;
  };
  bool
  send(send_info_t &send_info);

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
   */
  std::unique_ptr<deinit_t>
  enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging);

  /**
   * @brief Open a url in the default web browser.
   * @param url The url to open.
   */
  void
  open_url(const std::string &url);

  /**
   * @brief Attempt to gracefully terminate a process group.
   * @param native_handle The native handle of the process group.
   * @return `true` if termination was successfully requested.
   */
  bool
  request_process_group_exit(std::uintptr_t native_handle);

  /**
   * @brief Check if a process group still has running children.
   * @param native_handle The native handle of the process group.
   * @return `true` if processes are still running.
   */
  bool
  process_group_running(std::uintptr_t native_handle);

  input_t
  input();
  /**
   * @brief Get the current mouse position on screen
   * @param input The input_t instance to use.
   * @return Screen coordinates of the mouse.
   * @examples
   * auto [x, y] = get_mouse_loc(input);
   * @examples_end
   */
  util::point_t
  get_mouse_loc(input_t &input);
  void
  move_mouse(input_t &input, int deltaX, int deltaY);
  void
  abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y);
  void
  button_mouse(input_t &input, int button, bool release);
  void
  scroll(input_t &input, int distance);
  void
  hscroll(input_t &input, int distance);
  void
  keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags);
  void
  gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state);
  void
  unicode(input_t &input, char *utf8, int size);

  typedef deinit_t client_input_t;

  /**
   * @brief Allocate a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t>
  allocate_client_input_context(input_t &input);

  /**
   * @brief Send a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void
  touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief Send a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void
  pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen);

  /**
   * @brief Send a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void
  gamepad_touch(input_t &input, const gamepad_touch_t &touch);

  /**
   * @brief Send a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void
  gamepad_motion(input_t &input, const gamepad_motion_t &motion);

  /**
   * @brief Send a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void
  gamepad_battery(input_t &input, const gamepad_battery_t &battery);

  /**
   * @brief Create a new virtual gamepad.
   * @param input The global input context.
   * @param id The gamepad ID.
   * @param metadata Controller metadata from client (empty if none provided).
   * @param feedback_queue The queue for posting messages back to the client.
   * @return 0 on success.
   */
  int
  alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);
  void
  free_gamepad(input_t &input, int nr);

  /**
   * @brief Get the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t
  get_capabilities();

#define SERVICE_NAME "Sunshine"
#define SERVICE_TYPE "_nvstream._tcp"

  namespace publish {
    [[nodiscard]] std::unique_ptr<deinit_t>
    start();
  }

  [[nodiscard]] std::unique_ptr<deinit_t>
  init();

  /**
   * @brief Returns the current computer name in UTF-8.
   * @return Computer name or a placeholder upon failure.
   */
  std::string
  get_host_name();

  /**
   * @brief Gets the supported gamepads for this platform backend.
   * @details This may be called prior to `platf::input()`!
   * @param input Pointer to the platform's `input_t` or `nullptr`.
   * @return Vector of gamepad options and status.
   */
  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *input);

  struct high_precision_timer: private boost::noncopyable {
    virtual ~high_precision_timer() = default;

    /**
     * @brief Sleep for the duration
     * @param duration Sleep duration
     */
    virtual void
    sleep_for(const std::chrono::nanoseconds &duration) = 0;

    /**
     * @brief Check if platform-specific timer backend has been initialized successfully
     * @return `true` on success, `false` on error
     */
    virtual
    operator bool() = 0;
  };

  /**
   * @brief Create platform-specific timer capable of high-precision sleep
   * @return A unique pointer to timer
   */
  std::unique_ptr<high_precision_timer>
  create_high_precision_timer();

}  // namespace platf
