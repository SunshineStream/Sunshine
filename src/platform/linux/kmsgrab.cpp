/**
 * @file src/platform/linux/kmsgrab.cpp
 * @brief todo
 */
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/capability.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <filesystem>
#include <thread>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "src/video.h"

#include "cuda.h"
#include "graphics.h"
#include "vaapi.h"
#include "wayland.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf {

  namespace kms {

    class cap_sys_admin {
    public:
      cap_sys_admin() {
        caps = cap_get_proc();

        cap_value_t sys_admin = CAP_SYS_ADMIN;
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_SET) || cap_set_proc(caps)) {
          BOOST_LOG(error) << "Failed to gain CAP_SYS_ADMIN";
        }
      }

      ~cap_sys_admin() {
        cap_value_t sys_admin = CAP_SYS_ADMIN;
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_CLEAR) || cap_set_proc(caps)) {
          BOOST_LOG(error) << "Failed to drop CAP_SYS_ADMIN";
        }
        cap_free(caps);
      }

      cap_t caps;
    };

    class wrapper_fb {
    public:
      wrapper_fb(drmModeFB *fb):
          fb { fb }, fb_id { fb->fb_id }, width { fb->width }, height { fb->height } {
        pixel_format = DRM_FORMAT_XRGB8888;
        modifier = DRM_FORMAT_MOD_INVALID;
        std::fill_n(handles, 4, 0);
        std::fill_n(pitches, 4, 0);
        std::fill_n(offsets, 4, 0);
        handles[0] = fb->handle;
        pitches[0] = fb->pitch;
      }

      wrapper_fb(drmModeFB2 *fb2):
          fb2 { fb2 }, fb_id { fb2->fb_id }, width { fb2->width }, height { fb2->height } {
        pixel_format = fb2->pixel_format;
        modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS) ? fb2->modifier : DRM_FORMAT_MOD_INVALID;

        memcpy(handles, fb2->handles, sizeof(handles));
        memcpy(pitches, fb2->pitches, sizeof(pitches));
        memcpy(offsets, fb2->offsets, sizeof(offsets));
      }

      ~wrapper_fb() {
        if (fb) {
          drmModeFreeFB(fb);
        }
        else if (fb2) {
          drmModeFreeFB2(fb2);
        }
      }

      drmModeFB *fb = nullptr;
      drmModeFB2 *fb2 = nullptr;
      uint32_t fb_id;
      uint32_t width;
      uint32_t height;
      uint32_t pixel_format;
      uint64_t modifier;
      uint32_t handles[4];
      uint32_t pitches[4];
      uint32_t offsets[4];
    };

    using plane_res_t = util::safe_ptr<drmModePlaneRes, drmModeFreePlaneResources>;
    using encoder_t = util::safe_ptr<drmModeEncoder, drmModeFreeEncoder>;
    using res_t = util::safe_ptr<drmModeRes, drmModeFreeResources>;
    using plane_t = util::safe_ptr<drmModePlane, drmModeFreePlane>;
    using fb_t = std::unique_ptr<wrapper_fb>;
    using crtc_t = util::safe_ptr<drmModeCrtc, drmModeFreeCrtc>;
    using obj_prop_t = util::safe_ptr<drmModeObjectProperties, drmModeFreeObjectProperties>;
    using prop_t = util::safe_ptr<drmModePropertyRes, drmModeFreeProperty>;
    using prop_blob_t = util::safe_ptr<drmModePropertyBlobRes, drmModeFreePropertyBlob>;
    using version_t = util::safe_ptr<drmVersion, drmFreeVersion>;

    using conn_type_count_t = std::map<std::uint32_t, std::uint32_t>;

    static int env_width;
    static int env_height;

    std::string_view
    plane_type(std::uint64_t val) {
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

    struct connector_t {
      // For example: HDMI-A or HDMI
      std::uint32_t type;

      // Equals zero if not applicable
      std::uint32_t crtc_id;

      // For example HDMI-A-{index} or HDMI-{index}
      std::uint32_t index;

      // ID of the connector
      std::uint32_t connector_id;

      bool connected;
    };

    struct monitor_t {
      // Connector attributes
      std::uint32_t type;
      std::uint32_t index;

      // Monitor index in the global list
      std::uint32_t monitor_index;

      platf::touch_port_t viewport;
    };

    struct card_descriptor_t {
      std::string path;

      std::map<std::uint32_t, monitor_t> crtc_to_monitor;
    };

    static std::vector<card_descriptor_t> card_descriptors;

    static std::uint32_t
    from_view(const std::string_view &string) {
#define _CONVERT(x, y) \
  if (string == x) return DRM_MODE_CONNECTOR_##y

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
        std::string null_terminated_string { string };
        if (std::sscanf(null_terminated_string.c_str(), "Unknown%u", &type) == 1) {
          return type;
        }
      }

      BOOST_LOG(error) << "Unknown Monitor connector type ["sv << string << "]: Please report this to the GitHub issue tracker"sv;
      return DRM_MODE_CONNECTOR_Unknown;
    }

    class plane_it_t: public round_robin_util::it_wrap_t<plane_t::element_type, plane_it_t> {
    public:
      plane_it_t(int fd, std::uint32_t *plane_p, std::uint32_t *end):
          fd { fd }, plane_p { plane_p }, end { end } {
        load_next_valid_plane();
      }

      plane_it_t(int fd, std::uint32_t *end):
          fd { fd }, plane_p { end }, end { end } {}

      void
      load_next_valid_plane() {
        this->plane.reset();

        for (; plane_p != end; ++plane_p) {
          plane_t plane = drmModeGetPlane(fd, *plane_p);
          if (!plane) {
            BOOST_LOG(error) << "Couldn't get drm plane ["sv << (end - plane_p) << "]: "sv << strerror(errno);
            continue;
          }

          this->plane = util::make_shared<plane_t>(plane.release());
          break;
        }
      }

      void
      inc() {
        ++plane_p;
        load_next_valid_plane();
      }

      bool
      eq(const plane_it_t &other) const {
        return plane_p == other.plane_p;
      }

      plane_t::pointer
      get() {
        return plane.get();
      }

      int fd;
      std::uint32_t *plane_p;
      std::uint32_t *end;

      util::shared_t<plane_t> plane;
    };

    struct cursor_t {
      // Public properties used during blending
      bool visible = false;
      std::int32_t x, y;
      std::uint32_t dst_w, dst_h;
      std::uint32_t src_w, src_h;
      std::vector<std::uint8_t> pixels;
      unsigned long serial;

      // Private properties used for tracking cursor changes
      std::uint64_t prop_src_x, prop_src_y, prop_src_w, prop_src_h;
      std::uint32_t fb_id;
    };

    class card_t {
    public:
      using connector_interal_t = util::safe_ptr<drmModeConnector, drmModeFreeConnector>;

      int
      init(const char *path) {
        cap_sys_admin admin;
        fd.el = open(path, O_RDWR);

        if (fd.el < 0) {
          BOOST_LOG(error) << "Couldn't open: "sv << path << ": "sv << strerror(errno);
          return -1;
        }

        version_t ver { drmGetVersion(fd.el) };
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
        }
        else {
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

      fb_t
      fb(plane_t::pointer plane) {
        cap_sys_admin admin;

        auto fb2 = drmModeGetFB2(fd.el, plane->fb_id);
        if (fb2) {
          return std::make_unique<wrapper_fb>(fb2);
        }

        auto fb = drmModeGetFB(fd.el, plane->fb_id);
        if (fb) {
          return std::make_unique<wrapper_fb>(fb);
        }

        return nullptr;
      }

      crtc_t
      crtc(std::uint32_t id) {
        return drmModeGetCrtc(fd.el, id);
      }

      encoder_t
      encoder(std::uint32_t id) {
        return drmModeGetEncoder(fd.el, id);
      }

      res_t
      res() {
        return drmModeGetResources(fd.el);
      }

      bool
      is_nvidia() {
        version_t ver { drmGetVersion(fd.el) };
        return ver && ver->name && strncmp(ver->name, "nvidia-drm", 10) == 0;
      }

      bool
      is_cursor(std::uint32_t plane_id) {
        auto props = plane_props(plane_id);
        for (auto &[prop, val] : props) {
          if (prop->name == "type"sv) {
            if (val == DRM_PLANE_TYPE_CURSOR) {
              return true;
            }
            else {
              return false;
            }
          }
        }

        return false;
      }

      std::optional<std::uint64_t>
      prop_value_by_name(const std::vector<std::pair<prop_t, std::uint64_t>> &props, std::string_view name) {
        for (auto &[prop, val] : props) {
          if (prop->name == name) {
            return val;
          }
        }
        return std::nullopt;
      }

      std::uint32_t
      get_panel_orientation(std::uint32_t plane_id) {
        auto props = plane_props(plane_id);
        auto value = prop_value_by_name(props, "rotation"sv);
        if (value) {
          return *value;
        }

        BOOST_LOG(error) << "Failed to determine panel orientation, defaulting to landscape.";
        return DRM_MODE_ROTATE_0;
      }

      int
      get_crtc_index_by_id(std::uint32_t crtc_id) {
        auto resources = res();
        for (int i = 0; i < resources->count_crtcs; i++) {
          if (resources->crtcs[i] == crtc_id) {
            return i;
          }
        }
        return -1;
      }

      connector_interal_t
      connector(std::uint32_t id) {
        return drmModeGetConnector(fd.el, id);
      }

      std::vector<connector_t>
      monitors(conn_type_count_t &conn_type_count) {
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

          auto index = ++conn_type_count[conn->connector_type];

          monitors.emplace_back(connector_t {
            conn->connector_type,
            crtc_id,
            index,
            conn->connector_id,
            conn->connection == DRM_MODE_CONNECTED,
          });
        });

        return monitors;
      }

      file_t
      handleFD(std::uint32_t handle) {
        file_t fb_fd;

        auto status = drmPrimeHandleToFD(fd.el, handle, 0 /* flags */, &fb_fd.el);
        if (status) {
          return {};
        }

        return fb_fd;
      }

      std::vector<std::pair<prop_t, std::uint64_t>>
      props(std::uint32_t id, std::uint32_t type) {
        obj_prop_t obj_prop = drmModeObjectGetProperties(fd.el, id, type);
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

      std::vector<std::pair<prop_t, std::uint64_t>>
      plane_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_PLANE);
      }

      std::vector<std::pair<prop_t, std::uint64_t>>
      crtc_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_CRTC);
      }

      std::vector<std::pair<prop_t, std::uint64_t>>
      connector_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_CONNECTOR);
      }

      plane_t
      operator[](std::uint32_t index) {
        return drmModeGetPlane(fd.el, plane_res->planes[index]);
      }

      std::uint32_t
      count() {
        return plane_res->count_planes;
      }

      plane_it_t
      begin() const {
        return plane_it_t { fd.el, plane_res->planes, plane_res->planes + plane_res->count_planes };
      }

      plane_it_t
      end() const {
        return plane_it_t { fd.el, plane_res->planes + plane_res->count_planes };
      }

      file_t fd;
      file_t render_fd;
      plane_res_t plane_res;
    };

    std::map<std::uint32_t, monitor_t>
    map_crtc_to_monitor(const std::vector<connector_t> &connectors) {
      std::map<std::uint32_t, monitor_t> result;

      for (auto &connector : connectors) {
        result.emplace(connector.crtc_id,
          monitor_t {
            connector.type,
            connector.index,
          });
      }

      return result;
    }

    struct kms_img_t: public img_t {
      ~kms_img_t() override {
        delete[] data;
        data = nullptr;
      }
    };

    void
    print(plane_t::pointer plane, fb_t::pointer fb, crtc_t::pointer crtc) {
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

    class display_t: public platf::display_t {
    public:
      display_t(mem_type_e mem_type):
          platf::display_t(), mem_type { mem_type } {}

      int
      init(const std::string &display_name, const ::video::config_t &config) {
        delay = std::chrono::nanoseconds { 1s } / config.framerate;

        int monitor_index = util::from_view(display_name);
        int monitor = 0;

        fs::path card_dir { "/dev/dri"sv };
        for (auto &entry : fs::directory_iterator { card_dir }) {
          auto file = entry.path().filename();

          auto filestring = file.generic_string();
          if (filestring.size() < 4 || std::string_view { filestring }.substr(0, 4) != "card"sv) {
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

            this->env_width = ::platf::kms::env_width;
            this->env_height = ::platf::kms::env_height;

            auto monitor = pos->crtc_to_monitor.find(plane->crtc_id);
            if (monitor != std::end(pos->crtc_to_monitor)) {
              auto &viewport = monitor->second.viewport;

              width = viewport.width;
              height = viewport.height;

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
          }
          else if (plane->possible_crtcs != (1 << crtc_index)) {
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

      bool
      is_hdr() {
        if (!hdr_metadata_blob_id || *hdr_metadata_blob_id == 0) {
          return false;
        }

        prop_blob_t hdr_metadata_blob = drmModeGetPropertyBlob(card.fd.el, *hdr_metadata_blob_id);
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

      bool
      get_hdr_metadata(SS_HDR_METADATA &metadata) {
        // This performs all the metadata validation
        if (!is_hdr()) {
          return false;
        }

        prop_blob_t hdr_metadata_blob = drmModeGetPropertyBlob(card.fd.el, *hdr_metadata_blob_id);
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

      void
      update_cursor() {
        if (cursor_plane_id < 0) {
          return;
        }

        plane_t plane = drmModeGetPlane(card.fd.el, cursor_plane_id);

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
          }
          else if (prop->name == "CRTC_Y"sv) {
            prop_crtc_y = val;
          }
          else if (prop->name == "CRTC_W"sv) {
            prop_crtc_w = val;
          }
          else if (prop->name == "CRTC_H"sv) {
            prop_crtc_h = val;
          }
          else if (prop->name == "SRC_X"sv) {
            prop_src_x = val;
          }
          else if (prop->name == "SRC_Y"sv) {
            prop_src_y = val;
          }
          else if (prop->name == "SRC_W"sv) {
            prop_src_w = val;
          }
          else if (prop->name == "SRC_H"sv) {
            prop_src_h = val;
          }
        }

        if (!prop_crtc_w || !prop_crtc_h || !prop_crtc_x || !prop_crtc_y) {
          BOOST_LOG(error) << "Cursor plane is missing required plane CRTC properties!"sv;
          BOOST_LOG(error) << "Atomic mode-setting must be enabled to capture the cursor!"sv;
          cursor_plane_id = -1;
          captured_cursor.visible = false;
          return;
        }
        if (!prop_src_x || !prop_src_y || !prop_src_w || !prop_src_h) {
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
        }
        else if (plane->fb_id != captured_cursor.fb_id) {
          BOOST_LOG(debug) << "Refreshing cursor image after FB changed"sv;
          cursor_dirty = true;
        }
        else if (*prop_src_x != captured_cursor.prop_src_x ||
                 *prop_src_y != captured_cursor.prop_src_y ||
                 *prop_src_w != captured_cursor.prop_src_w ||
                 *prop_src_h != captured_cursor.prop_src_h) {
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

          // All known cursor planes in the wild are ARGB8888
          if (fb->pixel_format != DRM_FORMAT_ARGB8888) {
            BOOST_LOG(error) << "Unsupported non-ARGB8888 cursor format: "sv << fb->pixel_format;
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
          }
          else {
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

      inline capture_e
      refresh(file_t *file, egl::surface_descriptor_t *sd, std::optional<std::chrono::steady_clock::time_point> &frame_timestamp) {
        // Check for a change in HDR metadata
        if (connector_id) {
          auto connector_props = card.connector_props(*connector_id);
          if (hdr_metadata_blob_id != card.prop_value_by_name(connector_props, "HDR_OUTPUT_METADATA"sv)) {
            BOOST_LOG(info) << "Reinitializing capture after HDR metadata change"sv;
            return capture_e::reinit;
          }
        }

        plane_t plane = drmModeGetPlane(card.fd.el, plane_id);
        frame_timestamp = std::chrono::steady_clock::now();

        auto fb = card.fb(plane.get());
        if (!fb) {
          // This can happen if the display is being reconfigured while streaming
          BOOST_LOG(warning) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
          return capture_e::timeout;
        }

        if (!fb->handles[0]) {
          BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
          return capture_e::error;
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
            return capture_e::error;
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
          fb->height != img_height) {
          return capture_e::reinit;
        }

        update_cursor();

        return capture_e::ok;
      }

      mem_type_e mem_type;

      std::chrono::nanoseconds delay;

      int img_width, img_height;
      int img_offset_x, img_offset_y;

      int plane_id;
      int crtc_id;
      int crtc_index;

      std::optional<uint32_t> connector_id;
      std::optional<uint64_t> hdr_metadata_blob_id;

      int cursor_plane_id;
      cursor_t captured_cursor {};

      card_t card;
    };

    class display_ram_t: public display_t {
    public:
      display_ram_t(mem_type_e mem_type):
          display_t(mem_type) {}

      int
      init(const std::string &display_name, const ::video::config_t &config) {
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

      capture_e
      capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();

        sleep_overshoot_tracker.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();

          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
          }
          now = std::chrono::steady_clock::now();
          std::chrono::nanoseconds overshoot_ns = now - next_frame;
          log_sleep_overshoot(overshoot_ns);

          next_frame += delay;
          if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
            next_frame = now + delay;
          }

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::error:
            case platf::capture_e::interrupted:
              return status;
            case platf::capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return platf::capture_e::ok;
              }
              break;
            case platf::capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return platf::capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
              return status;
          }
        }

        return capture_e::ok;
      }

      std::unique_ptr<avcodec_encode_device_t>
      make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
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

      void
      blend_cursor(img_t &img) {
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
            }
            else {
              auto colors_out = (uint8_t *) &cursor_pixel;
              colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
              colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
              colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
            }
            ++pixels_begin;
          });
        }
      }

      capture_e
      snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
        file_t fb_fd[4];

        egl::surface_descriptor_t sd;

        std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
        auto status = refresh(fb_fd, &sd, frame_timestamp);
        if (status != capture_e::ok) {
          return status;
        }

        auto rgb_opt = egl::import_source(display.get(), sd);

        if (!rgb_opt) {
          return capture_e::error;
        }

        auto &rgb = *rgb_opt;

        gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);

        // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
        int w, h;
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

      std::shared_ptr<img_t>
      alloc_img() override {
        auto img = std::make_shared<kms_img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->pixel_pitch * width;
        img->data = new std::uint8_t[height * img->row_pitch];

        return img;
      }

      int
      dummy_img(platf::img_t *img) override {
        return 0;
      }

      gbm::gbm_t gbm;
      egl::display_t display;
      egl::ctx_t ctx;
    };

    class display_vram_t: public display_t {
    public:
      display_vram_t(mem_type_e mem_type):
          display_t(mem_type) {}

      std::unique_ptr<avcodec_encode_device_t>
      make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi) {
          return va::make_avcodec_encode_device(width, height, dup(card.render_fd.el), img_offset_x, img_offset_y, true);
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

      std::shared_ptr<img_t>
      alloc_img() override {
        auto img = std::make_shared<egl::img_descriptor_t>();

        img->width = width;
        img->height = height;
        img->serial = std::numeric_limits<decltype(img->serial)>::max();
        img->data = nullptr;
        img->pixel_pitch = 4;

        img->sequence = 0;
        std::fill_n(img->sd.fds, 4, -1);

        return img;
      }

      int
      dummy_img(platf::img_t *img) override {
        // Empty images are recognized as dummies by the zero sequence number
        return 0;
      }

      capture_e
      capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) {
        auto next_frame = std::chrono::steady_clock::now();

        sleep_overshoot_tracker.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();

          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
          }
          now = std::chrono::steady_clock::now();
          std::chrono::nanoseconds overshoot_ns = now - next_frame;
          log_sleep_overshoot(overshoot_ns);

          next_frame += delay;
          if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
            next_frame = now + delay;
          }

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::error:
            case platf::capture_e::interrupted:
              return status;
            case platf::capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return platf::capture_e::ok;
              }
              break;
            case platf::capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return platf::capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
              return status;
          }
        }

        return capture_e::ok;
      }

      capture_e
      snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds /* timeout */, bool cursor) {
        file_t fb_fd[4];

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (egl::img_descriptor_t *) img_out.get();
        img->reset();

        auto status = refresh(fb_fd, &img->sd, img->frame_timestamp);
        if (status != capture_e::ok) {
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
        }
        else {
          img->data = nullptr;
        }

        for (auto x = 0; x < 4; ++x) {
          fb_fd[x].release();
        }
        return capture_e::ok;
      }

      int
      init(const std::string &display_name, const ::video::config_t &config) {
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

      std::uint64_t sequence {};
    };

  }  // namespace kms

  std::shared_ptr<display_t>
  kms_display(mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    if (hwdevice_type == mem_type_e::vaapi || hwdevice_type == mem_type_e::cuda) {
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
   */
  void
  correlate_to_wayland(std::vector<kms::card_descriptor_t> &cds) {
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
      }
      else {
        index = std::max<int64_t>(1, util::from_view(name.substr(index_begin + 1)));
      }

      auto type = kms::from_view(name.substr(0, index_begin));

      for (auto &card_descriptor : cds) {
        for (auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
          if (monitor_descriptor.index == index && monitor_descriptor.type == type) {
            monitor_descriptor.viewport.offset_x = monitor->viewport.offset_x;
            monitor_descriptor.viewport.offset_y = monitor->viewport.offset_y;

            // A sanity check, it's guesswork after all.
            if (
              monitor_descriptor.viewport.width != monitor->viewport.width ||
              monitor_descriptor.viewport.height != monitor->viewport.height) {
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
  std::vector<std::string>
  kms_display_names(mem_type_e hwdevice_type) {
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

    fs::path card_dir { "/dev/dri"sv };
    for (auto &entry : fs::directory_iterator { card_dir }) {
      auto file = entry.path().filename();

      auto filestring = file.generic_string();
      if (std::string_view { filestring }.substr(0, 4) != "card"sv) {
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
        }
        else {
          continue;
        }
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
          BOOST_LOG((window_system != window_system_e::X11 || config::video.capture == "kms") ? fatal : error)
            << "You must run [sudo setcap cap_sys_admin+p $(readlink -f $(which sunshine))] for KMS display capture to work!\n"sv
            << "If you installed from AppImage or Flatpak, please refer to the official documentation:\n"sv
            << "https://docs.lizardbyte.dev/projects/sunshine/en/latest/about/setup.html#install"sv;
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
            (int) crtc->x,
            (int) crtc->y,
            (int) crtc->width,
            (int) crtc->height,
          };
          it->second.monitor_index = count;
        }

        kms::env_width = std::max(kms::env_width, (int) (crtc->x + crtc->width));
        kms::env_height = std::max(kms::env_height, (int) (crtc->y + crtc->height));

        kms::print(plane.get(), fb.get(), crtc.get());

        display_names.emplace_back(std::to_string(count++));
      }

      cds.emplace_back(kms::card_descriptor_t {
        std::move(file),
        std::move(crtc_to_monitor),
      });
    }

    if (!wl::init()) {
      correlate_to_wayland(cds);
    }

    // Deduce the full virtual desktop size
    kms::env_width = 0;
    kms::env_height = 0;

    for (auto &card_descriptor : cds) {
      for (auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
        BOOST_LOG(debug) << "Monitor description"sv;
        BOOST_LOG(debug) << "Resolution: "sv << monitor_descriptor.viewport.width << 'x' << monitor_descriptor.viewport.height;
        BOOST_LOG(debug) << "Offset: "sv << monitor_descriptor.viewport.offset_x << 'x' << monitor_descriptor.viewport.offset_y;

        kms::env_width = std::max(kms::env_width, (int) (monitor_descriptor.viewport.offset_x + monitor_descriptor.viewport.width));
        kms::env_height = std::max(kms::env_height, (int) (monitor_descriptor.viewport.offset_y + monitor_descriptor.viewport.height));
      }
    }

    BOOST_LOG(debug) << "Desktop resolution: "sv << kms::env_width << 'x' << kms::env_height;

    kms::card_descriptors = std::move(cds);

    return display_names;
  }

  std::vector<config::display_options_t>
  kms_display_options() {
    int count = 0;

    if (!fs::exists("/dev/dri")) {
      return {};
    }

    if (!gbm::create_device) {
      return {};
    }

    std::vector<config::display_options_t> display_options;

    kms::conn_type_count_t conn_type_count;

    fs::path card_dir { "/dev/dri"sv };
    for (auto &entry : fs::directory_iterator { card_dir }) {
      auto file = entry.path().filename();

      auto filestring = file.generic_string();
      if (std::string_view { filestring }.substr(0, 4) != "card"sv) {
        continue;
      }

      kms::card_t card;
      if (card.init(entry.path().c_str())) {
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
          continue;
        }

        if (!fb->handles[0]) {
          break;
        }

        // This appears to return the offset of the monitor
        auto crtc = card.crtc(plane->crtc_id);
        if (!crtc) {
          continue;
        }

        auto monitor = crtc_to_monitor[plane->crtc_id];

        auto option = config::display_options_t {
          count++,
          std::to_string(count) + ", type: " +  std::to_string(monitor.index), // TODO: Get real display name
          false // TODO: Dunno how to find if it is primary
        };
        display_options.emplace_back(option);
      }
    }

    return display_options;
  }

}  // namespace platf
