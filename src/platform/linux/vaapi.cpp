/**
 * @file src/platform/linux/vaapi.cpp
 * @brief todo
 */
#include <sstream>
#include <string>

#include <fcntl.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <va/va.h>
#include <va/va_drm.h>
#if !VA_CHECK_VERSION(1, 9, 0)
// vaSyncBuffer stub allows Sunshine built against libva <2.9.0 to link against ffmpeg on libva 2.9.0 or later
VAStatus
vaSyncBuffer(
  VADisplay dpy,
  VABufferID buf_id,
  uint64_t timeout_ns) {
  return VA_STATUS_ERROR_UNIMPLEMENTED;
}
#endif
}

#include "graphics.h"
#include "misc.h"
#include "src/config.h"
#include "src/main.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include "src/video.h"

using namespace std::literals;

extern "C" struct AVBufferRef;

namespace va {
  constexpr auto SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 = 0x40000000;
  constexpr auto EXPORT_SURFACE_WRITE_ONLY = 0x0002;
  constexpr auto EXPORT_SURFACE_SEPARATE_LAYERS = 0x0004;

  using VADisplay = void *;
  using VAStatus = int;
  using VAGenericID = unsigned int;
  using VASurfaceID = VAGenericID;

  struct DRMPRIMESurfaceDescriptor {
    // VA Pixel format fourcc of the whole surface (VA_FOURCC_*).
    uint32_t fourcc;

    uint32_t width;
    uint32_t height;

    // Number of distinct DRM objects making up the surface.
    uint32_t num_objects;

    struct {
      // DRM PRIME file descriptor for this object.
      // Needs to be closed manually
      int fd;

      // Total size of this object (may include regions which are not part of the surface)
      uint32_t size;
      // Format modifier applied to this object, not sure what that means
      uint64_t drm_format_modifier;
    } objects[4];

    // Number of layers making up the surface.
    uint32_t num_layers;
    struct {
      // DRM format fourcc of this layer (DRM_FOURCC_*).
      uint32_t drm_format;

      // Number of planes in this layer.
      uint32_t num_planes;

      // references objects --> DRMPRIMESurfaceDescriptor.objects[object_index[0]]
      uint32_t object_index[4];

      // Offset within the object of each plane.
      uint32_t offset[4];

      // Pitch of each plane.
      uint32_t pitch[4];
    } layers[4];
  };

  using display_t = util::safe_ptr_v2<void, VAStatus, vaTerminate>;

  int
  vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  class va_t: public platf::avcodec_encode_device_t {
  public:
    int
    init(int in_width, int in_height, file_t &&render_device) {
      file = std::move(render_device);

      if (!gbm::create_device) {
        BOOST_LOG(warning) << "libgbm not initialized"sv;
        return -1;
      }

      this->data = (void *) vaapi_init_avcodec_hardware_input_buffer;

      gbm.reset(gbm::create_device(file.el));
      if (!gbm) {
        char string[1024];
        BOOST_LOG(error) << "Couldn't create GBM device: ["sv << strerror_r(errno, string, sizeof(string)) << ']';
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

      width = in_width;
      height = in_height;

      return 0;
    }

    int
    set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx_buf, frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for VAAPI"sv;
          return -1;
        }
      }

      va::DRMPRIMESurfaceDescriptor prime;
      va::VASurfaceID surface = (std::uintptr_t) frame->data[3];
      auto hw_frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;

      auto status = vaExportSurfaceHandle(
        this->va_display,
        surface,
        va::SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        va::EXPORT_SURFACE_WRITE_ONLY | va::EXPORT_SURFACE_SEPARATE_LAYERS,
        &prime);
      if (status) {
        BOOST_LOG(error) << "Couldn't export va surface handle: ["sv << (int) surface << "]: "sv << vaErrorStr(status);

        return -1;
      }

      // Keep track of file descriptors
      std::array<file_t, egl::nv12_img_t::num_fds> fds;
      for (int x = 0; x < prime.num_objects; ++x) {
        fds[x] = prime.objects[x].fd;
      }

      if (prime.num_layers != 2) {
        BOOST_LOG(error) << "Invalid layer count for VA surface: expected 2, got "sv << prime.num_layers;
        return -1;
      }

      egl::surface_descriptor_t sds[2] = {};
      for (int plane = 0; plane < 2; ++plane) {
        auto &sd = sds[plane];
        auto &layer = prime.layers[plane];

        sd.fourcc = layer.drm_format;

        // UV plane is subsampled
        sd.width = prime.width / (plane == 0 ? 1 : 2);
        sd.height = prime.height / (plane == 0 ? 1 : 2);

        // The modifier must be the same for all planes
        sd.modifier = prime.objects[layer.object_index[0]].drm_format_modifier;

        std::fill_n(sd.fds, 4, -1);
        for (int x = 0; x < layer.num_planes; ++x) {
          sd.fds[x] = prime.objects[layer.object_index[x]].fd;
          sd.pitches[x] = layer.pitch[x];
          sd.offsets[x] = layer.offset[x];
        }
      }

      auto nv12_opt = egl::import_target(display.get(), std::move(fds), sds[0], sds[1]);
      if (!nv12_opt) {
        return -1;
      }

      auto sws_opt = egl::sws_t::make(width, height, frame->width, frame->height, hw_frames_ctx->sw_format);
      if (!sws_opt) {
        return -1;
      }

      this->sws = std::move(*sws_opt);
      this->nv12 = std::move(*nv12_opt);

      return 0;
    }

    void
    apply_colorspace() override {
      sws.apply_colorspace(colorspace);
    }

    va::display_t::pointer va_display;
    file_t file;

    gbm::gbm_t gbm;
    egl::display_t display;
    egl::ctx_t ctx;

    // This must be destroyed before display_t to ensure the GPU
    // driver is still loaded when vaDestroySurfaces() is called.
    frame_t hwframe;

    egl::sws_t sws;
    egl::nv12_t nv12;

    int width, height;
  };

  class va_ram_t: public va_t {
  public:
    int
    convert(platf::img_t &img) override {
      sws.load_ram(img);

      sws.convert(nv12->buf);
      return 0;
    }
  };

  class va_vram_t: public va_t {
  public:
    int
    convert(platf::img_t &img) override {
      auto &descriptor = (egl::img_descriptor_t &) img;

      if (descriptor.sequence == 0) {
        // For dummy images, use a blank RGB texture instead of importing a DMA-BUF
        rgb = egl::create_blank(img);
      }
      else if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;

        rgb = egl::rgb_t {};

        auto rgb_opt = egl::import_source(display.get(), descriptor.sd);

        if (!rgb_opt) {
          return -1;
        }

        rgb = std::move(*rgb_opt);
      }

      sws.load_vram(descriptor, offset_x, offset_y, rgb->tex[0]);

      sws.convert(nv12->buf);
      return 0;
    }

    int
    init(int in_width, int in_height, file_t &&render_device, int offset_x, int offset_y) {
      if (va_t::init(in_width, in_height, std::move(render_device))) {
        return -1;
      }

      sequence = 0;

      this->offset_x = offset_x;
      this->offset_y = offset_y;

      return 0;
    }

    std::uint64_t sequence;
    egl::rgb_t rgb;

    int offset_x, offset_y;
  };

  /**
   * This is a private structure of FFmpeg, I need this to manually create
   * a VAAPI hardware context
   *
   * xdisplay will not be used internally by FFmpeg
   */
  typedef struct VAAPIDevicePriv {
    union {
      void *xdisplay;
      int fd;
    } drm;
    int drm_fd;
  } VAAPIDevicePriv;

  /**
   * VAAPI connection details.
   *
   * Allocated as AVHWDeviceContext.hwctx
   */
  typedef struct AVVAAPIDeviceContext {
    /**
     * The VADisplay handle, to be filled by the user.
     */
    va::VADisplay display;
    /**
     * Driver quirks to apply - this is filled by av_hwdevice_ctx_init(),
     * with reference to a table of known drivers, unless the
     * AV_VAAPI_DRIVER_QUIRK_USER_SET bit is already present.  The user
     * may need to refer to this field when performing any later
     * operations using VAAPI with the same VADisplay.
     */
    unsigned int driver_quirks;
  } AVVAAPIDeviceContext;

  static void
  __log(void *level, const char *msg) {
    BOOST_LOG(*(boost::log::sources::severity_logger<int> *) level) << msg;
  }

  static void
  vaapi_hwdevice_ctx_free(AVHWDeviceContext *ctx) {
    auto hwctx = (AVVAAPIDeviceContext *) ctx->hwctx;
    auto priv = (VAAPIDevicePriv *) ctx->user_opaque;

    vaTerminate(hwctx->display);
    close(priv->drm_fd);
    av_freep(&priv);
  }

  int
  vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *base, AVBufferRef **hw_device_buf) {
    auto va = (va::va_t *) base;
    auto fd = dup(va->file.el);

    auto *priv = (VAAPIDevicePriv *) av_mallocz(sizeof(VAAPIDevicePriv));
    priv->drm_fd = fd;

    auto fg = util::fail_guard([fd, priv]() {
      close(fd);
      av_free(priv);
    });

    va::display_t display { vaGetDisplayDRM(fd) };
    if (!display) {
      auto render_device = config::video.adapter_name.empty() ? "/dev/dri/renderD128" : config::video.adapter_name.c_str();

      BOOST_LOG(error) << "Couldn't open a va display from DRM with device: "sv << render_device;
      return -1;
    }

    va->va_display = display.get();

    vaSetErrorCallback(display.get(), __log, &error);
    vaSetErrorCallback(display.get(), __log, &info);

    int major, minor;
    auto status = vaInitialize(display.get(), &major, &minor);
    if (status) {
      BOOST_LOG(error) << "Couldn't initialize va display: "sv << vaErrorStr(status);
      return -1;
    }

    BOOST_LOG(debug) << "vaapi vendor: "sv << vaQueryVendorString(display.get());

    *hw_device_buf = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    auto ctx = (AVHWDeviceContext *) (*hw_device_buf)->data;
    auto hwctx = (AVVAAPIDeviceContext *) ctx->hwctx;

    // Ownership of the VADisplay and DRM fd is now ours to manage via the free() function
    hwctx->display = display.release();
    ctx->user_opaque = priv;
    ctx->free = vaapi_hwdevice_ctx_free;
    fg.disable();

    auto err = av_hwdevice_ctx_init(*hw_device_buf);
    if (err) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return 0;
  }

  static bool
  query(display_t::pointer display, VAProfile profile) {
    std::vector<VAEntrypoint> entrypoints;
    entrypoints.resize(vaMaxNumEntrypoints(display));

    int count;
    auto status = vaQueryConfigEntrypoints(display, profile, entrypoints.data(), &count);
    if (status) {
      BOOST_LOG(error) << "Couldn't query entrypoints: "sv << vaErrorStr(status);
      return false;
    }
    entrypoints.resize(count);

    for (auto entrypoint : entrypoints) {
      if (entrypoint == VAEntrypointEncSlice || entrypoint == VAEntrypointEncSliceLP) {
        return true;
      }
    }

    return false;
  }

  bool
  validate(int fd) {
    va::display_t display { vaGetDisplayDRM(fd) };
    if (!display) {
      char string[1024];

      auto bytes = readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), string, sizeof(string));

      std::string_view render_device { string, (std::size_t) bytes };

      BOOST_LOG(error) << "Couldn't open a va display from DRM with device: "sv << render_device;
      return false;
    }

    int major, minor;
    auto status = vaInitialize(display.get(), &major, &minor);
    if (status) {
      BOOST_LOG(error) << "Couldn't initialize va display: "sv << vaErrorStr(status);
      return false;
    }

    if (!query(display.get(), VAProfileH264Main)) {
      return false;
    }

    if (video::active_hevc_mode > 1 && !query(display.get(), VAProfileHEVCMain)) {
      return false;
    }

    if (video::active_hevc_mode > 2 && !query(display.get(), VAProfileHEVCMain10)) {
      return false;
    }

    if (video::active_av1_mode > 1 && !query(display.get(), VAProfileAV1Profile0)) {
      return false;
    }

    return true;
  }

  std::unique_ptr<platf::avcodec_encode_device_t>
  make_avcodec_encode_device(int width, int height, file_t &&card, int offset_x, int offset_y, bool vram) {
    if (vram) {
      auto egl = std::make_unique<va::va_vram_t>();
      if (egl->init(width, height, std::move(card), offset_x, offset_y)) {
        return nullptr;
      }

      return egl;
    }

    else {
      auto egl = std::make_unique<va::va_ram_t>();
      if (egl->init(width, height, std::move(card))) {
        return nullptr;
      }

      return egl;
    }
  }

  std::unique_ptr<platf::avcodec_encode_device_t>
  make_avcodec_encode_device(int width, int height, int offset_x, int offset_y, bool vram) {
    auto render_device = config::video.adapter_name.empty() ? "/dev/dri/renderD128" : config::video.adapter_name.c_str();

    file_t file = open(render_device, O_RDWR);
    if (file.el < 0) {
      char string[1024];
      BOOST_LOG(error) << "Couldn't open "sv << render_device << ": " << strerror_r(errno, string, sizeof(string));

      return nullptr;
    }

    return make_avcodec_encode_device(width, height, std::move(file), offset_x, offset_y, vram);
  }

  std::unique_ptr<platf::avcodec_encode_device_t>
  make_avcodec_encode_device(int width, int height, bool vram) {
    return make_avcodec_encode_device(width, height, 0, 0, vram);
  }
}  // namespace va
