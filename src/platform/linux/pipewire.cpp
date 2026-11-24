/**
 * @file src/platform/linux/pipewire.cpp
 * @brief Shared classes for pipewire-based capture methods.
 */
// standard includes
#include <fstream>

// lib includes
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>

// local includes
#include "cuda.h"
#include "display_geometry.h"
#include "graphics.h"
#include "src/main.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

#if !PW_CHECK_VERSION(1, 6, 0)
constexpr int SPA_VIDEO_TRANSFER_SMPTE2084 = 14;  ///< Protocol or platform constant for spa video transfer smpte2084.
#endif

#if PW_CHECK_VERSION(0, 3, 75)
// Runtime linked library version checks are available. Check for pipewire 0.3.64 which documented object serial support and deprecated node id.
const bool SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL = pw_check_library_version(0, 3, 64);
#elifdef PW_KEY_TARGET_OBJECT
// Runtime linked library version checks are UNAVAILABLE but necessary PW_KEY_TARGET_OBJECT for object serial support is available.
constexpr bool SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL = true;
#else
// Pipewire object serials are unsupported without PW_KEY_TARGET_OBJECT (we define it here so compilation won't break but don't use it).
constexpr bool SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL = false;  ///< Whether PipeWire object serials should be used for matching.
  /**
   * @def PW_KEY_TARGET_OBJECT
   * @brief Macro for PW KEY TARGET OBJECT.
   */
  #define PW_KEY_TARGET_OBJECT "target.object"
#endif

namespace {
  // Buffer and limit constants
  constexpr int SPA_POD_BUFFER_SIZE = 4096;
  constexpr int MAX_PARAMS = 200;
  constexpr int MAX_DMABUF_FORMATS = 200;
  constexpr int MAX_DMABUF_MODIFIERS = 200;
}  // namespace

using namespace std::literals;

namespace pipewire {
  /**
   * @brief PipeWire SPA format mapped to Sunshine pixel format.
   */
  struct format_map_t {
    uint64_t fourcc;  ///< DRM fourcc pixel format.
    int32_t pw_format;  ///< Matching PipeWire SPA video format.
  };

  static constexpr std::array<format_map_t, 7> format_map = {{
    {DRM_FORMAT_XBGR2101010, SPA_VIDEO_FORMAT_xBGR_210LE},
    {DRM_FORMAT_BGRA1010102, SPA_VIDEO_FORMAT_ARGB_210LE},
    {DRM_FORMAT_RGBA1010102, SPA_VIDEO_FORMAT_ABGR_210LE},
    {DRM_FORMAT_ABGR2101010, SPA_VIDEO_FORMAT_RGBA_102LE},
    {DRM_FORMAT_ARGB2101010, SPA_VIDEO_FORMAT_BGRA_102LE},
    {DRM_FORMAT_ARGB8888, SPA_VIDEO_FORMAT_BGRA},
    {DRM_FORMAT_XRGB8888, SPA_VIDEO_FORMAT_BGRx},
  }};

  /**
   * @brief PipeWire capture state shared with callback threads.
   */
  struct shared_state_t {
    std::atomic<int> negotiated_width {0};  ///< Width negotiated with PipeWire for the stream.
    std::atomic<int> negotiated_height {0};  ///< Height negotiated with PipeWire for the stream.
    std::atomic<int> color_primaries {0};  ///< PipeWire color-primaries metadata for the stream.
    std::atomic<int> transfer_function {0};  ///< PipeWire transfer-function metadata for the stream.
    std::atomic<bool> stream_dead {false};  ///< Whether the PipeWire stream has been destroyed.
    std::atomic<bool> has_streamed {false};  ///< Whether this stream has ever reached PW_STREAM_STATE_STREAMING.
    pw_stream_state previous_state;  ///< Previous PipeWire stream state reported by callbacks.
    pw_stream_state current_state;  ///< Current PipeWire stream state reported by callbacks.
    std::string err_msg;  ///< Last PipeWire error message reported by the stream.
  };

  /**
   * @brief PipeWire stream handle, format, and shared state pointer.
   */
  struct stream_data_t {
    struct pw_stream *stream;  ///< PipeWire stream handle used for screencast frames.
    struct spa_hook stream_listener;  ///< Hook registering callbacks on the PipeWire stream.
    struct spa_video_info format;  ///< Negotiated PipeWire video format.
    struct pw_buffer *current_buffer;  ///< PipeWire buffer currently exposed to the capture thread.
    uint64_t drm_format;  ///< DRM format.
    std::shared_ptr<shared_state_t> shared;  ///< State shared between PipeWire callbacks and the capture backend.
    std::mutex frame_mutex;  ///< Synchronizes access to the current PipeWire frame.
    std::condition_variable frame_cv;  ///< Signals arrival or release of a PipeWire frame.
    size_t local_stride = 0;  ///< Local stride.
    bool frame_ready = false;  ///< Whether a PipeWire frame is ready to consume.
    // Two distinct memory pools
    std::vector<uint8_t> buffer_a;  ///< First staging buffer used for CPU-copy PipeWire frames.
    std::vector<uint8_t> buffer_b;  ///< Second staging buffer used for CPU-copy PipeWire frames.
    // Points to the buffer currently owned by fill_img
    std::vector<uint8_t> *front_buffer;  ///< Staging buffer currently readable by `fill_img`.
    // Points to the buffer currently being written by on_process
    std::vector<uint8_t> *back_buffer;  ///< Staging buffer currently writable by PipeWire callbacks.

    stream_data_t():
        front_buffer(&buffer_a),
        back_buffer(&buffer_b) {}
  };

  /**
   * @brief DMA-BUF format and modifier list advertised by PipeWire.
   */
  struct dmabuf_format_info_t {
    int32_t format;  ///< PipeWire SPA video format being advertised.
    uint64_t *modifiers;  ///< DRM format modifiers supported for the format.
    int n_modifiers;  ///< Number of entries in `modifiers`.
  };

  /**
   * @brief Pipewire image assembled for encoding.
   */
  struct img_descriptor_t: public egl::img_descriptor_t {
    ~img_descriptor_t() override {
      // Only free buffers this image actually owns. The memory-buffer capture
      // path points img->data at the PipeWire staging vector (front_buffer),
      // which is owned by pipewire_t -- deleting it here corrupts the heap.
      if (data && data_owned) {
        delete[] data;
      }
      data = nullptr;
      data_owned = false;
    }

    bool data_owned = false;  ///< Whether img->data is owned by this image and must be freed.
  };

  /**
   * @brief PipeWire core, context, and stream setup used for screencast capture.
   */
  class pipewire_t {
  public:
    pipewire_t():
        loop(pw_thread_loop_new("Pipewire thread", nullptr)) {
      BOOST_LOG(debug) << "[pipewire] Start PW thread loop"sv;
      pw_thread_loop_start(loop);
    }

    ~pipewire_t() {
      BOOST_LOG(debug) << "[pipewire] Destroying pipewire_t"sv;
      pw_thread_loop_lock(loop);

      // Lock the frame mutex to stop fill_img
      BOOST_LOG(debug) << "[pipewire] Stop fill_img"sv;
      {
        std::scoped_lock lock(stream_data.frame_mutex);
        stream_data.frame_ready = false;
        stream_data.current_buffer = nullptr;
      }

      // Release pipewire stream
      if (stream_data.stream) {
        BOOST_LOG(debug) << "[pipewire] Disconnect stream"sv;
        pw_stream_disconnect(stream_data.stream);
        BOOST_LOG(debug) << "[pipewire] Destroy stream"sv;
        pw_stream_destroy(stream_data.stream);
        stream_data.stream = nullptr;
      }
      // Release pipewire core
      if (core) {
        BOOST_LOG(debug) << "[pipewire] Disconnect PW core"sv;
        pw_core_disconnect(core);
        core = nullptr;
      }
      // Release pipewire context
      if (context) {
        BOOST_LOG(debug) << "[pipewire] Destroy PW context"sv;
        pw_context_destroy(context);
        context = nullptr;
      }
      // Release pipewire thread loop
      BOOST_LOG(debug) << "[pipewire] Stop PW thread loop"sv;
      pw_thread_loop_unlock(loop);
      pw_thread_loop_stop(loop);
      BOOST_LOG(debug) << "[pipewire] Destroy PW thread loop"sv;
      pw_thread_loop_destroy(loop);
    }

    /**
     * @brief Return the mutex protecting PipeWire frame state.
     *
     * @return Mutex used by producer and capture threads.
     */
    std::mutex &frame_mutex() {
      return stream_data.frame_mutex;
    }

    /**
     * @brief Return the condition variable signaled when frame state changes.
     *
     * @return Condition variable used to wait for frames or shutdown.
     */
    std::condition_variable &frame_cv() {
      return stream_data.frame_cv;
    }

    /**
     * @brief Check whether frame ready.
     *
     * @return True when PipeWire has delivered a frame ready for capture.
     */
    bool is_frame_ready() const {
      return stream_data.frame_ready;
    }

    /**
     * @brief Check and log whether the active session will require Sunshine to perform pacing.
     *
     * @param requested_framerate The framerate that we requested.
     * @param requested_delay The delay corresponding to the requested framerate.
     * @return True when Sunshine pacing is required.
     */
    bool is_pacing_required(AVRational requested_framerate, std::chrono::nanoseconds requested_delay) {
      AVRational negotiated_rate =
        {
          static_cast<int32_t>(stream_data.format.info.raw.max_framerate.num),
          static_cast<int32_t>(stream_data.format.info.raw.max_framerate.denom)
      };
      int rate_comparison = av_cmp_q(negotiated_rate, requested_framerate);
      bool variable_rate = negotiated_rate.num == 0 && negotiated_rate.den == 1;
      bool pacing_required = variable_rate || rate_comparison > 0;

      if (!variable_rate && rate_comparison < 0) {
        BOOST_LOG(warning)
          << "[pipewire] Sunshine frame pacing: disabled (negotiated rate lower than requested rate)"sv;
      } else {
        BOOST_LOG(info) << "[pipewire] Sunshine frame pacing: "sv
                        << (pacing_required ? std::format("enabled ({}ms)", std::chrono::duration<double, std::milli>(requested_delay).count()) : "disabled (event-driven capture)");
      }

      return pacing_required;
    }

    /**
     * @brief Set frame ready.
     *
     * @param ready Whether the PipeWire frame is ready for capture.
     */
    void set_frame_ready(bool ready) {
      stream_data.frame_ready = ready;
    }

    /**
     * @brief Initialize PipeWire core objects and optional stream negotiation.
     *
     * @param stream_fd Stream fd, or -1 to connect to the local PipeWire daemon. init() takes it over: PipeWire closes it
     *                  after a successful connection, and init() closes it when the connection could not be set up.
     * @param stream_node Stream node.
     * @param stream_object_serial Stream object serial.
     * @param shared_state Shared state.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(const int stream_fd, const uint32_t stream_node, const uint64_t stream_object_serial, std::shared_ptr<shared_state_t> shared_state) {
      node = stream_node;
      object_serial = stream_object_serial;
      stream_data.shared = std::move(shared_state);
      stream_data.shared->has_streamed.store(false);

      pw_thread_loop_lock(loop);
      BOOST_LOG(debug) << "[pipewire] Setup PW context"sv;
      context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
      if (context) {
        if (stream_fd >= 0) {
          BOOST_LOG(debug) << "[pipewire] Connect PW core on fd "sv << stream_fd;
          core = pw_context_connect_fd(context, stream_fd, nullptr, 0);
        } else {
          BOOST_LOG(debug) << "[pipewire] Connect PW local core"sv;
          core = pw_context_connect(context, nullptr, 0);
        }
        if (core) {
          pw_core_add_listener(core, &core_listener, &core_events, nullptr);
        } else {
          BOOST_LOG(debug) << "[pipewire] Failed to connect to PW core. Error: "sv << errno << "(" << strerror(errno) << ")"sv;
          // A failed pw_context_connect_fd() leaves the fd open
          if (stream_fd >= 0) {
            close(stream_fd);
          }
          pw_thread_loop_unlock(loop);
          return -1;
        }
      } else {
        BOOST_LOG(debug) << "[pipewire] Failed to setup PW context. Error: "sv << errno << "(" << strerror(errno) << ")"sv;
        if (stream_fd >= 0) {
          close(stream_fd);
        }
        pw_thread_loop_unlock(loop);
        return -1;
      }

      pw_thread_loop_unlock(loop);
      return 0;
    }

    /**
     * @brief Create the PipeWire stream if it is not already active.
     *
     * @param mem_type Mem type.
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     * @param target_framerate Target framerate expressed as AVRational.
     * @param dmabuf_infos Dmabuf infos.
     * @param n_dmabuf_infos N dmabuf infos.
     * @param display_is_nvidia Display is nvidia.
     * @return 0 when the PipeWire stream is configured; nonzero on negotiation failure.
     */
    int ensure_stream(const platf::mem_type_e mem_type, const uint32_t width, const uint32_t height, const AVRational target_framerate, const struct dmabuf_format_info_t *dmabuf_infos, const int n_dmabuf_infos, const bool display_is_nvidia) {
      pw_thread_loop_lock(loop);
      int result = 0;
      if (!stream_data.stream) {
        if (!core) {
          BOOST_LOG(debug) << "[pipewire] PW core not available. Cannot ensure stream."sv;
          pw_thread_loop_unlock(loop);
          return -1;
        }

        struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);

        BOOST_LOG(debug) << "[pipewire] Create PW stream"sv;
        stream_data.stream = pw_stream_new(core, "Sunshine Video Capture", props);
        pw_stream_add_listener(stream_data.stream, &stream_data.stream_listener, &stream_events, &stream_data);

        std::array<uint8_t, SPA_POD_BUFFER_SIZE> buffer;
        struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());

        int n_params = 0;
        std::array<const struct spa_pod *, MAX_PARAMS> params;

        // Add preferred parameters for DMA-BUF with modifiers
        // Use DMA-BUF for VAAPI, or for CUDA when the display GPU is NVIDIA (pure NVIDIA system).
        // On hybrid GPU systems (Intel+NVIDIA), DMA-BUFs come from the Intel GPU and cannot
        // be imported into CUDA, so we fall back to memory buffers in that case.
        bool use_dmabuf = n_dmabuf_infos > 0 && (mem_type == platf::mem_type_e::vaapi ||
                                                 mem_type == platf::mem_type_e::vulkan ||
                                                 (mem_type == platf::mem_type_e::cuda && display_is_nvidia));
        if (use_dmabuf) {
          for (int i = 0; i < n_dmabuf_infos; i++) {
            auto format_param = build_format_parameter(&pod_builder, width, height, target_framerate, dmabuf_infos[i].format, dmabuf_infos[i].modifiers, dmabuf_infos[i].n_modifiers);
            params[n_params] = format_param;
            n_params++;
          }
        }

        // Add fallback for memptr
        for (const auto &fmt : format_map) {
          auto format_param = build_format_parameter(&pod_builder, width, height, target_framerate, fmt.pw_format, nullptr, 0);
          params[n_params] = format_param;
          n_params++;
        }

        // Connection via pipewire object serial if it is supported and the serial is valid (lower 32-bits != SPA_ID_INVALID, see also PW_KEY_OBJECT_SERIAL docs)
        if (SUNSHINE_USE_PIPEWIRE_OBJECT_SERIAL && (object_serial & SPA_ID_INVALID) != SPA_ID_INVALID) {
          pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, object_serial);
          BOOST_LOG(debug) << "[pipewire] Connect PW stream on object serial: "sv << object_serial;
          result = pw_stream_connect(stream_data.stream, PW_DIRECTION_INPUT, PW_ID_ANY, (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params.data(), n_params);
          if (result < 0) {
            // Unset object serial for retry with node id
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, nullptr);
          }
        } else {
          result = -1;  // Mark failed so we try to connect via node id
        }
        // Connection via legacy (and deprecated) pipewire node id
        if (result < 0) {
          BOOST_LOG(debug) << "[pipewire] Connect PW stream on node: "sv << node;
          result = pw_stream_connect(stream_data.stream, PW_DIRECTION_INPUT, node, (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params.data(), n_params);
        }
      }

      pw_thread_loop_unlock(loop);
      return result;
    }

    /**
     * @brief Close img fds.
     *
     * @param img_descriptor Image descriptor whose duplicated DMA-BUF fds are closed.
     */
    static void close_img_fds(egl::img_descriptor_t *img_descriptor) {
      for (int &fd : img_descriptor->sd.fds) {
        if (fd >= 0) {
          close(fd);
          fd = -1;
        }
      }
    }

    /**
     * @brief Compositors that require version-based checks for optimal stream setup.
     */
    enum class compositor_type_e {
      unknown,  ///< Unknown
      gnome,  ///< GNOME Shell is a proxy for Mutter.
      kwin  ///< KWin
    };

    /**
     * @brief Compositor type and version information.
     */
    struct compositor_version_t {
      compositor_type_e type = compositor_type_e::unknown;  ///< Compositor type (gnome, kde...).
      std::vector<int> version;  ///< Compositor version.
    };

    /**
     * @brief Fetch compositor type and version for processing.
     *
     * @return A struct containing the compositor type and version.
     */
    static compositor_version_t get_running_compositor() {
      using enum compositor_type_e;

      if (auto version = get_running_compositor_version(kwin); !version.empty()) {
        return {kwin, std::move(version)};
      }

      if (auto version = get_running_compositor_version(gnome); !version.empty()) {
        return {gnome, std::move(version)};
      }

      return {};
    }

    /**
     * @brief Fetch compositor version information using DBus calls.
     *
     * @param compositor_type The compositor we should attempt to fetch version information for.
     * @return A vector with 2-3 elements containing the major.minor.micro versions or an empty vector if the version could not be determined.
     */
    static std::vector<int> get_running_compositor_version(enum compositor_type_e compositor_type) {
#if !GLIB_CHECK_VERSION(2, 74, 0)
      // Compatibility for Ubuntu 22.04 (Glib 2.72)
      constexpr auto G_REGEX_DEFAULT = static_cast<GRegexCompileFlags>(0);
      constexpr auto G_REGEX_MATCH_DEFAULT = static_cast<GRegexMatchFlags>(0);
#endif
      auto conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      std::vector<int> result;

      if (!conn) {
        return result;
      }

      const gchar *version_regex;
      GVariant *reply;

      using enum compositor_type_e;
      if (compositor_type == kwin) {
        version_regex = "KWin version: ([0-9]+)\\.([0-9]+)\\.([0-9]+)";
        reply = g_dbus_connection_call_sync(
          conn,
          "org.kde.KWin",
          "/KWin",
          "org.kde.KWin",
          "supportInformation",
          nullptr,
          G_VARIANT_TYPE("(s)"),
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          nullptr,
          nullptr
        );
      } else {
        version_regex = "([0-9]+)\\.([0-9]+)(?:\\.([0-9]+))?";
        reply = g_dbus_connection_call_sync(
          conn,
          "org.gnome.Shell",
          "/org/gnome/Shell",
          "org.freedesktop.DBus.Properties",
          "Get",
          g_variant_new("(ss)", "org.gnome.Shell", "ShellVersion"),
          G_VARIANT_TYPE("(v)"),
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          nullptr,
          nullptr
        );
      }

      if (!reply) {
        g_clear_object(&conn);
        return result;
      }

      g_autofree gchar *version_str = nullptr;

      if (compositor_type == kwin) {
        g_variant_get(reply, "(s)", &version_str);
      } else {
        GVariant *inner_variant = nullptr;
        g_variant_get(reply, "(v)", &inner_variant);
        if (inner_variant) {
          version_str = g_variant_dup_string(inner_variant, nullptr);
          g_variant_unref(inner_variant);
        }
      }

      if (!version_str) {
        g_variant_unref(reply);
        g_clear_object(&conn);
        return result;
      }

      if (auto *regex = g_regex_new(version_regex, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, nullptr); regex) {
        GMatchInfo *match_info = nullptr;
        g_regex_match(regex, version_str, G_REGEX_MATCH_DEFAULT, &match_info);

        if (g_match_info_matches(match_info)) {
          g_autofree const gchar *major = g_match_info_fetch(match_info, 1);
          g_autofree const gchar *minor = g_match_info_fetch(match_info, 2);
          g_autofree const gchar *micro = g_match_info_fetch(match_info, 3);

          if (major) {
            result.emplace_back(std::atoi(major));
          }
          if (minor) {
            result.emplace_back(std::atoi(minor));
          }
          if (micro && *micro != '\0') {
            result.emplace_back(std::atoi(micro));
          }
        }
        g_match_info_free(match_info);
        g_regex_unref(regex);
      }

      g_variant_unref(reply);
      g_clear_object(&conn);
      return result;
    }

    /**
     * @brief Determine if Pipewire's pts metadata is suitable for client pacing based on compositor type/version whitelist.
     *
     * @param compositor Struct containing compositor type and version.
     * @param selected_display_name Name of display - specifically the connector type - such as DP-1, HDMI-1, etc.
     * @return True if pts metadata is suitable.
     */
    static bool use_pipewire_pts(const compositor_version_t &compositor, const std::string &selected_display_name) {
      // KWin: use Pipewire pts metadata for versions 6.7.80+ (6.8 beta) or newer.
      // Mutter: use Pipewire pts metadata for Mutter 51 onwards, but pts is reliable only for virtual monitors (Meta-).
      // All other cases: don't use Pipewire pts metadata directly.

      bool use_pts = false;
      bool using_virtual_monitor = false;

      using enum compositor_type_e;
      switch (compositor.type) {
        case gnome:
          using_virtual_monitor = selected_display_name.starts_with("Meta-");
          use_pts = (using_virtual_monitor && compositor.version[0] >= 51);
          break;
        case kwin:
          use_pts = (compositor.version[0] > 6 || (compositor.version[0] == 6 && (compositor.version[1] > 7 || (compositor.version[1] == 7 && compositor.version[2] > 79))));
          break;
        default:
          break;
      }

      BOOST_LOG(info) << "[pipewire] Using frame_timestamp (pts) metadata source: "sv
                      << (use_pts ? "Pipewire (via compositor)" : PROJECT_NAME);
      return use_pts;
    }

    /**
     * @brief Determine if the active compositor is suited for variable rate capture based on type/version whitelist.
     *
     * @param compositor Struct containing the compositor type and version.
     * @return True if variable rate capture is suitable.
     */
    static bool
      use_variable_rate(const compositor_version_t &compositor) {
      // If the active compositor is KWin, request variable rate (0, 1) capture for versions 5.x-6.7.79 (up to 6.7 stable series).
      // Issue: KWin <=6.7 has a ~3% fixed-rate pacing deficit vs the requested framerate; variable rate avoids this and prioritizes gaming smoothness.
      //        KWin 6.7 regresses variable rate (desktop animations run at half speed, but doesn't affect in-game pacing). Ref: https://bugs.kde.org/show_bug.cgi?id=524129
      // Issue: KWin 6.8 still has desktop animation pacing issues with variable rate, but fixes the 3% fixed-rate pacing deficit. Fixed-rate pacing has
      //        new regression tied to 'commit-timing'/VK_KHR_present_timing support when Vsync/FIFO is enabled. Ref: https://bugs.kde.org/show_bug.cgi?id=525619
      // Summary: KWin 5.5-6.6 have excellent (variable) pacing. KWin 6.7 has poor desktop animation pacing (variable) but good game pacing.
      //          KWin 6.8+ will have good overall (fixed) pacing if #525619 can be resolved, otherwise we will update docs advising to disable VSync in games.

      // All other compositors (including Mutter) will default to variable rate.
      bool variable_rate = true;

      using enum compositor_type_e;
      if (compositor.type == kwin) {
        variable_rate = (compositor.version[0] == 5 || (compositor.version[0] == 6 && (compositor.version[1] < 7 || (compositor.version[1] == 7 && compositor.version[2] < 80))));
      }

      return variable_rate;
    }

    /**
     * @brief Copy PipeWire metadata into the Sunshine image descriptor.
     *
     * @param img_descriptor Image descriptor receiving timestamps, sequence, and damage flags.
     * @param buf Raw byte buffer used for serialization.
     */
    static void fill_img_metadata(egl::img_descriptor_t *img_descriptor, struct spa_buffer *buf) {
      struct spa_meta_header *h = static_cast<struct spa_meta_header *>(
        spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h))
      );

      img_descriptor->seq.reset();
      img_descriptor->pts.reset();
      if (h) {
        img_descriptor->seq = h->seq;
        if (h->pts > 0) {
          img_descriptor->pts = h->pts;
        }
      }
      img_descriptor->frame_timestamp = (img_descriptor->pts.has_value() && prefer_pipewire_pts) ?
                                          std::chrono::steady_clock::time_point(std::chrono::nanoseconds(img_descriptor->pts.value())) :
                                          std::chrono::steady_clock::now();

      if (buf->n_datas > 0) {
        img_descriptor->pw_flags = buf->datas[0].chunk->flags;
      }

      struct spa_meta_region *damage = static_cast<struct spa_meta_region *>(
        spa_buffer_find_meta_data(buf, SPA_META_VideoDamage, sizeof(*damage))
      );
      img_descriptor->pw_damage = (damage && damage->region.size.width > 0 && damage->region.size.height > 0) ? std::optional<bool>(true) : std::nullopt;
    }

    /**
     * @brief Populate a Sunshine image descriptor from PipeWire DMA-BUF planes.
     *
     * @param img_descriptor Image descriptor receiving duplicated fds and plane layout.
     * @param buf Raw byte buffer used for serialization.
     * @param d PipeWire listener data passed to the callback.
     */
    static void fill_img_dmabuf(egl::img_descriptor_t *img_descriptor, struct spa_buffer *buf, const stream_data_t &d) {
      img_descriptor->sd.width = d.format.info.raw.size.width;
      img_descriptor->sd.height = d.format.info.raw.size.height;
      img_descriptor->sd.modifier = d.format.info.raw.modifier;
      img_descriptor->sd.fourcc = d.drm_format;
      for (int i = 0; i < MIN(buf->n_datas, 4); i++) {
        img_descriptor->sd.fds[i] = dup(buf->datas[i].fd);
        img_descriptor->sd.pitches[i] = buf->datas[i].chunk->stride;
        img_descriptor->sd.offsets[i] = buf->datas[i].chunk->offset;
      }
    }

    /**
     * @brief Copy the latest PipeWire frame into Sunshine's image buffer.
     *
     * @param img Image or frame object to read from or populate.
     */
    void fill_img(platf::img_t *img) {
      pw_thread_loop_lock(loop);
      std::scoped_lock lock(stream_data.frame_mutex);

      if (stream_data.shared && stream_data.shared->stream_dead.load()) {
        img->data = nullptr;
        close_img_fds(static_cast<egl::img_descriptor_t *>(img));
        pw_thread_loop_unlock(loop);
        return;
      }

      if (!stream_data.current_buffer) {
        img->data = nullptr;
        pw_thread_loop_unlock(loop);
        return;
      }

      struct spa_buffer *buf = stream_data.current_buffer->buffer;
      if (buf->datas[0].chunk->size != 0) {
        auto *img_descriptor = static_cast<img_descriptor_t *>(img);
        fill_img_metadata(img_descriptor, buf);
        if (buf->datas[0].type == SPA_DATA_DmaBuf) {
          fill_img_dmabuf(img_descriptor, buf, stream_data);
        } else {
          img->data = stream_data.front_buffer->data();
          img_descriptor->data_owned = false;
          img->row_pitch = stream_data.local_stride;
          // NV12 is the only 1-byte-per-pixel format delivered on the memory
          // path; every other negotiated format is packed 4 bytes per pixel.
          img->pixel_pitch = (stream_data.format.info.raw.format == SPA_VIDEO_FORMAT_NV12) ? 1 : 4;
        }
      }

      pw_thread_loop_unlock(loop);
    }

    /**
     * @brief Set negotiate maxframerate.
     *
     * @param negotiate_maxframerate Negotiate maxframerate.
     */
    void set_negotiate_maxframerate(bool negotiate_maxframerate) {
      negotiate_maxframerate_ = negotiate_maxframerate;
    }

    inline static std::atomic prefer_pipewire_pts {false};  ///< Whether the session should directly passthrough Pipewire pts metadata for client pacing.
    inline static std::atomic negotiate_variable_rate {true};  ///< Whether the session should request variable rate (maxFrameRate = 0/1) capture.

  private:
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct stream_data_t stream_data;
    uint32_t node;
    uint64_t object_serial;
    bool negotiate_maxframerate_ = true;

    struct spa_pod *build_format_parameter(struct spa_pod_builder *b, uint32_t width, uint32_t height, AVRational target_framerate, int32_t format, uint64_t *modifiers, int n_modifiers) {
      struct spa_pod_frame object_frame;
      struct spa_pod_frame modifier_frame;
      std::array<struct spa_rectangle, 3> sizes;
      std::array<struct spa_fraction, 3> framerates;

      sizes[0] = SPA_RECTANGLE(width, height);  // Preferred
      sizes[1] = SPA_RECTANGLE(1, 1);
      sizes[2] = SPA_RECTANGLE(8192, 4096);

      framerates[0] = SPA_FRACTION(uint32_t(target_framerate.num), uint32_t(target_framerate.den));  // default/preferred
      framerates[1] = SPA_FRACTION(0, 1);  // min
      framerates[2] = SPA_FRACTION(1000, 1);  // max

      spa_pod_builder_push_object(b, &object_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
      spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sizes[0], &sizes[1], &sizes[2]), 0);
      if (negotiate_maxframerate_) {
        // Always request variable rate (0, 1) for framerate when populating maxFramerate with default,min,max values
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerates[1]), 0);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&framerates[0], &framerates[1], &framerates[2]), 0);
      } else {
        // Request target framerate (target_framerate) for framerate in fallback case
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerates[0]), 0);
      }

      if (format == SPA_VIDEO_FORMAT_xBGR_210LE) {
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_Id(SPA_VIDEO_COLOR_PRIMARIES_BT2020), 0);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_transferFunction, SPA_POD_Id(SPA_VIDEO_TRANSFER_SMPTE2084), 0);
      }

      if (n_modifiers) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum, 0);

        // Preferred value, we pick the first modifier be the preferred one
        spa_pod_builder_long(b, modifiers[0]);
        for (uint32_t i = 0; i < n_modifiers; i++) {
          spa_pod_builder_long(b, modifiers[i]);
        }

        spa_pod_builder_pop(b, &modifier_frame);
      }

      return static_cast<struct spa_pod *>(spa_pod_builder_pop(b, &object_frame));
    }

    static void on_core_info_cb([[maybe_unused]] void *user_data, const struct pw_core_info *pw_info) {  // NOSONAR(cpp:S5008): PipeWire event callbacks require a void pointer for user data.
      BOOST_LOG(info) << "[pipewire] Connected to pipewire version "sv << pw_info->version;
    }

    static void on_core_error_cb([[maybe_unused]] void *user_data, const uint32_t id, const int seq, [[maybe_unused]] int res, const char *message) {  // NOSONAR(cpp:S5008): PipeWire event callbacks require a void pointer for user data.
      BOOST_LOG(info) << "[pipewire] Pipewire Error, id:"sv << id << " seq:"sv << seq << " message: "sv << message;
    }

    constexpr static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .info = on_core_info_cb,
      .error = on_core_error_cb,
    };

  public:
    /**
     * @brief Record stream transitions, retaining startup success after subsequent pauses or errors.
     *
     * @param user_data Stream data registered with the PipeWire listener.
     * @param old Previous PipeWire stream state.
     * @param state New PipeWire stream state.
     * @param err_msg Error description, or nullptr for a normal transition.
     */
    static void on_stream_state_changed(void *user_data, enum pw_stream_state old, enum pw_stream_state state, const char *err_msg) {  // NOSONAR(cpp:S5008): PipeWire event callbacks require a void pointer for user data.
      if (err_msg != nullptr) {
        BOOST_LOG(info) << "[pipewire] PipeWire stream error '" << err_msg << "' on state: " << pw_stream_state_as_string(old)
                        << " -> " << pw_stream_state_as_string(state);
      } else {
        BOOST_LOG(info) << "[pipewire] PipeWire stream state: " << pw_stream_state_as_string(old)
                        << " -> " << pw_stream_state_as_string(state);
      }

      auto *d = static_cast<stream_data_t *>(user_data);

      switch (state) {
        case PW_STREAM_STATE_STREAMING:
          if (d->shared) {
            d->shared->has_streamed.store(true);
          }
          break;
        case PW_STREAM_STATE_PAUSED:
          if (d->shared && old == PW_STREAM_STATE_STREAMING) {
            {
              std::scoped_lock lock(d->frame_mutex);
              d->frame_ready = false;
              d->current_buffer = nullptr;
              d->shared->stream_dead.store(true);
              d->shared->current_state = state;
              d->shared->previous_state = old;
              d->shared->err_msg = "";
            }
            d->frame_cv.notify_all();
          }
          break;
        case PW_STREAM_STATE_ERROR:
          {
            std::scoped_lock lock(d->frame_mutex);
            d->shared->current_state = state;
            d->shared->previous_state = old;
            d->shared->err_msg = std::string(err_msg);
          }
          [[fallthrough]];
        case PW_STREAM_STATE_UNCONNECTED:
          if (d->shared) {
            d->shared->stream_dead.store(true);
            d->frame_cv.notify_all();
          }
          break;
        default:
          break;
      }
    }

  private:
    static void on_process(void *user_data) {  // NOSONAR(cpp:S5008): PipeWire event callbacks require a void pointer for user data.
      const auto d = static_cast<struct stream_data_t *>(user_data);
      struct pw_buffer *b = nullptr;

      // 1. Drain the queue: Always grab the most recent buffer
      while (struct pw_buffer *aux = pw_stream_dequeue_buffer(d->stream)) {
        if (b) {
          pw_stream_queue_buffer(d->stream, b);  // Return the older, unused buffer
        }
        b = aux;
      }

      if (!b) {
        return;
      }

      // 2. Fast Path: DMA-BUF
      if (b->buffer->datas[0].type == SPA_DATA_DmaBuf) {
        std::scoped_lock lock(d->frame_mutex);
        if (d->current_buffer) {
          pw_stream_queue_buffer(d->stream, d->current_buffer);
        }
        d->current_buffer = b;
        d->frame_ready = true;
      }
      // 3. Optimized Path: Software/MemPtr
      else if (b->buffer->datas[0].data != nullptr) {
        size_t size = b->buffer->datas[0].chunk->size;

        // Perform the copy to the BACK buffer while NOT holding the lock
        if (d->back_buffer->size() < size) {
          d->back_buffer->resize(size);
        }
        std::memcpy(d->back_buffer->data(), b->buffer->datas[0].data, size);

        {
          // Lock only for the pointer swap and state update
          std::scoped_lock lock(d->frame_mutex);
          std::swap(d->front_buffer, d->back_buffer);

          d->local_stride = b->buffer->datas[0].chunk->stride;
          d->frame_ready = true;
          d->current_buffer = b;
        }

        // Release the PW buffer immediately after copy
        pw_stream_queue_buffer(d->stream, b);
      }

      d->frame_cv.notify_one();
    }

    static void on_param_changed(void *user_data, uint32_t id, const struct spa_pod *param) {  // NOSONAR(cpp:S5008): PipeWire event callbacks require a void pointer for user data.
      const auto d = static_cast<struct stream_data_t *>(user_data);

      d->current_buffer = nullptr;

      if (param == nullptr || id != SPA_PARAM_Format) {
        return;
      }
      if (spa_format_parse(param, &d->format.media_type, &d->format.media_subtype) < 0) {
        return;
      }
      if (d->format.media_type != SPA_MEDIA_TYPE_video || d->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
      }
      if (spa_format_video_raw_parse(param, &d->format.info.raw) < 0) {
        return;
      }

      BOOST_LOG(info) << "[pipewire] Video format: "sv << d->format.info.raw.format;
      BOOST_LOG(info) << "[pipewire] Size: "sv << d->format.info.raw.size.width << "x"sv << d->format.info.raw.size.height;
      BOOST_LOG(info) << "[pipewire] Color primaries: "sv << d->format.info.raw.color_primaries;
      BOOST_LOG(info) << "[pipewire] Transfer function: "sv << d->format.info.raw.transfer_function;
      if (d->format.info.raw.max_framerate.num == 0 && d->format.info.raw.max_framerate.denom == 1) {
        BOOST_LOG(info) << "[pipewire] Compositor negotiated frame rate: 0/1 (variable rate capture)"sv;
      } else {
        BOOST_LOG(info) << "[pipewire] Compositor negotiated frame rate: "sv
                        << d->format.info.raw.framerate.num << "/"sv << d->format.info.raw.framerate.denom
                        << ", max: "sv << d->format.info.raw.max_framerate.num << "/"sv << d->format.info.raw.max_framerate.denom;
      }

      int physical_w = d->format.info.raw.size.width;
      int physical_h = d->format.info.raw.size.height;

      if (d->shared) {
        int old_w = d->shared->negotiated_width.load();
        int old_h = d->shared->negotiated_height.load();
        int old_color_primaries = d->shared->color_primaries.load();
        int old_transfer_function = d->shared->transfer_function.load();

        if (physical_w != old_w || physical_h != old_h) {
          d->shared->negotiated_width.store(physical_w);
          d->shared->negotiated_height.store(physical_h);
        }

        if (d->format.info.raw.color_primaries != old_color_primaries || d->format.info.raw.transfer_function != old_transfer_function) {
          d->shared->color_primaries.store(d->format.info.raw.color_primaries);
          d->shared->transfer_function.store(d->format.info.raw.transfer_function);
        }
      }

      uint64_t drm_format = 0;
      for (const auto &fmt : format_map) {
        if (fmt.pw_format == d->format.info.raw.format) {
          drm_format = fmt.fourcc;
        }
      }
      d->drm_format = drm_format;

      uint32_t buffer_types = 0;
      if (spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr && d->drm_format) {
        BOOST_LOG(info) << "[pipewire] using DMA-BUF buffers"sv;
        buffer_types |= 1 << SPA_DATA_DmaBuf;
      } else {
        BOOST_LOG(info) << "[pipewire] using memory buffers"sv;
        buffer_types |= 1 << SPA_DATA_MemPtr;
      }

      // Ack the buffer type and metadata
      std::array<uint8_t, SPA_POD_BUFFER_SIZE> buffer;
      std::array<const struct spa_pod *, 3> params;
      int n_params = 0;
      struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
      auto buffer_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types)));
      params[n_params] = buffer_param;
      n_params++;
      auto meta_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header))));
      params[n_params] = meta_param;
      n_params++;
      int videoDamageRegionCount = 16;
      auto damage_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * videoDamageRegionCount, sizeof(struct spa_meta_region) * 1, sizeof(struct spa_meta_region) * videoDamageRegionCount)));
      params[n_params] = damage_param;
      n_params++;

      pw_stream_update_params(d->stream, params.data(), n_params);
    }

    constexpr static const struct pw_stream_events stream_events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = on_stream_state_changed,
      .param_changed = on_param_changed,
      .process = on_process,
    };
  };

  /**
   * @brief Display capture backend that consumes frames from a PipeWire stream.
   */
  class pipewire_display_t: public platf::display_t {
  public:
    /**
     * @brief Initialize pipewire and check hwdevice type.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @return True when PipeWire is initialized and the hardware device type is supported.
     */
    static bool init_pipewire_and_check_hwdevice_type(platf::mem_type_e hwdevice_type) {
      // Initialize pipewire to load necessary modules
      pw_init(nullptr, nullptr);

      // Check if we have a matching hwdevice_type
      switch (hwdevice_type) {
        using enum platf::mem_type_e;
        case system:
        case vaapi:
        case cuda:
        case vulkan:
          return true;
        default:
          return false;
      }
    }

    /**
     *  @brief Configure the pipewire stream
     *  @param display_name provide a stream for this display_name
     *  @param out_pipewire_fd set to the pipewire fd for the stream during function call (or -1 for using the local context)
     *  @param out_pipewire_node set to the pipewire node of the stream during function call (or PW_ID_ANY to refer to object_serial)
     *  @param out_pipewire_objectserial set the pipewire object serial of the stream during function call
     *  @returns 0 if the stream successfully configured
     */
    virtual int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) = 0;

    /**
     * @brief Resolve the streamed connector and full desktop bounds for absolute input.
     */
    virtual void verify_and_update_display_parameters() {
      platf::desktop_geometry_t desktop;
      for (const auto &monitor : wl::monitors()) {
        desktop.add(monitor->viewport);
        if (monitor->viewport.offset_x == offset_x && monitor->viewport.offset_y == offset_y && ((monitor->viewport.width == width && monitor->viewport.height == height) || (monitor->viewport.logical_width == width && monitor->viewport.logical_height == height))) {
          if (logical_width <= 0 || logical_height <= 0) {
            logical_width = monitor->viewport.logical_width;
            logical_height = monitor->viewport.logical_height;
          }
          if (output_name.empty()) {
            output_name = monitor->name;
          }
        }
      }
      if (desktop.bounds().width > 0 && desktop.bounds().height > 0) {
        desktop.apply(*this);
      }
    }

    /**
     * @brief Initialize the PipeWire display backend for a selected stream.
     *
     * @param hwdevice_type Hardware device type requested for capture or encode.
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      // calculate frame interval we should capture at
      delay = ::video::capture_frame_interval(config);

      const static auto compositor = pipewire.get_running_compositor();

      // Determine if variable rate should be negotiated based on compositor type/versioning.
      pipewire.negotiate_variable_rate = pipewire.use_variable_rate(compositor);

      const AVRational fps = (pipewire.negotiate_variable_rate ? AVRational {0, 1} : ::video::framerate_to_rational(config));
      if (fps.den != 1) {
        BOOST_LOG(info) << "[pipewire] Requested frame rate: "sv << fps.num << "/"sv << fps.den << ", approx. "sv << av_q2d(fps) << " fps"sv;
      } else if (fps.num == 0 && fps.den == 1) {
        BOOST_LOG(info) << "[pipewire] Requested variable frame rate (Sunshine pacing required: "sv << std::chrono::duration<double, std::milli>(delay).count() << "ms)"sv;
      } else {
        BOOST_LOG(info) << "[pipewire] Requested frame rate: "sv << fps.num << "fps"sv;
      }
      this->target_framerate = fps;
      mem_type = hwdevice_type;

      if (get_dmabuf_modifiers() < 0) {
        return -1;
      }

      int pipewire_fd = -1;
      auto pipewire_node = PW_ID_ANY;  // Default for invalid stream from pipewire docs
      uint64_t pipewire_object_serial = SPA_ID_INVALID;  // Default for invalid stream from pipewire docs for PW_KEY_OBJECT_SERIAL
      // Fetch stream info
      if (configure_stream(display_name, pipewire_fd, pipewire_node, pipewire_object_serial) < 0 || (pipewire_node == PW_ID_ANY && (pipewire_object_serial & SPA_ID_INVALID) == SPA_ID_INVALID)) {
        BOOST_LOG(error) << "[pipewire] Could not find display with name: '"sv << display_name << "'";
        return -1;
      }
      BOOST_LOG(info) << "[pipewire] Streaming display '"sv << display_name << "' offset: "sv << offset_x << "x"sv << offset_y << " resolution: "sv << width << "x"sv << height;

      // Verify or update display parameters for streaming to ensure absolute touch inputs work as expected
      verify_and_update_display_parameters();

      // Determine if pts metadata should be sourced from Pipewire or sampled by Sunshine at time of capture.
      pipewire.prefer_pipewire_pts = pipewire.use_pipewire_pts(compositor, display_name);

      if (!shared_state) {
        shared_state = std::make_shared<shared_state_t>();
      } else {
        shared_state->stream_dead.store(false);
        shared_state->negotiated_width.store(0);
        shared_state->negotiated_height.store(0);
        shared_state->color_primaries.store(0);
        shared_state->transfer_function.store(0);
      }

      if (pipewire.init(pipewire_fd, pipewire_node, pipewire_object_serial, shared_state) < 0) {
        BOOST_LOG(error) << "[pipewire] Failed to init pipewire. pipewire_t::init() failed.";
        return -1;
      }

      // Start PipeWire now so format negotiation can proceed before capture start
      if (pipewire.ensure_stream(mem_type, width, height, target_framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia) < 0) {
        BOOST_LOG(error) << "[pipewire] Failed to ensure pipewire stream. pipewire_t::init() failed.";
        return -1;
      }

      // Wait for pipewire negotiation to finish so we have the proper negotiated dimensions
      int timeout_ms = 1500;
      int negotiated_w = 0;
      int negotiated_h = 0;
      while (timeout_ms > 0) {
        negotiated_w = shared_state->negotiated_width.load();
        negotiated_h = shared_state->negotiated_height.load();
        if (negotiated_w > 0 && negotiated_h > 0) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        timeout_ms -= 10;
      }
      // Set width and height to the values negotiated by pipewire
      if (negotiated_w > 0 && negotiated_h > 0 && (negotiated_w != width || negotiated_h != height)) {
        width = negotiated_w;
        height = negotiated_h;
        BOOST_LOG(info) << "[pipewire] Using negotiated Resolution: "sv << width << "x" << height;

        // Reset and update display parameters for negotiated resolution
        env_width = 0;
        env_height = 0;
        logical_height = 0;
        logical_width = 0;
        env_logical_height = 0;
        env_logical_width = 0;
        verify_and_update_display_parameters();
      }

      return 0;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Captured PipeWire image returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param show_cursor Show cursor.
     * @return Capture status reported to the streaming pipeline.
     */
    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool show_cursor) {
      // FIXME: show_cursor is ignored
      auto deadline = std::chrono::steady_clock::now() + timeout;
      int retries = 0;

      while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_for_frame(deadline)) {
          return platf::capture_e::timeout;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        auto *img_egl = static_cast<egl::img_descriptor_t *>(img_out.get());
        img_egl->reset();
        pipewire.fill_img(img_egl);

        // Check if we got valid data (either DMA-BUF fd or memory pointer), then filter duplicates
        if ((img_egl->sd.fds[0] >= 0 || img_egl->data != nullptr) && !is_buffer_redundant(img_egl)) {
          // Update frame metadata
          update_metadata(img_egl, retries);
          return platf::capture_e::ok;
        }

        // No valid frame yet, or it was a duplicate
        retries++;
      }
      return platf::capture_e::timeout;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      // Note: this img_t type is also used for memory buffers
      auto img = std::make_shared<img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;
      img->data_owned = false;
      img->sd.fds.fill(-1);

      return img;
    }

    /**
     * @brief Check stream dead.
     *
     * @param out_status Out status.
     * @return True when the PipeWire stream can no longer produce frames.
     */
    virtual bool check_stream_dead(platf::capture_e &out_status) {
      return false;  // Return to default stream dead handling.
    }

    /**
     * @brief Capture PipeWire frames until capture stops or the stream needs reinitialization.
     *
     * @param push_captured_image_cb Callback receiving a captured image or a capture timeout.
     * @param pull_free_image_cb Callback providing an available image buffer.
     * @param cursor Whether the cursor should be included in the captured image.
     * @return Final capture status reported to the streaming pipeline.
     */
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      using enum platf::capture_e;

      auto next_frame = std::chrono::steady_clock::now();

      if (pipewire.ensure_stream(mem_type, width, height, target_framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia) < 0) {
        BOOST_LOG(error) << "[pipewire] Failed to ensure pipewire stream. capture() failed with error.";
        return capture_error;
      }
      sleep_overshoot_logger.reset();

      // Check if pacing is required
      bool pacing_required = pipewire.is_pacing_required(target_framerate, delay);

      while (true) {
        // Check if PipeWire signaled a dead stream
        if (shared_state->stream_dead.exchange(false)) {
          // Additional custom error-handling for subclasses on stream dead event
          if (platf::capture_e status; check_stream_dead(status)) {
            return status;
          }
          // Re-init the capture if the stream is dead for any other reason
          BOOST_LOG(warning) << "[pipewire] PipeWire stream disconnected. Forcing session reset."sv;
          return reinit;
        }

        // Use unpaced event driven capture when possible
        if (pacing_required) {
          platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);
        }

        std::shared_ptr<platf::img_t> img_out;
        switch (const auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor)) {
          case reinit:
          case capture_error:
          case interrupted:
            pipewire.frame_cv().notify_all();
            return status;
          case timeout:
            if (!pull_free_image_cb(img_out)) {
              // Detect if shutdown is pending
              BOOST_LOG(debug) << "[pipewire] PipeWire: timeout -> shutdown pending -> interrupt nudge";
              pipewire.frame_cv().notify_all();
              return interrupted;
            }
            if (!push_captured_image_cb(std::move(img_out), false)) {
              BOOST_LOG(debug) << "[pipewire] PipeWire: timeout -> !push_captured_image_cb -> ok";
              return ok;
            }
            break;
          case ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              BOOST_LOG(debug) << "[pipewire] PipeWire: ok -> !push_captured_image_cb -> ok";
              return ok;
            }
            break;
          default:
            BOOST_LOG(error) << "[pipewire] Unrecognized capture status ["sv << std::to_underlying(status) << ']';
            return status;
        }
      }

      return ok;
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
        return va::make_avcodec_encode_device(width, height, n_dmabuf_infos > 0);
      }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan && n_dmabuf_infos > 0) {
        return vk::make_avcodec_encode_device_vram(width, height, 0, 0);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (display_is_nvidia && n_dmabuf_infos > 0) {
          // Display GPU is NVIDIA - can use DMA-BUF directly
          return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
        } else {
          // Hybrid system (Intel display + NVIDIA encode) - use memory buffer path
          // DMA-BUFs from Intel GPU cannot be imported into CUDA
          return cuda::make_avcodec_encode_device(width, height, false);
        }
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
      // Software encoders convert the dummy image immediately; provide a valid
      // (black) buffer instead of leaving img->data null, which makes sws fail
      // with EINVAL. The buffer is new[]-allocated and marked as owned so the
      // destructor releases it.
      if (img->data == nullptr) {
        const auto w = img->width;
        const auto h = img->height;
        if (w > 0 && h > 0) {
          img->data = new uint8_t[static_cast<size_t>(w) * h * 4]();  // NOSONAR(cpp:S5025) - buffer is owned by the image and freed by img_descriptor_t's destructor
          static_cast<img_descriptor_t *>(img)->data_owned = true;
          img->row_pitch = w * 4;
          img->pixel_pitch = 4;
        }
      }
      return 0;
    }

    /**
     * @brief Report whether the active display mode is HDR.
     *
     * @return True when the active display mode is HDR.
     */
    bool is_hdr() override {
      int color_primaries = shared_state->color_primaries.load();
      int transfer_function = shared_state->transfer_function.load();

      if (color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT2020 && transfer_function == SPA_VIDEO_TRANSFER_SMPTE2084) {
        return true;
      }

      return false;
    }

    /**
     * @brief Read HDR metadata for the active display mode.
     *
     * @param metadata Output structure populated with HDR metadata.
     * @return True when HDR metadata was written to the output structure.
     */
    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      int color_primaries = shared_state->color_primaries.load();
      int transfer_function = shared_state->transfer_function.load();

      if (color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT2020 && transfer_function == SPA_VIDEO_TRANSFER_SMPTE2084) {
        // Report Rec 2020 primaries
        metadata.displayPrimaries[0].x = 0.708f * 50000;
        metadata.displayPrimaries[0].y = 0.292f * 50000;
        metadata.displayPrimaries[1].x = 0.170f * 50000;
        metadata.displayPrimaries[1].y = 0.797f * 50000;
        metadata.displayPrimaries[2].x = 0.131f * 50000;
        metadata.displayPrimaries[2].y = 0.046f * 50000;
        metadata.whitePoint.x = 0.3127f * 50000;
        metadata.whitePoint.y = 0.3290f * 50000;

        // This is according to HDR10+ standards, should probably be based on actual data
        metadata.maxDisplayLuminance = 4000;
        metadata.minDisplayLuminance = 1;

        // These are content-specific metadata parameters that this interface doesn't give us
        metadata.maxContentLightLevel = 0;
        metadata.maxFrameAverageLightLevel = 0;
        metadata.maxFullFrameLuminance = 0;

        return true;
      }

      return false;
    }

  private:
    bool is_buffer_redundant(const egl::img_descriptor_t *img) {
      // Check for corrupted frame
      if (img->pw_flags.has_value() && (img->pw_flags.value() & SPA_CHUNK_FLAG_CORRUPTED)) {
        return true;
      }

      // If PTS is identical, only drop if damage metadata confirms no change
      if (img->pts.has_value() && last_pts.has_value() && img->pts.value() == last_pts.value()) {
        return img->pw_damage.has_value() && !img->pw_damage.value();
      }

      return false;
    }

    void update_metadata(egl::img_descriptor_t *img, int retries) {
      last_seq = img->seq;
      last_pts = img->pts;
      img->sequence = ++sequence;

      if (retries > 0) {
        BOOST_LOG(debug) << "[pipewire] Processed frame after " << retries << " redundant events."sv;
      }
    }

    bool wait_for_frame(std::chrono::steady_clock::time_point deadline) {
      std::unique_lock<std::mutex> lock(pipewire.frame_mutex());

      bool success = pipewire.frame_cv().wait_until(lock, deadline, [&] {
        return pipewire.is_frame_ready() || shared_state->stream_dead.load();
      });

      if (success) {
        pipewire.set_frame_ready(false);
        return true;
      }
      return false;
    }

    static bool pw_format_supported(uint64_t fourcc, std::array<EGLint, MAX_DMABUF_FORMATS> dmabuf_formats) {
      for (const auto &drm_format : dmabuf_formats) {
        if (drm_format == fourcc) {
          return true;
        }
      }
      return false;
    }

    void query_dmabuf_formats(EGLDisplay egl_display) {
      EGLint num_dmabuf_formats = 0;
      std::array<EGLint, MAX_DMABUF_FORMATS> dmabuf_formats = {0};
      eglQueryDmaBufFormatsEXT(egl_display, MAX_DMABUF_FORMATS, dmabuf_formats.data(), &num_dmabuf_formats);

      if (num_dmabuf_formats > MAX_DMABUF_FORMATS) {
        BOOST_LOG(warning) << "[pipewire] Some DMA-BUF formats are being ignored"sv;
      }

      for (const auto &fmt : format_map) {
        if (n_dmabuf_infos >= MAX_DMABUF_FORMATS) {
          break;
        }

        if (!pw_format_supported(fmt.fourcc, dmabuf_formats)) {
          continue;
        }

        EGLint num_modifiers = 0;
        std::array<EGLuint64KHR, MAX_DMABUF_MODIFIERS> mods = {0};
        eglQueryDmaBufModifiersEXT(egl_display, fmt.fourcc, MAX_DMABUF_MODIFIERS, mods.data(), nullptr, &num_modifiers);

        if (num_modifiers > MAX_DMABUF_MODIFIERS) {
          BOOST_LOG(warning) << "[pipewire] Some DMA-BUF modifiers are being ignored"sv;
        }

        dmabuf_infos[n_dmabuf_infos].format = fmt.pw_format;
        dmabuf_infos[n_dmabuf_infos].n_modifiers = MIN(num_modifiers, MAX_DMABUF_MODIFIERS);
        dmabuf_infos[n_dmabuf_infos].modifiers =
          static_cast<uint64_t *>(g_memdup2(mods.data(), sizeof(uint64_t) * dmabuf_infos[n_dmabuf_infos].n_modifiers));
        ++n_dmabuf_infos;
      }
    }

    int get_dmabuf_modifiers() {
      if (wl_display.init() < 0) {
        return -1;
      }

      auto egl_display = egl::make_display(wl_display.get());
      if (!egl_display) {
        return -1;
      }

      // Detect if this is a pure NVIDIA system (not hybrid Intel+NVIDIA)
      // On hybrid systems, the wayland compositor typically runs on Intel,
      // so DMA-BUFs from portal will come from Intel and cannot be imported into CUDA.
      // Check if Intel GPU exists - if so, assume hybrid system and disable CUDA DMA-BUF.
      bool has_intel_gpu = std::ifstream("/sys/class/drm/card0/device/vendor").good() ||
                           std::ifstream("/sys/class/drm/card1/device/vendor").good();
      if (has_intel_gpu) {
        // Read vendor IDs to check for Intel (0x8086)
        auto check_intel = [](const std::string &path) {
          if (std::ifstream f(path); f.good()) {
            std::string vendor;
            f >> vendor;
            return vendor == "0x8086";
          }
          return false;
        };
        bool intel_present = check_intel("/sys/class/drm/card0/device/vendor") ||
                             check_intel("/sys/class/drm/card1/device/vendor");
        if (intel_present) {
          BOOST_LOG(info) << "[pipewire] Hybrid GPU system detected (Intel + discrete) - CUDA will use memory buffers"sv;
          display_is_nvidia = false;
        } else {
          // No Intel GPU found, check if NVIDIA is present
          const char *vendor = eglQueryString(egl_display.get(), EGL_VENDOR);
          if (vendor && std::string_view(vendor).contains("NVIDIA")) {
            BOOST_LOG(info) << "[pipewire] Pure NVIDIA system - DMA-BUF will be enabled for CUDA"sv;
            display_is_nvidia = true;
          }
        }
      }

      if (eglQueryDmaBufFormatsEXT && eglQueryDmaBufModifiersEXT) {
        query_dmabuf_formats(egl_display.get());
      }

      return 0;
    }

    platf::mem_type_e mem_type;
    wl::display_t wl_display;
    std::array<struct dmabuf_format_info_t, MAX_DMABUF_FORMATS> dmabuf_infos;
    int n_dmabuf_infos;
    bool display_is_nvidia = false;  // Track if display GPU is NVIDIA
    std::chrono::nanoseconds delay;
    std::optional<std::uint64_t> last_pts {};
    std::optional<std::uint64_t> last_seq {};
    std::uint64_t sequence {};
    AVRational target_framerate;

  protected:
    // Allow subclasses to access for pipewire requirements setup and stream dead checks
    pipewire_t pipewire;  ///< Pipewire.
    std::shared_ptr<shared_state_t> shared_state;  ///< Shared state.
  };
}  // namespace pipewire
