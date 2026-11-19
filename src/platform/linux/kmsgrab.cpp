/**
 * @file src/platform/linux/kmsgrab.cpp
 * @brief Definitions for KMS screen capture.
 */
// standard includes
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <utility>

// platform includes
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/capability.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// local includes
#include "cuda.h"
#include "display_geometry.h"
#include "graphics.h"
#include "kms_plane.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf {

  namespace kms {

    namespace {  // Keep privileged implementation details anonymous/local to this translation unit

#if !defined(__FreeBSD__)
      /**
       * @brief Temporarily owns CAP_SYS_ADMIN while opening DRM capture resources.
       */
      class cap_sys_admin {
      public:
        cap_sys_admin() {
          caps = cap_get_proc();

          cap_value_t sys_admin = CAP_SYS_ADMIN;
          if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_SET) || cap_set_proc(caps)) {
            BOOST_LOG(error) << "Failed to gain CAP_SYS_ADMIN"sv;
          }
        }

        ~cap_sys_admin() {
          cap_value_t sys_admin = CAP_SYS_ADMIN;
          if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_CLEAR)) {
            BOOST_LOG(error) << "Failed to clear CAP_SYS_ADMIN capability flag"sv;
          } else if (cap_set_proc(caps)) {
            // CAP_SYS_ADMIN may already have been dropped by privileged_drm_worker.
            if (errno != EPERM) {
              BOOST_LOG(error) << "Failed to drop CAP_SYS_ADMIN"sv;
            }
          }
          cap_free(caps);
        }

        cap_t caps;  ///< Caps.
      };
#endif

      /**
       * @brief Reports that the privileged DRM worker rejected a task.
       */
      class privileged_drm_worker_stopped final: public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;  ///< Inherit standard runtime error constructors.
      };

      /**
       * @brief Set up privileged worker thread exclusively for handling DRM capture resources.
       */
      class privileged_drm_worker {
      public:
        static void ensure_started() {
          instance();
        }

        static void drop_worker_privileges() {
          instance().drop_privileges();
        }

        // deliberately align prototype to match path signature via init(const char *path)
        static int open_drm_card_fd_privileged(const char *path) {
          try {
            return instance().run([path] {
              return platf::open_drm_card_fd(path);
            });
          } catch (const privileged_drm_worker_stopped &) {
            return -1;
          }
        }

        static drmModeFB2Ptr drmModeGetFB2_privileged(int fd, uint32_t bufferId) {
          try {
            return instance().run([fd, bufferId] {
              return drmModeGetFB2(fd, bufferId);
            });
          } catch (const privileged_drm_worker_stopped &) {
            return nullptr;
          }
        }

        static drmModeFBPtr drmModeGetFB_privileged(int fd, uint32_t bufferId) {
          try {
            return instance().run([fd, bufferId] {
              return drmModeGetFB(fd, bufferId);
            });
          } catch (const privileged_drm_worker_stopped &) {
            return nullptr;
          }
        }

      private:
        static privileged_drm_worker &instance() {
          static privileged_drm_worker w;
          return w;
        }

        void drop_privileges() {
          instance().run([] {
            platf::drop_elevated_privileges(true);
          });
        }

        privileged_drm_worker():
            thread_ {[this] {
              sigset_t all;
              sigfillset(&all);
              if (pthread_sigmask(SIG_BLOCK, &all, nullptr) != 0) {
                BOOST_LOG(error) << "Failed to block signals in drm_worker"sv;
                queue_.stop();
                return;
              }

              platf::set_thread_name("drm_worker");
              for (;;) {
                auto task = queue_.pop();
                if (!task) {
                  break;
                }
                (*task)();
              }
            }} {
        }

        ~privileged_drm_worker() {
          queue_.stop();
        }

        template<class F>
        auto run(F &&f) -> std::invoke_result_t<F> {
          using R = std::invoke_result_t<F>;
          auto task = std::make_shared<std::packaged_task<R()>>(
            [f = std::forward<F>(f)]() mutable -> R {
#if !defined(__FreeBSD__)
              cap_sys_admin admin;
#endif
              return f();
            }
          );
          auto fut = task->get_future();

          if (!queue_.raise([task]() mutable {
                (*task)();
              })) {
            throw privileged_drm_worker_stopped {"privileged_drm_worker: task rejected (worker stopping)"};
          }

          return fut.get();
        }

        safe::queue_t<std::function<void()>> queue_ {32, safe::queue_t<std::function<void()>>::overflow_policy_e::reject};
        std::jthread thread_;
      };
    }  // namespace

#if defined(__linux__)
    /**
     * @brief Open a DRM card file descriptor using the privileged DRM worker.
     *
     * @param path Path to the DRM card node.
     * @return A file descriptor on success, or `-1` on failure.
     */
    int privileged_open_drm_card_fd(const char *path) {
      return privileged_drm_worker::open_drm_card_fd_privileged(path);
    }

    /**
     * @brief Allows the DRM privileged_drm_worker thread to be constructed early.
     */
    void ensure_privileged_drm_worker_started() {
      privileged_drm_worker::ensure_started();
    }

    /**
     * @brief Allows the DRM privileged_drm_worker thread to drop privileges.
     */
    void drop_drm_worker_privileges() {
      static std::once_flag flag;
      std::call_once(flag, []() {
        privileged_drm_worker::drop_worker_privileges();
      });
    }
#endif

    /**
     * @brief RAII wrapper for DRM framebuffer metadata and GEM handles.
     */
    class wrapper_fb {
    public:
      /**
       * @brief Wrap legacy DRM framebuffer metadata.
       *
       * @param card_fd DRM card file descriptor used to close GEM handles.
       * @param fb Legacy framebuffer metadata returned by drmModeGetFB.
       */
      wrapper_fb(uint32_t card_fd, drmModeFB *fb):
          card_fd {card_fd},
          fb {fb},
          fb_id {fb->fb_id},
          width {fb->width},
          height {fb->height},
          pixel_format {DRM_FORMAT_XRGB8888},
          modifier {DRM_FORMAT_MOD_INVALID} {
        handles.fill(0);
        pitches.fill(0);
        offsets.fill(0);
        handles[0] = fb->handle;
        pitches[0] = fb->pitch;
      }

      /**
       * @brief Wrap DRM framebuffer metadata with per-plane modifiers.
       *
       * @param card_fd DRM card file descriptor used to close GEM handles.
       * @param fb2 Framebuffer metadata returned by drmModeGetFB2.
       */
      wrapper_fb(uint32_t card_fd, drmModeFB2 *fb2):
          card_fd {card_fd},
          fb2 {fb2},
          fb_id {fb2->fb_id},
          width {fb2->width},
          height {fb2->height},
          pixel_format {fb2->pixel_format},
          modifier {(fb2->flags & DRM_MODE_FB_MODIFIERS) ? fb2->modifier : DRM_FORMAT_MOD_INVALID} {
        std::copy_n(fb2->handles, handles.size(), handles.begin());
        std::copy_n(fb2->pitches, pitches.size(), pitches.begin());
        std::copy_n(fb2->offsets, offsets.size(), offsets.begin());
      }

      ~wrapper_fb() {
        std::ranges::for_each(handles, [&](auto &handle) {
          if (handle) {
            struct drm_gem_close close_args = {};
            close_args.handle = handle;

            drmIoctl(card_fd, DRM_IOCTL_GEM_CLOSE, &close_args);
          }
        });

        if (fb) {
          drmModeFreeFB(fb);
        } else if (fb2) {
          drmModeFreeFB2(fb2);
        }
      }

      uint32_t card_fd;  ///< DRM card file descriptor that owns the framebuffer ID.
      drmModeFB *fb = nullptr;  ///< Legacy DRM framebuffer metadata when modifier data is unavailable.
      drmModeFB2 *fb2 = nullptr;  ///< DRM framebuffer metadata with modifier support.
      uint32_t fb_id;  ///< DRM framebuffer ID being captured.
      uint32_t width;  ///< Framebuffer width in pixels.
      uint32_t height;  ///< Framebuffer height in pixels.
      uint32_t pixel_format;  ///< DRM fourcc pixel format for the framebuffer.
      uint64_t modifier;  ///< DRM format modifier describing framebuffer memory layout.
      std::array<uint32_t, 4> handles;  ///< GEM handles for each framebuffer plane.
      std::array<uint32_t, 4> pitches;  ///< Row stride in bytes for each framebuffer plane.
      std::array<uint32_t, 4> offsets;  ///< Byte offset to the first pixel for each framebuffer plane.
    };

    /**
     * @brief DRM plane resource list released with `drmModeFreePlaneResources`.
     */
    using plane_res_t = util::safe_ptr<drmModePlaneRes, &drmModeFreePlaneResources>;
    /**
     * @brief DRM encoder pointer released with `drmModeFreeEncoder`.
     */
    using encoder_t = util::safe_ptr<drmModeEncoder, &drmModeFreeEncoder>;
    /**
     * @brief DRM resource list released with `drmModeFreeResources`.
     */
    using res_t = util::safe_ptr<drmModeRes, &drmModeFreeResources>;
    /**
     * @brief DRM plane pointer released with `drmModeFreePlane`.
     */
    using plane_t = util::safe_ptr<drmModePlane, &drmModeFreePlane>;
    /**
     * @brief DRM framebuffer pointer released with `drmModeFreeFB2`.
     */
    using fb_t = std::unique_ptr<wrapper_fb>;
    /**
     * @brief DRM CRTC pointer released with `drmModeFreeCrtc`.
     */
    using crtc_t = util::safe_ptr<drmModeCrtc, &drmModeFreeCrtc>;
    /**
     * @brief DRM object-property list released with `drmModeFreeObjectProperties`.
     */
    using obj_prop_t = util::safe_ptr<drmModeObjectProperties, &drmModeFreeObjectProperties>;
    /**
     * @brief DRM property pointer released with `drmModeFreeProperty`.
     */
    using prop_t = util::safe_ptr<drmModePropertyRes, &drmModeFreeProperty>;
    /**
     * @brief DRM property-blob pointer released with `drmModeFreePropertyBlob`.
     */
    using prop_blob_t = util::safe_ptr<drmModePropertyBlobRes, &drmModeFreePropertyBlob>;
    /**
     * @brief DRM version pointer released with `drmFreeVersion`.
     */
    using version_t = util::safe_ptr<drmVersion, &drmFreeVersion>;

    /**
     * @brief Counter map keyed by DRM connector type.
     */
    using conn_type_count_t = std::map<std::uint32_t, std::uint32_t>;

    static platf::touch_port_t env_bounds {};  ///< Full desktop origin and extents for absolute input.

    /**
     * @brief Convert a DRM plane type value to a diagnostic string.
     *
     * @param val Value assigned to the synchronized object.
     * @return Static text for the DRM plane type.
     */
    std::string_view plane_type(std::uint64_t val) {
      switch (val) {
        case DRM_PLANE_TYPE_OVERLAY:
          return "DRM_PLANE_TYPE_OVERLAY"sv;
        case DRM_PLANE_TYPE_PRIMARY:
          return "DRM_PLANE_TYPE_PRIMARY"sv;
        case DRM_PLANE_TYPE_CURSOR:
          return "DRM_PLANE_TYPE_CURSOR"sv;
      }

      return "UNKNOWN"sv;
    }

    /**
     * @brief DRM connector and mode information for one display.
     */
    struct connector_t {
      // For example: HDMI-A or HDMI
      std::uint32_t type;  ///< Type.

      // Equals zero if not applicable
      std::uint32_t crtc_id;  ///< Crtc ID.

      // For example HDMI-A-{index} or HDMI-{index}
      std::uint32_t index;  ///< Index.

      // ID of the connector
      std::uint32_t connector_id;  ///< Connector ID.

      bool connected;  ///< Whether the DRM connector is connected.
    };

    /**
     * @brief KMS monitor capture state and DRM resources.
     */
    struct monitor_t {
      // Connector attributes
      std::uint32_t type;  ///< Type.
      std::uint32_t index;  ///< Index.

      // Monitor index in the global list
      std::uint32_t monitor_index;  ///< Monitor index.

      platf::touch_port_t viewport;  ///< Viewport.
    };

    /**
     * @brief DRM card, device path, and render-node metadata.
     */
    struct card_descriptor_t {
      std::string path;  ///< Path.

      std::map<std::uint32_t, monitor_t> crtc_to_monitor;  ///< Crtc to monitor.
    };

    static std::vector<card_descriptor_t> card_descriptors;

    static std::uint32_t from_view(const std::string_view &string) {
#ifndef DOXYGEN
  #define _CONVERT(x, y) \
    if (string == x) \
    return DRM_MODE_CONNECTOR_##y
#endif

      // This list was created from the following sources:
      // https://gitlab.freedesktop.org/mesa/drm/-/blob/main/xf86drmMode.c (drmModeGetConnectorTypeName)
      // https://gitlab.freedesktop.org/wayland/weston/-/blob/e74f2897b9408b6356a555a0ce59146836307ff5/libweston/backend-drm/drm.c#L1458-1477
      // https://github.com/GNOME/mutter/blob/65d481594227ea7188c0416e8e00b57caeea214f/src/backends/meta-monitor-manager.c#L1618-L1639
      _CONVERT("VGA"sv, VGA);
      _CONVERT("DVII"sv, DVII);
      _CONVERT("DVI-I"sv, DVII);
      _CONVERT("DVID"sv, DVID);
      _CONVERT("DVI-D"sv, DVID);
      _CONVERT("DVIA"sv, DVIA);
      _CONVERT("DVI-A"sv, DVIA);
      _CONVERT("Composite"sv, Composite);
      _CONVERT("SVIDEO"sv, SVIDEO);
      _CONVERT("S-Video"sv, SVIDEO);
      _CONVERT("LVDS"sv, LVDS);
      _CONVERT("Component"sv, Component);
      _CONVERT("9PinDIN"sv, 9PinDIN);
      _CONVERT("DIN"sv, 9PinDIN);
      _CONVERT("DisplayPort"sv, DisplayPort);
      _CONVERT("DP"sv, DisplayPort);
      _CONVERT("HDMIA"sv, HDMIA);
      _CONVERT("HDMI-A"sv, HDMIA);
      _CONVERT("HDMI"sv, HDMIA);
      _CONVERT("HDMIB"sv, HDMIB);
      _CONVERT("HDMI-B"sv, HDMIB);
      _CONVERT("TV"sv, TV);
      _CONVERT("eDP"sv, eDP);
      _CONVERT("Meta"sv, VIRTUAL);  ///<  GNOME Shell (Mutter) virtual monitor
      _CONVERT("VIRTUAL"sv, VIRTUAL);
      _CONVERT("Virtual"sv, VIRTUAL);
      _CONVERT("DSI"sv, DSI);
      _CONVERT("DPI"sv, DPI);
      _CONVERT("WRITEBACK"sv, WRITEBACK);
      _CONVERT("Writeback"sv, WRITEBACK);
      _CONVERT("SPI"sv, SPI);
#ifdef DRM_MODE_CONNECTOR_USB
      _CONVERT("USB"sv, USB);
#endif

      // If the string starts with "Unknown", it may have the raw type
      // value appended to the string. Let's try to read it.
      if (string.find("Unknown"sv) == 0) {
        std::uint32_t type;
        std::string null_terminated_string {string};
        if (std::sscanf(null_terminated_string.c_str(), "Unknown%u", &type) == 1) {
          return type;
        }
      }

      BOOST_LOG(error) << "Unknown Monitor connector type ["sv << string << "]: Please report this to the GitHub issue tracker"sv;
      return DRM_MODE_CONNECTOR_Unknown;
    }

    /**
     * @brief Iterator over DRM planes and their associated properties.
     */
    class plane_it_t: public round_robin_util::it_wrap_t<plane_t::element_type, plane_it_t> {
    public:
      /**
       * @brief Create an iterator over DRM planes starting at a specific plane ID.
       *
       * @param fd DRM card file descriptor.
       * @param plane_p Current plane ID pointer.
       * @param end One-past-the-end plane ID pointer.
       */
      plane_it_t(int fd, std::uint32_t *plane_p, std::uint32_t *end):
          fd {fd},
          plane_p {plane_p},
          end {end} {
        load_next_valid_plane();
      }

      /**
       * @brief Create the end iterator for a DRM plane range.
       *
       * @param fd DRM card file descriptor.
       * @param end One-past-the-end plane ID pointer.
       */
      plane_it_t(int fd, std::uint32_t *end):
          fd {fd},
          plane_p {end},
          end {end} {
      }

      /**
       * @brief Load next valid plane.
       */
      void load_next_valid_plane() {
        this->plane.reset();

        for (; plane_p != end; ++plane_p) {
          plane_t next_plane {drmModeGetPlane(fd, *plane_p)};
          if (!next_plane) {
            BOOST_LOG(error) << "Couldn't get drm plane ["sv << (end - plane_p) << "]: "sv << strerror(errno);
            continue;
          }

          this->plane = util::make_shared<plane_t>(next_plane.release());
          break;
        }
      }

      /**
       * @brief Advance the iterator to the next element.
       */
      void inc() {
        ++plane_p;
        load_next_valid_plane();
      }

      /**
       * @brief Compare two iterators for equality.
       *
       * @param other Plane iterator to compare against.
       * @return True when both iterators reference the same plane position.
       */
      bool eq(const plane_it_t &other) const {
        return plane_p == other.plane_p;
      }

      /**
       * @brief Return the currently wrapped value or handle.
       *
       * @return Underlying native handle or object pointer.
       */
      plane_t::pointer get() {
        return plane.get();
      }

      int fd;  ///< DRM card file descriptor.
      std::uint32_t *plane_p;  ///< Current plane ID pointer.
      std::uint32_t *end;  ///< One-past-the-end plane ID pointer.

      util::shared_t<plane_t> plane;  ///< Plane.
    };

    /**
     * @brief Cursor position and visibility for the current capture frame.
     */
    struct cursor_t {
      // Public properties used during blending
      bool visible = false;  ///< Whether the KMS cursor plane is visible.
      std::int32_t x;  ///< X.
      std::int32_t y;  ///< Y.
      std::uint32_t dst_w;  ///< Dst w.
      std::uint32_t dst_h;  ///< Dst h.
      std::uint32_t src_w;  ///< Src w.
      std::uint32_t src_h;  ///< Src h.
      std::vector<std::uint8_t> pixels;  ///< Pixels.
      unsigned long serial;  ///< Serial.

      // Private properties used for tracking cursor changes
      std::uint64_t prop_src_x;  ///< Prop src x.
      std::uint64_t prop_src_y;  ///< Prop src y.
      std::uint64_t prop_src_w;  ///< Prop src w.
      std::uint64_t prop_src_h;  ///< Prop src h.
      std::uint32_t fb_id;  ///< Fb ID.
    };

    /**
     * @brief Return whether a DRM fourcc value is a supported 32-bit KMS cursor format.
     *
     * @param fmt DRM pixel format to inspect.
     * @return True when the format is one of the 8 accepted 32-bit cursor layouts,
     *         otherwise false.
     */
    bool is_cursor_32bpp_format(uint32_t fmt) {
      return fmt == DRM_FORMAT_XRGB8888 ||
             fmt == DRM_FORMAT_XBGR8888 ||
             fmt == DRM_FORMAT_RGBX8888 ||
             fmt == DRM_FORMAT_BGRX8888 ||
             fmt == DRM_FORMAT_ARGB8888 ||
             fmt == DRM_FORMAT_ABGR8888 ||
             fmt == DRM_FORMAT_RGBA8888 ||
             fmt == DRM_FORMAT_BGRA8888;
    }

    /**
     * @brief Convert a single 32-bit pixel to the little-endian ARGB8888 layout used by the capture pipeline.
     *
     * Rewrites the pixel in place to [B, G, R, A] byte order.
     *
     * @param[in,out] p Pointer to the source pixel bytes in the given format.
     * @param[in] fmt DRM pixel format of the source pixel.
     * @param[in] x_is_alpha True for cursor buffers where the kernel hardcodes DRM_FORMAT_HOST_XRGB8888
     *                   (see virtgpu_gem.c) but the fourth byte actually carries per-pixel alpha.
     *                   Set false for scanout buffers or formats with real alpha channels.
     * @return true if the format is supported; false otherwise.
     */
    bool convert_pixel_to_argb8888(std::uint8_t *p, std::uint32_t fmt, bool x_is_alpha = false) {
      const auto c0 = p[0];
      const auto c1 = p[1];
      const auto c2 = p[2];
      const auto c3 = p[3];

      std::uint8_t b;
      std::uint8_t g;
      std::uint8_t r;
      std::uint8_t a;

      switch (fmt) {
        case DRM_FORMAT_XRGB8888:
          b = c0;
          g = c1;
          r = c2;
          a = x_is_alpha ? c3 : 255;
          break;
        case DRM_FORMAT_XBGR8888:
          b = c2;
          g = c1;
          r = c0;
          a = x_is_alpha ? c3 : 255;
          break;
        case DRM_FORMAT_RGBX8888:
          b = c1;
          g = c2;
          r = c3;
          a = x_is_alpha ? c0 : 255;
          break;
        case DRM_FORMAT_BGRX8888:
          b = c3;
          g = c2;
          r = c1;
          a = x_is_alpha ? c0 : 255;
          break;

        case DRM_FORMAT_ARGB8888:
          b = c0;
          g = c1;
          r = c2;
          a = c3;
          break;
        case DRM_FORMAT_ABGR8888:
          b = c2;
          g = c1;
          r = c0;
          a = c3;
          break;
        case DRM_FORMAT_RGBA8888:
          b = c1;
          g = c2;
          r = c3;
          a = c0;
          break;
        case DRM_FORMAT_BGRA8888:
          b = c3;
          g = c2;
          r = c1;
          a = c0;
          break;

        default:
          return false;
      }

      p[0] = b;
      p[1] = g;
      p[2] = r;
      p[3] = a;
      return true;
    }

    /**
     * @brief DRM card, render node, and plane metadata used for KMS capture.
     */
    class card_t {
    public:
      /**
       * @brief Internal connector metadata tuple used while selecting a monitor.
       */
      using connector_interal_t = util::safe_ptr<drmModeConnector, &drmModeFreeConnector>;

      /**
       * @brief Open a DRM connector and cache its monitor metadata.
       *
       * @param path Filesystem path for the DRM device or resource.
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init(const char *path) {
        fd.el = platf::kms::privileged_drm_worker::open_drm_card_fd_privileged(path);
        if (fd.el < 0) {
          return -1;
        }

        version_t ver {drmGetVersion(fd.el)};
        BOOST_LOG(info) << path << " -> "sv << ((ver && ver->name) ? ver->name : "UNKNOWN");

        // Open the render node for this card to share with libva.
        // If it fails, we'll just share the primary node instead.
        char *rendernode_path = drmGetRenderDeviceNameFromFd(fd.el);
        if (rendernode_path) {
          BOOST_LOG(debug) << "Opening render node: "sv << rendernode_path;
          render_fd.el = open(rendernode_path, O_RDWR);
          if (render_fd.el < 0) {
            BOOST_LOG(warning) << "Couldn't open render node: "sv << rendernode_path << ": "sv << strerror(errno);
            render_fd.el = dup(fd.el);
          }
          free(rendernode_path);
        } else {
          BOOST_LOG(warning) << "No render device name for: "sv << path;
          render_fd.el = dup(fd.el);
        }

        if (drmSetClientCap(fd.el, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
          BOOST_LOG(error) << "GPU driver doesn't support universal planes: "sv << path;
          return -1;
        }

        if (drmSetClientCap(fd.el, DRM_CLIENT_CAP_ATOMIC, 1)) {
          BOOST_LOG(warning) << "GPU driver doesn't support atomic mode-setting: "sv << path;
#if defined(SUNSHINE_BUILD_X11)
          // We won't be able to capture the mouse cursor with KMS on non-atomic drivers,
          // so fall back to X11 if it's available and the user didn't explicitly force KMS.
          if (window_system == window_system_e::X11 && config::video.capture != "kms") {
            BOOST_LOG(info) << "Avoiding KMS capture under X11 due to lack of atomic mode-setting"sv;
            return -1;
          }
#endif
          BOOST_LOG(warning) << "Cursor capture may fail without atomic mode-setting support!"sv;
        }

        plane_res.reset(drmModeGetPlaneResources(fd.el));
        if (!plane_res) {
          BOOST_LOG(error) << "Couldn't get drm plane resources"sv;
          return -1;
        }

        return 0;
      }

      /**
       * @brief Return framebuffer metadata for the current CRTC.
       *
       * @param plane DRM plane whose framebuffer metadata should be read.
       * @return Framebuffer metadata wrapper, or nullptr when the framebuffer cannot be read.
       */
      fb_t fb(const drmModePlane *plane) {
        if (auto fb2 = platf::kms::privileged_drm_worker::drmModeGetFB2_privileged(fd.el, plane->fb_id); fb2) {
          return std::make_unique<wrapper_fb>(fd.el, fb2);
        }

        if (auto fb = platf::kms::privileged_drm_worker::drmModeGetFB_privileged(fd.el, plane->fb_id); fb) {
          return std::make_unique<wrapper_fb>(fd.el, fb);
        }

        return nullptr;
      }

      /**
       * @brief Return the DRM CRTC object for the monitor.
       *
       * @param id DRM CRTC ID.
       * @return Owning pointer to the DRM CRTC object.
       */
      crtc_t crtc(std::uint32_t id) {
        return crtc_t {drmModeGetCrtc(fd.el, id)};
      }

      /**
       * @brief Return the DRM encoder object for the connector.
       *
       * @param id DRM encoder ID.
       * @return Owning pointer to the DRM encoder object.
       */
      encoder_t encoder(std::uint32_t id) {
        return encoder_t {drmModeGetEncoder(fd.el, id)};
      }

      /**
       * @brief Return the DRM resource list for the card.
       *
       * @return DRM card resource list.
       */
      res_t res() {
        return res_t {drmModeGetResources(fd.el)};
      }

      /**
       * @brief Check whether nvidia.
       *
       * @return True when the DRM device appears to be driven by NVIDIA.
       */
      bool is_nvidia() {
        version_t ver {drmGetVersion(fd.el)};
        return ver && ver->name && strncmp(ver->name, "nvidia-drm", 10) == 0;
      }

      /**
       * @brief Check whether cursor.
       *
       * @param plane_id Plane ID.
       * @return True when the DRM plane is a cursor plane.
       */
      bool is_cursor(std::uint32_t plane_id) {
        auto props = plane_props(plane_id);
        for (auto &[prop, val] : props) {
          if (prop->name == "type"sv) {
            if (val == DRM_PLANE_TYPE_CURSOR) {
              return true;
            } else {
              return false;
            }
          }
        }

        return false;
      }

      /**
       * @brief Look up a DRM property value by property name.
       *
       * @param props DRM property collection to inspect.
       * @param name DRM property name to search for.
       * @return Property value when the property exists.
       */
      std::optional<std::uint64_t> prop_value_by_name(const std::vector<std::pair<prop_t, std::uint64_t>> &props, std::string_view name) {
        for (auto &[prop, val] : props) {
          if (prop->name == name) {
            return val;
          }
        }
        return std::nullopt;
      }

      /**
       * @brief Get panel orientation.
       *
       * @param plane_id Plane ID.
       * @return DRM rotation bitmask for the panel orientation.
       */
      std::uint32_t get_panel_orientation(std::uint32_t plane_id) {
        auto props = plane_props(plane_id);
        auto value = prop_value_by_name(props, "rotation"sv);
        if (value.has_value()) {
          return *value;
        }

        BOOST_LOG(error) << "Failed to determine panel orientation, defaulting to landscape.";
        return DRM_MODE_ROTATE_0;
      }

      /**
       * @brief Get crtc index by ID.
       *
       * @param crtc_id Crtc ID.
       * @return Zero-based CRTC index, or -1 when the CRTC ID is unknown.
       */
      int get_crtc_index_by_id(std::uint32_t crtc_id) {
        auto resources = res();
        for (int i = 0; i < resources->count_crtcs; i++) {
          if (resources->crtcs[i] == crtc_id) {
            return i;
          }
        }
        return -1;
      }

      /**
       * @brief Return the DRM connector object for the display.
       *
       * @param id DRM connector ID.
       * @return Owning pointer to the DRM connector object.
       */
      connector_interal_t connector(std::uint32_t id) {
        return connector_interal_t {drmModeGetConnector(fd.el, id)};
      }

      /**
       * @brief Refresh the monitor list reported by the display server.
       *
       * @param conn_type_count Conn type count.
       * @return Connector descriptors for monitors known to the DRM card.
       */
      std::vector<connector_t> monitors(conn_type_count_t &conn_type_count) {
        auto resources = res();
        if (!resources) {
          BOOST_LOG(error) << "Couldn't get connector resources"sv;
          return {};
        }

        std::vector<connector_t> monitors;
        std::for_each_n(resources->connectors, resources->count_connectors, [this, &conn_type_count, &monitors](std::uint32_t id) {
          auto conn = connector(id);

          std::uint32_t crtc_id = 0;

          if (conn->encoder_id) {
            auto enc = encoder(conn->encoder_id);
            if (enc) {
              crtc_id = enc->crtc_id;
            }
          }

          ++conn_type_count[conn->connector_type];

          monitors.emplace_back(connector_t {
            conn->connector_type,
            crtc_id,
            conn->connector_type_id,
            conn->connector_id,
            conn->connection == DRM_MODE_CONNECTED,
          });
        });

        return monitors;
      }

      /**
       * @brief Return the DRM file descriptor owned by the object.
       *
       * @param handle GEM handle exported by the DRM framebuffer.
       * @return DMA-BUF file descriptor for the GEM handle.
       */
      file_t handleFD(std::uint32_t handle) {
        file_t fb_fd;

        if (const auto status = drmPrimeHandleToFD(fd.el, handle, 0 /* flags */, &fb_fd.el); status) {
          return {};
        }

        return fb_fd;
      }

      /**
       * @brief Return DRM object properties for the resource.
       *
       * @param id DRM object ID.
       * @param type DRM object type.
       * @return DRM properties and their current values.
       */
      std::vector<std::pair<prop_t, std::uint64_t>> props(std::uint32_t id, std::uint32_t type) {
        obj_prop_t obj_prop {drmModeObjectGetProperties(fd.el, id, type)};
        if (!obj_prop) {
          return {};
        }

        std::vector<std::pair<prop_t, std::uint64_t>> props;
        props.reserve(obj_prop->count_props);

        for (auto x = 0; x < obj_prop->count_props; ++x) {
          props.emplace_back(drmModeGetProperty(fd.el, obj_prop->props[x]), obj_prop->prop_values[x]);
        }

        return props;
      }

      /**
       * @brief Return DRM plane properties.
       *
       * @param id DRM plane ID.
       * @return Plane properties and their current values.
       */
      std::vector<std::pair<prop_t, std::uint64_t>> plane_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_PLANE);
      }

      /**
       * @brief Return DRM CRTC properties.
       *
       * @param id DRM CRTC ID.
       * @return CRTC properties and their current values.
       */
      std::vector<std::pair<prop_t, std::uint64_t>> crtc_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_CRTC);
      }

      /**
       * @brief Return DRM connector properties.
       *
       * @param id DRM connector ID.
       * @return Connector properties and their current values.
       */
      std::vector<std::pair<prop_t, std::uint64_t>> connector_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_CONNECTOR);
      }

      /**
       * @brief Fetch DRM plane metadata by plane-list index.
       *
       * @param index Zero-based index into the DRM plane resource list.
       * @return Plane metadata for the requested DRM plane.
       */
      plane_t operator[](std::uint32_t index) {
        return plane_t {drmModeGetPlane(fd.el, plane_res->planes[index])};
      }

      /**
       * @brief Return the number of items in the wrapped DRM collection.
       *
       * @return Number of DRM planes reported by the card.
       */
      std::uint32_t count() {
        return plane_res->count_planes;
      }

      /**
       * @brief Return an iterator to the first byte in the buffer view.
       *
       * @return Iterator to the first element.
       */
      plane_it_t begin() const {
        return plane_it_t {fd.el, plane_res->planes, plane_res->planes + plane_res->count_planes};
      }

      /**
       * @brief Return an iterator one past the final byte in the buffer view.
       *
       * @return Iterator one past the last element.
       */
      plane_it_t end() const {
        return plane_it_t {fd.el, plane_res->planes + plane_res->count_planes};
      }

      file_t fd;  ///< DRM card file descriptor.
      file_t render_fd;  ///< Render fd.
      plane_res_t plane_res;  ///< Plane res.
    };

    /**
     * @brief Build a lookup from DRM CRTC IDs to monitor descriptors.
     *
     * @param connectors DRM connector list being mapped to monitors.
     * @return Map keyed by CRTC ID.
     */
    std::map<std::uint32_t, monitor_t> map_crtc_to_monitor(const std::vector<connector_t> &connectors) {
      std::map<std::uint32_t, monitor_t> result;

      for (auto &connector : connectors) {
        result.try_emplace(connector.crtc_id, monitor_t {
                                                .type = connector.type,
                                                .index = connector.index,
                                              });
      }

      return result;
    }

    /**
     * @brief Multi-source KMS image assembled for encoding.
     */
    struct kms_img_t: public img_t {
      ~kms_img_t() override {
        delete[] data;
        data = nullptr;
      }
    };

    /**
     * @brief Write a debug log representation of the input packet.
     *
     * @param plane DRM plane whose framebuffer metadata should be read.
     * @param fb Framebuffer object to bind or update.
     * @param crtc DRM CRTC identifier to map.
     */
    void print(plane_t::pointer plane, fb_t::pointer fb, crtc_t::pointer crtc) {
      if (crtc) {
        BOOST_LOG(debug) << "crtc("sv << crtc->x << ", "sv << crtc->y << ')';
        BOOST_LOG(debug) << "crtc("sv << crtc->width << ", "sv << crtc->height << ')';
        BOOST_LOG(debug) << "plane->possible_crtcs == "sv << plane->possible_crtcs;
      }

      BOOST_LOG(debug)
        << "x("sv << plane->x
        << ") y("sv << plane->y
        << ") crtc_x("sv << plane->crtc_x
        << ") crtc_y("sv << plane->crtc_y
        << ") crtc_id("sv << plane->crtc_id
        << ')';

      BOOST_LOG(debug)
        << "Resolution: "sv << fb->width << 'x' << fb->height
        << ": Pitch: "sv << fb->pitches[0]
        << ": Offset: "sv << fb->offsets[0];

      std::stringstream ss;

      ss << "Format ["sv;
      std::for_each_n(plane->formats, plane->count_formats - 1, [&ss](auto format) {
        ss << util::view(format) << ", "sv;
      });

      ss << util::view(plane->formats[plane->count_formats - 1]) << ']';

      BOOST_LOG(debug) << ss.str();
    }

    /**
     * @brief Map display name to monitor index
     *
     * @param display_name Name of the display to determine monitor index for (or monitor index string value)
     * @return monitor's display index
     */
    static int64_t map_display_name_to_monitor_index(const std::string_view &display_name) {
      // Handle (legacy) monitor index strings by converting them to integer
      if (display_name.empty() || std::ranges::all_of(display_name, ::isdigit)) {
        return util::from_view(display_name);
      }
      // display_name is a connector name (not empty and containing non-digits)
      for (auto &card_descriptor : kms::card_descriptors) {
        for (const auto &monitor_descriptor : card_descriptor.crtc_to_monitor | std::views::values) {
          if (display_name == std::format("{}-{}", drmModeGetConnectorTypeName(monitor_descriptor.type), monitor_descriptor.index)) {
            BOOST_LOG(info) << "Mapped '"sv << display_name << "' to kmsgrab monitor index " << monitor_descriptor.monitor_index;
            return monitor_descriptor.monitor_index;
          }
        }
      }
      BOOST_LOG(warning) << "Couldn't map '"sv << display_name << "' to a monitor index. Falling back to first monitor in list (index=0).";
      return 0;  // Fallback to first index if nothing can be matched
    }

    /**
     * @brief Base KMS display capture backend shared by RAM and VRAM paths.
     */
    class display_t: public platf::display_t {
    public:
      /**
       * @brief Explicitly initialize common KMS display state for the requested memory type.
       *
       * @param mem_type Mem type.
       */
      explicit display_t(mem_type_e mem_type):
          platf::display_t(),
          mem_type {mem_type} {
      }

      /**
       * @brief Initialize the base KMS capture state for the selected monitor.
       *
       * @param display_name Display name.
       * @param config Configuration values to apply.
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init(const std::string &display_name, const ::video::config_t &config) {
        delay = ::video::capture_frame_interval(config);

        int monitor_index = map_display_name_to_monitor_index(display_name);
        int monitor = 0;

        fs::path card_dir {"/dev/dri"sv};
        for (auto &entry : fs::directory_iterator {card_dir}) {
          auto file = entry.path().filename();

          auto filestring = file.generic_string();
          if (filestring.size() < 4 || std::string_view {filestring}.substr(0, 4) != "card"sv) {
            continue;
          }

          kms::card_t card;
          if (card.init(entry.path().c_str())) {
            continue;
          }

          // Skip non-Nvidia cards if we're looking for CUDA devices
          // unless NVENC is selected manually by the user
          if (mem_type == mem_type_e::cuda && !card.is_nvidia()) {
            BOOST_LOG(debug) << file << " is not a CUDA device"sv;
            if (config::video.encoder != "nvenc") {
              continue;
            }
          }

          // Skip Nvidia cards if we're looking for VAAPI devices
          // This is important for hybrid GPU laptops where the display
          // may be connected through NVIDIA but rendering happens on Intel
          if (mem_type == mem_type_e::vaapi && card.is_nvidia()) {
            BOOST_LOG(debug) << file << " is an NVIDIA card, skipping for VAAPI"sv;
            continue;
          }

          auto end = std::end(card);
          for (auto plane = std::begin(card); plane != end; ++plane) {
            // Skip unused planes
            if (!plane->fb_id) {
              continue;
            }

            if (card.is_cursor(plane->plane_id)) {
              continue;
            }

            if (monitor != monitor_index) {
              ++monitor;
              continue;
            }

            auto fb = card.fb(plane.get());
            if (!fb) {
              BOOST_LOG(error) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
              return -1;
            }

            if (!fb->handles[0]) {
              BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
              return -1;
            }

            for (int i = 0; i < 4; ++i) {
              if (!fb->handles[i]) {
                break;
              }

              auto fb_fd = card.handleFD(fb->handles[i]);
              if (fb_fd.el < 0) {
                BOOST_LOG(error) << "Couldn't get primary file descriptor for Framebuffer ["sv << fb->fb_id << "]: "sv << strerror(errno);
                continue;
              }
            }

            auto crtc = card.crtc(plane->crtc_id);
            if (!crtc) {
              BOOST_LOG(error) << "Couldn't get CRTC info: "sv << strerror(errno);
              continue;
            }

            BOOST_LOG(info) << "Found monitor for DRM screencasting"sv;

            // We need to find the correct /dev/dri/card{nr} to correlate the crtc_id with the monitor descriptor
            auto pos = std::find_if(std::begin(card_descriptors), std::end(card_descriptors), [&](card_descriptor_t &cd) {
              return cd.path == filestring;
            });

            if (pos == std::end(card_descriptors)) {
              // This code path shouldn't happen, but it's there just in case.
              // card_descriptors is part of the guesswork after all.
              BOOST_LOG(error) << "Couldn't find ["sv << entry.path() << "]: This shouldn't have happened :/"sv;
              return -1;
            }

            // TODO: surf_sd = fb->to_sd();

            kms::print(plane.get(), fb.get(), crtc.get());

            img_width = fb->width;
            img_height = fb->height;
            img_offset_x = crtc->x;
            img_offset_y = crtc->y;

            this->env_offset_x = ::platf::kms::env_bounds.offset_x;
            this->env_offset_y = ::platf::kms::env_bounds.offset_y;
            this->env_width = ::platf::kms::env_bounds.width;
            this->env_height = ::platf::kms::env_bounds.height;

            this->env_logical_width = ::platf::kms::env_bounds.logical_width;
            this->env_logical_height = ::platf::kms::env_bounds.logical_height;

            auto monitor_pos = pos->crtc_to_monitor.find(plane->crtc_id);
            if (monitor_pos != std::end(pos->crtc_to_monitor)) {
              auto &viewport = monitor_pos->second.viewport;

              width = viewport.width;
              height = viewport.height;

              logical_width = viewport.logical_width;
              logical_height = viewport.logical_height;

              switch (card.get_panel_orientation(plane->plane_id)) {
                case DRM_MODE_ROTATE_270:
                  BOOST_LOG(debug) << "Detected panel orientation at 90, swapping width and height.";
                  width = viewport.height;
                  height = viewport.width;
                  break;
                case DRM_MODE_ROTATE_90:
                case DRM_MODE_ROTATE_180:
                  BOOST_LOG(warning) << "Panel orientation is unsupported, screen capture may not work correctly.";
                  break;
              }

              offset_x = viewport.offset_x;
              offset_y = viewport.offset_y;
              output_name = viewport.output_name;
            }

            // This code path shouldn't happen, but it's there just in case.
            // crtc_to_monitor is part of the guesswork after all.
            else {
              BOOST_LOG(warning) << "Couldn't find crtc_id, this shouldn't have happened :\\"sv;
              width = crtc->width;
              height = crtc->height;
              offset_x = crtc->x;
              offset_y = crtc->y;
            }

            plane_id = plane->plane_id;
            crtc_id = plane->crtc_id;
            crtc_index = card.get_crtc_index_by_id(plane->crtc_id);

            // Find the connector for this CRTC
            kms::conn_type_count_t conn_type_count;
            for (auto &connector : card.monitors(conn_type_count)) {
              if (connector.crtc_id == crtc_id) {
                BOOST_LOG(info) << "Found connector ID ["sv << connector.connector_id << ']';

                connector_id = connector.connector_id;

                auto connector_props = card.connector_props(*connector_id);
                hdr_metadata_blob_id = card.prop_value_by_name(connector_props, "HDR_OUTPUT_METADATA"sv);
              }
            }

            this->card = std::move(card);
            goto break_loop;
          }
        }

        BOOST_LOG(error) << "Couldn't find monitor ["sv << monitor_index << ']';
        return -1;

      // Neatly break from nested for loop
      break_loop:

        // Look for the cursor plane for this CRTC
        cursor_plane_id = -1;
        auto end = std::end(card);
        for (auto plane = std::begin(card); plane != end; ++plane) {
          if (!card.is_cursor(plane->plane_id)) {
            continue;
          }

          // NB: We do not skip unused planes here because cursor planes
          // will look unused if the cursor is currently hidden.

          if (!(plane->possible_crtcs & (1 << crtc_index))) {
            // Skip cursor planes for other CRTCs
            continue;
          } else if (plane->possible_crtcs != (1 << crtc_index)) {
            // We assume a 1:1 mapping between cursor planes and CRTCs, which seems to
            // match the behavior of drivers in the real world. If it's violated, we'll
            // proceed anyway but print a warning in the log.
            BOOST_LOG(warning) << "Cursor plane spans multiple CRTCs!"sv;
          }

          BOOST_LOG(info) << "Found cursor plane ["sv << plane->plane_id << ']';
          cursor_plane_id = plane->plane_id;
          break;
        }

        if (cursor_plane_id < 0) {
          BOOST_LOG(warning) << "No KMS cursor plane found. Cursor may not be displayed while streaming!"sv;
        }

        return 0;
      }

      /**
       * @brief Report whether the active display mode is HDR.
       *
       * @return True when the active display mode is HDR.
       */
      bool is_hdr() override {
        if (!hdr_metadata_blob_id.has_value() || *hdr_metadata_blob_id == 0) {
          return false;
        }

        prop_blob_t hdr_metadata_blob {drmModeGetPropertyBlob(card.fd.el, *hdr_metadata_blob_id)};
        if (hdr_metadata_blob == nullptr) {
          BOOST_LOG(error) << "Unable to get HDR metadata blob: "sv << strerror(errno);
          return false;
        }

        if (hdr_metadata_blob->length < sizeof(uint32_t) + sizeof(hdr_metadata_infoframe)) {
          BOOST_LOG(error) << "HDR metadata blob is too small: "sv << hdr_metadata_blob->length;
          return false;
        }

        auto raw_metadata = (hdr_output_metadata *) hdr_metadata_blob->data;
        if (raw_metadata->metadata_type != 0) {  // HDMI_STATIC_METADATA_TYPE1
          BOOST_LOG(error) << "Unknown HDMI_STATIC_METADATA_TYPE value: "sv << raw_metadata->metadata_type;
          return false;
        }

        if (raw_metadata->hdmi_metadata_type1.metadata_type != 0) {  // Static Metadata Type 1
          BOOST_LOG(error) << "Unknown secondary metadata type value: "sv << raw_metadata->hdmi_metadata_type1.metadata_type;
          return false;
        }

        // We only support Traditional Gamma SDR or SMPTE 2084 PQ HDR EOTFs.
        // Print a warning if we encounter any others.
        switch (raw_metadata->hdmi_metadata_type1.eotf) {
          case 0:  // HDMI_EOTF_TRADITIONAL_GAMMA_SDR
            return false;
          case 1:  // HDMI_EOTF_TRADITIONAL_GAMMA_HDR
            BOOST_LOG(warning) << "Unsupported HDR EOTF: Traditional Gamma"sv;
            return true;
          case 2:  // HDMI_EOTF_SMPTE_ST2084
            return true;
          case 3:  // HDMI_EOTF_BT_2100_HLG
            BOOST_LOG(warning) << "Unsupported HDR EOTF: HLG"sv;
            return true;
          default:
            BOOST_LOG(warning) << "Unsupported HDR EOTF: "sv << raw_metadata->hdmi_metadata_type1.eotf;
            return true;
        }
      }

      /**
       * @brief Read HDR metadata for the active display mode.
       *
       * @param metadata Output structure populated with HDR metadata.
       * @return True when HDR metadata was written to the output structure.
       */
      bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
        // This performs all the metadata validation
        if (!is_hdr()) {
          return false;
        }

        prop_blob_t hdr_metadata_blob {drmModeGetPropertyBlob(card.fd.el, *hdr_metadata_blob_id)};
        if (hdr_metadata_blob == nullptr) {
          BOOST_LOG(error) << "Unable to get HDR metadata blob: "sv << strerror(errno);
          return false;
        }

        auto raw_metadata = (hdr_output_metadata *) hdr_metadata_blob->data;

        for (int i = 0; i < 3; i++) {
          metadata.displayPrimaries[i].x = raw_metadata->hdmi_metadata_type1.display_primaries[i].x;
          metadata.displayPrimaries[i].y = raw_metadata->hdmi_metadata_type1.display_primaries[i].y;
        }

        metadata.whitePoint.x = raw_metadata->hdmi_metadata_type1.white_point.x;
        metadata.whitePoint.y = raw_metadata->hdmi_metadata_type1.white_point.y;
        metadata.maxDisplayLuminance = raw_metadata->hdmi_metadata_type1.max_display_mastering_luminance;
        metadata.minDisplayLuminance = raw_metadata->hdmi_metadata_type1.min_display_mastering_luminance;
        metadata.maxContentLightLevel = raw_metadata->hdmi_metadata_type1.max_cll;
        metadata.maxFrameAverageLightLevel = raw_metadata->hdmi_metadata_type1.max_fall;

        return true;
      }

      /**
       * @brief Update cached cursor-plane image and position.
       */
      void update_cursor() {
        if (cursor_plane_id < 0) {
          return;
        }

        plane_t plane {drmModeGetPlane(card.fd.el, cursor_plane_id)};

        std::optional<std::int32_t> prop_crtc_x;
        std::optional<std::int32_t> prop_crtc_y;
        std::optional<std::uint32_t> prop_crtc_w;
        std::optional<std::uint32_t> prop_crtc_h;

        std::optional<std::uint64_t> prop_src_x;
        std::optional<std::uint64_t> prop_src_y;
        std::optional<std::uint64_t> prop_src_w;
        std::optional<std::uint64_t> prop_src_h;

        auto props = card.plane_props(cursor_plane_id);
        for (auto &[prop, val] : props) {
          if (prop->name == "CRTC_X"sv) {
            prop_crtc_x = val;
          } else if (prop->name == "CRTC_Y"sv) {
            prop_crtc_y = val;
          } else if (prop->name == "CRTC_W"sv) {
            prop_crtc_w = val;
          } else if (prop->name == "CRTC_H"sv) {
            prop_crtc_h = val;
          } else if (prop->name == "SRC_X"sv) {
            prop_src_x = val;
          } else if (prop->name == "SRC_Y"sv) {
            prop_src_y = val;
          } else if (prop->name == "SRC_W"sv) {
            prop_src_w = val;
          } else if (prop->name == "SRC_H"sv) {
            prop_src_h = val;
          }
        }

        if (!prop_crtc_w.has_value() || !prop_crtc_h.has_value() || !prop_crtc_x.has_value() || !prop_crtc_y.has_value()) {
          BOOST_LOG(error) << "Cursor plane is missing required plane CRTC properties!"sv;
          BOOST_LOG(error) << "Atomic mode-setting must be enabled to capture the cursor!"sv;
          cursor_plane_id = -1;
          captured_cursor.visible = false;
          return;
        }
        if (!prop_src_x.has_value() || !prop_src_y.has_value() || !prop_src_w.has_value() || !prop_src_h.has_value()) {
          BOOST_LOG(error) << "Cursor plane is missing required plane SRC properties!"sv;
          BOOST_LOG(error) << "Atomic mode-setting must be enabled to capture the cursor!"sv;
          cursor_plane_id = -1;
          captured_cursor.visible = false;
          return;
        }

        // Update the cursor position and size unconditionally
        captured_cursor.x = *prop_crtc_x;
        captured_cursor.y = *prop_crtc_y;
        captured_cursor.dst_w = *prop_crtc_w;
        captured_cursor.dst_h = *prop_crtc_h;

        // We're technically cheating a bit here by assuming that we can detect
        // changes to the cursor plane via property adjustments. If this isn't
        // true, we'll really have to mmap() the dmabuf and draw that every time.
        bool cursor_dirty = false;

        if (!plane->fb_id) {
          captured_cursor.visible = false;
          captured_cursor.fb_id = 0;
        } else if (plane->fb_id != captured_cursor.fb_id) {
          BOOST_LOG(debug) << "Refreshing cursor image after FB changed"sv;
          cursor_dirty = true;
        } else if (*prop_src_x != captured_cursor.prop_src_x || *prop_src_y != captured_cursor.prop_src_y || *prop_src_w != captured_cursor.prop_src_w || *prop_src_h != captured_cursor.prop_src_h) {
          BOOST_LOG(debug) << "Refreshing cursor image after source dimensions changed"sv;
          cursor_dirty = true;
        }

        // If the cursor is dirty, map it so we can download the new image
        if (cursor_dirty) {
          auto fb = card.fb(plane.get());
          if (!fb || !fb->handles[0]) {
            // This means the cursor is not currently visible
            captured_cursor.visible = false;
            return;
          }

          // All known cursor planes in the wild are 32 bpp
          if (!is_cursor_32bpp_format(fb->pixel_format)) {
            BOOST_LOG(error) << "Unsupported cursor format: "sv << fb->pixel_format;
            captured_cursor.visible = false;
            cursor_plane_id = -1;
            return;
          }

          // All known cursor planes in the wild require linear buffers
          if (fb->modifier != DRM_FORMAT_MOD_LINEAR && fb->modifier != DRM_FORMAT_MOD_INVALID) {
            BOOST_LOG(error) << "Unsupported non-linear cursor modifier: "sv << fb->modifier;
            captured_cursor.visible = false;
            cursor_plane_id = -1;
            return;
          }

          // The SRC_* properties are in Q16.16 fixed point, so convert to integers
          auto src_x = *prop_src_x >> 16;
          auto src_y = *prop_src_y >> 16;
          auto src_w = *prop_src_w >> 16;
          auto src_h = *prop_src_h >> 16;

          // Check for a legal source rectangle
          if (src_x + src_w > fb->width || src_y + src_h > fb->height) {
            BOOST_LOG(error) << "Illegal source size: ["sv << src_x + src_w << ',' << src_y + src_h << "] > ["sv << fb->width << ',' << fb->height << ']';
            captured_cursor.visible = false;
            return;
          }

          file_t plane_fd = card.handleFD(fb->handles[0]);
          if (plane_fd.el < 0) {
            captured_cursor.visible = false;
            return;
          }

          // We will map the entire region, but only copy what the source rectangle specifies
          size_t mapped_size = ((size_t) fb->pitches[0]) * fb->height;
          void *mapped_data = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, plane_fd.el, fb->offsets[0]);

          // If we got ENOSYS back, let's try to map it as a dumb buffer instead (required for Nvidia GPUs)
          if (mapped_data == MAP_FAILED && errno == ENOSYS) {
            drm_mode_map_dumb map = {};
            map.handle = fb->handles[0];
            if (drmIoctl(card.fd.el, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
              BOOST_LOG(error) << "Failed to map cursor FB as dumb buffer: "sv << strerror(errno);
              captured_cursor.visible = false;
              return;
            }

            mapped_data = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, card.fd.el, map.offset);
          }

          if (mapped_data == MAP_FAILED) {
            BOOST_LOG(error) << "Failed to mmap cursor FB: "sv << strerror(errno);
            captured_cursor.visible = false;
            return;
          }

          captured_cursor.pixels.resize(src_w * src_h * 4);

          // Prepare to read the dmabuf from the CPU
          struct dma_buf_sync sync;
          sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
          drmIoctl(plane_fd.el, DMA_BUF_IOCTL_SYNC, &sync);

          // If the image is tightly packed, copy it in one shot
          if (fb->pitches[0] == src_w * 4 && src_x == 0) {
            memcpy(captured_cursor.pixels.data(), &((std::uint8_t *) mapped_data)[src_y * fb->pitches[0]], src_h * fb->pitches[0]);
          } else {
            // Copy row by row to deal with mismatched pitch or an X offset
            auto pixel_dst = captured_cursor.pixels.data();
            for (int y = 0; y < src_h; y++) {
              memcpy(&pixel_dst[y * (src_w * 4)], &((std::uint8_t *) mapped_data)[(y + src_y) * fb->pitches[0] + (src_x * 4)], src_w * 4);
            }
          }

          // End the CPU read and unmap the dmabuf
          sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
          drmIoctl(plane_fd.el, DMA_BUF_IOCTL_SYNC, &sync);

          munmap(mapped_data, mapped_size);

          if (fb->pixel_format != DRM_FORMAT_ARGB8888) {
            // Convert the copied pixels to the format expected by the rest of code.
            for (std::size_t i = 0; i < captured_cursor.pixels.size(); i += 4) {
              convert_pixel_to_argb8888(
                captured_cursor.pixels.data() + i,
                fb->pixel_format,
                true
              );
            }
          }

          captured_cursor.visible = true;
          captured_cursor.src_w = src_w;
          captured_cursor.src_h = src_h;
          captured_cursor.prop_src_x = *prop_src_x;
          captured_cursor.prop_src_y = *prop_src_y;
          captured_cursor.prop_src_w = *prop_src_w;
          captured_cursor.prop_src_h = *prop_src_h;
          captured_cursor.fb_id = plane->fb_id;
          ++captured_cursor.serial;
        }
      }

      /**
       * @brief Refresh cached platform state from the operating system.
       *
       * @param file DMA-BUF file descriptors exported for the current framebuffer.
       * @param sd EGL surface descriptor to import.
       * @param frame_timestamp Output timestamp assigned to the captured frame.
       * @return Capture status after refreshing framebuffer and cursor state.
       */
      inline capture_e refresh(file_t *file, egl::surface_descriptor_t *sd, std::optional<std::chrono::steady_clock::time_point> &frame_timestamp) {
        // Check for a change in HDR metadata
        if (connector_id.has_value()) {
          auto connector_props = card.connector_props(*connector_id);
          if (hdr_metadata_blob_id != card.prop_value_by_name(connector_props, "HDR_OUTPUT_METADATA"sv)) {
            BOOST_LOG(info) << "Reinitializing capture after HDR metadata change"sv;
            return capture_e::reinit;
          }
        }

        plane_t plane {drmModeGetPlane(card.fd.el, plane_id)};
        frame_timestamp = std::chrono::steady_clock::now();

        // A plane that stays without a framebuffer will not get the picture back by itself:
        // the compositor has moved it to another plane, or restarted. Pick a plane again.
        if (const auto now = std::chrono::steady_clock::now(); empty_plane_timer.update(plane->fb_id != 0, now)) {
          BOOST_LOG(info) << "Reinitializing capture: plane ["sv << plane_id << "] has had no framebuffer for "sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(empty_plane_timer.empty_for(now)).count() << "ms"sv;
          return capture_e::reinit;
        }

        auto fb = card.fb(plane.get());
        if (!fb) {
          // This can happen if the display is being reconfigured while streaming
          BOOST_LOG(warning) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
          return capture_e::timeout;
        }

        if (!fb->handles[0]) {
          BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
          return capture_e::capture_error;
        }

        for (int y = 0; y < 4; ++y) {
          if (!fb->handles[y]) {
            // setting sd->fds[y] to a negative value indicates that sd->offsets[y] and sd->pitches[y]
            // are uninitialized and contain invalid values.
            sd->fds[y] = -1;
            // It's not clear whether there could still be valid handles left.
            // So, continue anyway.
            // TODO: Is this redundant?
            continue;
          }

          file[y] = card.handleFD(fb->handles[y]);
          if (file[y].el < 0) {
            BOOST_LOG(error) << "Couldn't get primary file descriptor for Framebuffer ["sv << fb->fb_id << "]: "sv << strerror(errno);
            return capture_e::capture_error;
          }

          sd->fds[y] = file[y].el;
          sd->offsets[y] = fb->offsets[y];
          sd->pitches[y] = fb->pitches[y];
        }

        sd->width = fb->width;
        sd->height = fb->height;
        sd->modifier = fb->modifier;
        sd->fourcc = fb->pixel_format;

        if (
          fb->width != img_width ||
          fb->height != img_height
        ) {
          return capture_e::reinit;
        }

        update_cursor();

        return capture_e::ok;
      }

      mem_type_e mem_type;  ///< Mem type.

      std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

      int img_width;  ///< Img width.
      int img_height;  ///< Img height.
      int img_offset_x;  ///< Img offset x.
      int img_offset_y;  ///< Img offset y.

      kms::empty_plane_timer_t empty_plane_timer {250ms};  ///< Reinitializes capture once the captured plane stays empty.

      int plane_id;  ///< Plane ID.
      int crtc_id;  ///< Crtc ID.
      int crtc_index;  ///< Crtc index.

      std::optional<uint32_t> connector_id;  ///< Connector ID.
      std::optional<uint64_t> hdr_metadata_blob_id;  ///< HDR metadata blob ID.

      int cursor_plane_id;  ///< Cursor plane ID.
      cursor_t captured_cursor {};  ///< Captured cursor.

      card_t card;  ///< Card.
    };

    /**
     * @brief KMS capture backend that copies frames into system memory.
     */
    class display_ram_t: public display_t {
    public:
      /**
       * @brief Explicitly initialize a KMS display backend that copies frames through RAM.
       *
       * @param mem_type Mem type.
       */
      explicit display_ram_t(mem_type_e mem_type):
          display_t(mem_type) {
      }

      /**
       * @brief Initialize KMS capture that copies frames into system memory.
       *
       * @param display_name Display name.
       * @param config Configuration values to apply.
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init(const std::string &display_name, const ::video::config_t &config) {
        if (!gbm::create_device) {
          BOOST_LOG(warning) << "libgbm not initialized"sv;
          return -1;
        }

        if (display_t::init(display_name, config)) {
          return -1;
        }

        gbm.reset(gbm::create_device(card.fd.el));
        if (!gbm) {
          BOOST_LOG(error) << "Couldn't create GBM device: ["sv << util::hex(eglGetError()).to_string_view() << ']';
          return -1;
        }

        display = egl::make_display(gbm.get());
        if (!display) {
          return -1;
        }

        auto ctx_opt = egl::make_ctx(display.get());
        if (!ctx_opt) {
          return -1;
        }

        ctx = std::move(*ctx_opt);

        return 0;
      }

      /**
       * @brief Capture KMS frames until capture stops or the display needs reinitialization.
       *
       * @param push_captured_image_cb Callback receiving a captured image or a capture timeout.
       * @param pull_free_image_cb Callback providing an available image buffer.
       * @param cursor Whether the cursor should be included in the captured image.
       * @return Final capture status reported to the streaming pipeline.
       */
      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        using enum platf::capture_e;

        auto next_frame = std::chrono::steady_clock::now();

        sleep_overshoot_logger.reset();

        while (true) {
          platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
          switch (status) {
            case reinit:
            case capture_error:
            case interrupted:
              return status;
            case timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return ok;
              }
              break;
            case ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
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
      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi) {
          return va::make_avcodec_encode_device(width, height, false);
        }
#endif

#ifdef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          return cuda::make_avcodec_encode_device(width, height, false);
        }
#endif

        return std::make_unique<avcodec_encode_device_t>();
      }

      /**
       * @brief Blend the captured cursor plane into a KMS frame.
       *
       * @param img Image or frame object to read from or populate.
       */
      void blend_cursor(img_t &img) {
        // TODO: Cursor scaling is not supported in this codepath.
        // We always draw the cursor at the source size.
        auto pixels = (int *) img.data;

        int32_t screen_height = img.height;
        int32_t screen_width = img.width;

        // This is the position in the target that we will start drawing the cursor
        auto cursor_x = std::max<int32_t>(0, captured_cursor.x - img_offset_x);
        auto cursor_y = std::max<int32_t>(0, captured_cursor.y - img_offset_y);

        // If the cursor is partially off screen, the coordinates may be negative
        // which means we will draw the top-right visible portion of the cursor only.
        auto cursor_delta_x = cursor_x - std::max<int32_t>(-captured_cursor.src_w, captured_cursor.x - img_offset_x);
        auto cursor_delta_y = cursor_y - std::max<int32_t>(-captured_cursor.src_h, captured_cursor.y - img_offset_y);

        auto delta_height = std::min<uint32_t>(captured_cursor.src_h, std::max<int32_t>(0, screen_height - cursor_y)) - cursor_delta_y;
        auto delta_width = std::min<uint32_t>(captured_cursor.src_w, std::max<int32_t>(0, screen_width - cursor_x)) - cursor_delta_x;
        for (auto y = 0; y < delta_height; ++y) {
          // Offset into the cursor image to skip drawing the parts of the cursor image that are off screen
          //
          // NB: We must access the elements via the data() function because cursor_end may point to the
          // the first element beyond the valid range of the vector. Using vector's [] operator in that
          // manner is undefined behavior (and triggers errors when using debug libc++), while doing the
          // same with an array is fine.
          auto cursor_begin = (uint32_t *) &captured_cursor.pixels.data()[((y + cursor_delta_y) * captured_cursor.src_w + cursor_delta_x) * 4];
          auto cursor_end = (uint32_t *) &captured_cursor.pixels.data()[((y + cursor_delta_y) * captured_cursor.src_w + delta_width + cursor_delta_x) * 4];

          auto pixels_begin = &pixels[(y + cursor_y) * (img.row_pitch / img.pixel_pitch) + cursor_x];

          std::for_each(cursor_begin, cursor_end, [&](uint32_t cursor_pixel) {
            auto colors_in = (uint8_t *) pixels_begin;

            auto alpha = (*(uint *) &cursor_pixel) >> 24u;
            if (alpha == 255) {
              *pixels_begin = cursor_pixel;
            } else {
              auto colors_out = (uint8_t *) &cursor_pixel;
              colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
              colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
              colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
            }
            ++pixels_begin;
          });
        }
      }

      /**
       * @brief Capture a display frame into the provided image object.
       *
       * @param pull_free_image_cb Callback that provides an available image buffer.
       * @param img_out Captured KMS image returned to the streaming pipeline.
       * @param timeout Maximum time to wait for the operation.
       * @param cursor Cursor image or visibility state to composite.
       * @return Capture status reported to the streaming pipeline.
       */
      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
        std::array<file_t, 4> fb_fd;

        egl::surface_descriptor_t sd;

        std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
        auto status = refresh(fb_fd.data(), &sd, frame_timestamp);
        if (status != capture_e::ok) {
          return status;
        }

        auto rgb_opt = egl::import_source(display.get(), sd);

        if (!rgb_opt) {
          return capture_e::capture_error;
        }

        auto &rgb = *rgb_opt;

        gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);

        // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
        int h;
        int w;
        gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        BOOST_LOG(debug) << "width and height: w "sv << w << " h "sv << h;

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        gl::ctx.GetTextureSubImage(rgb->tex[0], 0, img_offset_x, img_offset_y, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);

        img_out->frame_timestamp = frame_timestamp;

        if (cursor && captured_cursor.visible) {
          blend_cursor(*img_out);
        }

        return capture_e::ok;
      }

      /**
       * @brief Allocate an image buffer compatible with this display backend.
       *
       * @return Allocated img object, or null when unavailable.
       */
      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<kms_img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->pixel_pitch * width;
        img->data = new std::uint8_t[height * img->row_pitch];

        return img;
      }

      /**
       * @brief Populate a fallback image when real capture data is unavailable.
       *
       * @param img Image or frame object to read from or populate.
       * @return Capture status reported to the streaming pipeline.
       */
      int dummy_img(platf::img_t *img) override {
        return 0;
      }

      gbm::gbm_t gbm;  ///< GBM device used for buffer allocation.
      egl::display_t display;  ///< EGL display created from the GBM device.
      egl::ctx_t ctx;  ///< EGL context used to copy KMS frames into RAM.
    };

    /**
     * @brief KMS capture backend that exports frames as GPU resources.
     */
    class display_vram_t: public display_t {
    public:
      /**
       * @brief Explicitly initialize a KMS display backend that exports frames as GPU resources.
       *
       * @param mem_type Mem type.
       */
      explicit display_vram_t(mem_type_e mem_type):
          display_t(mem_type) {
      }

      /**
       * @brief Create AVCodec encode device.
       *
       * @param pix_fmt Sunshine pixel format to convert or allocate for.
       * @return Constructed AVCodec encode device object.
       */
      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi) {
          return va::make_avcodec_encode_device(width, height, dup(card.render_fd.el), img_offset_x, img_offset_y, true);
        }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
        if (mem_type == mem_type_e::vulkan) {
          return vk::make_avcodec_encode_device_vram(width, height, img_offset_x, img_offset_y);
        }
#endif

#ifdef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          return cuda::make_avcodec_gl_encode_device(width, height, img_offset_x, img_offset_y);
        }
#endif

        BOOST_LOG(error) << "Unsupported pixel format for egl::display_vram_t: "sv << platf::from_pix_fmt(pix_fmt);
        return nullptr;
      }

      /**
       * @brief Allocate an image buffer compatible with this display backend.
       *
       * @return Allocated img object, or null when unavailable.
       */
      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<egl::img_descriptor_t>();

        img->width = width;
        img->height = height;
        img->serial = std::numeric_limits<decltype(img->serial)>::max();
        img->data = nullptr;
        img->pixel_pitch = 4;

        img->sequence = 0;
        img->sd.fds.fill(-1);

        return img;
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
       * @brief Capture KMS frames until capture stops or the display needs reinitialization.
       *
       * @param push_captured_image_cb Callback receiving a captured image or a capture timeout.
       * @param pull_free_image_cb Callback providing an available image buffer.
       * @param cursor Whether the cursor should be included in the captured image.
       * @return Final capture status reported to the streaming pipeline.
       */
      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        using enum platf::capture_e;

        auto next_frame = std::chrono::steady_clock::now();

        sleep_overshoot_logger.reset();

        while (true) {
          platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
          switch (status) {
            case reinit:
            case capture_error:
            case interrupted:
              return status;
            case timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return ok;
              }
              break;
            case ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
              return status;
          }
        }

        return ok;
      }

      /**
       * @brief Capture a display frame into the provided image object.
       *
       * @tparam PullFreeImage Callback type used to retrieve a free capture image.
       * @param pull_free_image_cb Callback retrieving a free capture image.
       * @param img_out Captured KMS image descriptor returned to the streaming pipeline.
       * @param cursor Cursor image or visibility state to composite.
       * @return Capture status reported to the streaming pipeline.
       */
      template<typename PullFreeImage>
      capture_e snapshot(const PullFreeImage &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds /* timeout */, bool cursor) {
        std::array<file_t, 4> fb_fd;

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (egl::img_descriptor_t *) img_out.get();
        img->reset();

        if (const auto status = refresh(fb_fd.data(), &img->sd, img->frame_timestamp); status != capture_e::ok) {
          return status;
        }

        img->sequence = ++sequence;

        if (cursor && captured_cursor.visible) {
          // Copy new cursor pixel data if it's been updated
          if (img->serial != captured_cursor.serial) {
            img->buffer = captured_cursor.pixels;
            img->serial = captured_cursor.serial;
          }

          img->x = captured_cursor.x;
          img->y = captured_cursor.y;
          img->src_w = captured_cursor.src_w;
          img->src_h = captured_cursor.src_h;
          img->width = captured_cursor.dst_w;
          img->height = captured_cursor.dst_h;
          img->pixel_pitch = 4;
          img->row_pitch = img->pixel_pitch * img->width;
          img->data = img->buffer.data();
        } else {
          img->data = nullptr;
        }

        for (auto x = 0; x < 4; ++x) {
          fb_fd[x].release();
        }
        return capture_e::ok;
      }

      /**
       * @brief Initialize KMS capture that exports frames as GPU resources.
       *
       * @param display_name Display name.
       * @param config Configuration values to apply.
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init(const std::string &display_name, const ::video::config_t &config) {
        if (display_t::init(display_name, config)) {
          return -1;
        }

#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi && !va::validate(card.render_fd.el)) {
          BOOST_LOG(warning) << "Monitor "sv << display_name << " doesn't support hardware encoding. Reverting back to GPU -> RAM -> GPU"sv;
          return -1;
        }
#endif

#ifndef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          BOOST_LOG(warning) << "Attempting to use NVENC without CUDA support. Reverting back to GPU -> RAM -> GPU"sv;
          return -1;
        }
#endif

        return 0;
      }

      std::uint64_t sequence {};  ///< Monotonic capture sequence assigned to KMS frames.
    };

  }  // namespace kms

  /**
   * @brief Create a KMS display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return KMS display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    if (hwdevice_type == mem_type_e::vaapi || hwdevice_type == mem_type_e::cuda || hwdevice_type == mem_type_e::vulkan) {
      auto disp = std::make_shared<kms::display_vram_t>(hwdevice_type);

      if (!disp->init(display_name, config)) {
        return disp;
      }

      // In the case of failure, attempt the old method for VAAPI
    }

    auto disp = std::make_shared<kms::display_ram_t>(hwdevice_type);

    if (disp->init(display_name, config)) {
      return nullptr;
    }

    return disp;
  }

  /**
   * On Wayland, it's not possible to determine the position of the monitor on the desktop with KMS.
   * Wayland does allow applications to query attached monitors on the desktop,
   * however, the naming scheme is not standardized across implementations.
   *
   * As a result, correlating the KMS output to the wayland outputs is guess work at best.
   * But, it's necessary for absolute mouse coordinates to work.
   *
   * This is an ugly hack :(
   * @param cds Capture display state used by the KMS backend.
   */
  void correlate_to_wayland(std::vector<kms::card_descriptor_t> &cds) {
    auto monitors = wl::monitors();

    BOOST_LOG(info) << "-------- Start of KMS monitor list --------"sv;

    for (auto &monitor : monitors) {
      std::string_view name = monitor->name;

      // Try to convert names in the format:
      // {type}-{index}
      // {index} is n'th occurrence of {type}
      auto index_begin = name.find_last_of('-');

      std::uint32_t index;
      if (index_begin == std::string_view::npos) {
        index = 1;
      } else {
        index = std::max<int64_t>(1, util::from_view(name.substr(index_begin + 1)));
      }

      auto type = kms::from_view(name.substr(0, index_begin));

      for (auto &card_descriptor : cds) {
        for (auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
          if (monitor_descriptor.index == index && monitor_descriptor.type == type) {
            monitor_descriptor.viewport.output_name = monitor->name;
            monitor_descriptor.viewport.offset_x = monitor->viewport.offset_x;
            monitor_descriptor.viewport.offset_y = monitor->viewport.offset_y;
            monitor_descriptor.viewport.logical_width = monitor->viewport.logical_width;
            monitor_descriptor.viewport.logical_height = monitor->viewport.logical_height;

            // A sanity check, it's guesswork after all.
            if (
              monitor_descriptor.viewport.width != monitor->viewport.width ||
              monitor_descriptor.viewport.height != monitor->viewport.height
            ) {
              BOOST_LOG(warning)
                << "Mismatch on expected Resolution compared to actual resolution: "sv
                << monitor_descriptor.viewport.width << 'x' << monitor_descriptor.viewport.height
                << " vs "sv
                << monitor->viewport.width << 'x' << monitor->viewport.height;
            }

            BOOST_LOG(info) << "Monitor " << monitor_descriptor.monitor_index << " is "sv << name << ": "sv << monitor->description;
            goto break_for_loop;
          }
        }
      }
    break_for_loop:

      BOOST_LOG(verbose) << "Reduced to name: "sv << name << ": "sv << index;
    }

    BOOST_LOG(info) << "--------- End of KMS monitor list ---------"sv;
  }

  // A list of names of displays accepted as display_name
  /**
   * @brief Enumerate display names accepted by the KMS backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @return KMS display names, or an empty list when KMS capture is unavailable.
   */
  std::vector<std::string> kms_display_names(mem_type_e hwdevice_type) {
    int count = 0;

    if (!fs::exists("/dev/dri")) {
      BOOST_LOG(warning) << "Couldn't find /dev/dri, kmsgrab won't be enabled"sv;
      return {};
    }

    if (!gbm::create_device) {
      BOOST_LOG(warning) << "libgbm not initialized"sv;
      return {};
    }

    kms::conn_type_count_t conn_type_count;

    std::vector<kms::card_descriptor_t> cds;
    std::vector<std::string> display_names;

    fs::path card_dir {"/dev/dri"sv};
    for (auto &entry : fs::directory_iterator {card_dir}) {
      auto file = entry.path().filename();

      auto filestring = file.generic_string();
      if (std::string_view {filestring}.substr(0, 4) != "card"sv) {
        continue;
      }

      kms::card_t card;
      if (card.init(entry.path().c_str())) {
        continue;
      }

      // Skip non-Nvidia cards if we're looking for CUDA devices
      // unless NVENC is selected manually by the user
      if (hwdevice_type == mem_type_e::cuda && !card.is_nvidia()) {
        BOOST_LOG(debug) << file << " is not a CUDA device"sv;
        if (config::video.encoder == "nvenc") {
          BOOST_LOG(warning) << "Using NVENC with your display connected to a different GPU may not work properly!"sv;
        } else {
          continue;
        }
      }

      // Skip Nvidia cards if we're looking for VAAPI devices
      // This is important for hybrid GPU laptops where the display
      // may be connected through NVIDIA but rendering happens on Intel
      if (hwdevice_type == mem_type_e::vaapi && card.is_nvidia()) {
        BOOST_LOG(debug) << file << " is an NVIDIA card, skipping for VAAPI"sv;
        continue;
      }

      auto crtc_to_monitor = kms::map_crtc_to_monitor(card.monitors(conn_type_count));

      auto end = std::end(card);
      for (auto plane = std::begin(card); plane != end; ++plane) {
        // Skip unused planes
        if (!plane->fb_id) {
          continue;
        }

        if (card.is_cursor(plane->plane_id)) {
          continue;
        }

        auto fb = card.fb(plane.get());
        if (!fb) {
          BOOST_LOG(error) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
          continue;
        }

        if (!fb->handles[0]) {
          BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
#if defined(SUNSHINE_BUILD_FLATPAK) || defined(SUNSHINE_BUILD_APPIMAGE)
          BOOST_LOG((config::video.capture == "kms") ? fatal : error)
            << "AppImage and Flatpak do not support KMS capture. Use another capture method."sv;
#endif
          break;
        }

        // This appears to return the offset of the monitor
        auto crtc = card.crtc(plane->crtc_id);
        if (!crtc) {
          BOOST_LOG(error) << "Couldn't get CRTC info: "sv << strerror(errno);
          continue;
        }

        auto it = crtc_to_monitor.find(plane->crtc_id);
        if (it != std::end(crtc_to_monitor)) {
          it->second.viewport = platf::touch_port_t {
            .offset_x = (int) crtc->x,
            .offset_y = (int) crtc->y,
            .width = (int) crtc->width,
            .height = (int) crtc->height,
          };
          it->second.monitor_index = count;
        }

        kms::print(plane.get(), fb.get(), crtc.get());
        it->second.viewport.output_name = std::format("{}-{}", drmModeGetConnectorTypeName(it->second.type), it->second.index);
        display_names.emplace_back(it->second.viewport.output_name);
        count++;
      }

      cds.emplace_back(kms::card_descriptor_t {
        std::move(file),
        std::move(crtc_to_monitor),
      });
    }

    if (!wl::init()) {
      correlate_to_wayland(cds);
    }

    platf::desktop_geometry_t desktop;
    for (const auto &card_descriptor : cds) {
      for (const auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
        desktop.add(monitor_descriptor.viewport);
      }
    }
    kms::env_bounds = desktop.bounds();
    BOOST_LOG(debug) << "Desktop resolution: "sv << kms::env_bounds.width << 'x' << kms::env_bounds.height;

    kms::card_descriptors = std::move(cds);

    BOOST_LOG(debug) << "Final KMS display_names return list: " << (display_names | std::views::join_with(' ') | std::ranges::to<std::string>());
    return display_names;
  }

}  // namespace platf
