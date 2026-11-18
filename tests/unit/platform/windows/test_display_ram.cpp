/**
 * @file tests/unit/platform/windows/test_display_ram.cpp
 * @brief Tests for rotated Windows desktop capture into RAM images.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <array>
  #include <cstdint>
  #include <cstring>
  #include <vector>

  // local includes
  #include <src/platform/windows/display.h>

namespace {
  /**
   * @brief Expected desktop pixels for a native portrait panel with six distinct pixels.
   */
  struct rotation_case_t {
    DXGI_MODE_ROTATION rotation;  ///< Panel-to-desktop rotation.
    int width;  ///< Desktop width.
    int height;  ///< Desktop height.
    std::array<std::uint32_t, 6> pixels;  ///< Expected pixels in desktop row order.
  };

  constexpr std::array rotation_cases {
    rotation_case_t {DXGI_MODE_ROTATION_UNSPECIFIED, 2, 3, {1, 2, 3, 4, 5, 6}},
    rotation_case_t {DXGI_MODE_ROTATION_IDENTITY, 2, 3, {1, 2, 3, 4, 5, 6}},
    rotation_case_t {DXGI_MODE_ROTATION_ROTATE90, 3, 2, {5, 3, 1, 6, 4, 2}},
    rotation_case_t {DXGI_MODE_ROTATION_ROTATE180, 2, 3, {6, 5, 4, 3, 2, 1}},
    rotation_case_t {DXGI_MODE_ROTATION_ROTATE270, 3, 2, {2, 4, 6, 1, 3, 5}},
  };  ///< Expected output for every DXGI rotation.

  /**
   * @brief Desktop Duplication substitute that supplies real D3D textures and controlled frame events.
   */
  class fake_duplication_t final: public IDXGIOutputDuplication {
  public:
    platf::dxgi::resource_t resource;  ///< Texture exposed as the acquired desktop resource.
    DXGI_OUTDUPL_FRAME_INFO frame_info {.AccumulatedFrames = 1};  ///< Next frame metadata.
    HRESULT acquire_result = S_OK;  ///< Result returned by the next acquisition.
    HRESULT release_result = S_OK;  ///< Result returned when releasing a frame.

    /**
     * @brief Reject unused interface queries.
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): Windows COM signature
      *object = nullptr;
      return E_NOINTERFACE;
    }

    /**
     * @brief Keep the stack-owned test double alive.
     */
    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    /**
     * @brief Leave destruction to the test scope.
     */
    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    /**
     * @brief Reject unused DXGI private data writes.
     */
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject unused DXGI private interface writes.
     */
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject unused DXGI private data reads.
     */
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject unused DXGI parent queries.
     */
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override {  // NOSONAR(cpp:S5008): Windows COM signature
      return E_NOTIMPL;
    }

    /**
     * @brief Return an empty descriptor for unused duplication queries.
     */
    void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC *desc) override {
      *desc = {};
    }

    /**
     * @brief Supply the configured texture and metadata unless acquisition fails.
     */
    HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT, DXGI_OUTDUPL_FRAME_INFO *info, IDXGIResource **desktop) override {
      if (FAILED(acquire_result)) {
        return acquire_result;
      }
      *info = frame_info;
      *desktop = resource.get();
      (*desktop)->AddRef();
      return S_OK;
    }

    /**
     * @brief Reject unused dirty rectangle queries.
     */
    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT, RECT *, UINT *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject unused move rectangle queries.
     */
    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT, DXGI_OUTDUPL_MOVE_RECT *, UINT *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject pointer shape updates; tests supply the cached cursor directly.
     */
    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT, void *, UINT *, DXGI_OUTDUPL_POINTER_SHAPE_INFO *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject direct surface mapping; capture must map its staging texture.
     */
    HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject direct surface unmapping.
     */
    HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override {
      return E_NOTIMPL;
    }

    /**
     * @brief Simulate releasing a frame or losing access after a display mode change.
     */
    HRESULT STDMETHODCALLTYPE ReleaseFrame() override {
      return release_result;
    }
  };

  /**
   * @brief Read one desktop pixel without alignment assumptions.
   *
   * @param image Captured image.
   * @param x Horizontal desktop coordinate.
   * @param y Vertical desktop coordinate.
   * @return Pixel value, including every color channel.
   */
  std::uint32_t read_pixel(const platf::img_t &image, int x, int y) {
    std::uint32_t pixel;
    std::memcpy(&pixel, image.data + y * image.row_pitch + x * image.pixel_pitch, sizeof(pixel));
    return pixel;
  }

  /**
   * @brief Provide WARP textures to the production Desktop Duplication RAM snapshot path.
   */
  class ram_capture_t {
  public:
    fake_duplication_t duplication;  ///< Stack-owned duplication interface, outliving the display.
    platf::dxgi::display_ddup_ram_t display;  ///< Production capture implementation under test.

    /**
     * @brief Initialize an in-memory D3D device and a six-pixel native portrait surface.
     *
     * @param orientation Expected desktop orientation.
     * @param format BGRA capture format.
     */
    void initialize(const rotation_case_t &orientation, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
      ID3D11Device *device = nullptr;
      ID3D11DeviceContext *context = nullptr;
      ASSERT_EQ(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), S_OK);
      display.device.reset(device);
      display.device_ctx.reset(context);
      display.width = orientation.width;
      display.height = orientation.height;
      display.width_before_rotation = 2;
      display.height_before_rotation = 3;
      display.display_rotation = orientation.rotation;
      display.capture_format = DXGI_FORMAT_UNKNOWN;
      display.cursor = {};
      display.img_info = {};
      display.dup.dup.reset(&duplication);

      const std::array<std::uint32_t, 6> pixels {1, 2, 3, 4, 5, 6};
      const D3D11_SUBRESOURCE_DATA initial_data {pixels.data(), 2 * sizeof(std::uint32_t), 0};
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = 2;
      desc.Height = 3;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      platf::dxgi::texture2d_t texture;
      ASSERT_EQ(device->CreateTexture2D(&desc, &initial_data, &texture), S_OK);
      ASSERT_EQ(texture->QueryInterface(IID_IDXGIResource, reinterpret_cast<void **>(&duplication.resource)), S_OK);
    }

    /**
     * @brief Capture into a newly allocated or previously used RAM image.
     *
     * @param image Image reused between captures.
     * @param cursor Whether to draw the separately supplied cursor.
     * @return Production snapshot result.
     */
    platf::capture_e snapshot(std::shared_ptr<platf::img_t> &image, bool cursor = false) {
      if (!image) {
        image = display.alloc_img();
      }
      return display.snapshot([&image](auto &output) {
        output = image;
        return true;
      },
                              image,
                              std::chrono::milliseconds {0},
                              cursor);
    }
  };
}  // namespace

/**
 * @brief Rotate complete pixels while respecting independent source and destination padding.
 */
TEST(WindowsRamCaptureTest, CopiesEveryOrientationWithIndependentRowPadding) {
  std::array<std::uint32_t, 9> source {1, 2, 0xDEADBEEF, 3, 4, 0xDEADBEEF, 5, 6, 0xDEADBEEF};
  const auto original_source = source;
  for (const auto &orientation : rotation_cases) {
    SCOPED_TRACE(orientation.rotation);
    const auto pitch = orientation.width + 2;
    std::vector<std::uint32_t> output(pitch * orientation.height + 2, 0xABCD1234);
    platf::img_t image;
    image.width = orientation.width;
    image.height = orientation.height;
    image.pixel_pitch = sizeof(std::uint32_t);
    image.row_pitch = pitch * sizeof(std::uint32_t);
    image.data = reinterpret_cast<std::uint8_t *>(output.data() + 1);
    const D3D11_MAPPED_SUBRESOURCE mapping {source.data(), 3 * sizeof(std::uint32_t), 0};
    platf::dxgi::copy_mapped_image(mapping, image, orientation.rotation);
    for (int y = 0; y < image.height; ++y) {
      for (int x = 0; x < image.width; ++x) {
        EXPECT_EQ(read_pixel(image, x, y), orientation.pixels[y * image.width + x]);
      }
      EXPECT_EQ(output[1 + y * pitch + image.width], 0xABCD1234U);
      EXPECT_EQ(output[2 + y * pitch + image.width], 0xABCD1234U);
    }
    EXPECT_EQ(output.front(), 0xABCD1234U);
    EXPECT_EQ(output.back(), 0xABCD1234U);
    EXPECT_EQ(source, original_source);
  }
}

/**
 * @brief Exercise real texture creation, staging, mapping, rotation, and image reuse for every mode.
 *
 * Verify that a pointer-only update with a fully initialized timestamp reuses the staged desktop image.
 */
TEST(WindowsRamCaptureTest, CapturesRotatedPortraitPanelWithoutReinitialization) {
  for (auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM}) {
    for (const auto &orientation : rotation_cases) {
      SCOPED_TRACE(orientation.rotation);
      SCOPED_TRACE(format);
      ram_capture_t capture;
      ASSERT_NO_FATAL_FAILURE(capture.initialize(orientation, format));
      std::shared_ptr<platf::img_t> image;
      ASSERT_EQ(capture.snapshot(image), platf::capture_e::ok);
      EXPECT_EQ(image->width, orientation.width);
      EXPECT_EQ(image->height, orientation.height);
      EXPECT_EQ(image->row_pitch, orientation.width * 4);
      D3D11_TEXTURE2D_DESC staging;
      capture.display.texture->GetDesc(&staging);
      EXPECT_EQ(staging.Width, 2U);
      EXPECT_EQ(staging.Height, 3U);
      for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
          EXPECT_EQ(read_pixel(*image, x, y), orientation.pixels[y * image->width + x]);
        }
      }
      const auto data = image->data;
      capture.duplication.frame_info = {};
      capture.duplication.frame_info.LastMouseUpdateTime.QuadPart = 1;
      ASSERT_EQ(capture.snapshot(image), platf::capture_e::ok);
      EXPECT_EQ(image->data, data);
      EXPECT_EQ(read_pixel(*image, 0, 0), orientation.pixels.front());
    }
  }
}

/**
 * @brief Draw the cursor in desktop coordinates after rotating the underlying pixels.
 */
TEST(WindowsRamCaptureTest, CompositesCursorAfterRotation) {
  for (const auto &orientation : rotation_cases) {
    SCOPED_TRACE(orientation.rotation);
    ram_capture_t capture;
    ASSERT_NO_FATAL_FAILURE(capture.initialize(orientation));
    const std::uint32_t cursor_pixel = 0xFF123456;
    capture.display.cursor.img_data.resize(sizeof(cursor_pixel));
    std::memcpy(capture.display.cursor.img_data.data(), &cursor_pixel, sizeof(cursor_pixel));
    capture.display.cursor.shape_info = {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 1, 1, 4, {0, 0}};
    capture.display.cursor.x = 1;
    capture.display.cursor.y = 1;
    capture.display.cursor.visible = true;
    std::shared_ptr<platf::img_t> image;
    ASSERT_EQ(capture.snapshot(image, true), platf::capture_e::ok);
    EXPECT_EQ(read_pixel(*image, 1, 1), cursor_pixel);
    EXPECT_EQ(read_pixel(*image, 0, 0), orientation.pixels.front());
    ASSERT_EQ(capture.snapshot(image, false), platf::capture_e::ok);
    EXPECT_EQ(read_pixel(*image, 1, 1), orientation.pixels[orientation.width + 1]);
  }
}

/**
 * @brief Genuine surface size changes still request capture reinitialization.
 */
TEST(WindowsRamCaptureTest, ReinitializesOnUnexpectedSurfaceDimensions) {
  for (bool change_width : {false, true}) {
    ram_capture_t capture;
    ASSERT_NO_FATAL_FAILURE(capture.initialize(rotation_cases[2]));
    if (change_width) {
      ++capture.display.width_before_rotation;
    } else {
      ++capture.display.height_before_rotation;
    }
    std::shared_ptr<platf::img_t> image;
    EXPECT_EQ(capture.snapshot(image), platf::capture_e::reinit);
  }
}

/**
 * @brief Display mode changes invalidate duplication and let the pipeline recreate capture resources.
 */
TEST(WindowsRamCaptureTest, ReinitializesAfterAccessLostDuringAcquisitionOrRelease) {
  ram_capture_t capture;
  ASSERT_NO_FATAL_FAILURE(capture.initialize(rotation_cases[2]));
  std::shared_ptr<platf::img_t> image;
  capture.duplication.acquire_result = DXGI_ERROR_ACCESS_LOST;
  EXPECT_EQ(capture.snapshot(image), platf::capture_e::reinit);
  capture.duplication.acquire_result = S_OK;
  ASSERT_EQ(capture.snapshot(image), platf::capture_e::ok);
  capture.duplication.release_result = DXGI_ERROR_ACCESS_LOST;
  EXPECT_EQ(capture.snapshot(image), platf::capture_e::reinit);
}

/**
 * @brief Both RAM backends allocate using desktop width rather than the source texture stride.
 */
TEST(WindowsRamCaptureTest, AllocatesPackedImagesAndReplacesIncompatibleBuffers) {
  platf::dxgi::display_wgc_ram_t display;
  display.width = 3;
  display.height = 2;
  display.capture_format = DXGI_FORMAT_UNKNOWN;
  auto image = display.alloc_img();
  ASSERT_EQ(display.dummy_img(image.get()), 0);
  EXPECT_EQ(image->row_pitch, 12);
  EXPECT_EQ(read_pixel(*image, 2, 1), 0U);
  EXPECT_NE(display.complete_img(image.get(), false), 0);
  display.capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
  display.img_info = {nullptr, 256, 0};
  ASSERT_EQ(display.complete_img(image.get(), false), 0);
  EXPECT_EQ(image->row_pitch, 12);
  auto data = image->data;
  ASSERT_EQ(display.complete_img(image.get(), false), 0);
  EXPECT_EQ(image->data, data);
  // Model an image reused from a pool whose previous stride included padding.
  delete[] image->data;
  image->row_pitch = 256;
  image->data = new std::uint8_t[image->row_pitch * image->height];
  ASSERT_EQ(display.complete_img(image.get(), false), 0);
  EXPECT_EQ(image->row_pitch, 12);
  EXPECT_NE(image->data, nullptr);
  EXPECT_EQ(display.dummy_img(image.get()), 0);
  EXPECT_EQ(read_pixel(*image, 2, 1), 0U);
}
#endif
