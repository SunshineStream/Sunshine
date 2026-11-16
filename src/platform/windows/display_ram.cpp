/**
 * @file src/platform/windows/display_ram.cpp
 * @brief Definitions for handling ram.
 */
// standard includes
#include <cstddef>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  /**
   * @brief Captured frame buffer shared between capture and encode stages.
   */
  struct img_t: public ::platf::img_t {
    /**
     * @brief Destroy the RAM-backed DXGI image.
     */
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  void copy_mapped_image(const D3D11_MAPPED_SUBRESOURCE &source, ::platf::img_t &destination, DXGI_MODE_ROTATION rotation) {
    const auto source_data = static_cast<const std::uint8_t *>(source.pData);
    const auto source_pitch = static_cast<std::ptrdiff_t>(source.RowPitch);
    const auto pixel_pitch = destination.pixel_pitch;

    for (int y = 0; y < destination.height; ++y) {
      auto destination_row = destination.data + y * destination.row_pitch;
      const std::uint8_t *source_row;
      std::ptrdiff_t source_step;
      switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
          source_row = source_data + (destination.width - 1) * source_pitch + y * pixel_pitch;
          source_step = -source_pitch;
          break;
        case DXGI_MODE_ROTATION_ROTATE180:
          source_row = source_data + (destination.height - 1 - y) * source_pitch + (destination.width - 1) * pixel_pitch;
          source_step = -pixel_pitch;
          break;
        case DXGI_MODE_ROTATION_ROTATE270:
          source_row = source_data + (destination.height - 1 - y) * pixel_pitch;
          source_step = source_pitch;
          break;
        default:
          std::copy_n(source_data + y * source_pitch, destination.width * pixel_pitch, destination_row);
          continue;
      }

      for (int x = 0; x < destination.width; ++x) {
        std::copy_n(source_row + x * source_step, pixel_pitch, destination_row + x * pixel_pitch);
      }
    }
  }

  /**
   * @brief Blend a monochrome cursor shape into a RAM frame.
   *
   * @param cursor Cursor image or visibility state to composite.
   * @param img Image or frame object to read from or populate.
   */
  void blend_cursor_monochrome(const cursor_t &cursor, img_t &img) {
    int height = cursor.shape_info.Height / 2;
    int width = cursor.shape_info.Width;
    int pitch = cursor.shape_info.Pitch;

    // img cursor.{x,y} < 0, skip parts of the cursor.img_data
    auto cursor_skip_y = -std::min(0, cursor.y);
    auto cursor_skip_x = -std::min(0, cursor.x);

    // img cursor.{x,y} > img.{x,y}, truncate parts of the cursor.img_data
    auto cursor_truncate_y = std::max(0, cursor.y - img.height);
    auto cursor_truncate_x = std::max(0, cursor.x - img.width);

    auto cursor_width = width - cursor_skip_x - cursor_truncate_x;
    auto cursor_height = height - cursor_skip_y - cursor_truncate_y;

    if (cursor_height > height || cursor_width > width) {
      return;
    }

    auto img_skip_y = std::max(0, cursor.y);
    auto img_skip_x = std::max(0, cursor.x);

    auto cursor_img_data = cursor.img_data.data() + cursor_skip_y * pitch;

    int delta_height = std::min(cursor_height - cursor_truncate_y, std::max(0, img.height - img_skip_y));
    int delta_width = std::min(cursor_width - cursor_truncate_x, std::max(0, img.width - img_skip_x));

    auto pixels_per_byte = width / pitch;
    auto bytes_per_row = delta_width / pixels_per_byte;

    auto img_data = (int *) img.data;
    for (int i = 0; i < delta_height; ++i) {
      auto and_mask = &cursor_img_data[i * pitch];
      auto xor_mask = &cursor_img_data[(i + height) * pitch];

      auto img_pixel_p = &img_data[(i + img_skip_y) * (img.row_pitch / img.pixel_pitch) + img_skip_x];

      auto skip_x = cursor_skip_x;
      for (int x = 0; x < bytes_per_row; ++x) {
        for (auto bit = 0u; bit < 8; ++bit) {
          if (skip_x > 0) {
            --skip_x;

            continue;
          }

          int and_ = *and_mask & (1 << (7 - bit)) ? -1 : 0;
          int xor_ = *xor_mask & (1 << (7 - bit)) ? -1 : 0;

          *img_pixel_p &= and_;
          *img_pixel_p ^= xor_;

          ++img_pixel_p;
        }

        ++and_mask;
        ++xor_mask;
      }
    }
  }

  /**
   * @brief Alpha-blend a color cursor pixel into a captured RAM frame.
   *
   * @param img_pixel_p Pixel in the destination frame.
   * @param cursor_pixel ARGB cursor pixel to blend.
   */
  void apply_color_alpha(int *img_pixel_p, int cursor_pixel) {
    auto colors_out = (std::uint8_t *) &cursor_pixel;
    auto colors_in = (std::uint8_t *) img_pixel_p;

    // TODO: When use of IDXGIOutput5 is implemented, support different color formats
    auto alpha = colors_out[3];
    if (alpha == 255) {
      *img_pixel_p = cursor_pixel;
    } else {
      colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
      colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
      colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
    }
  }

  /**
   * @brief Apply a masked-color cursor pixel into a captured RAM frame.
   *
   * @param img_pixel_p Pixel in the destination frame.
   * @param cursor_pixel ARGB cursor pixel with masked-cursor semantics.
   */
  void apply_color_masked(int *img_pixel_p, int cursor_pixel) {
    // TODO: When use of IDXGIOutput5 is implemented, support different color formats
    auto alpha = ((std::uint8_t *) &cursor_pixel)[3];
    if (alpha == 0xFF) {
      *img_pixel_p ^= cursor_pixel;
    } else {
      *img_pixel_p = cursor_pixel;
    }
  }

  /**
   * @brief Blend a color cursor shape into a RAM frame.
   *
   * @param cursor Cursor image or visibility state to composite.
   * @param img Image or frame object to read from or populate.
   * @param masked Whether the cursor image uses an AND/XOR mask.
   */
  void blend_cursor_color(const cursor_t &cursor, img_t &img, const bool masked) {
    int height = cursor.shape_info.Height;
    int width = cursor.shape_info.Width;
    int pitch = cursor.shape_info.Pitch;

    // img cursor.y < 0, skip parts of the cursor.img_data
    auto cursor_skip_y = -std::min(0, cursor.y);
    auto cursor_skip_x = -std::min(0, cursor.x);

    // img cursor.{x,y} > img.{x,y}, truncate parts of the cursor.img_data
    auto cursor_truncate_y = std::max(0, cursor.y - img.height);
    auto cursor_truncate_x = std::max(0, cursor.x - img.width);

    auto img_skip_y = std::max(0, cursor.y);
    auto img_skip_x = std::max(0, cursor.x);

    auto cursor_width = width - cursor_skip_x - cursor_truncate_x;
    auto cursor_height = height - cursor_skip_y - cursor_truncate_y;

    if (cursor_height > height || cursor_width > width) {
      return;
    }

    auto cursor_img_data = (int *) &cursor.img_data[cursor_skip_y * pitch];

    int delta_height = std::min(cursor_height - cursor_truncate_y, std::max(0, img.height - img_skip_y));
    int delta_width = std::min(cursor_width - cursor_truncate_x, std::max(0, img.width - img_skip_x));

    auto img_data = (int *) img.data;

    for (int i = 0; i < delta_height; ++i) {
      auto cursor_begin = &cursor_img_data[i * cursor.shape_info.Width + cursor_skip_x];
      auto cursor_end = &cursor_begin[delta_width];

      auto img_pixel_p = &img_data[(i + img_skip_y) * (img.row_pitch / img.pixel_pitch) + img_skip_x];
      std::for_each(cursor_begin, cursor_end, [&](int cursor_pixel) {
        if (masked) {
          apply_color_masked(img_pixel_p, cursor_pixel);
        } else {
          apply_color_alpha(img_pixel_p, cursor_pixel);
        }
        ++img_pixel_p;
      });
    }
  }

  /**
   * @brief Blend the current cursor image into a RAM frame.
   *
   * @param cursor Cursor image or visibility state to composite.
   * @param img Image or frame object to read from or populate.
   */
  void blend_cursor(const cursor_t &cursor, img_t &img) {
    switch (cursor.shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        blend_cursor_color(cursor, img, false);
        break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        blend_cursor_monochrome(cursor, img);
        break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        blend_cursor_color(cursor, img, true);
        break;
      default:
        BOOST_LOG(warning) << "Unsupported cursor format ["sv << cursor.shape_info.Type << ']';
    }
  }

  capture_e display_ddup_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res {res_p};

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.AccumulatedFrames != 0 || frame_info.LastPresentTime.QuadPart != 0;
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
      auto &img_data = cursor.img_data;

      img_data.resize(frame_info.PointerShapeBufferSize);

      UINT dummy;
      status = dup.dup->GetFramePointerShape(img_data.size(), img_data.data(), &dummy, &cursor.shape_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::capture_error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor.x = frame_info.PointerPosition.Position.x;
      cursor.y = frame_info.PointerPosition.Position.y;
      cursor.visible = frame_info.PointerPosition.Visible;
    }

    if (frame_update_flag) {
      {
        texture2d_t src {};
        status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);

        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::capture_error;
        }

        D3D11_TEXTURE2D_DESC desc;
        src->GetDesc(&desc);

        // If we don't know the capture format yet, grab it from this texture and create the staging texture
        if (capture_format == DXGI_FORMAT_UNKNOWN) {
          capture_format = desc.Format;
          BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

          D3D11_TEXTURE2D_DESC t {};
          t.Width = width_before_rotation;
          t.Height = height_before_rotation;
          t.MipLevels = 1;
          t.ArraySize = 1;
          t.SampleDesc.Count = 1;
          t.Usage = D3D11_USAGE_STAGING;
          t.Format = capture_format;
          t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

          auto texture_status = device->CreateTexture2D(&t, nullptr, &texture);

          if (FAILED(texture_status)) {
            BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(texture_status).to_string_view() << ']';
            return capture_e::capture_error;
          }
        }

        // It's possible for our display enumeration to race with mode changes and result in
        // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
        if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
          BOOST_LOG(info) << "Capture size changed ["sv << width_before_rotation << 'x' << height_before_rotation << " -> "sv << desc.Width << 'x' << desc.Height << ']';
          return capture_e::reinit;
        }

        // It's also possible for the capture format to change on the fly. If that happens,
        // reinitialize capture to try format detection again and create new images.
        if (capture_format != desc.Format) {
          BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
          return capture_e::reinit;
        }

        // Copy from GPU to CPU
        device_ctx->CopyResource(texture.get(), src.get());
      }
    }

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // If we don't know the final capture format yet, encode a dummy image
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(debug) << "Capture format is still unknown. Encoding a blank image"sv;

      if (dummy_img(img)) {
        return capture_e::capture_error;
      }
    } else {
      // Map the staging texture for CPU access (making it inaccessible for the GPU)
      status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::capture_error;
      }

      // Now that we know the capture format, we can finish creating the image
      if (complete_img(img, false)) {
        device_ctx->Unmap(texture.get(), 0);
        img_info.pData = nullptr;
        return capture_e::capture_error;
      }

      // Rotate before cursor composition so both pixels and pointer use desktop coordinates.
      copy_mapped_image(img_info, *img, display_rotation);

      // Unmap the staging texture to allow GPU access again
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
    }

    if (cursor_visible && cursor.visible) {
      blend_cursor(cursor, *img);
    }

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_ddup_ram_t::release_snapshot() {
    return dup.release_frame();
  }

  std::shared_ptr<platf::img_t> display_ram_t::alloc_img() {
    auto img = std::make_shared<img_t>();

    // Initialize fields that are format-independent
    img->width = width;
    img->height = height;

    return img;
  }

  int display_ram_t::complete_img(platf::img_t *img, bool dummy) {
    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_ram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    img->pixel_pitch = get_pixel_pitch();

    // The mapped texture may have different dimensions and padding before rotation.
    const auto row_pitch = img->pixel_pitch * img->width;
    if (img->row_pitch != row_pitch) {
      img->row_pitch = row_pitch;
      delete[] img->data;
      img->data = nullptr;
    }

    if (!img->data) {
      img->data = new std::uint8_t[img->row_pitch * height];
    }

    return 0;
  }

  /**
   * @memberof platf::dxgi::display_ram_t
   */
  int display_ram_t::dummy_img(platf::img_t *img) {
    if (complete_img(img, true)) {
      return -1;
    }

    std::fill_n((std::uint8_t *) img->data, height * img->row_pitch, 0);
    return 0;
  }

  std::vector<DXGI_FORMAT> display_ram_t::get_supported_capture_formats() {
    return {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM};
  }

  int display_ddup_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    return 0;
  }

  std::unique_ptr<avcodec_encode_device_t> display_ram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    return std::make_unique<avcodec_encode_device_t>();
  }

}  // namespace platf::dxgi
