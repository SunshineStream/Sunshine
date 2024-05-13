/**
 * @file src/platform/linux/x11grab.cpp
 * @brief todo
 */
#include "src/platform/common.h"

#include <fstream>
#include <thread>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>

#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/task_pool.h"
#include "src/video.h"

#include "cuda.h"
#include "graphics.h"
#include "misc.h"
#include "vaapi.h"
#include "x11grab.h"

using namespace std::literals;

namespace platf {
  int
  load_xcb();
  int
  load_x11();

  namespace x11 {
#define _FN(x, ret, args)    \
  typedef ret(*x##_fn) args; \
  static x##_fn x

    _FN(GetImage, XImage *,
      (
        Display * display,
        Drawable d,
        int x, int y,
        unsigned int width, unsigned int height,
        unsigned long plane_mask,
        int format));

    _FN(OpenDisplay, Display *, (_Xconst char *display_name));
    _FN(GetWindowAttributes, Status,
      (
        Display * display,
        Window w,
        XWindowAttributes *window_attributes_return));

    _FN(CloseDisplay, int, (Display * display));
    _FN(Free, int, (void *data));
    _FN(InitThreads, Status, (void) );

    namespace rr {
      _FN(GetScreenResources, XRRScreenResources *, (Display * dpy, Window window));
      _FN(GetOutputPrimary, RROutput, (Display * dpy, Window window));
      _FN(GetOutputInfo, XRROutputInfo *, (Display * dpy, XRRScreenResources *resources, RROutput output));
      _FN(GetCrtcInfo, XRRCrtcInfo *, (Display * dpy, XRRScreenResources *resources, RRCrtc crtc));
      _FN(FreeScreenResources, void, (XRRScreenResources * resources));
      _FN(FreeOutputInfo, void, (XRROutputInfo * outputInfo));
      _FN(FreeCrtcInfo, void, (XRRCrtcInfo * crtcInfo));

      static int
      init() {
        static void *handle { nullptr };
        static bool funcs_loaded = false;

        if (funcs_loaded) return 0;

        if (!handle) {
          handle = dyn::handle({ "libXrandr.so.2", "libXrandr.so" });
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          { (dyn::apiproc *) &GetScreenResources, "XRRGetScreenResources" },
          { (dyn::apiproc *) &GetOutputPrimary, "XRRGetOutputPrimary" },
          { (dyn::apiproc *) &GetOutputInfo, "XRRGetOutputInfo" },
          { (dyn::apiproc *) &GetCrtcInfo, "XRRGetCrtcInfo" },
          { (dyn::apiproc *) &FreeScreenResources, "XRRFreeScreenResources" },
          { (dyn::apiproc *) &FreeOutputInfo, "XRRFreeOutputInfo" },
          { (dyn::apiproc *) &FreeCrtcInfo, "XRRFreeCrtcInfo" },
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }

    }  // namespace rr
    namespace fix {
      _FN(GetCursorImage, XFixesCursorImage *, (Display * dpy));

      static int
      init() {
        static void *handle { nullptr };
        static bool funcs_loaded = false;

        if (funcs_loaded) return 0;

        if (!handle) {
          handle = dyn::handle({ "libXfixes.so.3", "libXfixes.so" });
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          { (dyn::apiproc *) &GetCursorImage, "XFixesGetCursorImage" },
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }
    }  // namespace fix

    static int
    init() {
      static void *handle { nullptr };
      static bool funcs_loaded = false;

      if (funcs_loaded) return 0;

      if (!handle) {
        handle = dyn::handle({ "libX11.so.6", "libX11.so" });
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        { (dyn::apiproc *) &GetImage, "XGetImage" },
        { (dyn::apiproc *) &OpenDisplay, "XOpenDisplay" },
        { (dyn::apiproc *) &GetWindowAttributes, "XGetWindowAttributes" },
        { (dyn::apiproc *) &Free, "XFree" },
        { (dyn::apiproc *) &CloseDisplay, "XCloseDisplay" },
        { (dyn::apiproc *) &InitThreads, "XInitThreads" },
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }
  }  // namespace x11

  namespace xcb {
    static xcb_extension_t *shm_id;

    _FN(shm_get_image_reply, xcb_shm_get_image_reply_t *,
      (
        xcb_connection_t * c,
        xcb_shm_get_image_cookie_t cookie,
        xcb_generic_error_t **e));

    _FN(shm_get_image_unchecked, xcb_shm_get_image_cookie_t,
      (
        xcb_connection_t * c,
        xcb_drawable_t drawable,
        int16_t x, int16_t y,
        uint16_t width, uint16_t height,
        uint32_t plane_mask,
        uint8_t format,
        xcb_shm_seg_t shmseg,
        uint32_t offset));

    _FN(shm_attach, xcb_void_cookie_t,
      (xcb_connection_t * c,
        xcb_shm_seg_t shmseg,
        uint32_t shmid,
        uint8_t read_only));

    _FN(get_extension_data, xcb_query_extension_reply_t *,
      (xcb_connection_t * c, xcb_extension_t *ext));

    _FN(get_setup, xcb_setup_t *, (xcb_connection_t * c));
    _FN(disconnect, void, (xcb_connection_t * c));
    _FN(connection_has_error, int, (xcb_connection_t * c));
    _FN(connect, xcb_connection_t *, (const char *displayname, int *screenp));
    _FN(setup_roots_iterator, xcb_screen_iterator_t, (const xcb_setup_t *R));
    _FN(generate_id, std::uint32_t, (xcb_connection_t * c));

    int
    init_shm() {
      static void *handle { nullptr };
      static bool funcs_loaded = false;

      if (funcs_loaded) return 0;

      if (!handle) {
        handle = dyn::handle({ "libxcb-shm.so.0", "libxcb-shm.so" });
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        { (dyn::apiproc *) &shm_id, "xcb_shm_id" },
        { (dyn::apiproc *) &shm_get_image_reply, "xcb_shm_get_image_reply" },
        { (dyn::apiproc *) &shm_get_image_unchecked, "xcb_shm_get_image_unchecked" },
        { (dyn::apiproc *) &shm_attach, "xcb_shm_attach" },
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    int
    init() {
      static void *handle { nullptr };
      static bool funcs_loaded = false;

      if (funcs_loaded) return 0;

      if (!handle) {
        handle = dyn::handle({ "libxcb.so.1", "libxcb.so" });
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        { (dyn::apiproc *) &get_extension_data, "xcb_get_extension_data" },
        { (dyn::apiproc *) &get_setup, "xcb_get_setup" },
        { (dyn::apiproc *) &disconnect, "xcb_disconnect" },
        { (dyn::apiproc *) &connection_has_error, "xcb_connection_has_error" },
        { (dyn::apiproc *) &connect, "xcb_connect" },
        { (dyn::apiproc *) &setup_roots_iterator, "xcb_setup_roots_iterator" },
        { (dyn::apiproc *) &generate_id, "xcb_generate_id" },
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

#undef _FN
  }  // namespace xcb

  void
  freeImage(XImage *);
  void
  freeX(XFixesCursorImage *);

  using xcb_connect_t = util::dyn_safe_ptr<xcb_connection_t, &xcb::disconnect>;
  using xcb_img_t = util::c_ptr<xcb_shm_get_image_reply_t>;

  using ximg_t = util::safe_ptr<XImage, freeImage>;
  using xcursor_t = util::safe_ptr<XFixesCursorImage, freeX>;

  using crtc_info_t = util::dyn_safe_ptr<_XRRCrtcInfo, &x11::rr::FreeCrtcInfo>;
  using output_info_t = util::dyn_safe_ptr<_XRROutputInfo, &x11::rr::FreeOutputInfo>;
  using screen_res_t = util::dyn_safe_ptr<_XRRScreenResources, &x11::rr::FreeScreenResources>;

  class shm_id_t {
  public:
    shm_id_t():
        id { -1 } {}
    shm_id_t(int id):
        id { id } {}
    shm_id_t(shm_id_t &&other) noexcept:
        id(other.id) {
      other.id = -1;
    }

    ~shm_id_t() {
      if (id != -1) {
        shmctl(id, IPC_RMID, nullptr);
        id = -1;
      }
    }
    int id;
  };

  class shm_data_t {
  public:
    shm_data_t():
        data { (void *) -1 } {}
    shm_data_t(void *data):
        data { data } {}

    shm_data_t(shm_data_t &&other) noexcept:
        data(other.data) {
      other.data = (void *) -1;
    }

    ~shm_data_t() {
      if ((std::uintptr_t) data != -1) {
        shmdt(data);
      }
    }

    void *data;
  };

  struct x11_img_t: public img_t {
    ximg_t img;
  };

  struct shm_img_t: public img_t {
    ~shm_img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  static void
  blend_cursor(Display *display, img_t &img, int offsetX, int offsetY) {
    xcursor_t overlay { x11::fix::GetCursorImage(display) };

    if (!overlay) {
      BOOST_LOG(error) << "Couldn't get cursor from XFixesGetCursorImage"sv;
      return;
    }

    overlay->x -= overlay->xhot;
    overlay->y -= overlay->yhot;

    overlay->x -= offsetX;
    overlay->y -= offsetY;

    overlay->x = std::max((short) 0, overlay->x);
    overlay->y = std::max((short) 0, overlay->y);

    auto pixels = (int *) img.data;

    auto screen_height = img.height;
    auto screen_width = img.width;

    auto delta_height = std::min<uint16_t>(overlay->height, std::max(0, screen_height - overlay->y));
    auto delta_width = std::min<uint16_t>(overlay->width, std::max(0, screen_width - overlay->x));
    for (auto y = 0; y < delta_height; ++y) {
      auto overlay_begin = &overlay->pixels[y * overlay->width];
      auto overlay_end = &overlay->pixels[y * overlay->width + delta_width];

      auto pixels_begin = &pixels[(y + overlay->y) * (img.row_pitch / img.pixel_pitch) + overlay->x];

      std::for_each(overlay_begin, overlay_end, [&](long pixel) {
        int *pixel_p = (int *) &pixel;

        auto colors_in = (uint8_t *) pixels_begin;

        auto alpha = (*(uint *) pixel_p) >> 24u;
        if (alpha == 255) {
          *pixels_begin = *pixel_p;
        }
        else {
          auto colors_out = (uint8_t *) pixel_p;
          colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
          colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
          colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
        }
        ++pixels_begin;
      });
    }
  }

  struct x11_attr_t: public display_t {
    std::chrono::nanoseconds delay;

    x11::xdisplay_t xdisplay;
    Window xwindow;
    XWindowAttributes xattr;

    mem_type_e mem_type;

    /**
     * Last X (NOT the streamed monitor!) size.
     * This way we can trigger reinitialization if the dimensions changed while streaming
     */
    // int env_width, env_height;

    x11_attr_t(mem_type_e mem_type):
        xdisplay { x11::OpenDisplay(nullptr) }, xwindow {}, xattr {}, mem_type { mem_type } {
      x11::InitThreads();
    }

    int
    init(const std::string &display_name, const ::video::config_t &config) {
      if (!xdisplay) {
        BOOST_LOG(error) << "Could not open X11 display"sv;
        return -1;
      }

      delay = std::chrono::nanoseconds { 1s } / config.framerate;

      xwindow = DefaultRootWindow(xdisplay.get());

      refresh();

      int streamedMonitor = -1;
      if (!display_name.empty()) {
        streamedMonitor = (int) util::from_view(display_name);
      }

      if (streamedMonitor != -1) {
        BOOST_LOG(info) << "Configuring selected display ("sv << streamedMonitor << ") to stream"sv;
        screen_res_t screenr { x11::rr::GetScreenResources(xdisplay.get(), xwindow) };
        int output = screenr->noutput;

        output_info_t result;
        int monitor = 0;
        for (int x = 0; x < output; ++x) {
          output_info_t out_info { x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x]) };
          if (out_info) {
            if (monitor++ == streamedMonitor) {
              result = std::move(out_info);
              break;
            }
          }
        }

        if (!result) {
          BOOST_LOG(error) << "Could not stream display number ["sv << streamedMonitor << "], there are only ["sv << monitor << "] displays."sv;
          return -1;
        }

        if (result->crtc) {
          crtc_info_t crt_info { x11::rr::GetCrtcInfo(xdisplay.get(), screenr.get(), result->crtc) };
          BOOST_LOG(info)
            << "Streaming display: "sv << result->name << " with res "sv << crt_info->width << 'x' << crt_info->height << " offset by "sv << crt_info->x << 'x' << crt_info->y;

          width = crt_info->width;
          height = crt_info->height;
          offset_x = crt_info->x;
          offset_y = crt_info->y;
        }
        else {
          BOOST_LOG(warning) << "Couldn't get requested display info, defaulting to recording entire virtual desktop"sv;
          width = xattr.width;
          height = xattr.height;
        }
      }
      else {
        width = xattr.width;
        height = xattr.height;
      }

      env_width = xattr.width;
      env_height = xattr.height;

      return 0;
    }

    /**
     * Called when the display attributes should change.
     */
    void
    refresh() {
      x11::GetWindowAttributes(xdisplay.get(), xwindow, &xattr);  // Update xattr's
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

    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      refresh();

      // The whole X server changed, so we must reinit everything
      if (xattr.width != env_width || xattr.height != env_height) {
        BOOST_LOG(warning) << "X dimensions changed in non-SHM mode, request reinit"sv;
        return capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (x11_img_t *) img_out.get();

      XImage *x_img { x11::GetImage(xdisplay.get(), xwindow, offset_x, offset_y, width, height, AllPlanes, ZPixmap) };
      img->frame_timestamp = std::chrono::steady_clock::now();

      img->width = x_img->width;
      img->height = x_img->height;
      img->data = (uint8_t *) x_img->data;
      img->row_pitch = x_img->bytes_per_line;
      img->pixel_pitch = x_img->bits_per_pixel / 8;
      img->img.reset(x_img);

      if (cursor) {
        blend_cursor(xdisplay.get(), *img, offset_x, offset_y);
      }

      return capture_e::ok;
    }

    std::shared_ptr<img_t>
    alloc_img() override {
      return std::make_shared<x11_img_t>();
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

    int
    dummy_img(img_t *img) override {
      // TODO: stop cheating and give black image
      if (!img) {
        return -1;
      };
      auto pull_dummy_img_callback = [&img](std::shared_ptr<platf::img_t> &img_out) -> bool {
        img_out = img->shared_from_this();
        return true;
      };
      std::shared_ptr<platf::img_t> img_out;
      snapshot(pull_dummy_img_callback, img_out, 0s, true);
      return 0;
    }
  };

  struct shm_attr_t: public x11_attr_t {
    x11::xdisplay_t shm_xdisplay;  // Prevent race condition with x11_attr_t::xdisplay
    xcb_connect_t xcb;
    xcb_screen_t *display;
    std::uint32_t seg;

    shm_id_t shm_id;

    shm_data_t data;

    task_pool_util::TaskPool::task_id_t refresh_task_id;

    void
    delayed_refresh() {
      refresh();

      refresh_task_id = task_pool.pushDelayed(&shm_attr_t::delayed_refresh, 2s, this).task_id;
    }

    shm_attr_t(mem_type_e mem_type):
        x11_attr_t(mem_type), shm_xdisplay { x11::OpenDisplay(nullptr) } {
      refresh_task_id = task_pool.pushDelayed(&shm_attr_t::delayed_refresh, 2s, this).task_id;
    }

    ~shm_attr_t() override {
      while (!task_pool.cancel(refresh_task_id))
        ;
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

    capture_e
    snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      // The whole X server changed, so we must reinit everything
      if (xattr.width != env_width || xattr.height != env_height) {
        BOOST_LOG(warning) << "X dimensions changed in SHM mode, request reinit"sv;
        return capture_e::reinit;
      }
      else {
        auto img_cookie = xcb::shm_get_image_unchecked(xcb.get(), display->root, offset_x, offset_y, width, height, ~0, XCB_IMAGE_FORMAT_Z_PIXMAP, seg, 0);
        auto frame_timestamp = std::chrono::steady_clock::now();

        xcb_img_t img_reply { xcb::shm_get_image_reply(xcb.get(), img_cookie, nullptr) };
        if (!img_reply) {
          BOOST_LOG(error) << "Could not get image reply"sv;
          return capture_e::reinit;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        std::copy_n((std::uint8_t *) data.data, frame_size(), img_out->data);
        img_out->frame_timestamp = frame_timestamp;

        if (cursor) {
          blend_cursor(shm_xdisplay.get(), *img_out, offset_x, offset_y);
        }

        return capture_e::ok;
      }
    }

    std::shared_ptr<img_t>
    alloc_img() override {
      auto img = std::make_shared<shm_img_t>();
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

    int
    init(const std::string &display_name, const ::video::config_t &config) {
      if (x11_attr_t::init(display_name, config)) {
        return 1;
      }

      shm_xdisplay.reset(x11::OpenDisplay(nullptr));
      xcb.reset(xcb::connect(nullptr, nullptr));
      if (xcb::connection_has_error(xcb.get())) {
        return -1;
      }

      if (!xcb::get_extension_data(xcb.get(), xcb::shm_id)->present) {
        BOOST_LOG(error) << "Missing SHM extension"sv;

        return -1;
      }

      auto iter = xcb::setup_roots_iterator(xcb::get_setup(xcb.get()));
      display = iter.data;
      seg = xcb::generate_id(xcb.get());

      shm_id.id = shmget(IPC_PRIVATE, frame_size(), IPC_CREAT | 0777);
      if (shm_id.id == -1) {
        BOOST_LOG(error) << "shmget failed"sv;
        return -1;
      }

      xcb::shm_attach(xcb.get(), seg, shm_id.id, false);
      data.data = shmat(shm_id.id, nullptr, 0);

      if ((uintptr_t) data.data == -1) {
        BOOST_LOG(error) << "shmat failed"sv;

        return -1;
      }

      return 0;
    }

    std::uint32_t
    frame_size() {
      return width * height * 4;
    }
  };

  std::shared_ptr<display_t>
  x11_display(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize x11 display with the given hw device type"sv;
      return nullptr;
    }

    if (xcb::init_shm() || xcb::init() || x11::init() || x11::rr::init() || x11::fix::init()) {
      BOOST_LOG(error) << "Couldn't init x11 libraries"sv;

      return nullptr;
    }

    // Attempt to use shared memory X11 to avoid copying the frame
    auto shm_disp = std::make_shared<shm_attr_t>(hwdevice_type);

    auto status = shm_disp->init(display_name, config);
    if (status > 0) {
      // x11_attr_t::init() failed, don't bother trying again.
      return nullptr;
    }

    if (status == 0) {
      return shm_disp;
    }

    // Fallback
    auto x11_disp = std::make_shared<x11_attr_t>(hwdevice_type);
    if (x11_disp->init(display_name, config)) {
      return nullptr;
    }

    return x11_disp;
  }

  std::vector<std::string>
  x11_display_names() {
    if (load_x11() || load_xcb()) {
      BOOST_LOG(error) << "Couldn't init x11 libraries"sv;

      return {};
    }

    BOOST_LOG(info) << "Detecting displays"sv;

    x11::xdisplay_t xdisplay { x11::OpenDisplay(nullptr) };
    if (!xdisplay) {
      return {};
    }

    auto xwindow = DefaultRootWindow(xdisplay.get());
    screen_res_t screenr { x11::rr::GetScreenResources(xdisplay.get(), xwindow) };
    int output = screenr->noutput;

    int monitor = 0;
    for (int x = 0; x < output; ++x) {
      output_info_t out_info { x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x]) };
      if (out_info) {
        BOOST_LOG(info) << "Detected display: "sv << out_info->name << " (id: "sv << monitor << ")"sv << out_info->name << " connected: "sv << (out_info->connection == RR_Connected);
        ++monitor;
      }
    }

    std::vector<std::string> names;
    names.reserve(monitor);

    for (auto x = 0; x < monitor; ++x) {
      names.emplace_back(std::to_string(x));
    }

    return names;
  }

  std::vector<config::display_options_t>
  x11_display_options() {
    if (load_x11() || load_xcb()) {
      return {};
    }

    x11::xdisplay_t xdisplay { x11::OpenDisplay(nullptr) };
    if (!xdisplay) {
      return {};
    }

    auto xwindow = DefaultRootWindow(xdisplay.get());
    screen_res_t screenr { x11::rr::GetScreenResources(xdisplay.get(), xwindow) };
    int output = screenr->noutput;

    auto main_display = x11::rr::GetOutputPrimary(xdisplay.get(), xwindow);

    std::vector<config::display_options_t> display_options;

    int monitor = 0;
    for (int x = 0; x < output; ++x) {
      output_info_t out_info { x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x]) };
      if (out_info) {
		std::string name = out_info->name;
        auto is_connected_value = (out_info->connection == RR_Connected) ? "true" : "false";
        auto option = config::display_options_t {
          monitor,
          name + " connected: " + is_connected_value,
          main_display == screenr->outputs[x]
        };
        display_options.emplace_back(option);
        ++monitor;
      }
    }

    return display_options;
  }

  void
  freeImage(XImage *p) {
    XDestroyImage(p);
  }
  void
  freeX(XFixesCursorImage *p) {
    x11::Free(p);
  }

  int
  load_xcb() {
    // This will be called once only
    static int xcb_status = xcb::init_shm() || xcb::init();

    return xcb_status;
  }

  int
  load_x11() {
    // This will be called once only
    static int x11_status =
      window_system == window_system_e::NONE ||
      x11::init() || x11::rr::init() || x11::fix::init();

    return x11_status;
  }

  namespace x11 {
    std::optional<cursor_t>
    cursor_t::make() {
      if (load_x11()) {
        return std::nullopt;
      }

      cursor_t cursor;

      cursor.ctx.reset((cursor_ctx_t::pointer) x11::OpenDisplay(nullptr));

      return cursor;
    }

    void
    cursor_t::capture(egl::cursor_t &img) {
      auto display = (xdisplay_t::pointer) ctx.get();

      xcursor_t xcursor = fix::GetCursorImage(display);

      if (img.serial != xcursor->cursor_serial) {
        auto buf_size = xcursor->width * xcursor->height * sizeof(int);

        if (img.buffer.size() < buf_size) {
          img.buffer.resize(buf_size);
        }

        std::transform(xcursor->pixels, xcursor->pixels + buf_size / 4, (int *) img.buffer.data(), [](long pixel) -> int {
          return pixel;
        });
      }

      img.data = img.buffer.data();
      img.width = img.src_w = xcursor->width;
      img.height = img.src_h = xcursor->height;
      img.x = xcursor->x - xcursor->xhot;
      img.y = xcursor->y - xcursor->yhot;
      img.pixel_pitch = 4;
      img.row_pitch = img.pixel_pitch * img.width;
      img.serial = xcursor->cursor_serial;
    }

    void
    cursor_t::blend(img_t &img, int offsetX, int offsetY) {
      blend_cursor((xdisplay_t::pointer) ctx.get(), img, offsetX, offsetY);
    }

    xdisplay_t
    make_display() {
      return OpenDisplay(nullptr);
    }

    void
    freeDisplay(_XDisplay *xdisplay) {
      CloseDisplay(xdisplay);
    }

    void
    freeCursorCtx(cursor_ctx_t::pointer ctx) {
      CloseDisplay((xdisplay_t::pointer) ctx);
    }
  }  // namespace x11
}  // namespace platf
