/**
 * @file src/platform/linux/graphics.cpp
 * @brief Definitions for graphics related functions.
 */
// standard includes
#include <array>
#include <fcntl.h>
#include <future>
#include <stdexcept>
#include <string_view>
#include <tuple>

// local includes
#include "graphics.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/video.h"

// platform includes
#if !defined(__FreeBSD__)
  #include <sys/capability.h>
#endif

extern "C" {
#include <libavutil/pixdesc.h>
}

// I want to have as little build dependencies as possible
// There aren't that many DRM_FORMAT I need to use, so define them here
//
// They aren't likely to change any time soon.
/**
 * @def fourcc_code(a, b, c, d)
 * @brief Macro for fourcc code.
 */
#define fourcc_code(a, b, c, d) ((std::uint32_t) (a) | ((std::uint32_t) (b) << 8) | ((std::uint32_t) (c) << 16) | ((std::uint32_t) (d) << 24))
/**
 * @def fourcc_mod_code(vendor, val)
 * @brief Macro for fourcc mod code.
 */
#define fourcc_mod_code(vendor, val) ((((uint64_t) vendor) << 56) | ((val) & 0x00ffffffffffffffULL))
/**
 * @def DRM_FORMAT_MOD_INVALID
 * @brief Macro for DRM FORMAT MOD INVALID.
 */
#define DRM_FORMAT_MOD_INVALID fourcc_mod_code(0, ((1ULL << 56) - 1))

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  /**
   * @def SUNSHINE_SHADERS_DIR
   * @brief Macro for SUNSHINE SHADERS DIR.
   */
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/opengl"
#endif

using namespace std::literals;

namespace gl {
  GladGLContext ctx;  ///< Loaded OpenGL function table for the active context.

  static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC egl_image_target_texture_2d_fn = nullptr;

  /**
   * @brief Bind an EGL image to the current OpenGL texture target.
   */
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC egl_image_target_texture_2d() {
    return egl_image_target_texture_2d_fn;
  }

  /**
   * @brief Drain and log pending OpenGL errors.
   */
  void drain_errors(const std::string_view &prefix) {
    GLenum err;
    while ((err = ctx.GetError()) != GL_NO_ERROR) {
      BOOST_LOG(error) << "GL: "sv << prefix << ": ["sv << util::hex(err).to_string_view() << ']';
    }
  }

  tex_t::~tex_t() {
    if (size() != 0) {
      ctx.DeleteTextures(size(), begin());
    }
  }

  tex_t tex_t::make(std::size_t count) {
    tex_t textures {count};

    ctx.GenTextures(textures.size(), textures.begin());

    std::array<float, 4> color = {0.0f, 0.0f, 0.0f, 1.0f};

    for (auto tex : textures) {
      gl::ctx.BindTexture(GL_TEXTURE_2D, tex);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  // x
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);  // y
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl::ctx.TexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color.data());
    }

    return textures;
  }

  frame_buf_t::~frame_buf_t() {
    if (begin()) {
      ctx.DeleteFramebuffers(size(), begin());
    }
  }

  frame_buf_t frame_buf_t::make(std::size_t count) {
    frame_buf_t frame_buf {count};

    ctx.GenFramebuffers(frame_buf.size(), frame_buf.begin());

    return frame_buf;
  }

  void frame_buf_t::copy(int id, int texture, int offset_x, int offset_y, int width, int height) {
    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, (*this)[id]);
    gl::ctx.ReadBuffer(GL_COLOR_ATTACHMENT0 + id);
    gl::ctx.BindTexture(GL_TEXTURE_2D, texture);
    gl::ctx.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, offset_x, offset_y, width, height);
  }

  std::string shader_t::err_str() {
    int length;
    ctx.GetShaderiv(handle(), GL_INFO_LOG_LENGTH, &length);

    std::string string;
    string.resize(length);

    ctx.GetShaderInfoLog(handle(), length, &length, string.data());

    string.resize(length - 1);

    return string;
  }

  util::Either<shader_t, std::string> shader_t::compile(const std::string_view &source, GLenum type) {
    shader_t shader;

    auto data = source.data();
    GLint length = source.length();

    shader._shader.el = ctx.CreateShader(type);
    ctx.ShaderSource(shader.handle(), 1, &data, &length);
    ctx.CompileShader(shader.handle());

    int status = 0;
    ctx.GetShaderiv(shader.handle(), GL_COMPILE_STATUS, &status);

    if (!status) {
      return shader.err_str();
    }

    return shader;
  }

  GLuint shader_t::handle() const {
    return _shader.el;
  }

  buffer_t buffer_t::make(util::buffer_t<GLint> &&offsets, const char *block, const std::string_view &data) {
    buffer_t buffer;
    buffer._block = block;
    buffer._size = data.size();
    buffer._offsets = std::move(offsets);

    ctx.GenBuffers(1, &buffer._buffer.el);
    ctx.BindBuffer(GL_UNIFORM_BUFFER, buffer.handle());
    ctx.BufferData(GL_UNIFORM_BUFFER, data.size(), (const std::uint8_t *) data.data(), GL_DYNAMIC_DRAW);

    return buffer;
  }

  GLuint buffer_t::handle() const {
    return _buffer.el;
  }

  const char *buffer_t::block() const {
    return _block;
  }

  void buffer_t::update(const std::string_view &view, std::size_t offset) {
    ctx.BindBuffer(GL_UNIFORM_BUFFER, handle());
    ctx.BufferSubData(GL_UNIFORM_BUFFER, offset, view.size(), (const void *) view.data());
  }

  void buffer_t::update(std::string_view *members, std::size_t count, std::size_t offset) {
    util::buffer_t<std::uint8_t> buffer {_size};

    for (int x = 0; x < count; ++x) {
      auto val = members[x];

      std::copy_n((const std::uint8_t *) val.data(), val.size(), &buffer[_offsets[x]]);
    }

    update(util::view(buffer.begin(), buffer.end()), offset);
  }

  std::string program_t::err_str() {
    int length;
    ctx.GetProgramiv(handle(), GL_INFO_LOG_LENGTH, &length);

    std::string string;
    string.resize(length);

    ctx.GetShaderInfoLog(handle(), length, &length, string.data());

    string.resize(length - 1);

    return string;
  }

  util::Either<program_t, std::string> program_t::link(const shader_t &vert, const shader_t &frag) {
    program_t program;

    program._program.el = ctx.CreateProgram();

    ctx.AttachShader(program.handle(), vert.handle());
    ctx.AttachShader(program.handle(), frag.handle());

    // p_handle stores a copy of the program handle, since program will be moved before
    // the fail guard function is called.
    auto fg = util::fail_guard([p_handle = program.handle(), &vert, &frag]() {
      ctx.DetachShader(p_handle, vert.handle());
      ctx.DetachShader(p_handle, frag.handle());
    });

    ctx.LinkProgram(program.handle());

    int status = 0;
    ctx.GetProgramiv(program.handle(), GL_LINK_STATUS, &status);

    if (!status) {
      return program.err_str();
    }

    return program;
  }

  void program_t::bind(const buffer_t &buffer) {
    ctx.UseProgram(handle());
    auto i = ctx.GetUniformBlockIndex(handle(), buffer.block());

    ctx.BindBufferBase(GL_UNIFORM_BUFFER, i, buffer.handle());
  }

  std::optional<buffer_t> program_t::uniform(const char *block, std::pair<const char *, std::string_view> *members, std::size_t count) {
    auto i = ctx.GetUniformBlockIndex(handle(), block);
    if (i == GL_INVALID_INDEX) {
      BOOST_LOG(error) << "Couldn't find index of ["sv << block << ']';
      return std::nullopt;
    }

    int size;
    ctx.GetActiveUniformBlockiv(handle(), i, GL_UNIFORM_BLOCK_DATA_SIZE, &size);

    bool error_flag = false;

    util::buffer_t<GLint> offsets {count};
    auto indices = (std::uint32_t *) alloca(count * sizeof(std::uint32_t));
    auto names = (const char **) alloca(count * sizeof(const char *));
    auto names_p = names;

    std::for_each_n(members, count, [names_p](auto &member) mutable {
      *names_p++ = std::get<0>(member);
    });

    std::fill_n(indices, count, GL_INVALID_INDEX);
    ctx.GetUniformIndices(handle(), count, names, indices);

    for (int x = 0; x < count; ++x) {
      if (indices[x] == GL_INVALID_INDEX) {
        error_flag = true;

        BOOST_LOG(error) << "Couldn't find ["sv << block << '.' << members[x].first << ']';
      }
    }

    if (error_flag) {
      return std::nullopt;
    }

    ctx.GetActiveUniformsiv(handle(), count, indices, GL_UNIFORM_OFFSET, offsets.begin());
    util::buffer_t<std::uint8_t> buffer {(std::size_t) size};

    for (int x = 0; x < count; ++x) {
      auto val = std::get<1>(members[x]);

      std::copy_n((const std::uint8_t *) val.data(), val.size(), &buffer[offsets[x]]);
    }

    return buffer_t::make(std::move(offsets), block, std::string_view {(char *) buffer.begin(), buffer.size()});
  }

  GLuint program_t::handle() const {
    return _program.el;
  }

}  // namespace gl

namespace gbm {
  device_destroy_fn device_destroy;  ///< Device destroy.
  create_device_fn create_device;  ///< Create device.

  /**
   * @brief Load GBM symbols required for EGL device creation.
   */
  int init() {
    static dyn::library_t *handle {nullptr};
    static bool funcs_loaded = false;

    if (funcs_loaded) {
      return 0;
    }

    if (!handle) {
      handle = dyn::handle({"libgbm.so.1", "libgbm.so"});
      if (!handle) {
        return -1;
      }
    }

    std::vector<std::tuple<GLADapiproc *, const char *>> funcs {
      {(GLADapiproc *) &device_destroy, "gbm_device_destroy"},
      {(GLADapiproc *) &create_device, "gbm_create_device"},
    };

    if (dyn::load(handle, funcs)) {
      return -1;
    }

    funcs_loaded = true;
    return 0;
  }
}  // namespace gbm

namespace egl {

  namespace {  // Keep privileged implementation details anonymous/local to this translation unit

#if !defined(__FreeBSD__)
    /**
     * @brief Temporarily owns CAP_SYS_NICE while creating EGL contexts.
     */
    class cap_sys_nice {
    public:
      static bool verify_cap_sys_nice() {
        cap_t caps = cap_get_proc();
        cap_value_t sys_nice = CAP_SYS_NICE;
        cap_flag_value_t value;
        cap_get_flag(caps, sys_nice, CAP_EFFECTIVE, &value);
        cap_free(caps);

        if (value != CAP_SET) {
          BOOST_LOG(debug) << "Failed to verify CAP_SYS_NICE effective capability"sv;
          return false;
        }

        return true;
      }

      cap_sys_nice() {
        caps = cap_get_proc();

        cap_value_t sys_nice = CAP_SYS_NICE;
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_nice, CAP_SET) || cap_set_proc(caps)) {
          BOOST_LOG(debug) << "Failed to gain CAP_SYS_NICE";
        }
      }

      ~cap_sys_nice() {
        cap_value_t sys_nice = CAP_SYS_NICE;
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_nice, CAP_CLEAR) || cap_set_proc(caps)) {
          BOOST_LOG(debug) << "Failed to drop CAP_SYS_NICE";
        }
        cap_free(caps);
      }

      cap_t caps;  ///< Caps
    };
#endif

    /**
     * @brief Reports that the privileged EGL worker rejected a task.
     */
    class privileged_egl_worker_stopped final: public std::runtime_error {
    public:
      using std::runtime_error::runtime_error;  ///< Inherit standard runtime error constructors.
    };

    class privileged_egl_worker {
    public:
      static void ensure_started() {
        instance();
      }

      static EGLContext eglCreateContext_privileged(EGLDisplay display, EGLConfig config, EGLContext share_context, EGLint const *attrib_list, bool &nice_warning) {
        try {
          return instance().run([display, config, share_context, attrib_list, &nice_warning] {
            nice_warning = false;
#if !defined(__FreeBSD__)
            nice_warning = !egl::cap_sys_nice::verify_cap_sys_nice();
#endif

            if (!eglBindAPI(EGL_OPENGL_API)) {
              BOOST_LOG(error) << "Couldn't bind API in privileged thread: ["sv << util::hex(eglGetError()).to_string_view() << ']';
              return EGL_NO_CONTEXT;
            }
            EGLContext privileged_ctx = eglCreateContext(display, config, share_context, attrib_list);
            if (privileged_ctx == EGL_NO_CONTEXT) {
              BOOST_LOG(error) << "Couldn't create EGL context in privileged thread: ["sv << util::hex(eglGetError()).to_string_view() << ']';
            }
            return privileged_ctx;
          });
        } catch (const privileged_egl_worker_stopped &e) {
          BOOST_LOG(error) << "Couldn't execute privileged EGL context creation: "sv << e.what();
          return EGL_NO_CONTEXT;
        }
      }

    private:
      static privileged_egl_worker &instance() {
        static privileged_egl_worker w;
        return w;
      }

      privileged_egl_worker():
          thread_ {[this] {
            sigset_t all;
            sigfillset(&all);
            if (pthread_sigmask(SIG_BLOCK, &all, nullptr) != 0) {
              BOOST_LOG(error) << "Failed to block signals in egl_worker"sv;
              queue_.stop();
              return;
            }

            platf::set_thread_name("egl_worker");
            for (;;) {
              auto task = queue_.pop();
              if (!task) {
                break;
              }
              (*task)();
            }
          }} {
      }

      ~privileged_egl_worker() {
        queue_.stop();
      }

      template<class F>
      auto run(F &&f) -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(
          [f = std::forward<F>(f)]() mutable -> R {
#if !defined(__FreeBSD__)
            cap_sys_nice nice;
#endif
            return f();
          }
        );
        auto fut = task->get_future();

        if (!queue_.raise([task]() mutable {
              (*task)();
            })) {
          throw privileged_egl_worker_stopped {"privileged_egl_worker: task rejected (worker stopping)"};
        }

        return fut.get();
      }

      safe::queue_t<std::function<void()>> queue_ {32, safe::queue_t<std::function<void()>>::overflow_policy_e::reject};
      std::jthread thread_;
    };
  }  // namespace

  /**
   * @brief Allows the EGL privileged worker thread to be constructed early.
   */
  void ensure_privileged_egl_worker_started() {
    privileged_egl_worker::ensure_started();
  }

  /**
   * @brief Log EGL failure details and return an error code.
   */
  bool fail() {
    return eglGetError() != EGL_SUCCESS;
  }

  display_t make_display(std::variant<gbm::gbm_t::pointer, wl_display *, _XDisplay *> native_display) {
    int egl_platform;
    void *native_display_p;

    switch (native_display.index()) {
      case 0:
        egl_platform = EGL_PLATFORM_GBM_MESA;
        native_display_p = std::get<0>(native_display);
        break;
      case 1:
        egl_platform = EGL_PLATFORM_WAYLAND_KHR;
        native_display_p = std::get<1>(native_display);
        break;
      case 2:
        egl_platform = EGL_PLATFORM_X11_KHR;
        native_display_p = std::get<2>(native_display);
        break;
      default:
        BOOST_LOG(error) << "egl::make_display(): Index ["sv << native_display.index() << "] not implemented"sv;
        return nullptr;
    }

    // Initialize EGL with the selected platform and its native display handle.
    EGLDisplay raw_display = EGL_NO_DISPLAY;

    if (eglGetPlatformDisplayEXT) {
      raw_display = eglGetPlatformDisplayEXT(egl_platform, native_display_p, nullptr);
    } else if (eglGetPlatformDisplay) {
      raw_display = eglGetPlatformDisplay(egl_platform, native_display_p, nullptr);
    }

    if (raw_display == EGL_NO_DISPLAY) {
      BOOST_LOG(error) << "Couldn't open EGL display: ["sv
                       << util::hex(eglGetError()).to_string_view() << ']';
      return nullptr;
    }
    display_t display {raw_display};

    int major;
    int minor;
    if (!eglInitialize(display.get(), &major, &minor)) {
      BOOST_LOG(error) << "Couldn't initialize EGL display: ["sv << util::hex(eglGetError()).to_string_view() << ']';
      return nullptr;
    }

    if (!gladLoaderLoadEGL(display.get())) {
      BOOST_LOG(error) << "Failed to reload EGL for initialized display"sv;
      return nullptr;
    }

    const char *extension_st = eglQueryString(display.get(), EGL_EXTENSIONS);
    const char *version = eglQueryString(display.get(), EGL_VERSION);
    const char *vendor = eglQueryString(display.get(), EGL_VENDOR);
    const char *apis = eglQueryString(display.get(), EGL_CLIENT_APIS);

    BOOST_LOG(debug) << "EGL: ["sv << vendor << "]: version ["sv << version << ']';
    BOOST_LOG(debug) << "API's supported: ["sv << apis << ']';

    std::array<const char *, 4> extensions {
      "EGL_KHR_create_context",
      "EGL_KHR_surfaceless_context",
      "EGL_EXT_image_dma_buf_import",
      "EGL_EXT_image_dma_buf_import_modifiers",
    };

    for (auto ext : extensions) {
      if (!std::string_view {extension_st}.contains(ext)) {
        BOOST_LOG(error) << "Missing extension: ["sv << ext << ']';
        return nullptr;
      }
    }

    return display;
  }

  /**
   * @brief Create an EGL/OpenGL context for capture or conversion.
   */
  std::optional<ctx_t> make_ctx(display_t::pointer display) {
    bool nice_warning = false;

    constexpr std::array<int, 3> conf_attr {
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_BIT,
      EGL_NONE
    };

    int count;
    EGLConfig conf;
    if (!eglChooseConfig(display, conf_attr.data(), &conf, 1, &count)) {
      BOOST_LOG(error) << "Couldn't set config attributes: ["sv << util::hex(eglGetError()).to_string_view() << ']';
      return std::nullopt;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
      BOOST_LOG(error) << "Couldn't bind API: ["sv << util::hex(eglGetError()).to_string_view() << ']';
      return std::nullopt;
    }

    const char *extension_st = eglQueryString(display, EGL_EXTENSIONS);

    std::vector<EGLint> attr;
    attr.push_back(EGL_CONTEXT_CLIENT_VERSION);
    attr.push_back(3);

    // Only add the high priority attribute if the driver explicitly supports it
    if (extension_st && std::string_view(extension_st).contains("EGL_IMG_context_priority"sv)) {
      BOOST_LOG(debug) << "EGL: High priority context supported"sv;
      attr.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG);
      attr.push_back(EGL_CONTEXT_PRIORITY_HIGH_IMG);
    }
    attr.push_back(EGL_NONE);

    EGLContext raw_ctx = egl::privileged_egl_worker::eglCreateContext_privileged(display, conf, EGL_NO_CONTEXT, attr.data(), nice_warning);
    if (raw_ctx == EGL_NO_CONTEXT) {
      return std::nullopt;
    }

    ctx_t ctx {display, raw_ctx};

    EGLint actual_priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
    std::string actual_priority_str = "MEDIUM";
    if (eglQueryContext(display, raw_ctx, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &actual_priority)) {
      if (actual_priority == EGL_CONTEXT_PRIORITY_HIGH_IMG) {
        actual_priority_str = "HIGH";
      }
      if (nice_warning) {
        BOOST_LOG(warning) << "EGL: context priority set to "sv << actual_priority_str << " but CAP_SYS_NICE capability is missing"sv;
      } else {
        BOOST_LOG(info) << "EGL: context priority set to "sv << actual_priority_str;
      }
    }

    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, raw_ctx)) {
      BOOST_LOG(error) << "Couldn't make current display"sv;
      return std::nullopt;
    }

    if (!gladLoadGLContext(&gl::ctx, eglGetProcAddress)) {
      BOOST_LOG(error) << "Couldn't load OpenGL library"sv;
      return std::nullopt;
    }

    gl::egl_image_target_texture_2d_fn =
      (gl::PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLADapiproc) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!gl::egl_image_target_texture_2d_fn) {
      BOOST_LOG(warning) << "GL: glEGLImageTargetTexture2DOES not available; DMA-BUF import will fail"sv;
    }

    // GetString returns const GLubyte* (unsigned char*); convert to std::string safely (avoids sonar cpp:S6996).
    auto gl_string = [](const GLubyte *s) {
      std::string result;
      while (s && *s) {
        result += static_cast<char>(*s++);
      }
      return result;
    };
    const auto gl_vendor = gl_string(gl::ctx.GetString(GL_VENDOR));
    const auto gl_renderer = gl_string(gl::ctx.GetString(GL_RENDERER));
    const auto gl_version = gl_string(gl::ctx.GetString(GL_VERSION));
    const auto gl_shader = gl_string(gl::ctx.GetString(GL_SHADING_LANGUAGE_VERSION));
    BOOST_LOG(debug) << "GL: vendor: "sv << gl_vendor;
    BOOST_LOG(debug) << "GL: renderer: "sv << gl_renderer;
    BOOST_LOG(debug) << "GL: version: "sv << gl_version;
    BOOST_LOG(debug) << "GL: shader: "sv << gl_shader;

    gl::ctx.PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    return ctx;
  }

  /**
   * @brief EGL attribute pair describing one DMA-BUF plane.
   */
  struct plane_attr_t {
    EGLAttrib fd;  ///< EGL attribute key for a plane file descriptor.
    EGLAttrib offset;  ///< Offset.
    EGLAttrib pitch;  ///< Pitch.
    EGLAttrib lo;  ///< Lo.
    EGLAttrib hi;  ///< Hi.
  };

  /**
   * @brief Build EGL attributes for one DMA-BUF plane.
   *
   * @param plane_indice Zero-based plane index in the DMA-BUF descriptor.
   * @return EGL attribute keys for that plane's file descriptor, offset, pitch, and modifier.
   */
  inline plane_attr_t get_plane(std::uint32_t plane_indice) {
    switch (plane_indice) {
      case 0:
        return {
          EGL_DMA_BUF_PLANE0_FD_EXT,
          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
          EGL_DMA_BUF_PLANE0_PITCH_EXT,
          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        };
      case 1:
        return {
          EGL_DMA_BUF_PLANE1_FD_EXT,
          EGL_DMA_BUF_PLANE1_OFFSET_EXT,
          EGL_DMA_BUF_PLANE1_PITCH_EXT,
          EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        };
      case 2:
        return {
          EGL_DMA_BUF_PLANE2_FD_EXT,
          EGL_DMA_BUF_PLANE2_OFFSET_EXT,
          EGL_DMA_BUF_PLANE2_PITCH_EXT,
          EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        };
      case 3:
        return {
          EGL_DMA_BUF_PLANE3_FD_EXT,
          EGL_DMA_BUF_PLANE3_OFFSET_EXT,
          EGL_DMA_BUF_PLANE3_PITCH_EXT,
          EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
        };
    }

    // Avoid warning
    return {};
  }

  /**
   * @brief Get EGL attributes for eglCreateImage() to import the provided surface.
   * @param surface The surface descriptor.
   * @return Vector of EGL attributes.
   */
  std::vector<EGLAttrib> surface_descriptor_to_egl_attribs(const surface_descriptor_t &surface) {
    std::vector<EGLAttrib> attribs;

    attribs.emplace_back(EGL_WIDTH);
    attribs.emplace_back(surface.width);
    attribs.emplace_back(EGL_HEIGHT);
    attribs.emplace_back(surface.height);
    attribs.emplace_back(EGL_LINUX_DRM_FOURCC_EXT);
    attribs.emplace_back(surface.fourcc);

    for (auto x = 0; x < 4; ++x) {
      auto fd = surface.fds[x];
      if (fd < 0) {
        continue;
      }

      auto plane_attr = get_plane(x);

      attribs.emplace_back(plane_attr.fd);
      attribs.emplace_back(fd);
      attribs.emplace_back(plane_attr.offset);
      attribs.emplace_back(surface.offsets[x]);
      attribs.emplace_back(plane_attr.pitch);
      attribs.emplace_back(surface.pitches[x]);

      if (surface.modifier != DRM_FORMAT_MOD_INVALID) {
        attribs.emplace_back(plane_attr.lo);
        attribs.emplace_back(surface.modifier & 0xFFFFFFFF);
        attribs.emplace_back(plane_attr.hi);
        attribs.emplace_back(surface.modifier >> 32);
      }
    }

    attribs.emplace_back(EGL_NONE);
    return attribs;
  }

  /**
   * @brief Import the source frame texture for EGL/OpenGL conversion.
   *
   * @param egl_display EGL display used to create the image.
   * @param xrgb XRGB surface descriptor to import.
   * @return Imported RGB image, or empty when import fails.
   */
  std::optional<rgb_t> import_source(display_t::pointer egl_display, const surface_descriptor_t &xrgb) {
    const auto attribs = surface_descriptor_to_egl_attribs(xrgb);

    rgb_t rgb {
      egl_display,
      eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data()),
      gl::tex_t::make(1)
    };

    if (!rgb->xrgb8) {
      auto err = eglGetError();

      // Fallback: retry without modifier if GPU rejects the display's format modifier
      // Some displays (e.g. USB/DisplayLink) report modifiers the GPU cannot handle
      if (err == EGL_BAD_MATCH && xrgb.modifier != DRM_FORMAT_MOD_INVALID) {
        surface_descriptor_t xrgb_linear = xrgb;
        xrgb_linear.modifier = DRM_FORMAT_MOD_INVALID;
        auto attribs_linear = surface_descriptor_to_egl_attribs(xrgb_linear);

        rgb = {
          egl_display,
          eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs_linear.data()),
          gl::tex_t::make(1)
        };

        if (!rgb->xrgb8) {
          BOOST_LOG(error) << "Couldn't import RGB Image: "sv << util::hex(err).to_string_view();
          return std::nullopt;
        }
      } else {
        BOOST_LOG(error) << "Couldn't import RGB Image: "sv << util::hex(err).to_string_view();
        return std::nullopt;
      }
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);
    if (!gl::egl_image_target_texture_2d()) {
      BOOST_LOG(error) << "glEGLImageTargetTexture2DOES is not available; cannot import RGB DMA-BUF"sv;
      return std::nullopt;
    }
    gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, rgb->xrgb8);

    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

    gl_drain_errors;

    return rgb;
  }

  /**
   * @brief Create a black RGB texture of the specified image size.
   * @param img The image to use for texture sizing.
   * @return The new RGB texture.
   */
  rgb_t create_blank(platf::img_t &img) {
    rgb_t rgb {
      EGL_NO_DISPLAY,
      EGL_NO_IMAGE,
      gl::tex_t::make(1)
    };

    gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);
    gl::ctx.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, img.width, img.height);
    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

    auto framebuf = gl::frame_buf_t::make(1);
    framebuf.bind(&rgb->tex[0], &rgb->tex[0] + 1);

    GLenum attachment = GL_COLOR_ATTACHMENT0;
    gl::ctx.DrawBuffers(1, &attachment);
    const std::array<GLuint, 4> rgb_black = {0, 0, 0, 0};
    gl::ctx.ClearBufferuiv(GL_COLOR, 0, rgb_black.data());

    gl_drain_errors;

    return rgb;
  }

  // Constants for clear black color Y, U, V. U & V are same so:
  const std::array<float, 4> y_black = {0.0f, 0.0f, 0.0f, 0.0f};  ///< Y black.
  const std::array<float, 4> uv_black = {0.5f, 0.5f, 0.5f, 0.5f};  ///< Uv black.

  /**
   * @brief Bind NV12 target framebuffers to their Y and UV plane textures.
   *
   * @param nv12 Imported NV12 target whose textures receive rendered output.
   */
  void nv12_bind_framebuffers(nv12_t &nv12) {
    constexpr std::array<GLenum, 2> attachments {{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1}};

    for (size_t x = 0; x < attachments.size(); ++x) {
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, nv12->buf[x]);
      gl::ctx.DrawBuffers(1, &attachments[x]);
      gl::ctx.ClearBufferfv(GL_COLOR, 0, x == 0 ? y_black.data() : uv_black.data());
    }

    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);

    gl_drain_errors;
  }

  /**
   * @brief Bind YUV444 target framebuffers to their Y, U, and V plane textures.
   *
   * @param yuv444 Imported YUV444 target whose textures receive rendered output.
   */
  void yuv44_bind_framebuffers(yuv444_t &yuv444) {
    constexpr std::array<GLenum, 3> attachments {{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2}};

    for (size_t x = 0; x < attachments.size(); ++x) {
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, yuv444->buf[x]);
      gl::ctx.DrawBuffers(1, &attachments[x]);
      gl::ctx.ClearBufferfv(GL_COLOR, 0, x == 0 ? y_black.data() : uv_black.data());
    }

    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);

    gl_drain_errors;
  }

  /**
   * @brief Import the output frame target for EGL/OpenGL conversion.
   *
   * @param egl_display EGL display used to create the images.
   * @param fds File descriptors backing the NV12 image.
   * @param y Luma plane descriptor.
   * @param uv Chroma plane descriptor.
   * @return Imported NV12 image, or empty when import fails.
   */
  std::optional<nv12_t> import_target(display_t::pointer egl_display, std::array<file_t, nv12_img_t::num_fds> &&fds, const surface_descriptor_t &y, const surface_descriptor_t &uv) {
    auto y_attribs = surface_descriptor_to_egl_attribs(y);
    auto uv_attribs = surface_descriptor_to_egl_attribs(uv);

    nv12_t nv12 {
      egl_display,
      eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, y_attribs.data()),
      eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, uv_attribs.data()),
      gl::tex_t::make(2),
      gl::frame_buf_t::make(2),
      std::move(fds)
    };

    if (!nv12->r8 || !nv12->bg88) {
      BOOST_LOG(error) << "Couldn't import YUV target: "sv << util::hex(eglGetError()).to_string_view();

      return std::nullopt;
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, nv12->tex[0]);
    if (!gl::egl_image_target_texture_2d()) {
      BOOST_LOG(error) << "glEGLImageTargetTexture2DOES is not available; cannot import YUV DMA-BUF"sv;
      return std::nullopt;
    }
    gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, nv12->r8);

    gl::ctx.BindTexture(GL_TEXTURE_2D, nv12->tex[1]);
    gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, nv12->bg88);

    nv12->buf.bind(std::begin(nv12->tex), std::end(nv12->tex));

    nv12_bind_framebuffers(nv12);

    return nv12;
  }

  /**
   * @brief Import a YUV444 target image from DMA-BUF descriptors.
   *
   * @param egl_display EGL display used to create the images.
   * @param fds File descriptors backing the YUV444 image.
   * @param y Luma plane descriptor.
   * @param u U chroma plane descriptor.
   * @param v V chroma plane descriptor.
   * @return Imported YUV444 image, or empty when import fails.
   */
  std::optional<yuv444_t> import_target_yuv444(
    display_t::pointer egl_display,
    std::array<file_t, yuv444_img_t::num_fds> &&fds,
    const surface_descriptor_t &y,
    const surface_descriptor_t &u,
    const surface_descriptor_t &v
  ) {
    auto y_attribs = surface_descriptor_to_egl_attribs(y);
    auto u_attribs = surface_descriptor_to_egl_attribs(u);
    auto v_attribs = surface_descriptor_to_egl_attribs(v);

    yuv444_t yuv444 {
      egl_display,
      eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, y_attribs.data()),
      eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, u_attribs.data()),
      eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, v_attribs.data()),
      gl::tex_t::make(3),
      gl::frame_buf_t::make(3),
      std::move(fds)
    };

    if (!yuv444->r8 || !yuv444->g8 || !yuv444->b8) {
      BOOST_LOG(error) << "Couldn't import YUV target: "sv << util::hex(eglGetError()).to_string_view();

      return std::nullopt;
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, yuv444->tex[0]);
    if (!gl::egl_image_target_texture_2d()) {
      BOOST_LOG(error) << "glEGLImageTargetTexture2DOES is not available; cannot import YUV DMA-BUF"sv;
      return std::nullopt;
    }
    gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, yuv444->r8);

    gl::ctx.BindTexture(GL_TEXTURE_2D, yuv444->tex[1]);
    gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, yuv444->g8);

    gl::ctx.BindTexture(GL_TEXTURE_2D, yuv444->tex[2]);
    gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, yuv444->b8);

    yuv444->buf.bind(std::begin(yuv444->tex), std::end(yuv444->tex));

    yuv44_bind_framebuffers(yuv444);

    return yuv444;
  }

  /**
   * @brief Allocate storage for each plane texture of a render target.
   *
   * @param tex Plane textures, one per entry in `planes`.
   * @param planes Sized internal format, width and height of each plane.
   * @param storage Optional allocator. Uses `TexStorage2D` when empty.
   * @return True when every plane was allocated.
   */
  template<std::size_t N>
  static bool allocate_target_storage(const gl::tex_t &tex, const std::array<std::tuple<GLint, int, int>, N> &planes, const target_storage_fn &storage) {
    for (std::size_t i = 0; i < N; ++i) {
      const auto &[format, plane_width, plane_height] = planes[i];

      gl::ctx.BindTexture(GL_TEXTURE_2D, tex[i]);
      if (!storage) {
        gl::ctx.TexStorage2D(GL_TEXTURE_2D, 1, format, plane_width, plane_height);
      } else if (storage(i, format, plane_width, plane_height)) {
        return false;
      }
    }

    return true;
  }

  /**
   * @brief Create biplanar YUV textures to render into.
   * @param width Width of the target frame.
   * @param height Height of the target frame.
   * @param format Format of the target frame.
   * @param storage Optional allocator for the plane textures' storage. Uses `TexStorage2D` when empty.
   * @return The new RGB texture.
   */
  std::optional<nv12_t> create_nv12_target(int width, int height, AVPixelFormat format, const target_storage_fn &storage) {
    nv12_t nv12 {
      nv12_img_t {
        .display = EGL_NO_DISPLAY,
        .r8 = EGL_NO_IMAGE,
        .bg88 = EGL_NO_IMAGE,
        .tex = gl::tex_t::make(2),
        .buf = gl::frame_buf_t::make(2),
      },
    };

    GLint y_format;
    GLint uv_format;

    // Determine the size of each plane element
    auto fmt_desc = av_pix_fmt_desc_get(format);
    if (fmt_desc->comp[0].depth <= 8) {
      y_format = GL_R8;
      uv_format = GL_RG8;
    } else if (fmt_desc->comp[0].depth <= 16) {
      y_format = GL_R16;
      uv_format = GL_RG16;
    } else {
      BOOST_LOG(error) << "Unsupported target pixel format: "sv << format;
      return std::nullopt;
    }

    const std::array<std::tuple<GLint, int, int>, 2> planes {{
      {y_format, width, height},
      {uv_format, width >> fmt_desc->log2_chroma_w, height >> fmt_desc->log2_chroma_h},
    }};
    if (!allocate_target_storage(nv12->tex, planes, storage)) {
      return std::nullopt;
    }

    nv12->buf.bind(std::begin(nv12->tex), std::end(nv12->tex));

    nv12_bind_framebuffers(nv12);

    return nv12;
  }

  /**
   * @brief Create YUV444 target.
   */
  std::optional<yuv444_t> create_yuv444_target(int width, int height, AVPixelFormat format, const target_storage_fn &storage) {
    yuv444_t yuv444 {
      yuv444_img_t {
        .display = EGL_NO_DISPLAY,
        .r8 = EGL_NO_IMAGE,
        .g8 = EGL_NO_IMAGE,
        .b8 = EGL_NO_IMAGE,
        .tex = gl::tex_t::make(3),
        .buf = gl::frame_buf_t::make(3),
      },
    };

    GLint y_format;
    GLint u_format;
    GLint v_format;

    // Determine the size of each plane element
    auto fmt_desc = av_pix_fmt_desc_get(format);
    if (fmt_desc->comp[0].depth <= 8) {
      y_format = GL_R8;
      u_format = GL_R8;
      v_format = GL_R8;
    } else if (fmt_desc->comp[0].depth <= 16) {
      y_format = GL_R16;
      u_format = GL_R16;
      v_format = GL_R16;
    } else {
      BOOST_LOG(error) << "Unsupported target pixel format: "sv << format;
      return std::nullopt;
    }

    const std::array<std::tuple<GLint, int, int>, 3> planes {{
      {y_format, width, height},
      {u_format, width, height},
      {v_format, width, height},
    }};
    if (!allocate_target_storage(yuv444->tex, planes, storage)) {
      return std::nullopt;
    }

    yuv444->buf.bind(std::begin(yuv444->tex), std::end(yuv444->tex));

    yuv44_bind_framebuffers(yuv444);

    return yuv444;
  }

  void sws_t::apply_colorspace(const video::sunshine_colorspace_t &colorspace, bool is_yuv444) {
    auto color_p = video::color_vectors_from_colorspace(colorspace, true);

    std::array<std::string_view, 5> members {
      util::view(color_p->color_vec_y),
      util::view(color_p->color_vec_u),
      util::view(color_p->color_vec_v),
      util::view(color_p->range_y),
      util::view(color_p->range_uv),
    };

    color_matrix.update(members.data(), members.size());

    int planesCount = is_yuv444 ? 3 : 2;

    for (int i = 0; i < planesCount; i++) {
      program[i].bind(color_matrix);
    }
  }

  /**
   * @brief Configure the EGL/OpenGL scaling and colorspace conversion pipeline.
   *
   * @param sws Software-scaling pipeline to configure.
   * @param color_p Color p.
   * @param tex Texture resource used by the converter.
   * @param is_yuv444 Is YUV444.
   * @return 0 when shaders, framebuffers, and color uniforms are ready; nonzero on failure.
   */
  int configure_sws_pipeline(sws_t &sws, const video::color_t *color_p, gl::tex_t &&tex, bool is_yuv444) {
    std::array<std::pair<const char *, std::string_view>, 5> members {{
      std::make_pair("color_vec_y", util::view(color_p->color_vec_y)),
      std::make_pair("color_vec_u", util::view(color_p->color_vec_u)),
      std::make_pair("color_vec_v", util::view(color_p->color_vec_v)),
      std::make_pair("range_y", util::view(color_p->range_y)),
      std::make_pair("range_uv", util::view(color_p->range_uv)),
    }};

    auto color_matrix = sws.program[0].uniform("ColorMatrix", members.data(), members.size());
    if (!color_matrix) {
      return -1;
    }

    sws.color_matrix = std::move(*color_matrix);

    sws.tex = std::move(tex);

    sws.cursor_framebuffer = gl::frame_buf_t::make(1);
    sws.cursor_framebuffer.bind(&sws.tex[0], &sws.tex[1]);

    int programCount = is_yuv444 ? 3 : 2;

    for (int i = 0; i < programCount; i++) {
      sws.program[i].bind(sws.color_matrix);
    }

    gl::ctx.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl_drain_errors;
    return 0;
  }

  std::optional<sws_t> sws_t::make_nv12(int in_width, int in_height, int out_width, int out_height, gl::tex_t &&tex) {
    sws_t sws;

    sws.serial = std::numeric_limits<std::uint64_t>::max();

    // Ensure aspect ratio is maintained
    auto scalar = std::fminf(out_width / (float) in_width, out_height / (float) in_height);
    auto out_width_f = in_width * scalar;
    auto out_height_f = in_height * scalar;

    // result is always positive
    auto offsetX_f = (out_width - out_width_f) / 2;
    auto offsetY_f = (out_height - out_height_f) / 2;

    sws.out_width = out_width_f;
    sws.out_height = out_height_f;

    sws.in_width = in_width;
    sws.in_height = in_height;

    sws.offsetX = offsetX_f;
    sws.offsetY = offsetY_f;

    auto width_i = 1.0f / sws.out_width;

    {
      constexpr std::array<const char *, 5> sources {{
        SUNSHINE_SHADERS_DIR "/ConvertUV.frag",
        SUNSHINE_SHADERS_DIR "/ConvertUV.vert",
        SUNSHINE_SHADERS_DIR "/ConvertY.frag",
        SUNSHINE_SHADERS_DIR "/Scene.vert",
        SUNSHINE_SHADERS_DIR "/Scene.frag",
      }};

      constexpr std::array<GLenum, 2> shader_type {{
        GL_FRAGMENT_SHADER,
        GL_VERTEX_SHADER,
      }};

      constexpr auto count = sources.size();
      std::array<util::Either<gl::shader_t, std::string>, count> compiled_sources;

      bool error_flag = false;
      for (size_t x = 0; x < count; ++x) {
        auto &compiled_source = compiled_sources[x];

        compiled_source = gl::shader_t::compile(file_handler::read_file(sources[x]), shader_type[x % 2]);
        gl_drain_errors;

        if (compiled_source.has_right()) {
          BOOST_LOG(error) << sources[x] << ": "sv << compiled_source.right();
          error_flag = true;
        }
      }

      if (error_flag) {
        return std::nullopt;
      }

      auto program = gl::program_t::link(compiled_sources[3].left(), compiled_sources[4].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker: "sv << program.right();
        return std::nullopt;
      }

      // Cursor - shader
      sws.program[2] = std::move(program.left());

      program = gl::program_t::link(compiled_sources[1].left(), compiled_sources[0].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker: "sv << program.right();
        return std::nullopt;
      }

      // UV - shader
      sws.program[1] = std::move(program.left());

      program = gl::program_t::link(compiled_sources[3].left(), compiled_sources[2].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker: "sv << program.right();
        return std::nullopt;
      }

      // Y - shader
      sws.program[0] = std::move(program.left());
    }

    auto loc_width_i = gl::ctx.GetUniformLocation(sws.program[1].handle(), "width_i");
    if (loc_width_i < 0) {
      BOOST_LOG(error) << "Couldn't find uniform [width_i]"sv;
      return std::nullopt;
    }

    gl::ctx.UseProgram(sws.program[1].handle());
    gl::ctx.Uniform1fv(loc_width_i, 1, &width_i);

    auto color_p = video::color_vectors_from_colorspace({video::colorspace_e::rec601, false, 8}, true);

    int pipeline = configure_sws_pipeline(sws, color_p, std::move(tex), false);
    if (pipeline < 0) {
      return std::nullopt;
    }

    return sws;
  }

  std::optional<sws_t> sws_t::make_yuv444(int in_width, int in_height, int out_width, int out_height, gl::tex_t &&tex) {
    sws_t sws;

    sws.serial = std::numeric_limits<std::uint64_t>::max();

    // Ensure aspect ratio is maintained
    auto scalar = std::fminf(out_width / (float) in_width, out_height / (float) in_height);
    auto out_width_f = in_width * scalar;
    auto out_height_f = in_height * scalar;

    // result is always positive
    auto offsetX_f = out_width - out_width_f;
    auto offsetY_f = out_height - out_height_f;

    sws.out_width = out_width_f;
    sws.out_height = out_height_f;

    sws.in_width = in_width;
    sws.in_height = in_height;

    sws.offsetX = offsetX_f;
    sws.offsetY = offsetY_f;

    {
      constexpr std::array<const char *, 5> sources {{
        SUNSHINE_SHADERS_DIR "/Scene.vert",
        SUNSHINE_SHADERS_DIR "/ConvertV.frag",
        SUNSHINE_SHADERS_DIR "/ConvertU.frag",
        SUNSHINE_SHADERS_DIR "/ConvertY.frag",
        SUNSHINE_SHADERS_DIR "/Scene.frag",
      }};

      constexpr std::array<GLenum, 2> shader_type {{
        GL_FRAGMENT_SHADER,
        GL_VERTEX_SHADER,
      }};

      constexpr auto count = sources.size();
      std::array<util::Either<gl::shader_t, std::string>, count> compiled_sources;

      bool error_flag = false;
      for (int x = 0; x < count; ++x) {
        auto &compiled_source = compiled_sources[x];

        int num = x == 0 ? 1 : 0;
        compiled_source = gl::shader_t::compile(file_handler::read_file(sources[x]), shader_type[num]);
        gl_drain_errors;

        if (compiled_source.has_right()) {
          BOOST_LOG(error) << sources[x] << ": "sv << compiled_source.right();
          error_flag = true;
        }
      }

      if (error_flag) {
        return std::nullopt;
      }

      auto program = gl::program_t::link(compiled_sources[0].left(), compiled_sources[4].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker (cursor shader): "sv << program.right();
        return std::nullopt;
      }

      // Cursor - shader
      sws.program[3] = std::move(program.left());

      program = gl::program_t::link(compiled_sources[0].left(), compiled_sources[1].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker (V - shader): "sv << program.right();
        return std::nullopt;
      }

      // V - shader
      sws.program[2] = std::move(program.left());

      program = gl::program_t::link(compiled_sources[0].left(), compiled_sources[2].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker (U - shader): "sv << program.right();
        return std::nullopt;
      }

      // U - shader
      sws.program[1] = std::move(program.left());

      program = gl::program_t::link(compiled_sources[0].left(), compiled_sources[3].left());
      if (program.has_right()) {
        BOOST_LOG(error) << "GL linker (Y - shader): "sv << program.right();
        return std::nullopt;
      }

      // Y - shader
      sws.program[0] = std::move(program.left());
    }

    auto color_p = video::color_vectors_from_colorspace({video::colorspace_e::rec709, true, 8}, false);

    int pipeline = configure_sws_pipeline(sws, color_p, std::move(tex), true);
    if (pipeline < 0) {
      return std::nullopt;
    }

    return sws;
  }

  int sws_t::blank(gl::frame_buf_t &fb, int offsetX_, int offsetY_, int width, int height, bool is_yuv444) {
    auto f = [this, &offsetX_, &offsetY_, &width, &height]() {
      std::swap(offsetX_, this->offsetX);
      std::swap(offsetY_, this->offsetY);
      std::swap(width, this->out_width);
      std::swap(height, this->out_height);
    };

    f();
    auto fg = util::fail_guard(f);
    if (is_yuv444) {
      return convert_yuv444(fb);
    }
    return convert_nv12(fb);
  }

  std::optional<sws_t> sws_t::make(int in_width, int in_height, int out_width, int out_height, AVPixelFormat format, bool is_yuv444) {
    GLint gl_format;

    // Decide the bit depth format of the backing texture based the target frame format
    auto fmt_desc = av_pix_fmt_desc_get(format);
    switch (fmt_desc->comp[0].depth) {
      case 8:
        gl_format = GL_RGBA8;
        break;

      case 10:
        gl_format = GL_RGB10_A2;
        break;

      case 12:
        gl_format = GL_RGBA12;
        break;

      case 16:
        gl_format = GL_RGBA16;
        break;

      default:
        BOOST_LOG(error) << "Unsupported pixel format for EGL frame: "sv << (int) format;
        return std::nullopt;
    }

    auto tex = gl::tex_t::make(2);
    gl::ctx.BindTexture(GL_TEXTURE_2D, tex[0]);
    gl::ctx.TexStorage2D(GL_TEXTURE_2D, 1, gl_format, in_width, in_height);

    if (is_yuv444) {
      return make_yuv444(in_width, in_height, out_width, out_height, std::move(tex));
    }
    return make_nv12(in_width, in_height, out_width, out_height, std::move(tex));
  }

  void sws_t::load_ram(platf::img_t &img) {
    loaded_texture = tex[0];

    gl::ctx.BindTexture(GL_TEXTURE_2D, loaded_texture);
    gl::ctx.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width, img.height, GL_BGRA, GL_UNSIGNED_BYTE, img.data);
  }

  void sws_t::load_vram(img_descriptor_t &img, int offset_x, int offset_y, int texture, bool is_yuv444) {
    // When only a sub-part of the image must be encoded...
    const bool copy = offset_x || offset_y || img.sd.width != in_width || img.sd.height != in_height;
    if (copy) {
      auto framebuf = gl::frame_buf_t::make(1);
      framebuf.bind(&texture, &texture + 1);

      loaded_texture = tex[0];
      framebuf.copy(0, loaded_texture, offset_x, offset_y, in_width, in_height);
    } else {
      loaded_texture = texture;
    }

    if (img.data) {
      GLenum attachment = GL_COLOR_ATTACHMENT0;

      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, cursor_framebuffer[0]);

      // For NV12 cursor program index is 2, for YUV444 it's 3
      const int cursor_program = is_yuv444 ? 3 : 2;
      gl::ctx.UseProgram(program[cursor_program].handle());

      // When a copy has already been made...
      if (!copy) {
        gl::ctx.BindTexture(GL_TEXTURE_2D, texture);
        gl::ctx.DrawBuffers(1, &attachment);

        gl::ctx.Viewport(0, 0, in_width, in_height);
        gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);

        loaded_texture = tex[0];
      }

      gl::ctx.BindTexture(GL_TEXTURE_2D, tex[1]);
      if (serial != img.serial) {
        serial = img.serial;

        gl::ctx.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.src_w, img.src_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, img.data);
      }

      gl::ctx.Enable(GL_BLEND);

      gl::ctx.DrawBuffers(1, &attachment);

#ifndef NDEBUG
      auto status = gl::ctx.CheckFramebufferStatus(GL_FRAMEBUFFER);
      if (status != GL_FRAMEBUFFER_COMPLETE) {
        BOOST_LOG(error) << "Pass Cursor: CheckFramebufferStatus() --> [0x"sv << util::hex(status).to_string_view() << ']';
        return;
      }
#endif

      gl::ctx.Viewport(img.x, img.y, img.width, img.height);
      gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);

      gl::ctx.Disable(GL_BLEND);

      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  int sws_t::draw_programs_to_buffers(GLenum attachments[], gl::frame_buf_t &fb, int count, bool is_yuv444) {
    for (int x = 0; x < count; ++x) {
      gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, fb[x]);
      gl::ctx.DrawBuffers(1, &attachments[x]);

#ifndef NDEBUG
      auto status = gl::ctx.CheckFramebufferStatus(GL_FRAMEBUFFER);
      if (status != GL_FRAMEBUFFER_COMPLETE) {
        BOOST_LOG(error) << "Pass "sv << x << ": CheckFramebufferStatus() --> [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
#endif

      int sizeCoef = is_yuv444 ? 1 : x + 1;

      gl::ctx.UseProgram(program[x].handle());
      gl::ctx.Viewport(offsetX / sizeCoef, offsetY / sizeCoef, out_width / sizeCoef, out_height / sizeCoef);
      gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    return 0;
  }

  int sws_t::convert_nv12(gl::frame_buf_t &fb) {
    gl::ctx.BindTexture(GL_TEXTURE_2D, loaded_texture);

    std::array<GLenum, 2> attachments {
      GL_COLOR_ATTACHMENT0,
      GL_COLOR_ATTACHMENT1
    };

    int attachmentsCount = attachments.size();

    int drawBuffers = draw_programs_to_buffers(attachments.data(), fb, attachmentsCount, false);
    if (drawBuffers < 0) {
      return -1;
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

    gl::ctx.Flush();

    return 0;
  }

  int sws_t::convert_yuv444(gl::frame_buf_t &fb) {
    gl::ctx.BindTexture(GL_TEXTURE_2D, loaded_texture);

    std::array<GLenum, 3> attachments {
      GL_COLOR_ATTACHMENT0,
      GL_COLOR_ATTACHMENT1,
      GL_COLOR_ATTACHMENT2
    };

    int attachmentsCount = attachments.size();

    int drawBuffers = draw_programs_to_buffers(attachments.data(), fb, attachmentsCount, true);
    if (drawBuffers < 0) {
      return -1;
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

    gl::ctx.Flush();

    return 0;
  }
}  // namespace egl

/**
 * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
 *
 * @param frame Video or graphics frame being processed.
 */
void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}
