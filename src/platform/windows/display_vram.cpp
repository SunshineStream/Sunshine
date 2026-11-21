/**
 * @file src/platform/windows/display_vram.cpp
 * @brief Definitions for handling video ram.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>

// platform includes
#include <d3dcompiler.h>
#include <DirectXMath.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// lib includes
#include <AMF/core/Factory.h>
#include <boost/algorithm/string/predicate.hpp>

// local includes
#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/nvenc/nvenc_config.h"
#include "src/nvenc/nvenc_dynamic_factory.h"
#include "src/video.h"
#include "utf_utils.h"

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  /**
   * @def SUNSHINE_SHADERS_DIR
   * @brief Macro for SUNSHINE SHADERS DIR.
   */
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif
namespace platf {
  using namespace std::literals;
}

static void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}

/**
 * @brief FFmpeg hardware frame pointer released with `av_frame_free`.
 */
using frame_t = util::safe_ptr<AVFrame, &free_frame>;

namespace platf::dxgi {

  /**
   * @brief Create a buffer object or message.
   *
   * @param device D3D, audio, or platform device used by the operation.
   * @param t Initial value used to populate the GPU buffer.
   * @return Constructed buffer object.
   */
  template<class T>
  buf_t make_buffer(device_t::pointer device, const T &t) {
    static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");

    D3D11_BUFFER_DESC buffer_desc {
      .ByteWidth = sizeof(T),
      .Usage = D3D11_USAGE_IMMUTABLE,
      .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };

    D3D11_SUBRESOURCE_DATA init_data {
      .pSysMem = &t,
    };

    buf_t::pointer buf_p;
    auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return buf_t {buf_p};
  }

  /**
   * @brief Create a blend object or message.
   *
   * @param device D3D, audio, or platform device used by the operation.
   * @param enable Whether the blend state should enable blending.
   * @param invert Whether the blend state should invert the color channels.
   * @return Constructed blend object.
   */
  blend_t make_blend(device_t::pointer device, bool enable, bool invert) {
    D3D11_BLEND_DESC bdesc {};
    auto &rt = bdesc.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (enable) {
      rt.BlendOp = D3D11_BLEND_OP_ADD;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

      if (invert) {
        // Invert colors
        rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
      } else {
        // Regular alpha blending
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      }

      rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
      rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    }

    blend_t blend;
    auto status = device->CreateBlendState(&bdesc, &blend);
    if (status) {
      BOOST_LOG(error) << "Failed to create blend state: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blend;
  }

  blob_t convert_yuv420_packed_uv_type0_ps_hlsl;  ///< Convert yuv420 packed uv type0 ps hlsl.
  blob_t convert_yuv420_packed_uv_type0_ps_linear_hlsl;  ///< Convert yuv420 packed uv type0 ps linear hlsl.
  blob_t convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl;  ///< Convert yuv420 packed uv type0 ps perceptual quantizer hlsl.
  blob_t convert_yuv420_packed_uv_type0_vs_hlsl;  ///< Convert yuv420 packed uv type0 vs hlsl.
  blob_t convert_yuv420_packed_uv_type0s_ps_hlsl;  ///< Convert yuv420 packed uv type0s ps hlsl.
  blob_t convert_yuv420_packed_uv_type0s_ps_linear_hlsl;  ///< Convert yuv420 packed uv type0s ps linear hlsl.
  blob_t convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl;  ///< Convert yuv420 packed uv type0s ps perceptual quantizer hlsl.
  blob_t convert_yuv420_packed_uv_type0s_vs_hlsl;  ///< Convert yuv420 packed uv type0s vs hlsl.
  blob_t convert_yuv420_planar_y_ps_hlsl;  ///< Convert yuv420 planar y ps hlsl.
  blob_t convert_yuv420_planar_y_ps_linear_hlsl;  ///< Convert yuv420 planar y ps linear hlsl.
  blob_t convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl;  ///< Convert yuv420 planar y ps perceptual quantizer hlsl.
  blob_t convert_yuv420_planar_y_vs_hlsl;  ///< Convert yuv420 planar y vs hlsl.
  blob_t convert_yuv444_packed_ayuv_ps_hlsl;  ///< Convert YUV444 packed ayuv ps hlsl.
  blob_t convert_yuv444_packed_ayuv_ps_linear_hlsl;  ///< Convert YUV444 packed ayuv ps linear hlsl.
  blob_t convert_yuv444_packed_vs_hlsl;  ///< Convert YUV444 packed vs hlsl.
  blob_t convert_yuv444_planar_ps_hlsl;  ///< Convert YUV444 planar ps hlsl.
  blob_t convert_yuv444_planar_ps_linear_hlsl;  ///< Convert YUV444 planar ps linear hlsl.
  blob_t convert_yuv444_planar_ps_perceptual_quantizer_hlsl;  ///< Convert YUV444 planar ps perceptual quantizer hlsl.
  blob_t convert_yuv444_packed_y410_ps_hlsl;  ///< Convert YUV444 packed y410 ps hlsl.
  blob_t convert_yuv444_packed_y410_ps_linear_hlsl;  ///< Convert YUV444 packed y410 ps linear hlsl.
  blob_t convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl;  ///< Convert YUV444 packed y410 ps perceptual quantizer hlsl.
  blob_t convert_yuv444_planar_vs_hlsl;  ///< Convert YUV444 planar vs hlsl.
  blob_t cursor_ps_hlsl;  ///< Cursor ps hlsl.
  blob_t cursor_ps_normalize_white_hlsl;  ///< Cursor ps normalize white hlsl.
  blob_t cursor_vs_hlsl;  ///< Cursor vs hlsl.

  fp16_transfer_e select_fp16_transfer(DXGI_FORMAT format, bool hdr, bool gamma_encoded_sdr) {
    using enum fp16_transfer_e;

    if (hdr) {
      // NV12 and AYUV have no PQ shaders, so HDR captures fall back to the linear shaders
      const bool has_pq_shaders = format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_Y410;
      return has_pq_shaders ? perceptual_quantizer : linear;
    }

    return gamma_encoded_sdr ? gamma_encoded : linear;
  }

  /**
   * @brief D3D-backed captured image and duplication metadata.
   */
  struct img_d3d_t: public platf::img_t {
    // These objects are owned by the display_t's ID3D11Device
    texture2d_t capture_texture;  ///< Capture texture.
    render_target_t capture_rt;  ///< Capture rt.
    keyed_mutex_t capture_mutex;  ///< Capture mutex.

    // This is the shared handle used by hwdevice_t to open capture_texture
    HANDLE encoder_texture_handle = {};  ///< Encoder texture handle.

    // Set to true if the image corresponds to a dummy texture used prior to
    // the first successful capture of a desktop frame
    bool dummy = false;  ///< Whether this image is a dummy placeholder.

    // Set to true if the image is blank (contains no content at all, including a cursor)
    bool blank = true;  ///< Whether the texture currently contains a blank frame.

    // Unique identifier for this image
    uint32_t id = 0;  ///< Unique identifier used to cache encoder resources for this image.

    // DXGI format of this image texture
    DXGI_FORMAT format;  ///< DXGI format of the captured texture.

    virtual ~img_d3d_t() override {
      if (encoder_texture_handle) {
        CloseHandle(encoder_texture_handle);
      }
    };
  };

  /**
   * @brief Keyed-mutex guard used while sharing a D3D texture.
   */
  struct texture_lock_helper {
    keyed_mutex_t _mutex;  ///< D3D keyed mutex acquired for shared-texture access.
    bool _locked = false;  ///< Whether the keyed mutex is currently locked.

    texture_lock_helper(const texture_lock_helper &) = delete;
    texture_lock_helper &operator=(const texture_lock_helper &) = delete;

    /**
     * @brief Move a keyed-mutex lock helper while preserving lock ownership.
     *
     * @param other Helper whose acquired keyed mutex is transferred.
     */
    texture_lock_helper(texture_lock_helper &&other):
        _locked {other._locked} {
      _mutex.reset(other._mutex.release());
      other._locked = false;
    }

    /**
     * @brief Assign state from another instance while preserving ownership semantics.
     *
     * @param other Source object whose state is copied or moved into this object.
     * @return Reference or value produced by the operator.
     */
    texture_lock_helper &operator=(texture_lock_helper &&other) {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
      return *this;
    }

    /**
     * @brief Explicitly acquire a D3D keyed mutex for scoped texture access.
     *
     * @param mutex Keyed mutex to acquire for texture access.
     */
    explicit texture_lock_helper(IDXGIKeyedMutex *mutex):
        _mutex(mutex) {
      if (_mutex) {
        _mutex->AddRef();
      }
    }

    ~texture_lock_helper() {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
    }

    /**
     * @brief Acquire the underlying lock or keyed mutex.
     *
     * @return True when the keyed mutex is acquired.
     */
    bool lock() {
      if (_locked) {
        return true;
      }
      HRESULT status = _mutex->AcquireSync(0, INFINITE);
      if (status == S_OK) {
        _locked = true;
      } else {
        BOOST_LOG(error) << "Failed to acquire texture mutex [0x"sv << util::hex(status).to_string_view() << ']';
      }
      return _locked;
    }
  };

  /**
   * @brief Convert DXGI cursor mask bytes into pixels for XOR blending.
   *
   * @param img_data Raw pointer-shape bytes returned by DXGI.
   * @param shape_info DXGI metadata describing the pointer shape.
   * @return Constructed cursor xor image object.
   */
  util::buffer_t<std::uint8_t> make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t inverted = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // This type doesn't require any XOR-blending
        return {};
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::ranges::for_each(std::span {(std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img)}, [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended as is.
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended by make_cursor_alpha_image().
              // We make them transparent for the XOR-blended cursor image.
              pixel = transparent;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome is handled below
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    const auto *and_mask = reinterpret_cast<const std::byte *>(std::begin(img_data));
    const auto *xor_mask = and_mask + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        const auto bit = std::byte {1} << c;
        auto color_type = ((*and_mask & bit) != std::byte {0} ? 1 : 0) + ((*xor_mask & bit) != std::byte {0} ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black (handled by alpha-blending)
          case 2:  // Opaque white (handled by alpha-blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
          case 3:  // Inverse of screen
            *pixel_data = inverted;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  /**
   * @brief Convert DXGI cursor mask bytes into pixels for alpha blending.
   *
   * @param img_data Raw pointer-shape bytes returned by DXGI.
   * @param shape_info DXGI metadata describing the pointer shape.
   * @return Constructed cursor alpha image object.
   */
  util::buffer_t<std::uint8_t> make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t black = 0xFF000000;
    constexpr std::uint32_t white = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::ranges::for_each(std::span {(std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img)}, [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended by make_cursor_xor_image().
              // We make them transparent for the alpha-blended cursor image.
              pixel = transparent;
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended as opaque with the alpha-blended image.
              pixel |= 0xFF000000;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // Color cursors are just an ARGB bitmap which requires no processing.
        return img_data;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome cursors are handled below.
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    const auto *and_mask = reinterpret_cast<const std::byte *>(std::begin(img_data));
    const auto *xor_mask = and_mask + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        const auto bit = std::byte {1} << c;
        auto color_type = ((*and_mask & bit) != std::byte {0} ? 1 : 0) + ((*xor_mask & bit) != std::byte {0} ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black
            *pixel_data = black;
            break;
          case 2:  // Opaque white
            *pixel_data = white;
            break;
          case 3:  // Inverse of screen (handled by XOR blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  /**
   * @brief Compile an HLSL shader from source text.
   *
   * @param file Optional stdio file handle connected to the child process.
   * @param entrypoint Function entry point to resolve from the library.
   * @param shader_model Shader model.
   * @return Compiled shader blob, or an empty blob when compilation fails.
   */
  blob_t compile_shader(LPCSTR file, LPCSTR entrypoint, LPCSTR shader_model) {
    blob_t::pointer msg_p = nullptr;
    blob_t::pointer compiled_p;

    DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;

#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto wFile = utf_utils::from_utf8(file);
    auto status = D3DCompileFromFile(wFile.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);

    if (msg_p) {
      BOOST_LOG(warning) << std::string_view {(const char *) msg_p->GetBufferPointer(), msg_p->GetBufferSize() - 1};
      msg_p->Release();
    }

    if (status) {
      BOOST_LOG(error) << "Couldn't compile ["sv << file << "] [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blob_t {compiled_p};
  }

  /**
   * @brief Compile an HLSL pixel shader from source text.
   *
   * @param file Optional stdio file handle connected to the child process.
   * @return Compiled pixel shader object, or an empty handle when compilation fails.
   */
  blob_t compile_pixel_shader(LPCSTR file) {
    return compile_shader(file, "main_ps", "ps_5_0");
  }

  /**
   * @brief Compile an HLSL vertex shader from source text.
   *
   * @param file Optional stdio file handle connected to the child process.
   * @return Compiled vertex shader object, or an empty handle when compilation fails.
   */
  blob_t compile_vertex_shader(LPCSTR file) {
    return compile_shader(file, "main_vs", "vs_5_0");
  }

  /**
   * @brief Shared D3D11 conversion resources for AVCodec and NVENC encode devices.
   */
  class d3d_base_encode_device final {
  public:
    /**
     * @brief Convert a captured D3D image into encoder input textures.
     *
     * @param img_base D3D image supplied by the capture backend.
     * @return Conversion status.
     */
    int convert(platf::img_t &img_base) {
      // Garbage collect mapped capture images whose weak references have expired
      for (auto it = img_ctx_map.begin(); it != img_ctx_map.end();) {
        if (it->second.img_weak.expired()) {
          it = img_ctx_map.erase(it);
        } else {
          it++;
        }
      }

      auto &img = (img_d3d_t &) img_base;
      if (!img.blank) {
        auto &img_ctx = img_ctx_map[img.id];

        // Open the shared capture texture with our ID3D11Device
        if (initialize_image_context(img, img_ctx)) {
          return -1;
        }

        // Acquire encoder mutex to synchronize with capture code
        auto status = img_ctx.encoder_mutex->AcquireSync(0, INFINITE);
        if (status != S_OK) {
          BOOST_LOG(error) << "Failed to acquire encoder mutex [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        auto draw = [&](auto &input, auto &y_or_yuv_viewports, auto &uv_viewport) {
          device_ctx->PSSetShaderResources(0, 1, &input);

          // Draw Y/YUV
          device_ctx->OMSetRenderTargets(1, &out_Y_or_YUV_rtv, nullptr);
          device_ctx->VSSetShader(convert_Y_or_YUV_vs.get(), nullptr, 0);
          device_ctx->PSSetShader(img.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? convert_Y_or_YUV_fp16_ps.get() : convert_Y_or_YUV_ps.get(), nullptr, 0);
          auto viewport_count = (format == DXGI_FORMAT_R16_UINT) ? 3 : 1;
          assert(viewport_count <= y_or_yuv_viewports.size());
          device_ctx->RSSetViewports(viewport_count, y_or_yuv_viewports.data());
          device_ctx->Draw(3 * viewport_count, 0);  // vertex shader will spread vertices across viewports

          // Draw UV if needed
          if (out_UV_rtv) {
            assert(format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010);
            device_ctx->OMSetRenderTargets(1, &out_UV_rtv, nullptr);
            device_ctx->VSSetShader(convert_UV_vs.get(), nullptr, 0);
            device_ctx->PSSetShader(img.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? convert_UV_fp16_ps.get() : convert_UV_ps.get(), nullptr, 0);
            device_ctx->RSSetViewports(1, &uv_viewport);
            device_ctx->Draw(3, 0);
          }
        };

        // Clear render target view(s) once so that the aspect ratio mismatch "bars" appear black
        if (!rtvs_cleared) {
          auto black = create_black_texture_for_rtv_clear();
          if (black) {
            draw(black, out_Y_or_YUV_viewports_for_clear, out_UV_viewport_for_clear);
          }
          rtvs_cleared = true;
        }

        // Draw captured frame
        draw(img_ctx.encoder_input_res, out_Y_or_YUV_viewports, out_UV_viewport);

        // Release encoder mutex to allow capture code to reuse this image
        img_ctx.encoder_mutex->ReleaseSync(0);

        ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
        device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
      }

      return 0;
    }

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     *
     * @param colorspace Colorimetry information used for conversion or encoding.
     */
    void apply_colorspace(const ::video::sunshine_colorspace_t &colorspace) {
      auto color_vectors = ::video::color_vectors_from_colorspace(colorspace, true);

      if (format == DXGI_FORMAT_AYUV || format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_Y410) {
        color_vectors = ::video::color_vectors_from_colorspace(colorspace, false);
      }

      if (!color_vectors) {
        BOOST_LOG(error) << "No vector data for colorspace"sv;
        return;
      }

      auto color_matrix = make_buffer(device.get(), *color_vectors);
      if (!color_matrix) {
        BOOST_LOG(warning) << "Failed to create color matrix"sv;
        return;
      }

      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);
      this->color_matrix = std::move(color_matrix);
    }

    /**
     * @brief Create D3D11 output textures, views, and shaders for frame conversion.
     *
     * @param frame_texture Frame texture.
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     * @return 0 when output resources are initialized; nonzero on D3D failure.
     */
    int init_output(ID3D11Texture2D *frame_texture, int width, int height) {
      // The underlying frame pool owns the texture, so we must reference it for ourselves
      frame_texture->AddRef();
      output_texture.reset(frame_texture);

      auto status = S_OK;

#ifndef DOXYGEN
  #define create_vertex_shader_helper(x, y) \
    if (FAILED(status = device->CreateVertexShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
      BOOST_LOG(error) << "Failed to create vertex shader " << #x << ": " << util::log_hex(status); \
      return -1; \
    }
#endif
#ifndef DOXYGEN
  #define create_pixel_shader_helper(x, y) \
    if (FAILED(status = device->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
      BOOST_LOG(error) << "Failed to create pixel shader " << #x << ": " << util::log_hex(status); \
      return -1; \
    }
#endif

      const bool downscaling = display->width > width || display->height > height;

      const auto fp16_transfer = select_fp16_transfer(format, display->is_hdr(), config::video.fp16_sdr_gamma_encoded);
      if (fp16_transfer == fp16_transfer_e::gamma_encoded) {
        BOOST_LOG(info) << "Treating FP16 SDR capture as gamma-encoded (fp16_sdr_gamma_encoded)"sv;
      }

      // Formats without PQ shaders pass their linear shader as perceptual_quantizer, which is never selected for them
      auto fp16_ps = [fp16_transfer](blob_t &gamma_encoded, blob_t &linear, blob_t &perceptual_quantizer) -> blob_t & {
        switch (fp16_transfer) {
          case fp16_transfer_e::gamma_encoded:
            return gamma_encoded;
          case fp16_transfer_e::perceptual_quantizer:
            return perceptual_quantizer;
          default:
            return linear;
        }
      };

      switch (format) {
        case DXGI_FORMAT_NV12:
          // Semi-planar 8-bit YUV 4:2:0
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(fp16_ps(convert_yuv420_planar_y_ps_hlsl, convert_yuv420_planar_y_ps_linear_hlsl, convert_yuv420_planar_y_ps_linear_hlsl), convert_Y_or_YUV_fp16_ps);
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(fp16_ps(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_yuv420_packed_uv_type0s_ps_linear_hlsl), convert_UV_fp16_ps);
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(fp16_ps(convert_yuv420_packed_uv_type0_ps_hlsl, convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_yuv420_packed_uv_type0_ps_linear_hlsl), convert_UV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_P010:
          // Semi-planar 16-bit YUV 4:2:0, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(fp16_ps(convert_yuv420_planar_y_ps_hlsl, convert_yuv420_planar_y_ps_linear_hlsl, convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl), convert_Y_or_YUV_fp16_ps);
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(fp16_ps(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl), convert_UV_fp16_ps);
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(fp16_ps(convert_yuv420_packed_uv_type0_ps_hlsl, convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl), convert_UV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_R16_UINT:
          // Planar 16-bit YUV 4:4:4, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv444_planar_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_planar_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(fp16_ps(convert_yuv444_planar_ps_hlsl, convert_yuv444_planar_ps_linear_hlsl, convert_yuv444_planar_ps_perceptual_quantizer_hlsl), convert_Y_or_YUV_fp16_ps);
          break;

        case DXGI_FORMAT_AYUV:
          // Packed 8-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(fp16_ps(convert_yuv444_packed_ayuv_ps_hlsl, convert_yuv444_packed_ayuv_ps_linear_hlsl, convert_yuv444_packed_ayuv_ps_linear_hlsl), convert_Y_or_YUV_fp16_ps);
          break;

        case DXGI_FORMAT_Y410:
          // Packed 10-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_y410_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(fp16_ps(convert_yuv444_packed_y410_ps_hlsl, convert_yuv444_packed_y410_ps_linear_hlsl, convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl), convert_Y_or_YUV_fp16_ps);
          break;

        default:
          BOOST_LOG(error) << "Unable to create shaders because of the unrecognized surface format";
          return -1;
      }

#undef create_vertex_shader_helper
#undef create_pixel_shader_helper

      auto out_width = width;
      auto out_height = height;

      float in_width = display->width;
      float in_height = display->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf(out_width / in_width, out_height / in_height);
      auto out_width_f = in_width * scalar;
      auto out_height_f = in_height * scalar;

      // result is always positive
      auto offsetX = (out_width - out_width_f) / 2;
      auto offsetY = (out_height - out_height_f) / 2;

      out_Y_or_YUV_viewports[0] = {offsetX, offsetY, out_width_f, out_height_f, 0.0f, 1.0f};  // Y plane
      out_Y_or_YUV_viewports[1] = out_Y_or_YUV_viewports[0];  // U plane
      out_Y_or_YUV_viewports[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports[2] = out_Y_or_YUV_viewports[1];  // V plane
      out_Y_or_YUV_viewports[2].TopLeftY += out_height;

      out_Y_or_YUV_viewports_for_clear[0] = {0, 0, (float) out_width, (float) out_height, 0.0f, 1.0f};  // Y plane
      out_Y_or_YUV_viewports_for_clear[1] = out_Y_or_YUV_viewports_for_clear[0];  // U plane
      out_Y_or_YUV_viewports_for_clear[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports_for_clear[2] = out_Y_or_YUV_viewports_for_clear[1];  // V plane
      out_Y_or_YUV_viewports_for_clear[2].TopLeftY += out_height;

      out_UV_viewport = {offsetX / 2, offsetY / 2, out_width_f / 2, out_height_f / 2, 0.0f, 1.0f};
      out_UV_viewport_for_clear = {0, 0, (float) out_width / 2, (float) out_height / 2, 0.0f, 1.0f};

      std::array<float, 16 / sizeof(float)> subsample_offset_in {1.0f / (float) out_width_f, 1.0f / (float) out_height_f};  // aligned to 16-byte
      subsample_offset = make_buffer(device.get(), subsample_offset_in);

      if (!subsample_offset) {
        BOOST_LOG(error) << "Failed to create subsample offset vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(0, 1, &subsample_offset);

      {
        int32_t rotation_modifier = display->display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display->display_rotation - 1;
        std::array<int32_t, 16 / sizeof(int32_t)> rotation_data {-rotation_modifier};  // aligned to 16-byte
        auto rotation = make_buffer(device.get(), rotation_data);
        if (!rotation) {
          BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
          return -1;
        }
        device_ctx->VSSetConstantBuffers(1, 1, &rotation);
      }

      DXGI_FORMAT rtv_Y_or_YUV_format = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT rtv_UV_format = DXGI_FORMAT_UNKNOWN;
      bool rtv_simple_clear = false;

      switch (format) {
        case DXGI_FORMAT_NV12:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8_UNORM;
          rtv_UV_format = DXGI_FORMAT_R8G8_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_P010:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UNORM;
          rtv_UV_format = DXGI_FORMAT_R16G16_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_AYUV:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8G8B8A8_UINT;
          break;

        case DXGI_FORMAT_R16_UINT:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UINT;
          break;

        case DXGI_FORMAT_Y410:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R10G10B10A2_UINT;
          break;

        default:
          BOOST_LOG(error) << "Unable to create render target views because of the unrecognized surface format";
          return -1;
      }

      auto create_rtv = [&](auto &rt, DXGI_FORMAT rt_format) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = rt_format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        auto view_status = device->CreateRenderTargetView(output_texture.get(), &rtv_desc, &rt);
        if (FAILED(view_status)) {
          BOOST_LOG(error) << "Failed to create render target view: " << util::log_hex(view_status);
          return false;
        }

        return true;
      };

      // Create Y/YUV render target view
      if (!create_rtv(out_Y_or_YUV_rtv, rtv_Y_or_YUV_format)) {
        return -1;
      }

      // Create UV render target view if needed
      if (rtv_UV_format != DXGI_FORMAT_UNKNOWN && !create_rtv(out_UV_rtv, rtv_UV_format)) {
        return -1;
      }

      if (rtv_simple_clear) {
        // Clear the RTVs to ensure the aspect ratio padding is black
        const std::array<float, 4> y_black = {0.0f, 0.0f, 0.0f, 0.0f};
        device_ctx->ClearRenderTargetView(out_Y_or_YUV_rtv.get(), y_black.data());
        if (out_UV_rtv) {
          const std::array<float, 4> uv_black = {0.5f, 0.5f, 0.5f, 0.5f};
          device_ctx->ClearRenderTargetView(out_UV_rtv.get(), uv_black.data());
        }
        rtvs_cleared = true;
      } else {
        // Can't use ClearRenderTargetView(), will clear on first convert()
        rtvs_cleared = false;
      }

      return 0;
    }

    /**
     * @brief Initialize shared D3D conversion resources for the encoder.
     *
     * @param capture_display Display object or identifier associated with the operation.
     * @param adapter_p Adapter p.
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(std::shared_ptr<platf::display_t> capture_display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      switch (pix_fmt) {
        using enum pix_fmt_e;
        case nv12:
          format = DXGI_FORMAT_NV12;
          break;

        case p010:
          format = DXGI_FORMAT_P010;
          break;

        case ayuv:
          format = DXGI_FORMAT_AYUV;
          break;

        case yuv444p16:
          format = DXGI_FORMAT_R16_UINT;
          break;

        case y410:
          format = DXGI_FORMAT_Y410;
          break;

        default:
          BOOST_LOG(error) << "D3D11 backend doesn't support pixel format: " << from_pix_fmt(pix_fmt);
          return -1;
      }

      std::array<D3D_FEATURE_LEVEL, 7> featureLevels {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
      };

      HRESULT status = D3D11CreateDevice(
        adapter_p,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_FLAGS | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels.data(),
        featureLevels.size(),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &device_ctx
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create encoder D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase encoding GPU thread priority. Please run application as administrator for optimal performance.";
      }

      auto default_color_vectors = ::video::color_vectors_from_colorspace({::video::colorspace_e::rec601, false, 8}, true);
      if (!default_color_vectors) {
        BOOST_LOG(error) << "Missing color vectors for Rec. 601"sv;
        return -1;
      }

      color_matrix = make_buffer(device.get(), *default_color_vectors);
      if (!color_matrix) {
        BOOST_LOG(error) << "Failed to create color matrix buffer"sv;
        return -1;
      }
      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);

      this->display = std::dynamic_pointer_cast<display_base_t>(capture_display);
      if (!this->display) {
        return -1;
      }
      capture_display = nullptr;

      blend_disable = make_blend(device.get(), false, false);
      if (!blend_disable) {
        return -1;
      }

      D3D11_SAMPLER_DESC sampler_desc {};
      sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sampler_desc.MinLOD = 0;
      sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

      status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
      device_ctx->PSSetSamplers(0, 1, &sampler_linear);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      return 0;
    }

    /**
     * @brief D3D texture and format information for encoder input.
     */
    struct encoder_img_ctx_t {
      // Used to determine if the underlying texture changes.
      // Not safe for actual use by the encoder!
      texture2d_t::const_pointer capture_texture_p;  ///< Capture texture p.

      texture2d_t encoder_texture;  ///< Encoder texture.
      shader_res_t encoder_input_res;  ///< Encoder input res.
      keyed_mutex_t encoder_mutex;  ///< Encoder mutex.

      std::weak_ptr<const platf::img_t> img_weak;  ///< Captured image lifetime tracked for cache cleanup.

      /**
       * @brief Reset the object to its initial empty state.
       */
      void reset() {
        capture_texture_p = nullptr;
        encoder_texture.reset();
        encoder_input_res.reset();
        encoder_mutex.reset();
        img_weak.reset();
      }
    };

    /**
     * @brief Initialize encoder-side D3D resources for a captured image.
     *
     * @param img Image or frame object to read from or populate.
     * @param img_ctx Cached encoder resources associated with the image ID.
     * @return 0 when the shared texture is opened and bound; nonzero on D3D failure.
     */
    int initialize_image_context(const img_d3d_t &img, encoder_img_ctx_t &img_ctx) {
      // If we've already opened the shared texture, we're done
      if (img_ctx.encoder_texture && img.capture_texture.get() == img_ctx.capture_texture_p) {
        return 0;
      }

      // Reset this image context in case it was used before with a different texture.
      // Textures can change when transitioning from a dummy image to a real image.
      img_ctx.reset();

      device1_t device1;
      auto status = device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query ID3D11Device1 [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Open a handle to the shared texture
      status = device1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &img_ctx.encoder_texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to open shared image texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Get the keyed mutex to synchronize with the capture code
      status = img_ctx.encoder_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img_ctx.encoder_mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Create the SRV for the encoder texture
      status = device->CreateShaderResourceView(img_ctx.encoder_texture.get(), nullptr, &img_ctx.encoder_input_res);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create shader resource view for encoding [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      img_ctx.capture_texture_p = img.capture_texture.get();

      img_ctx.img_weak = img.weak_from_this();

      return 0;
    }

    /**
     * @brief Create black texture for rtv clear.
     *
     * @return Created black texture for rtv clear object or status.
     */
    shader_res_t create_black_texture_for_rtv_clear() {
      constexpr auto width = 32;
      constexpr auto height = 32;

      D3D11_TEXTURE2D_DESC texture_desc = {};
      texture_desc.Width = width;
      texture_desc.Height = height;
      texture_desc.MipLevels = 1;
      texture_desc.ArraySize = 1;
      texture_desc.SampleDesc.Count = 1;
      texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
      texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      std::vector<uint8_t> mem(4 * width * height, 0);
      D3D11_SUBRESOURCE_DATA texture_data = {mem.data(), 4 * width, 0};

      texture2d_t texture;
      auto status = device->CreateTexture2D(&texture_desc, &texture_data, &texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture: " << util::log_hex(status);
        return {};
      }

      shader_res_t resource_view;
      status = device->CreateShaderResourceView(texture.get(), nullptr, &resource_view);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture resource view: " << util::log_hex(status);
        return {};
      }

      return resource_view;
    }

    ::video::color_t *color_p;  ///< Color p.

    buf_t subsample_offset;  ///< Subsample offset.
    buf_t color_matrix;  ///< Color matrix.

    blend_t blend_disable;  ///< Blend disable.
    sampler_state_t sampler_linear;  ///< Sampler linear.

    render_target_t out_Y_or_YUV_rtv;  ///< Out y or YUV rtv.
    render_target_t out_UV_rtv;  ///< Out UV rtv.
    bool rtvs_cleared = false;  ///< Whether render-target views have been cleared for the frame.

    // d3d_img_t::id -> encoder_img_ctx_t
    // These store the encoder textures for each img_t that passes through
    // convert(). We can't store them in the img_t itself because it is shared
    // amongst multiple hwdevice_t objects (and therefore multiple ID3D11Devices).
    std::map<uint32_t, encoder_img_ctx_t> img_ctx_map;  ///< Encoder resources cached by captured image ID.

    std::shared_ptr<display_base_t> display;  ///< Display capture backend that supplies source textures.

    vs_t convert_Y_or_YUV_vs;  ///< Convert y or YUV vs.
    ps_t convert_Y_or_YUV_ps;  ///< Convert y or YUV ps.
    ps_t convert_Y_or_YUV_fp16_ps;  ///< Convert y or YUV fp16 ps.

    vs_t convert_UV_vs;  ///< Convert UV vs.
    ps_t convert_UV_ps;  ///< Convert UV ps.
    ps_t convert_UV_fp16_ps;  ///< Convert UV fp16 ps.

    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports;  ///< Out y or YUV viewports.
    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports_for_clear;  ///< Out y or YUV viewports for clear.
    D3D11_VIEWPORT out_UV_viewport;  ///< Out UV viewport.
    D3D11_VIEWPORT out_UV_viewport_for_clear;  ///< Out UV viewport for clear.

    DXGI_FORMAT format;  ///< DXGI format required by the encoder input texture.

    device_t device;  ///< D3D11 device used for encoder-side texture conversion.
    device_ctx_t device_ctx;  ///< D3D11 device context used to issue conversion commands.

    texture2d_t output_texture;  ///< Output texture.
  };

  /**
   * @brief D3D11 encode device that exposes captured textures to FFmpeg AVCodec.
   */
  class d3d_avcodec_encode_device_t: public avcodec_encode_device_t {
  public:
    /**
     * @brief Initialize the D3D11 AVCodec encode device.
     *
     * @param display Display object or identifier associated with the operation.
     * @param adapter_p Adapter p.
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      int result = base.init(display, adapter_p, pix_fmt);
      d3d11_device = base.device.get();
      hardware = d3d11_device != nullptr;
      return result;
    }

    /**
     * @brief Convert a captured D3D texture for FFmpeg AVCodec encoding.
     *
     * @param img_base D3D image supplied by the capture backend.
     * @return Conversion status.
     */
    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     */
    void apply_colorspace() override {
      base.apply_colorspace(colorspace);
    }

    /**
     * @brief Configure FFmpeg hardware frames for D3D11 encoder input textures.
     *
     * @param frames FFmpeg hardware frames context to initialize.
     */
    void init_hwframes(AVHWFramesContext *frames) override {
      // We may be called with a QSV or D3D11VA context
      if (frames->device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto d3d11_frames = (AVD3D11VAFramesContext *) frames->hwctx;

        // The encoder requires textures with D3D11_BIND_RENDER_TARGET set
        d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d11_frames->MiscFlags = 0;
      }

      // We require a single texture
      frames->initial_pool_size = 1;
    }

    /**
     * @brief Prepare the D3D device before FFmpeg derives a child hardware context.
     *
     * @param hw_device_type FFmpeg hardware device type requested for context derivation.
     * @return 0 when context derivation may continue; nonzero to abort.
     */
    int prepare_to_derive_context(int hw_device_type) override {
      // QuickSync requires our device to be multithread-protected
      if (hw_device_type == AV_HWDEVICE_TYPE_QSV) {
        multithread_t mt;

        auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        mt->SetMultithreadProtected(TRUE);
      }

      return 0;
    }

    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     *
     * @param new_frame Video or graphics frame being processed.
     * @param hw_frames_ctx FFmpeg hardware frames context associated with the frame.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(new_frame);
      this->frame = new_frame;

      // Populate this frame with a hardware buffer if one isn't there already
      if (!new_frame->buf[0]) {
        auto err = av_hwframe_get_buffer(hw_frames_ctx, new_frame, 0);
        if (err) {
          std::array<char, AV_ERROR_MAX_STRING_SIZE> err_str {0};
          BOOST_LOG(error) << "Failed to get hwframe buffer: "sv << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }
      }

      // If this is a frame from a derived context, we'll need to map it to D3D11
      ID3D11Texture2D *frame_texture;
      if (new_frame->format != AV_PIX_FMT_D3D11) {
        frame_t d3d11_frame {av_frame_alloc()};

        d3d11_frame->format = AV_PIX_FMT_D3D11;

        auto err = av_hwframe_map(d3d11_frame.get(), new_frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (err) {
          std::array<char, AV_ERROR_MAX_STRING_SIZE> err_str {0};
          BOOST_LOG(error) << "Failed to map D3D11 frame: "sv << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }

        // Get the texture from the mapped frame
        frame_texture = (ID3D11Texture2D *) d3d11_frame->data[0];
      } else {
        // Otherwise, we can just use the texture inside the original frame
        frame_texture = (ID3D11Texture2D *) new_frame->data[0];
      }

      return base.init_output(frame_texture, new_frame->width, new_frame->height);
    }

  private:
    d3d_base_encode_device base;
    frame_t hwframe;
  };

  /**
   * @brief D3D11 encode device that exposes captured textures to NVENC.
   */
  class d3d_nvenc_encode_device_t: public nvenc_encode_device_t {
  public:
    /**
     * @brief Create D3D11 and NVENC resources for texture-based encoding.
     *
     * @param display Display object or identifier associated with the operation.
     * @param adapter_p Adapter p.
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return True when the D3D11 device resources are initialized.
     */
    bool init_device(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      if (base.init(display, adapter_p, pix_fmt)) {
        return false;
      }

      auto factory = nvenc::nvenc_dynamic_factory::get();
      if (!factory) {
        return false;
      }

      if (pix_fmt == pix_fmt_e::yuv444p16) {
        nvenc_d3d = factory->create_nvenc_d3d11_on_cuda(base.device.get());
      } else {
        nvenc_d3d = factory->create_nvenc_d3d11_native(base.device.get());
      }
      if (!nvenc_d3d) {
        return false;
      }

      buffer_format = pix_fmt;
      nvenc = nvenc_d3d.get();

      return true;
    }

    /**
     * @brief Initialize the platform encoder for the client stream configuration.
     *
     * @param client_config Client stream configuration negotiated for this session.
     * @param encoder_colorspace Colorimetry information used for conversion or encoding.
     * @return True when the NVENC encoder initializes for the client configuration.
     */
    bool init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &encoder_colorspace) override {
      if (!nvenc_d3d) {
        return false;
      }

      if (!nvenc_d3d->create_encoder(config::video.nv, client_config, encoder_colorspace, buffer_format)) {
        return false;
      }

      base.apply_colorspace(encoder_colorspace);
      return base.init_output(nvenc_d3d->get_input_texture(), client_config.width, client_config.height) == 0;
    }

    /**
     * @brief Convert a captured D3D texture for NVENC encoding.
     *
     * @param img_base D3D image supplied by the capture backend.
     * @return Conversion status.
     */
    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

  private:
    d3d_base_encode_device base;
    std::unique_ptr<nvenc::nvenc_d3d11_interface> nvenc_d3d;
    platf::pix_fmt_e buffer_format = platf::pix_fmt_e::unknown;
  };

  /**
   * @brief Set cursor texture.
   *
   * @param device D3D, audio, or platform device used by the operation.
   * @param cursor Cursor image or visibility state to composite.
   * @param cursor_img Cursor img.
   * @param shape_info Shape info.
   * @return True when the cursor image is uploaded to the D3D texture.
   */
  bool set_cursor_texture(device_t::pointer device, gpu_cursor_t &cursor, util::buffer_t<std::uint8_t> &&cursor_img, DXGI_OUTDUPL_POINTER_SHAPE_INFO &shape_info) {
    // This cursor image may not be used
    if (cursor_img.size() == 0) {
      cursor.input_res.reset();
      cursor.set_texture(0, 0, nullptr);
      return true;
    }

    D3D11_SUBRESOURCE_DATA data {
      std::begin(cursor_img),
      4 * shape_info.Width,
      0
    };

    // Create texture for cursor
    D3D11_TEXTURE2D_DESC t {};
    t.Width = shape_info.Width;
    t.Height = cursor_img.size() / data.SysMemPitch;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    texture2d_t texture;
    auto status = device->CreateTexture2D(&t, &data, &texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create mouse texture [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    // Free resources before allocating on the next line.
    cursor.input_res.reset();
    status = device->CreateShaderResourceView(texture.get(), nullptr, &cursor.input_res);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create cursor shader resource view [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    cursor.set_texture(t.Width, t.Height, std::move(texture));
    return true;
  }

  capture_e display_ddup_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res {res_p};

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // Translate QueryPerformanceCounter() value to steady_clock time point
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    if (frame_info.PointerShapeBufferSize > 0) {
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};

      util::buffer_t<std::uint8_t> img_data {frame_info.PointerShapeBufferSize};

      UINT dummy;
      status = dup.dup->GetFramePointerShape(img_data.size(), std::begin(img_data), &dummy, &shape_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::capture_error;
      }

      auto alpha_cursor_img = make_cursor_alpha_image(img_data, shape_info);
      auto xor_cursor_img = make_cursor_xor_image(img_data, shape_info);

      if (!set_cursor_texture(device.get(), cursor_alpha, std::move(alpha_cursor_img), shape_info) || !set_cursor_texture(device.get(), cursor_xor, std::move(xor_cursor_img), shape_info)) {
        return capture_e::capture_error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor_alpha.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);

      cursor_xor.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);
    }

    const bool blend_mouse_cursor_flag = (cursor_alpha.visible || cursor_xor.visible) && cursor_visible;

    texture2d_t src {};
    if (frame_update_flag) {
      // Get the texture object from this frame
      status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::capture_error;
      }

      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);

      // It's possible for our display enumeration to race with mode changes and result in
      // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
      if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
        return capture_e::reinit;
      }

      // If we don't know the capture format yet, grab it from this texture
      if (capture_format == DXGI_FORMAT_UNKNOWN) {
        capture_format = desc.Format;
        BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      }

      // It's also possible for the capture format to change on the fly. If that happens,
      // reinitialize capture to try format detection again and create new images.
      if (capture_format != desc.Format) {
        BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
        return capture_e::reinit;
      }
    }

    enum class lfa {
      nothing,
      replace_surface_with_img,
      replace_img_with_surface,
      copy_src_to_img,
      copy_src_to_surface,
    };

    enum class ofa {
      forward_last_img,
      copy_last_surface_and_blend_cursor,
      dummy_fallback,
    };

    auto last_frame_action = lfa::nothing;
    auto out_frame_action = ofa::dummy_fallback;

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      // We don't know the final capture format yet, so we will encode a black dummy image
      last_frame_action = lfa::nothing;
      out_frame_action = ofa::dummy_fallback;
    } else {
      if (src) {
        // We got a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...and we need to blend the mouse cursor onto it.
          // Copy the frame to intermediate surface so we can blend this and future mouse cursor updates
          // without new frames from DesktopDuplication. We use direct3d surface directly here and not
          // an image from pull_free_image_cb mainly because it's lighter (surface sharing between
          // direct3d devices produce significant memory overhead).
          last_frame_action = lfa::copy_src_to_surface;
          // Copy the intermediate surface to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...and we don't need to blend the mouse cursor.
          // Copy the frame to a new image from pull_free_image_cb and save the shared pointer to the image
          // in case the mouse cursor appears without a new frame from DesktopDuplication.
          last_frame_action = lfa::copy_src_to_img;
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      } else if (!std::holds_alternative<std::monostate>(last_frame_variant)) {
        // We didn't get a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...but we need to blend the mouse cursor.
          if (std::holds_alternative<std::shared_ptr<platf::img_t>>(last_frame_variant)) {
            // We have the shared pointer of the last image, replace it with intermediate surface
            // while copying contents so we can blend this and future mouse cursor updates.
            last_frame_action = lfa::replace_img_with_surface;
          }
          // Copy the intermediate surface which contains last DesktopDuplication frame
          // to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...and we don't need to blend the mouse cursor.
          // This happens when the mouse cursor disappears from screen,
          // or there's mouse cursor on screen, but its drawing is disabled in sunshine.
          if (std::holds_alternative<texture2d_t>(last_frame_variant)) {
            // We have the intermediate surface that was used as the mouse cursor blending base.
            // Replace it with an image from pull_free_image_cb copying contents and freeing up the surface memory.
            // Save the shared pointer to the image in case the mouse cursor reappears.
            last_frame_action = lfa::replace_surface_with_img;
          }
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
    }

    auto create_surface = [&](texture2d_t &surface) {
      // Try to reuse the old surface if it hasn't been destroyed yet.
      if (old_surface_delayed_destruction) {
        surface.reset(old_surface_delayed_destruction.release());
        return true;
      }

      // Otherwise create a new surface.
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = capture_format;
      t.BindFlags = 0;
      status = device->CreateTexture2D(&t, nullptr, &surface);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create frame copy texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      return true;
    };

    auto get_locked_d3d_img = [&](std::shared_ptr<platf::img_t> &img, bool dummy = false) -> std::tuple<std::shared_ptr<img_d3d_t>, texture_lock_helper> {
      auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

      // Finish creating the image (if it hasn't happened already),
      // also creates synchronization primitives for shared access from multiple direct3d devices.
      if (complete_img(d3d_img.get(), dummy)) {
        return {nullptr, texture_lock_helper {nullptr}};
      }

      // This image is shared between capture direct3d device and encoders direct3d devices,
      // we must acquire lock before doing anything to it.
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (!lock_helper.lock()) {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return {nullptr, texture_lock_helper {nullptr}};
      }

      // Clear the blank flag now that we're ready to capture into the image
      d3d_img->blank = false;

      return {std::move(d3d_img), std::move(lock_helper)};
    };

    switch (last_frame_action) {
      case lfa::nothing:
        {
          break;
        }

      case lfa::replace_surface_with_img:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::capture_error;
          }

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::capture_error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());

          // We delay the destruction of intermediate surface in case the mouse cursor reappears shortly.
          old_surface_delayed_destruction.reset(p_surface->release());
          old_surface_timestamp = std::chrono::steady_clock::now();

          last_frame_variant = img;
          break;
        }

      case lfa::replace_img_with_surface:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::capture_error;
          }
          auto [d3d_img, lock] = get_locked_d3d_img(*p_img);
          if (!d3d_img) {
            return capture_e::capture_error;
          }

          p_img = nullptr;
          last_frame_variant = texture2d_t {};
          auto &surface = std::get<texture2d_t>(last_frame_variant);
          if (!create_surface(surface)) {
            return capture_e::capture_error;
          }

          device_ctx->CopyResource(surface.get(), d3d_img->capture_texture.get());
          break;
        }

      case lfa::copy_src_to_img:
        {
          last_frame_variant = {};

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::capture_error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
          last_frame_variant = img;
          break;
        }

      case lfa::copy_src_to_surface:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            last_frame_variant = texture2d_t {};
            p_surface = std::get_if<texture2d_t>(&last_frame_variant);
            if (!create_surface(*p_surface)) {
              return capture_e::capture_error;
            }
          }
          device_ctx->CopyResource(p_surface->get(), src.get());
          break;
        }
    }

    auto blend_cursor = [&](img_d3d_t &d3d_img) {
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);

      if (cursor_alpha.texture.get()) {
        // Perform an alpha blending operation
        device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
        device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
        device_ctx->Draw(3, 0);
      }

      if (cursor_xor.texture.get()) {
        // Perform an invert blending without touching alpha values
        device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
        device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
        device_ctx->Draw(3, 0);
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

      ID3D11RenderTargetView *emptyRenderTarget = nullptr;
      device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
    };

    switch (out_frame_action) {
      case ofa::forward_last_img:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::capture_error;
          }
          img_out = *p_img;
          break;
        }

      case ofa::copy_last_surface_and_blend_cursor:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::capture_error;
          }
          if (!blend_mouse_cursor_flag) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::capture_error;
          }

          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img_out);
          if (!d3d_img) {
            return capture_e::capture_error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());
          blend_cursor(*d3d_img);
          break;
        }

      case ofa::dummy_fallback:
        {
          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          // Clear the image if it has been used as a dummy.
          // It can have the mouse cursor blended onto it.
          auto old_d3d_img = (img_d3d_t *) img_out.get();
          bool reclear_dummy = !old_d3d_img->blank && old_d3d_img->capture_texture;

          auto [d3d_img, lock] = get_locked_d3d_img(img_out, true);
          if (!d3d_img) {
            return capture_e::capture_error;
          }

          if (reclear_dummy) {
            const std::array<float, 4> rgb_black = {0.0f, 0.0f, 0.0f, 0.0f};
            device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), rgb_black.data());
          }

          if (blend_mouse_cursor_flag) {
            blend_cursor(*d3d_img);
          }

          break;
        }
    }

    // Perform delayed destruction of the unused surface if the time is due.
    if (old_surface_delayed_destruction && old_surface_timestamp + 10s < std::chrono::steady_clock::now()) {
      old_surface_delayed_destruction.reset();
    }

    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_ddup_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_ddup_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create scene vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      std::array<int32_t, 16 / sizeof(int32_t)> rotation_data {rotation_modifier};  // aligned to 16-byte
      auto rotation = make_buffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // This shader will normalize scRGB white levels to a user-defined white level
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Use a 300 nit target for the mouse cursor. We should really get
      // the user's SDR white level in nits, but there is no API that
      // provides that information to Win32 apps.
      std::array<float, 16 / sizeof(float)> white_multiplier_data {300.0f / 80.f};  // aligned to 16-byte
      auto white_multiplier = make_buffer(device.get(), white_multiplier_data);
      if (!white_multiplier) {
        BOOST_LOG(warning) << "Failed to create cursor blending (normalized white) white multiplier constant buffer";
        return -1;
      }

      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    } else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_alpha || !blend_invert) {
      return -1;
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx->PSSetSamplers(0, 1, &sampler_linear);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return 0;
  }

  /**
   * Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   */
  capture_e display_wgc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
      } else {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return capture_e::capture_error;
      }
    } else {
      return capture_e::capture_error;
    }
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_wgc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    return 0;
  }

  std::shared_ptr<platf::img_t> display_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();

    // Initialize format-independent fields
    img->width = width_before_rotation;
    img->height = height_before_rotation;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  int display_vram_t::complete_img(platf::img_t *img_base, bool dummy) {
    auto img = (img_d3d_t *) img_base;

    // If this already has a capture texture and it's not switching dummy state, nothing to do
    if (img->capture_texture && img->dummy == dummy) {
      return 0;
    }

    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_vram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    // Reset the image (in case this was previously a dummy)
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->data = nullptr;
    if (img->encoder_texture_handle) {
      CloseHandle(img->encoder_texture_handle);
      img->encoder_texture_handle = nullptr;
    }

    // Initialize format-dependent fields
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->dummy = dummy;
    img->format = (capture_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : capture_format;

    D3D11_TEXTURE2D_DESC t {};
    t.Width = img->width;
    t.Height = img->height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = img->format;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto status = device->CreateTexture2D(&t, nullptr, &img->capture_texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateRenderTargetView(img->capture_texture.get(), nullptr, &img->capture_rt);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get the keyed mutex to synchronize with the encoding code
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img->capture_mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    resource1_t resource;
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIResource1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Create a handle for the encoder device to use to open this texture
    status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &img->encoder_texture_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create shared texture handle [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    img->data = (std::uint8_t *) img->capture_texture.get();

    return 0;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  /**
   * @memberof platf::dxgi::display_vram_t
   */
  int display_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::vector<DXGI_FORMAT> display_vram_t::get_supported_capture_formats() {
    return {
      // scRGB FP16 is the ideal format for Wide Color Gamut and Advanced Color
      // displays (both SDR and HDR). This format uses linear gamma, so we will
      // use a linear->PQ shader for HDR and a linear->sRGB shader for SDR.
      DXGI_FORMAT_R16G16B16A16_FLOAT,

      // DXGI_FORMAT_R10G10B10A2_UNORM seems like it might give us frames already
      // converted to SMPTE 2084 PQ, however it seems to actually just clamp the
      // scRGB FP16 values that DWM is using when the desktop format is scRGB FP16.
      //
      // If there is a case where the desktop format is really SMPTE 2084 PQ, it
      // might make sense to support capturing it without conversion to scRGB,
      // but we avoid it for now.

      // We include the 8-bit modes too for when the display is in SDR mode,
      // while the client stream is HDR-capable. These UNORM formats can
      // use our normal pixel shaders that expect sRGB input.
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8X8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
    };
  }

  /**
   * @brief Check that a given codec is supported by the display device.
   * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
   * @param config The codec configuration.
   * @return `true` if supported, `false` otherwise.
   */
  bool display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD
      // If it's not an AMF encoder, it's not compatible with an AMD GPU
      if (!boost::algorithm::ends_with(name, "_amf")) {
        return false;
      }

      // Perform AMF version checks if we're using an AMD GPU. This check is placed in display_vram_t
      // to avoid hitting the display_ram_t path which uses software encoding and doesn't touch AMF.
      HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
      if (amfrt) {
        auto unload_amfrt = util::fail_guard([amfrt]() {
          FreeLibrary(amfrt);
        });

        auto fnAMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
        if (fnAMFQueryVersion) {
          amf_uint64 version;
          auto result = fnAMFQueryVersion(&version);
          if (result == AMF_OK) {
            if (config.videoFormat == 2 && version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
              // AMF 1.4.30 adds ultra low latency mode for AV1. Don't use AV1 on earlier versions.
              // This corresponds to driver version 23.5.2 (23.10.01.45) or newer.
              BOOST_LOG(warning) << "AV1 encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports AV1 encoding, update your graphics drivers!"sv;
              return false;
            } else if (config.dynamicRange && version < AMF_MAKE_FULL_VERSION(1, 4, 23, 0)) {
              // Older versions of the AMD AMF runtime can crash when fed P010 surfaces.
              // Fail if AMF version is below 1.4.23 where HEVC Main10 encoding was introduced.
              // AMF 1.4.23 corresponds to driver version 21.12.1 (21.40.11.03) or newer.
              BOOST_LOG(warning) << "HDR encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports HEVC Main10 encoding, update your graphics drivers!"sv;
              return false;
            }
          } else {
            BOOST_LOG(warning) << "AMFQueryVersion() failed: "sv << result;
          }
        } else {
          BOOST_LOG(warning) << "AMF DLL missing export: "sv << AMF_QUERY_VERSION_FUNCTION_NAME;
        }
      } else {
        BOOST_LOG(warning) << "Detected AMD GPU but AMF failed to load"sv;
      }
    } else if (adapter_desc.VendorId == 0x8086) {  // Intel
      // If it's not a QSV encoder, it's not compatible with an Intel GPU
      if (!boost::algorithm::ends_with(name, "_qsv")) {
        return false;
      }
      if (config.chromaSamplingType == 1) {
        if (config.videoFormat == 0 || config.videoFormat == 2) {
          // QSV doesn't support 4:4:4 in H.264 or AV1
          return false;
        }
        // TODO: Blacklist HEVC 4:4:4 based on adapter model
      }
    } else if (adapter_desc.VendorId == 0x10de) {  // Nvidia
      // If it's not an NVENC encoder, it's not compatible with an Nvidia GPU
      if (!boost::algorithm::ends_with(name, "_nvenc")) {
        return false;
      }
    } else if (adapter_desc.VendorId == 0x4D4F4351 ||  // Qualcomm (QCOM as MOQC reversed)
               adapter_desc.VendorId == 0x5143) {  // Qualcomm alternate ID
      // If it's not a MediaFoundation encoder, it's not compatible with a Qualcomm GPU
      if (!boost::algorithm::ends_with(name, "_mf")) {
        return false;
      }
    } else {
      BOOST_LOG(warning) << "Unknown GPU vendor ID: " << util::hex(adapter_desc.VendorId).to_string_view();
    }

    return true;
  }

  std::unique_ptr<avcodec_encode_device_t> display_vram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    auto encode_device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (encode_device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return encode_device;
  }

  std::unique_ptr<nvenc_encode_device_t> display_vram_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    auto encode_device = std::make_unique<d3d_nvenc_encode_device_t>();
    if (!encode_device->init_device(shared_from_this(), adapter.get(), pix_fmt)) {
      return nullptr;
    }
    return encode_device;
  }

  /**
   * @brief Initialize global D3D11 desktop duplication support.
   */
  int init() {
    BOOST_LOG(info) << "Compiling shaders..."sv;

#ifndef DOXYGEN
  #define compile_vertex_shader_helper(x) \
    if (!(x##_hlsl = compile_vertex_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
      return -1;
#endif
#ifndef DOXYGEN
  #define compile_pixel_shader_helper(x) \
    if (!(x##_hlsl = compile_pixel_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
      return -1;
#endif

    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv420_planar_y_vs);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear);
    compile_vertex_shader_helper(convert_yuv444_packed_vs);
    compile_pixel_shader_helper(convert_yuv444_planar_ps);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer);
    compile_vertex_shader_helper(convert_yuv444_planar_vs);
    compile_pixel_shader_helper(cursor_ps);
    compile_pixel_shader_helper(cursor_ps_normalize_white);
    compile_vertex_shader_helper(cursor_vs);

    BOOST_LOG(info) << "Compiled shaders"sv;

#undef compile_vertex_shader_helper
#undef compile_pixel_shader_helper

    return 0;
  }
}  // namespace platf::dxgi

namespace video::pyrowave {
  /**
   * @brief Share converted D3D11 pixels and a timeline fence with Vulkan on the same adapter.
   */
  class d3d11_input_t final: public gpu_input_t {
  public:
    using gpu_input_t::gpu_input_t;

    /**
     * @brief Release imports before destroying the Vulkan device and their D3D11 resources.
     */
    ~d3d11_input_t() override {
      if (image) {
        api.destroy_image(image);
      }
      if (sync) {
        api.destroy_sync(sync);
      }
      if (device) {
        api.destroy_device(device);
      }
    }

    /**
     * @brief Import reusable shared resources and let PyroWave describe their sampled image views.
     * @param frames D3D11 conversion pool.
     * @return True when texture and fence imports succeed.
     */
    bool init(AVHWFramesContext *frames) {
      if (!frames->device_ctx || frames->device_ctx->type != AV_HWDEVICE_TYPE_D3D11VA) {
        return false;
      }
      const auto format = frames->sw_format;
      VkFormat image_format;
      switch (format) {
        case AV_PIX_FMT_NV12:
          dxgi_format = DXGI_FORMAT_NV12;
          image_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
          break;
        case AV_PIX_FMT_P010:
          dxgi_format = DXGI_FORMAT_P010;
          image_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
          break;
        case AV_PIX_FMT_VUYX:
          dxgi_format = DXGI_FORMAT_AYUV;
          image_format = VK_FORMAT_R8G8B8A8_UNORM;
          break;
        case AV_PIX_FMT_XV30:
          dxgi_format = DXGI_FORMAT_Y410;
          image_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
          break;
        default:
          return false;
      }
      auto source = static_cast<AVD3D11VADeviceContext *>(frames->device_ctx->hwctx)->device;
      capture_device = source;
      platf::dxgi::dxgi_t dxgi;
      util::safe_ptr<IDXGIAdapter, &platf::dxgi::Release<IDXGIAdapter>> adapter;
      platf::dxgi::device_ctx_t immediate;
      DXGI_ADAPTER_DESC description {};
      if (FAILED(source->QueryInterface(IID_ID3D11Device5, (void **) &d3d_device)) || FAILED(source->QueryInterface(IID_IDXGIDevice, (void **) &dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description))) {
        return false;
      }
      source->GetImmediateContext(&immediate);
      if (FAILED(immediate->QueryInterface(IID_ID3D11DeviceContext4, (void **) &context))) {
        return false;
      }
      pyrowave_luid luid {};
      static_assert(sizeof(luid) == sizeof(description.AdapterLuid));
      std::memcpy(&luid, &description.AdapterLuid, sizeof(luid));
      if (api.match_device(0, 0, nullptr, nullptr, &luid, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM, &device) != PYROWAVE_SUCCESS) {
        return false;
      }
      width = frames->width;
      height = frames->height;
      D3D11_TEXTURE2D_DESC texture_info {};
      texture_info.Width = width;
      texture_info.Height = height;
      texture_info.MipLevels = 1;
      texture_info.ArraySize = 1;
      texture_info.Format = dxgi_format;
      texture_info.SampleDesc.Count = 1;
      texture_info.Usage = D3D11_USAGE_DEFAULT;
      texture_info.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      texture_info.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
      platf::dxgi::resource1_t resource;
      util::safe_ptr_v2<void, BOOL, &CloseHandle> texture_handle;
      util::safe_ptr_v2<void, BOOL, &CloseHandle> fence_handle;
      if (FAILED(d3d_device->CreateTexture2D(&texture_info, nullptr, &texture)) || FAILED(texture->QueryInterface(IID_IDXGIResource1, (void **) &resource)) || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &texture_handle)) || FAILED(d3d_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, (void **) &fence)) || FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle))) {
        return false;
      }
      VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      image_info.imageType = VK_IMAGE_TYPE_2D;
      image_info.format = image_format;
      image_info.extent = {uint32_t(width), uint32_t(height), 1};
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      pyrowave_image_create_info import {};
      import.device = device;
      import.external_handle = reinterpret_cast<pyrowave_os_handle>(texture_handle.get());
      import.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
      import.image_create_info = &image_info;
      if (api.import_image(&import, &image) != PYROWAVE_SUCCESS) {
        return false;
      }
      texture_handle.release();  // Successful imports take ownership of the NT handle.
      pyrowave_sync_object_create_info sync_info {};
      sync_info.device = device;
      sync_info.external_handle = reinterpret_cast<pyrowave_os_handle>(fence_handle.get());
      sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
      sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
      if (api.import_sync(&sync_info, &sync) != PYROWAVE_SUCCESS) {
        return false;
      }
      fence_handle.release();
      if (format == AV_PIX_FMT_VUYX || format == AV_PIX_FMT_XV30) {
        pyrowave_image_view view {};
        return api.image_view(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &view) == PYROWAVE_SUCCESS &&
               map_gpu_frame(format, std::span(&view, 1), buffers, scaling_factor);
      }
      for (int component = 0; component < 3; ++component) {
        if (api.image_view(image, VkImageAspectFlagBits(VK_IMAGE_ASPECT_PLANE_0_BIT << component), VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[component]) != PYROWAVE_SUCCESS) {
          return false;
        }
      }
      return true;
    }

    /** @copydoc gpu_input_t::prepare */
    bool prepare(AVFrame *frame) override {
      if (frame->format != AV_PIX_FMT_D3D11 || !frame->data[0] || frame->width != width || frame->height != height) {
        return false;
      }
      auto source = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
      platf::dxgi::device_t source_device;
      source->GetDevice(&source_device);
      D3D11_TEXTURE2D_DESC description {};
      source->GetDesc(&description);
      const auto layer = reinterpret_cast<uintptr_t>(frame->data[1]);
      if (source_device.get() != capture_device || description.Format != dxgi_format || description.SampleDesc.Count != 1 || description.MipLevels != 1 || layer >= description.ArraySize || description.Width < uint32_t(width) || description.Height < uint32_t(height)) {
        return false;
      }
      if (timeline && FAILED(context->Wait(fence.get(), timeline))) {
        return false;
      }
      const D3D11_BOX region {0, 0, 0, uint32_t(width), uint32_t(height), 1};
      context->CopySubresourceRegion(texture.get(), 0, 0, 0, 0, source, UINT(layer), &region);
      if (FAILED(context->Signal(fence.get(), ++timeline))) {
        return false;
      }
      context->Flush();
      return true;
    }

    /** @copydoc gpu_input_t::encode */
    pyrowave_result encode(pyrowave_encoder encoder, const pyrowave_rate_control &rate_control) override {
      const pyrowave_gpu_external_reference reference {image, VK_QUEUE_FAMILY_EXTERNAL};
      const auto semaphore = api.sync_semaphore(sync);
      const pyrowave_gpu_sync_operation acquire {&reference, 1, {semaphore, timeline}};
      const pyrowave_gpu_sync_operation release {&reference, 1, {semaphore, timeline + 1}};
      api.set_scaling_factor(encoder, scaling_factor);
      const auto result = api.encode_gpu(encoder, &acquire, &release, &buffers, &rate_control);
      if (result == PYROWAVE_SUCCESS) {
        ++timeline;
      }
      return result;
    }

  private:
    util::safe_ptr<ID3D11Device5, &platf::dxgi::Release<ID3D11Device5>> d3d_device;  ///< Capture adapter's device.
    ID3D11Device *capture_device = nullptr;  ///< Base interface retained through d3d_device, used to validate source textures.
    util::safe_ptr<ID3D11DeviceContext4, &platf::dxgi::Release<ID3D11DeviceContext4>> context;  ///< Conversion and copy command stream.
    util::safe_ptr<ID3D11Fence, &platf::dxgi::Release<ID3D11Fence>> fence;  ///< Shared timeline protecting the texture.
    platf::dxgi::texture2d_t texture;  ///< Reusable shared copy of the converter's output.
    pyrowave_image image {};  ///< Vulkan import of texture.
    pyrowave_sync_object sync {};  ///< Vulkan import of fence.
    pyrowave_gpu_buffers buffers {};  ///< Component views of the imported texture.
    float scaling_factor = 1.0f;  ///< Normalization of the imported pixel format.
    uint64_t timeline = 0;  ///< Last value queued by D3D11 or Vulkan.
    DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;  ///< Native texture layout.
    int width = 0;  ///< Conversion output width.
    int height = 0;  ///< Conversion output height.
  };

  std::unique_ptr<gpu_input_t> make_d3d11_input(AVHWFramesContext *frames, const api_t &api) {
    auto input = std::make_unique<d3d11_input_t>(api);
    return input->init(frames) ? std::move(input) : nullptr;
  }
}  // namespace video::pyrowave
