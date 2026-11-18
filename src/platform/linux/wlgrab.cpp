/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

// platform includes
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>

// local includes
#include "cuda.h"
#include "display_geometry.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

using namespace std::literals;

namespace wl {
  static bool force_ram_capture = false;  // NOSONAR(cpp:S5421): Retains SHM selection across VRAM-to-RAM capture backend recreation.

  /**
   * @brief Source byte offsets feeding each destination BGRA channel.
   */
  struct shm_layout_t {
    int bpp;  ///< Bytes per source pixel.
    int b;  ///< Source byte offset of the blue channel.
    int g;  ///< Source byte offset of the green channel.
    int r;  ///< Source byte offset of the red channel.
  };

  /**
   * @brief Map a `wl_shm` pixel format onto BGRA channel offsets.
   *
   * On little-endian systems XRGB8888 already arrives as BGRA bytes.
   * The 24-bit formats also describe packed values, not byte order.
   *
   * @param format `wl_shm` pixel format reported by the compositor.
   * @return Channel layout, or empty when the format is unsupported.
   */
  static std::optional<shm_layout_t> shm_layout(std::uint32_t format) {
    switch (format) {
      case WL_SHM_FORMAT_XRGB8888:
      case WL_SHM_FORMAT_ARGB8888:
        return shm_layout_t {4, 0, 1, 2};
      case WL_SHM_FORMAT_XBGR8888:
      case WL_SHM_FORMAT_ABGR8888:
        return shm_layout_t {4, 2, 1, 0};
      case WL_SHM_FORMAT_BGR888:
        return shm_layout_t {3, 2, 1, 0};
      case WL_SHM_FORMAT_RGB888:
        return shm_layout_t {3, 0, 1, 2};
      default:
        return std::nullopt;
    }
  }

  /**
   * @brief Copy a shared-memory frame into a BGRA image buffer.
   */
  bool convert_shm_frame(const frame_t &frame, platf::img_t &img) {
    auto layout = shm_layout(frame.shm_format);
    if (!layout) {
      BOOST_LOG(error) << "[wlgrab] Unsupported SHM format: 0x" << std::hex << frame.shm_format << std::dec;
      return false;
    }

    const auto size = shm_buffer_size(frame.shm_format, frame.sd.width, frame.sd.height, frame.shm_stride);
    if (!size || frame.shm_data.size() < *size || !img.data || img.width <= 0 || img.height <= 0 || static_cast<std::uint32_t>(img.width) != frame.sd.width || static_cast<std::uint32_t>(img.height) != frame.sd.height || img.pixel_pitch != 4 || img.row_pitch <= 0 || img.width > img.row_pitch / 4) {
      return false;
    }

    auto src = frame.shm_data.data();
    auto dst = static_cast<std::uint8_t *>(img.data);
    auto row_bytes = static_cast<std::size_t>(img.width) * 4;
    bool passthrough = layout->bpp == 4 && layout->b == 0;

    for (int y = 0; y < img.height; y++) {
      auto source_y = frame.shm_y_invert ? img.height - 1 - y : y;
      auto row_src = src + static_cast<std::size_t>(source_y) * frame.shm_stride;
      auto row_dst = dst + static_cast<std::size_t>(y) * img.row_pitch;

      if (passthrough) {
        std::memcpy(row_dst, row_src, row_bytes);
        continue;
      }

      for (int x = 0; x < img.width; x++) {
        row_dst[x * 4 + 0] = row_src[x * layout->bpp + layout->b];
        row_dst[x * 4 + 1] = row_src[x * layout->bpp + layout->g];
        row_dst[x * 4 + 2] = row_src[x * layout->bpp + layout->r];
        row_dst[x * 4 + 3] = 0xFF;
      }
    }

    return true;
  }

  namespace {
    int open_render_node_read_only(const char *path) {
      return open(path, O_RDONLY | O_CLOEXEC);
    }

    const drm_device_accessors_t drm_device_accessors {
      .open_render_node = open_render_node_read_only,
      .get_device = drmGetDevice2,
      .free_device = drmFreeDevice,
    };
  }  // namespace

  /**
   * @brief Resolve a DRM render node path, following udev by-path aliases.
   */
  std::string resolve_drm_node_path(const std::string &path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
      return path;
    }
    return canonical.string();
  }

  /**
   * @brief Parse one hexadecimal PCI address component with full consumption.
   *
   * @param token Component text to parse, without any 0x prefix.
   * @param max_digits Maximum accepted component width.
   * @return Parsed value, or no value when the token is empty, too wide, or not plain hex.
   */
  static std::optional<std::uint32_t> parse_pci_component(std::string_view token, std::size_t max_digits) {
    if (token.empty() || token.size() > max_digits) {
      return std::nullopt;
    }
    std::uint32_t value {};
    if (const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 16); result.ec == std::errc {} && result.ptr == token.data() + token.size()) {
      return value;
    }
    return std::nullopt;
  }

  std::optional<pci_bus_id> parse_pci_bus_id(std::string_view bus_id) {
    const auto first = bus_id.find(':');
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    const auto second = bus_id.find(':', first + 1);
    if (second == std::string_view::npos) {
      return std::nullopt;
    }
    const auto dot = bus_id.find('.', second + 1);
    if (dot == std::string_view::npos) {
      return std::nullopt;
    }
    const auto domain = parse_pci_component(bus_id.substr(0, first), 8);
    const auto bus = parse_pci_component(bus_id.substr(first + 1, second - first - 1), 2);
    const auto device = parse_pci_component(bus_id.substr(second + 1, dot - second - 1), 2);
    const auto function = parse_pci_component(bus_id.substr(dot + 1), 1);
    if (!domain.has_value() || !bus.has_value() || !device.has_value() || !function.has_value() || *function > 0x7) {
      return std::nullopt;
    }
    return pci_bus_id {*domain, static_cast<std::uint8_t>(*bus), static_cast<std::uint8_t>(*device), static_cast<std::uint8_t>(*function)};
  }

  std::optional<std::string> render_node_pci_bus_id(const std::string &render_path) {
    return render_node_pci_bus_id(render_path, drm_device_accessors);
  }

  std::optional<std::string> render_node_pci_bus_id(const std::string &render_path, const drm_device_accessors_t &accessors) {
    const int fd = accessors.open_render_node(render_path.c_str());
    if (fd < 0) {
      return std::nullopt;
    }
    drmDevicePtr device {};
    const int status = accessors.get_device(fd, 0, &device);
    close(fd);
    if (status != 0 || !device) {
      return std::nullopt;
    }
    if (device->bustype != DRM_BUS_PCI || !device->businfo.pci) {
      accessors.free_device(&device);
      return std::nullopt;
    }
    const auto pci = device->businfo.pci;
    std::string bus_id = std::format("{:04x}:{:02x}:{:02x}.{:x}", pci->domain, pci->bus, pci->dev, pci->func);
    accessors.free_device(&device);
    return bus_id;
  }

#ifdef SUNSHINE_BUILD_CUDA
  static std::optional<std::string> default_cuda_pci_bus_id(int cuda_index) {
    return cuda::device_pci_bus_id(cuda_index);
  }
#else
  static std::optional<std::string> default_cuda_pci_bus_id(int /*cuda_index*/) {
    return std::nullopt;
  }
#endif

  bool capture_node_is_cuda_device(const render_node_pci_reader_t &read_render_pci, const cuda_device_pci_reader_t &read_cuda_pci, const render_path_resolver_t &resolve_path, int cuda_index) {
    const auto render_path = platf::resolve_render_device();
    const auto resolved_path = resolve_path ? resolve_path(render_path) : resolve_drm_node_path(render_path);
    const auto render_pci = read_render_pci ? read_render_pci(resolved_path) : render_node_pci_bus_id(resolved_path);
    if (!render_pci) {
      BOOST_LOG(warning) << "[wlgrab] Could not read PCI bus id for ["sv << render_path << "], assuming cross-GPU capture"sv;
      return false;
    }
    const auto cuda_pci = read_cuda_pci ? read_cuda_pci(cuda_index) : default_cuda_pci_bus_id(cuda_index);
    if (!cuda_pci) {
      BOOST_LOG(warning) << "[wlgrab] Could not read PCI bus id for CUDA device ["sv << cuda_index << "], assuming cross-GPU capture"sv;
      return false;
    }
    const auto render_id = parse_pci_bus_id(*render_pci);
    const auto cuda_id = parse_pci_bus_id(*cuda_pci);
    if (!render_id || !cuda_id) {
      BOOST_LOG(warning) << "[wlgrab] Could not parse PCI bus ids (capture ["sv << *render_pci << "], CUDA ["sv << *cuda_pci << "]), assuming cross-GPU capture"sv;
      return false;
    }
    return *render_id == *cuda_id;
  }

  bool use_vram_capture(platf::mem_type_e hwdevice_type, const render_node_pci_reader_t &read_render_pci, const cuda_device_pci_reader_t &read_cuda_pci) {
    using enum platf::mem_type_e;
    if (hwdevice_type == vaapi) {
      return true;
    }

    if (hwdevice_type == vulkan) {
      return true;
    }

#ifdef SUNSHINE_BUILD_CUDA
    if (hwdevice_type == cuda) {
      // Zero-copy is only valid when capture and NVENC run on the same GPU, so
      // compare PCI identity against the CUDA encode device instead of trusting
      // the vendor name: split render/encode across two discrete NVIDIA GPUs
      // must take the bridge too. Otherwise (e.g. AMD/Intel render + NVIDIA
      // NVENC) fall back to GPU -> RAM -> GPU: wlr_ram_t capture plus cuda_ram_t
      // conversion.
      const bool same_gpu = capture_node_is_cuda_device(read_render_pci, read_cuda_pci);
      BOOST_LOG(info) << "[wlgrab] CUDA capture path: "sv << (same_gpu ? "zero-copy VRAM (capture node is the CUDA device)"sv : "GPU -> RAM -> GPU bridge (cross-GPU system)"sv);
      return same_gpu;
    }
#endif

    return false;
  }

  bool use_damage_capture(std::uint32_t screencopy_version, double stream_fps, std::int32_t refresh_mhz) {
    return screencopy_version >= 2 && stream_fps > 0 && refresh_mhz / 1000.0 >= stream_fps * 1.5;
  }

  bool capture_needs_reinit(int frame_width, int frame_height, int width, int height, int output_transform, int transform) {
    return frame_width != width || frame_height != height || output_transform != transform;
  }

  void apply_monitor_geometry(platf::display_t &display, const monitor_t &monitor) {
    display.offset_x = monitor.viewport.offset_x;
    display.offset_y = monitor.viewport.offset_y;
    display.width = monitor.viewport.width;
    display.height = monitor.viewport.height;
    display.output_name = monitor.name;
    display.logical_width = monitor.viewport.logical_width;
    display.logical_height = monitor.viewport.logical_height;
    display.transform = monitor.viewport.transform;
  }

  std::chrono::steady_clock::time_point next_damage_request(
    std::chrono::steady_clock::time_point next_request,
    std::chrono::steady_clock::time_point frame_time,
    std::chrono::nanoseconds delay
  ) {
    return std::max(next_request, frame_time - delay / 4) + delay;
  }

  std::chrono::steady_clock::time_point damage_frame_time(
    std::optional<std::chrono::steady_clock::time_point> compositor_time,
    std::chrono::steady_clock::time_point now,
    std::chrono::nanoseconds delay
  ) {
    if (compositor_time && *compositor_time <= now && *compositor_time > now - delay) {
      return *compositor_time;
    }

    return now;
  }

  screencopy_request_e next_screencopy_request(bool pending, bool requested_cursor, bool cursor, bool event_driven, bool have_frame) {
    using enum screencopy_request_e;
    if (pending) {
      return requested_cursor == cursor ? keep : copy;
    }

    // The first frame is asked for outright, and so is the first one after
    // the cursor setting changed. On a picture that is not changing, a
    // request for the next change would leave the stream without anything
    // to show, or with the cursor as it was, until something moved.
    const bool up_to_date = have_frame && requested_cursor == cursor;
    return event_driven && up_to_date ? copy_with_damage : copy;
  }

  bool should_wait_for_damage_request(
    std::chrono::steady_clock::time_point next_request,
    std::chrono::steady_clock::time_point now,
    std::chrono::nanoseconds delay
  ) {
    return next_request > now && next_request < now + delay;
  }

  /**
   * @brief Captured frame buffer shared between capture and encode stages.
   */
  struct img_t: public platf::img_t {
    /**
     * @brief Destroy the Wayland capture image.
     */
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  /**
   * @brief Wayland screencopy capture backend shared by RAM and VRAM paths.
   */
  class wlr_t: public platf::display_t {
  public:
    /**
     * @brief Initialize Wayland screencopy capture for the selected output.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      // calculate frame interval we should capture at
      delay = ::video::capture_frame_interval(config);
      const AVRational fps = ::video::framerate_to_rational(config);
      if (fps.den != 1) {
        BOOST_LOG(info) << "[wlgrab] Requested frame rate [" << fps.num << "/" << fps.den << ", approx. " << av_q2d(fps) << " fps]";
      } else {
        BOOST_LOG(info) << "[wlgrab] Requested frame rate [" << fps.num << "fps]";
      }

      mem_type = hwdevice_type;
#ifdef SUNSHINE_BUILD_CUDA
      // Cross-GPU NVENC cannot import tiled dmabufs, so resolve once here whether capture
      // must allocate linear buffers instead. The render node cannot change mid-stream, and
      // snapshot() runs per frame and must not redo device queries (or repeat warnings) per frame.
      prefer_linear_capture = mem_type == platf::mem_type_e::cuda && !capture_node_is_cuda_device();
#endif

      if (display.init()) {
        return -1;
      }

      interface.listen(display.registry());

      display.roundtrip();

      if (!interface[wl::interface_t::XDG_OUTPUT]) {
        BOOST_LOG(error) << "[wlgrab] Missing Wayland wire for xdg_output"sv;
        return -1;
      }

      if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
        BOOST_LOG(error) << "[wlgrab] Missing Wayland wire for wlr-export-dmabuf"sv;
        return -1;
      }

      // Populate xdg_output info (name, viewport) for every monitor up
      // front so we can match by stable name in addition to index.
      for (auto &m : interface.monitors) {
        m->listen(interface.output_manager);
      }
      display.roundtrip();

      auto monitor = interface.monitors[0].get();

      if (!display_name.empty()) {
        // Match by xdg_output name first (stable across hotplug, e.g.
        // "eDP-1", "HEADLESS-2"). Fall back to numeric index for
        // backward compatibility with existing configs.
        bool matched = false;
        for (auto &m : interface.monitors) {
          if (m->name == display_name) {
            monitor = m.get();
            matched = true;
            break;
          }
        }
        if (!matched) {
          auto streamedMonitor = util::from_view(display_name);
          if (streamedMonitor >= 0 && streamedMonitor < interface.monitors.size()) {
            monitor = interface.monitors[streamedMonitor].get();
          }
        }
      }

      output = monitor->output;

      // copy_with_damage arrived in version 2 of the protocol. With it the
      // compositor answers when the output has a new picture, so capture
      // follows the compositor's frames instead of sampling them on a clock
      // of our own.
      //
      // Only on an output that refreshes at least half again as fast as the
      // stream. A paced copy makes the compositor commit, and on an output
      // running at the stream's own rate those commits are part of what keeps
      // its clients on time: measured on a wlroots headless output at 60 Hz
      // with a 60 fps stream, taking them away dropped delivery from 60 to
      // about 55. With the output at twice the rate the same change went from
      // up to one repeated picture in ten to none.
      const double stream_fps = av_q2d(fps);
      const double output_fps = monitor->refresh_mhz / 1000.0;
      event_driven = use_damage_capture(interface.screencopy_version, stream_fps, monitor->refresh_mhz);
      BOOST_LOG(info) << "[wlgrab] Frame capture: "sv << (event_driven ? "event-driven"sv : "paced"sv) << " (output "sv << output_fps << " Hz, stream "sv << stream_fps << " fps)"sv;

      apply_monitor_geometry(*this, *monitor);
      this->monitor = monitor;

      platf::desktop_geometry_t desktop;
      for (const auto &monitor_entry : interface.monitors) {
        desktop.add(monitor_entry->viewport);
      }
      desktop.apply(*this);

      BOOST_LOG(info) << "[wlgrab] Selected monitor ["sv << monitor->description << "] for streaming"sv;
      BOOST_LOG(debug) << "[wlgrab] Offset: "sv << offset_x << 'x' << offset_y;
      BOOST_LOG(debug) << "[wlgrab] Resolution: "sv << width << 'x' << height;
      BOOST_LOG(debug) << "[wlgrab] Logical Resolution: "sv << logical_width << 'x' << logical_height;
      BOOST_LOG(debug) << "[wlgrab] Desktop Resolution: "sv << env_width << 'x' << env_height;
      BOOST_LOG(debug) << "[wlgrab] Logical Desktop Resolution: "sv << env_logical_width << 'x' << env_logical_height;

      return 0;
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Final capture status when callbacks stop capture or the display needs reinitialization.
     */
    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @param encoder_modifiers Optional encoder modifiers to intersect with compositor modifiers.
     * @return Capture status reported to the streaming pipeline.
     */
    inline platf::capture_e acquire_frame(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor, const std::map<std::uint32_t, std::vector<std::uint64_t>> *encoder_modifiers = nullptr) {
      auto to = std::chrono::steady_clock::now() + timeout;

      // Dispatch events until we get a new frame or the timeout expires.
      // A request that outlived the last timeout is still the one to wait
      // for, unless the cursor setting it was made with is out of date.
      const bool pending = dmabuf.status == dmabuf_t::WAITING;
      auto request = next_screencopy_request(pending, requested_cursor, cursor, event_driven, have_frame);
      if (request != screencopy_request_e::keep && pending && !dmabuf.cancel()) {
        // Its buffer is still being set up. It is replaced on the next call.
        request = screencopy_request_e::keep;
      }
      if (request != screencopy_request_e::keep) {
        dmabuf.with_damage = request == screencopy_request_e::copy_with_damage;
        requested_cursor = cursor;
        dmabuf.listen(interface.screencopy_manager, interface.dmabuf_interface, &interface.supported_modifiers, interface.shm_interface, output, cursor, encoder_modifiers, prefer_linear_capture);
      }
      do {
        auto remaining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - std::chrono::steady_clock::now());
        if (remaining_time_ms.count() < 0 || !display.dispatch(remaining_time_ms)) {
          return platf::capture_e::timeout;
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

      auto current_frame = dmabuf.current_frame;

      auto frame_time = damage_frame_time(current_frame->frame_timestamp, std::chrono::steady_clock::now(), delay);
      next_request = next_damage_request(next_request, frame_time, delay);
      have_frame = true;

      if (
        dmabuf.status == dmabuf_t::REINIT ||
        capture_needs_reinit(current_frame->sd.width, current_frame->sd.height, width, height, monitor->viewport.transform, transform)
      ) {
        return platf::capture_e::reinit;
      }

      return platf::capture_e::ok;
    }

    /**
     * @brief Wait until the next frame may be asked for.
     *
     * Paced capture sleeps to a clock of its own and then asks for a frame.
     * That request makes the compositor commit at a time Sunshine chose, and
     * the same picture can be captured twice while another is never seen.
     *
     * Event-driven capture has no clock. The request is answered by the next
     * commit that changed the picture, so a frame is captured once, when it
     * exists. What is left to do here is keep an output that changes faster
     * than the stream from being captured at its own rate, and that is a
     * budget rather than a clock: every captured frame moves the earliest
     * time of the next request on by one frame interval. A frame that arrives
     * late resets the budget to a quarter interval before its own time, so a
     * source running at the stream's rate is asked for with room to spare,
     * while a faster one is held to the stream's rate on average.
     *
     * @param next_frame Next deadline of the paced clock, unused when event-driven.
     */
    void wait_for_next_request(std::chrono::steady_clock::time_point &next_frame) {
      if (!event_driven) {
        platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);
        return;
      }

      if (should_wait_for_damage_request(next_request, std::chrono::steady_clock::now(), delay)) {
        std::this_thread::sleep_until(next_request);
      }
    }

    /**
     * @brief Capture frames until the stream stops or the display has to be set up again.
     *
     * @param push_captured_image_cb Callback that takes a captured image, or a timeout without one.
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param cursor Whether the cursor is to be part of the picture, read anew for every frame.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      using enum platf::capture_e;

      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        wait_for_next_request(next_frame);

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case reinit:
          case capture_error:
          case interrupted:
            return status;
          case timeout:
          case ok:
            if (!push_captured_image_cb(std::move(img_out), status == ok)) {
              return ok;
            }
            break;
          default:
            BOOST_LOG(error) << "[wlgrab] Unrecognized capture status ["sv << std::to_underlying(status) << ']';
            return status;
        }
      }

      return ok;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    virtual platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) = 0;

    platf::mem_type_e mem_type;  ///< Mem type.
    bool prefer_linear_capture {false};  ///< Linear copy buffers, resolved once in init() for cross-GPU CUDA encode.

    std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.
    std::chrono::steady_clock::time_point next_request {};  ///< Earliest time the next frame may be asked for when event-driven.
    bool event_driven {false};  ///< Whether frames are requested with copy_with_damage.
    bool have_frame {false};  ///< Whether this capture has delivered a frame yet.
    bool requested_cursor {false};  ///< Cursor setting the last screencopy request was made with.

    wl::display_t display;  ///< Wayland display connection used for capture.
    interface_t interface;  ///< Wayland registry interfaces required by screencopy.
    dmabuf_t dmabuf;  ///< DMA-BUF feedback and format state advertised by the compositor.

    wl_output *output;  ///< Wayland output selected for capture.
    wl::monitor_t *monitor {};  ///< Monitor selected for capture.
  };

  /**
   * @brief Wayland screencopy backend that copies frames into system memory.
   */
  class wlr_ram_t: public wlr_t {
  public:
    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) override {
      auto status = wlr_t::acquire_frame(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }

      auto current_frame = dmabuf.current_frame;

      if (current_frame->is_shm) {
        // Pixels are already in system memory; EGL import would be pointless
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        if (!convert_shm_frame(*current_frame, *img_out)) {
          return platf::capture_e::reinit;
        }

        img_out->frame_timestamp = current_frame->frame_timestamp;

        return platf::capture_e::ok;
      }

      if (!egl_display) {
        return platf::capture_e::reinit;
      }

      auto rgb_opt = egl::import_source(egl_display.get(), current_frame->sd);

      if (!rgb_opt) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      gl::ctx.BindTexture(GL_TEXTURE_2D, (*rgb_opt)->tex[0]);

      // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
      int h;
      int w;
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      BOOST_LOG(debug) << "[wlgrab] width and height: w "sv << w << " h "sv << h;

      gl::ctx.GetTextureSubImage((*rgb_opt)->tex[0], 0, 0, 0, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

      img_out->frame_timestamp = current_frame->frame_timestamp;

      return platf::capture_e::ok;
    }

    /**
     * @brief Initialize Wayland capture that copies frames into system memory.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
        return -1;
      }

      egl_display = egl::make_display(display.get());
      if (!egl_display) {
        BOOST_LOG(warning) << "[wlgrab] EGL init failed, using SHM-only capture"sv;
        dmabuf.allow_dmabuf = false;
        return 0;
      }

      auto ctx_opt = egl::make_ctx(egl_display.get());
      if (!ctx_opt) {
        BOOST_LOG(warning) << "[wlgrab] EGL context creation failed, using SHM-only capture"sv;
        egl_display.reset();
        dmabuf.allow_dmabuf = false;
        return 0;
      }

      ctx = std::move(*ctx_opt);

      return 0;
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, false);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_encode_device(width, height, false);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->data = new std::uint8_t[height * img->row_pitch];

      return img;
    }

    egl::display_t egl_display;  ///< EGL display.
    egl::ctx_t ctx;  ///< EGL context used for wlroots capture conversion.
  };

  /**
   * @brief Wayland screencopy backend that exports frames as GPU resources.
   */
  class wlr_vram_t: public wlr_t {
  public:
    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured wlroots image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) override {
      // For vulkan, use intersected modifiers; for others, pass nullptr (use compositor modifiers)
      const std::map<std::uint32_t, std::vector<std::uint64_t>> *mods = intersected_modifiers.empty() ? nullptr : &intersected_modifiers;
      auto status = wlr_t::acquire_frame(pull_free_image_cb, img_out, timeout, cursor, mods);
      if (status != platf::capture_e::ok) {
        return status;
      }

      auto current_frame = dmabuf.current_frame;

      if (current_frame->is_shm) {
        // The VRAM path has no DMA-BUF to hand to the encoder; restart on the RAM path
        if (mem_type == platf::mem_type_e::vulkan) {
          BOOST_LOG(error) << "[wlgrab] SHM capture is not supported by the Vulkan encoder"sv;
          return platf::capture_e::capture_error;
        }
        BOOST_LOG(warning) << "[wlgrab] Got a SHM frame in the VRAM path, switching to RAM capture"sv;
        force_ram_capture = true;
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (egl::img_descriptor_t *) img_out.get();
      img->reset();

      ++sequence;
      img->sequence = sequence;

      img->sd = current_frame->sd;
      img->frame_timestamp = current_frame->frame_timestamp;

      // Prevent dmabuf from closing the file descriptors.
      current_frame->sd.fds.fill(-1);

      return platf::capture_e::ok;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;

      // File descriptors aren't open
      img->sd.fds.fill(-1);

      return img;
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        return vk::make_avcodec_encode_device_vram(width, height, 0, 0);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(platf::img_t *img) override {
      // Empty images are recognized as dummies by the zero sequence number
      return 0;
    }

    /**
     * @brief Initialize encoder modifiers for vulkan capture.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     */
    void init_encoder_modifiers(platf::mem_type_e hwdevice_type) {
      if (hwdevice_type == platf::mem_type_e::vulkan) {
#ifdef SUNSHINE_BUILD_VULKAN
        encoder_modifiers = vk::get_supported_capture_modifiers();
        if (!encoder_modifiers.empty()) {
          intersected_modifiers = wl::intersect_modifiers(interface.supported_modifiers, encoder_modifiers);
          if (intersected_modifiers.empty()) {
            BOOST_LOG(warning) << "[wlgrab] No common modifiers between compositor and vulkan encoder"sv;
          }
        }
#endif
      }
    }

    std::uint64_t sequence {};  ///< Monotonic capture sequence assigned to Wayland frames.
    std::map<std::uint32_t, std::vector<std::uint64_t>> encoder_modifiers;  ///< Modifiers supported by the vulkan encoder.
    std::map<std::uint32_t, std::vector<std::uint64_t>> intersected_modifiers;  ///< Common modifiers between compositor and encoder.
  };

}  // namespace wl

namespace platf {
  /**
   * @brief Create a Wayland capture backend for the requested memory type.
   */
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda && hwdevice_type != platf::mem_type_e::vulkan) {
      BOOST_LOG(error) << "[wlgrab] Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    if ((hwdevice_type == platf::mem_type_e::vulkan || !wl::force_ram_capture) && wl::use_vram_capture(hwdevice_type)) {
      auto wlr = std::make_shared<wl::wlr_vram_t>();
      if (wlr->init(hwdevice_type, display_name, config)) {
        return nullptr;
      }

      // Initialize encoder modifiers for vulkan capture after interface setup
      wlr->init_encoder_modifiers(hwdevice_type);

      return wlr;
    }

#ifndef SUNSHINE_BUILD_CUDA
    if (hwdevice_type == platf::mem_type_e::cuda) {
      BOOST_LOG(warning) << "This build does not include CUDA support. Falling back to GPU -> RAM -> GPU for NVENC."sv;
    }
#endif
    if (wl::force_ram_capture) {
      BOOST_LOG(info) << "[wlgrab] Using RAM capture path (DMA-BUF unavailable)"sv;
    }

    auto wlr = std::make_shared<wl::wlr_ram_t>();
    if (wlr->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return wlr;
  }

  /**
   * @brief Enumerate capture display names reported by the Wayland compositor.
   */
  std::vector<std::string> wl_display_names() {
    std::vector<std::string> display_names;

    wl::display_t display;
    if (display.init()) {
      return {};
    }

    wl::interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[wl::interface_t::XDG_OUTPUT]) {
      BOOST_LOG(warning) << "[wlgrab] Missing Wayland wire for xdg_output"sv;
      return {};
    }

    if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
      BOOST_LOG(warning) << "[wlgrab] Missing Wayland wire for wlr-export-dmabuf"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    BOOST_LOG(info) << "[wlgrab] -------- Start of Wayland monitor list --------"sv;

    for (int x = 0; x < interface.monitors.size(); ++x) {
      auto monitor = interface.monitors[x].get();

      BOOST_LOG(info) << "[wlgrab] Monitor " << x << " is "sv << monitor->name << ": "sv << monitor->description;

      display_names.emplace_back(monitor->name.empty() ? std::to_string(x) : monitor->name);
    }

    BOOST_LOG(info) << "[wlgrab] --------- End of Wayland monitor list ---------"sv;

    return display_names;
  }

}  // namespace platf
