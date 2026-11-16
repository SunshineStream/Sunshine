/**
 * @file src/platform/linux/cuda.cpp
 * @brief Definitions for CUDA encoding.
 */
// standard includes
#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// lib includes
#include <ffnvcodec/dynlink_loader.h>
#include <NvFBC.h>
#ifdef SUNSHINE_BUILD_VULKAN
  #include <vulkan/vulkan.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
}

// local includes
#include "cuda.h"
#include "cuda_gl_textures.h"
#include "graphics.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/video.h"
#include "wayland.h"

/**
 * @def SUNSHINE_STRINGVIEW_HELPER(x)
 * @brief Macro for SUNSHINE STRINGVIEW HELPER.
 */
#define SUNSHINE_STRINGVIEW_HELPER(x) x##sv
/**
 * @def SUNSHINE_STRINGVIEW(x)
 * @brief Macro for SUNSHINE STRINGVIEW.
 */
#define SUNSHINE_STRINGVIEW(x) SUNSHINE_STRINGVIEW_HELPER(x)

/**
 * @def CU_CHECK(x, y)
 * @brief Macro for CU CHECK.
 */
#define CU_CHECK(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
  return -1

/**
 * @def CU_CHECK_IGNORE(x, y)
 * @brief Macro for CU CHECK IGNORE.
 */
#define CU_CHECK_IGNORE(x, y) \
  check((x), SUNSHINE_STRINGVIEW(y ": "))

namespace fs = std::filesystem;

using namespace std::literals;

namespace cuda {
  constexpr auto cudaDevAttrMaxThreadsPerBlock = (CUdevice_attribute) 1;  ///< CUDA dev attr max threads per block.
  constexpr auto cudaDevAttrMaxThreadsPerMultiProcessor = (CUdevice_attribute) 39;  ///< CUDA dev attr max threads per multi processor.

  /**
   * @brief Convert a CUDA result code into Sunshine's capture status.
   *
   * @param sv String view containing the text to inspect.
   * @param name Human-readable name to assign.
   * @param description Human-readable description used in log output.
   */
  void pass_error(const std::string_view &sv, const char *name, const char *description) {
    BOOST_LOG(error) << sv << name << ':' << description;
  }

  /**
   * @brief Release a Core Foundation object when the wrapper is destroyed.
   *
   * @param cf Core Foundation object passed to the scoped releaser.
   */
  void cff(CudaFunctions *cf) {
    cuda_free_functions(&cf);
  }

  /**
   * @brief Handle to a CUDA dynamic-library function table.
   */
  using cdf_t = util::safe_ptr<CudaFunctions, cff>;

  static cdf_t cdf;

  /**
   * @brief Log a failed CUDA call with its numeric result and safe fallback error text.
   *
   * Error lookups may fail or return null, so only use text from successful, non-null lookups.
   *
   * @param functions CUDA function table.
   * @param result Result of the call.
   * @param sv Message to log on failure.
   * @return 0 on success or -1 on failure.
   */
  inline static int check(const CudaFunctions &functions, CUresult result, const std::string_view &sv) {
    if (result != CUDA_SUCCESS) {
      const char *name = nullptr;
      const char *description = nullptr;

      if (functions.cuGetErrorName(result, &name) != CUDA_SUCCESS || !name) {
        name = "Unknown CUDA error";
      }
      if (functions.cuGetErrorString(result, &description) != CUDA_SUCCESS || !description) {
        description = "No error description available";
      }

      BOOST_LOG(error) << sv << name << " (" << static_cast<int>(result) << "): " << description;
      return -1;
    }

    return 0;
  }

  /**
   * @brief Log a failed call through the loaded CUDA functions.
   *
   * @param result Result of the call.
   * @param sv Message to log on failure.
   * @return 0 on success or -1 on failure.
   */
  inline static int check(CUresult result, const std::string_view &sv) {
    return check(*cdf, result, sv);
  }

  /**
   * @brief Release stream resources.
   *
   * @param stream CUDA stream or PipeWire stream involved in the operation.
   */
  void freeStream(CUstream stream) {
    CU_CHECK_IGNORE(cdf->cuStreamDestroy(stream), "Couldn't destroy cuda stream");
  }

  /**
   * @brief Unregister a CUDA graphics resource if it is still registered.
   *
   * @param resource CUDA graphics resource being mapped or unmapped.
   */
  void unregisterResource(CUgraphicsResource resource) {
    CU_CHECK_IGNORE(cdf->cuGraphicsUnregisterResource(resource), "Couldn't unregister resource");
  }

  /**
   * @brief CUDA graphics resource pointer released with `cuGraphicsUnregisterResource`.
   */
  using registered_resource_t = util::safe_ptr<CUgraphicsResource_st, unregisterResource>;

  /**
   * @brief CUDA image wrapper that owns mapped graphics resources for one frame.
   */
  class img_t: public platf::img_t {
  public:
    tex_t tex;  ///< CUDA texture object used as the conversion source.
  };

  /**
   * @brief Load a CUDA function table unless it is already loaded, and initialize CUDA through it.
   *
   * @param table Table to load.
   * @param load Function that fills the table, returning 0 on success.
   * @return 0 on success, -1 on failure.
   */
  int init(CudaFunctions **table, const function_table_loader_t &load) {
    // Load the table only once: running streams call through it on every frame, and reloading empties
    // it first. The mutex stops streams that start at the same time from loading it concurrently.
    static std::mutex load_mutex;
    std::scoped_lock lock {load_mutex};

    if (*table == nullptr) {
      if (auto status = load(table)) {
        BOOST_LOG(error) << "Couldn't load cuda: "sv << status;

        return -1;
      }
    }

    if (check(**table, (*table)->cuInit(0), "Couldn't initialize cuda: "sv)) {
      return -1;
    }

    return 0;
  }

  /**
   * @brief Load the CUDA driver library the first time, and initialize CUDA.
   *
   * @return 0 on success; nonzero or negative platform status on failure.
   */
  int init() {
    return init(&cdf, [](CudaFunctions **table) {
      return cuda_load_functions(table, nullptr);
    });
  }

  /**
   * @brief CUDA encode device that imports captured frames into CUDA memory.
   */
  class cuda_t: public platf::avcodec_encode_device_t {
  public:
    /**
     * @brief Initialize the CUDA device context used for frame conversion.
     *
     * @param in_width In width.
     * @param in_height In height.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(int in_width, int in_height) {
      if (!cdf) {
        BOOST_LOG(warning) << "cuda not initialized"sv;
        return -1;
      }

      hardware = true;

      width = in_width;
      height = in_height;

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

      auto hwframe_ctx = (AVHWFramesContext *) hw_frames_ctx->data;

      if (hwframe_ctx->sw_format != AV_PIX_FMT_NV12 && hwframe_ctx->sw_format != AV_PIX_FMT_YUV444P) {
        BOOST_LOG(error) << "cuda::cuda_t doesn't support any format other than AV_PIX_FMT_NV12 and AV_PIX_FMT_YUV444P"sv;
        return -1;
      }

      if (!new_frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx, new_frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for NVENC"sv;
          return -1;
        }
      }

      is_yuv444 = (hwframe_ctx->sw_format == AV_PIX_FMT_YUV444P);

      auto cuda_ctx = (AVCUDADeviceContext *) hwframe_ctx->device_ctx->hwctx;

      stream = make_stream();
      if (!stream) {
        return -1;
      }

      cuda_ctx->stream = stream.get();

      auto sws_opt = sws_t::make(width, height, new_frame->width, new_frame->height, width * 4);
      if (!sws_opt) {
        return -1;
      }

      sws = std::move(*sws_opt);

      linear_interpolation = width != new_frame->width || height != new_frame->height;

      return 0;
    }

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     */
    void apply_colorspace() override {
      sws.apply_colorspace(colorspace);

      auto tex = tex_t::make(height, width * 4);
      if (!tex) {
        return;
      }

      // The default green color is ugly.
      // Update the background color
      platf::img_t img;
      img.width = width;
      img.height = height;
      img.pixel_pitch = 4;
      img.row_pitch = img.width * img.pixel_pitch;

      std::vector<std::uint8_t> image_data;
      image_data.resize(img.row_pitch * img.height);

      img.data = image_data.data();

      if (sws.load_ram(img, tex->array)) {
        return;
      }

      if (is_yuv444) {
        sws.convert_yuv444(frame->data[0], frame->data[1], frame->data[2], frame->linesize[0], tex->texture.linear, stream.get(), {frame->width, frame->height, 0, 0});
      } else {
        sws.convert_nv12(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex->texture.linear, stream.get(), {frame->width, frame->height, 0, 0});
      }
    }

    /**
     * @brief Select the CUDA texture object for the configured filtering mode.
     *
     * @param tex Texture resource used by the converter.
     * @return CUDA texture object using linear or point sampling.
     */
    cudaTextureObject_t tex_obj(const tex_t &tex) const {
      return linear_interpolation ? tex.texture.linear : tex.texture.point;
    }

    stream_t stream;  ///< CUDA stream used for asynchronous conversion work.
    frame_t hwframe;  ///< FFmpeg hardware frame backed by CUDA resources.

    int height;  ///< Frame or display height in pixels.
    int width;  ///< Frame or display width in pixels.

    // When height and width don't change, it's not necessary to use linear interpolation
    bool linear_interpolation;  ///< Whether the CUDA converter uses linear interpolation.

    bool is_yuv444;  ///< Whether the CUDA converter outputs YUV 4:4:4.

    sws_t sws;  ///< Software scaler used for CUDA frame conversion fallback paths.
  };

  /**
   * @brief CUDA encode device path that converts frames through system memory.
   */
  class cuda_ram_t: public cuda_t {
  public:
    /**
     * @brief Convert a captured frame through CUDA into system-memory encoder input.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (is_yuv444) {
        return sws.load_ram(img, tex.array) || sws.convert_yuv444(frame->data[0], frame->data[1], frame->data[2], frame->linesize[0], tex_obj(tex), stream.get());
      }
      return sws.load_ram(img, tex.array) || sws.convert_nv12(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(tex), stream.get());
    }

    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     *
     * @param new_frame Video or graphics frame being processed.
     * @param hw_frames_ctx FFmpeg hardware frames context associated with the frame.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) override {
      if (cuda_t::set_frame(new_frame, hw_frames_ctx)) {
        return -1;
      }

      auto tex_opt = tex_t::make(height, width * 4);
      if (!tex_opt) {
        return -1;
      }

      tex = std::move(*tex_opt);

      return 0;
    }

    tex_t tex;  ///< CUDA texture object used as the conversion source.
  };

  /**
   * @brief CUDA encode device path that keeps converted frames in GPU memory.
   */
  class cuda_vram_t: public cuda_t {
  public:
    /**
     * @brief Convert a captured frame through CUDA into GPU encoder input.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (is_yuv444) {
        return sws.convert_yuv444(frame->data[0], frame->data[1], frame->data[2], frame->linesize[0], tex_obj(((img_t *) &img)->tex), stream.get());
      }
      return sws.convert_nv12(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(((img_t *) &img)->tex), stream.get());
    }
  };

  namespace {
    bool cuda_is_initialized() {
      return static_cast<bool>(cdf);
    }

    CUresult cuda_get_device(CUdevice *device, int index) {
      return cdf->cuDeviceGet(device, index);
    }

    CUresult cuda_get_pci_bus_id(char *pci_bus_id, int len, CUdevice device) {
      return cdf->cuDeviceGetPCIBusId(pci_bus_id, len, device);
    }

    const cuda_pci_accessors_t cuda_pci_accessors {
      .is_initialized = cuda_is_initialized,
      .initialize = init,
      .get_device = cuda_get_device,
      .get_pci_bus_id = cuda_get_pci_bus_id,
    };
  }  // namespace

  /**
   * @brief Query the PCI bus id of a CUDA device.
   * @param index CUDA device index to query.
   * @return Lowercase PCI bus id (e.g. "00000000:0f:00.0"), or no value when
   *         CUDA is unavailable or the query fails.
   */
  std::optional<std::string> device_pci_bus_id(int index) {
    return device_pci_bus_id(index, cuda_pci_accessors);
  }

  /**
   * @brief Query the PCI bus id of a CUDA device with injected driver operations.
   * @param index CUDA device index to query.
   * @param accessors Driver operations used to initialize CUDA and query the device.
   * @return Lowercase PCI bus id (e.g. "00000000:0f:00.0"), or no value when
   *         CUDA is unavailable or the query fails.
   */
  std::optional<std::string> device_pci_bus_id(int index, const cuda_pci_accessors_t &accessors) {
    if (!accessors.is_initialized() && accessors.initialize() != 0) {
      return std::nullopt;
    }

    CUdevice device;
    const auto device_status = accessors.get_device(&device, index);
    if (device_status != CUDA_SUCCESS) {
      BOOST_LOG(error) << "Couldn't get CUDA device: "sv << device_status;
      return std::nullopt;
    }

    // Room for both the legacy "DDDDDDDD:BB:DD.F" rendering and the short
    // "DDDD:BB:DD.F" form current drivers return, plus NUL.
    std::array<char, 32> pci_bus_id {};
    const auto bus_id_status = accessors.get_pci_bus_id(pci_bus_id.data(), pci_bus_id.size(), device);
    if (bus_id_status != CUDA_SUCCESS) {
      BOOST_LOG(error) << "Couldn't get CUDA device PCI bus ID: "sv << bus_id_status;
      return std::nullopt;
    }

    // Linux renders PCI addresses lowercase while CUDA uses uppercase.
    std::string bus_id {pci_bus_id.data()};
    std::transform(bus_id.begin(), bus_id.end(), bus_id.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return bus_id;
  }

  /**
   * @brief Opens the DRM device associated with the CUDA device index.
   * @param index CUDA device index to open.
   * @return File descriptor or -1 on failure.
   */
  file_t open_drm_fd_for_cuda_device(int index) {
    CUdevice device;
    CU_CHECK(cdf->cuDeviceGet(&device, index), "Couldn't get CUDA device");

    // There's no way to directly go from CUDA to a DRM device, so we'll
    // use sysfs to look up the DRM device name from the PCI ID.
    std::array<char, 13> pci_bus_id;
    CU_CHECK(cdf->cuDeviceGetPCIBusId(pci_bus_id.data(), pci_bus_id.size(), device), "Couldn't get CUDA device PCI bus ID");
    BOOST_LOG(debug) << "Found CUDA device with PCI bus ID: "sv << pci_bus_id.data();

    // Linux uses lowercase hexadecimal while CUDA uses uppercase
    std::ranges::transform(pci_bus_id, pci_bus_id.begin(), [](char c) {
      return std::tolower(c);
    });

    // Look for the DRM nodes in sysfs. The render node is preferred: GBM only needs
    // rendering, and headless containers are often given renderD* without card*.
    try {
      char sysfs_path[PATH_MAX];
      std::snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/pci/devices/%s/drm", pci_bus_id.data());
      fs::path sysfs_dir {sysfs_path};
      fs::path dri_path {"/dev/dri"sv};

      for (auto &entry : fs::directory_iterator {sysfs_dir}) {
        auto file = entry.path().filename();
        auto filestring = file.generic_string();
        if (std::string_view {filestring}.substr(0, 7) != "renderD"sv) {
          continue;
        }

        BOOST_LOG(debug) << "Found DRM render node: "sv << filestring;

        auto device_path = dri_path / file;
        int fd = open(device_path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
          return fd;
        }

        std::array<char, 1024> message;
        BOOST_LOG(warning) << "Couldn't open render node "sv << device_path.string() << ": "sv
                           << strerror_r(errno, message.data(), message.size()) << ", falling back to the primary node"sv;
      }

      for (auto &entry : fs::directory_iterator {sysfs_dir}) {
        auto file = entry.path().filename();
        auto filestring = file.generic_string();
        if (std::string_view {filestring}.substr(0, 4) != "card"sv) {
          continue;
        }

        BOOST_LOG(debug) << "Found DRM primary node: "sv << filestring;

        auto device_path = dri_path / file;
        int fd = platf::kms::privileged_open_drm_card_fd(device_path.c_str());
        if (fd >= 0) {
          return fd;
        }
      }
    } catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(error) << "Failed to read sysfs: "sv << err.what();
    }

    BOOST_LOG(error) << "Unable to find DRM device with PCI bus ID: "sv << pci_bus_id.data();
    return -1;
  }

  /**
   * @brief Block until GL has finished the commands submitted so far.
   *
   * @param ops GL fence calls.
   * @return 0 on success or -1 when the fence couldn't be created, timed out or failed.
   */
  int wait_for_gl_fence(const gl_fence_ops_t &ops) {
    if (!ops.create()) {
      BOOST_LOG(error) << "Couldn't create GL fence"sv;
      return -1;
    }

    auto status = ops.client_wait();
    ops.destroy();
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
      BOOST_LOG(error) << "Waiting for GL fence failed: "sv << util::hex(status).to_string_view();
      return -1;
    }

    return 0;
  }

  /**
   * @brief Hand one converted frame from GL to CUDA.
   *
   * With shared planes, GL and CUDA take turns through CPU waits. Otherwise the plane
   * textures are mapped into CUDA for the copies only. Whatever a frame acquires is
   * released even when a copy fails.
   *
   * @param shared_planes Whether the planes are shared through external memory.
   * @param plane_count Number of planes to copy.
   * @param ops Steps of the handoff.
   * @return 0 on success or -1 on failure.
   */
  int gl_cuda_handoff(bool shared_planes, int plane_count, const gl_cuda_handoff_ops_t &ops) {
    if (shared_planes && ops.wait_for_cuda()) {
      return -1;
    }

    ops.render();

    if (shared_planes ? ops.wait_for_gl() : ops.map()) {
      return -1;
    }

    // Stops at the first failed copy.
    const bool copied = std::ranges::all_of(std::views::iota(0, plane_count), [&ops](int plane) {
      return ops.copy(plane) == 0;
    });

    // Release what this frame acquired even when a copy failed.
    const int release_status = shared_planes ? ops.record_copies() : ops.unmap();
    return copied && release_status == 0 ? 0 : -1;
  }

  /**
   * @brief Find the GL, CUDA and channel layout of a shared plane's format.
   *
   * @param internal_format Sized GL internal format of the plane.
   * @return The plane format, or no value when it can't be shared.
   */
  std::optional<shared_plane_format_t> shared_plane_format(std::uint32_t internal_format) {
    constexpr std::array<shared_plane_format_t, 4> formats {{
      {GL_R8, CU_AD_FORMAT_UNSIGNED_INT8, 1},
      {GL_RG8, CU_AD_FORMAT_UNSIGNED_INT8, 2},
      {GL_R16, CU_AD_FORMAT_UNSIGNED_INT16, 1},
      {GL_RG16, CU_AD_FORMAT_UNSIGNED_INT16, 2},
    }};
    const auto format = std::ranges::find(formats, internal_format, &shared_plane_format_t::gl_format);
    if (format == formats.end()) {
      BOOST_LOG(error) << "Unsupported shared plane format: "sv << util::hex(internal_format).to_string_view();
      return std::nullopt;
    }

    return *format;
  }

#ifdef SUNSHINE_BUILD_VULKAN
  /**
   * @brief Find the Vulkan format of a shared plane.
   *
   * @param internal_format Sized GL internal format of the plane.
   * @return The Vulkan format, or `VK_FORMAT_UNDEFINED` when it can't be shared.
   */
  VkFormat shared_plane_vk_format(std::uint32_t internal_format) {
    switch (internal_format) {
      case GL_R8:
        return VK_FORMAT_R8_UNORM;
      case GL_RG8:
        return VK_FORMAT_R8G8_UNORM;
      case GL_R16:
        return VK_FORMAT_R16_UNORM;
      case GL_RG16:
        return VK_FORMAT_R16G16_UNORM;
      default:
        return VK_FORMAT_UNDEFINED;
    }
  }

  /**
   * @brief Describe the Vulkan image behind a shared plane.
   *
   * `EXT_external_objects` requires an image that aliases a GL texture to have the mutable
   * format bit (issue 17) and every usage its format supports (issue 13), because GL has
   * no way to tell the driver how the image is used.
   *
   * @param internal_format Sized GL internal format of the plane, one shared_plane_format() accepts.
   * @param width Plane width.
   * @param height Plane height.
   * @param optimal_tiling_features Features the plane's Vulkan format has with optimal tiling.
   * @param external_info Export info to chain into the image info. Must outlive the result.
   * @return The image's creation info.
   */
  VkImageCreateInfo shared_plane_image_info(std::uint32_t internal_format, int width, int height, VkFormatFeatureFlags optimal_tiling_features, const VkExternalMemoryImageCreateInfo &external_info) {
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &external_info;
    image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = shared_plane_vk_format(internal_format);
    image_info.extent = {(std::uint32_t) width, (std::uint32_t) height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (optimal_tiling_features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
      image_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    return image_info;
  }

  /**
   * @brief Shares plane memory by allocating it with Vulkan and importing it into GL and CUDA.
   */
  class vulkan_shared_memory_t final: public shared_memory_driver_t {
  public:
    /**
     * @brief Construct the driver.
     *
     * @param vulkan_calls Vulkan entry points.
     * @param cuda_functions Returns the CUDA function table, which loads after construction.
     */
    vulkan_shared_memory_t(const vulkan_calls_t &vulkan_calls, cuda_functions_fn cuda_functions):
        vk {vulkan_calls},
        cu_functions {std::move(cuda_functions)} {
    }

    bool supported() override {
      if (!gl::ctx.EXT_memory_object_fd) {
        BOOST_LOG(info) << "GL lacks GL_EXT_memory_object_fd"sv;
        return false;
      }

      if (!cu()->cuDeviceGetUuid || !cu()->cuImportExternalMemory) {
        BOOST_LOG(info) << "CUDA driver lacks external memory interop"sv;
        return false;
      }

      return true;
    }

    int create_device() override {
      CUdevice cu_device;
      if (cu_check(cu()->cuCtxGetDevice(&cu_device), "Couldn't get CUDA device: "sv)) {
        return -1;
      }
      CUuuid uuid;
      if (cu_check(cu()->cuDeviceGetUuid(&uuid, cu_device), "Couldn't get CUDA device UUID: "sv)) {
        return -1;
      }

      VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.apiVersion = VK_API_VERSION_1_1;

      VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      instance_info.pApplicationInfo = &app;
      if (vk.create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        BOOST_LOG(info) << "Couldn't create Vulkan instance for GL/CUDA sharing"sv;
        return -1;
      }

      physical_device = find_physical_device(uuid);
      if (physical_device == VK_NULL_HANDLE) {
        BOOST_LOG(info) << "No Vulkan device matches the CUDA device"sv;
        return -1;
      }

      const float priority = 1.0f;
      VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue_info.queueFamilyIndex = 0;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &priority;

      const char *extension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
      VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      device_info.queueCreateInfoCount = 1;
      device_info.pQueueCreateInfos = &queue_info;
      device_info.enabledExtensionCount = 1;
      device_info.ppEnabledExtensionNames = &extension;
      if (vk.create_device(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
        BOOST_LOG(info) << "Couldn't create Vulkan device with external memory export"sv;
        return -1;
      }

      get_memory_fd = vk.load_get_memory_fd(device);
      return get_memory_fd ? 0 : -1;
    }

    void destroy_device() override {
      vk.destroy_device(device, nullptr);
      vk.destroy_instance(instance, nullptr);
      instance = VK_NULL_HANDLE;
      physical_device = VK_NULL_HANDLE;
      device = VK_NULL_HANDLE;
      get_memory_fd = nullptr;
    }

    int create_event() override {
      return cu_check(cu()->cuEventCreate(&copy_done, CU_EVENT_DISABLE_TIMING), std::string_view {"Couldn't create CUDA copy event: "});
    }

    void destroy_event() override {
      if (copy_done) {
        cu_check(cu()->cuEventDestroy(copy_done), "Couldn't destroy CUDA event: "sv);
        copy_done = nullptr;
      }
    }

    int wait_event() override {
      return cu_check(cu()->cuEventSynchronize(copy_done), "Couldn't wait for previous CUDA copy: "sv);
    }

    int record_event(CUstream stream) override {
      return cu_check(cu()->cuEventRecord(copy_done, stream), "Couldn't record CUDA copy event: "sv);
    }

    int allocate_memory(std::size_t plane, std::uint32_t internal_format, int width, int height, std::uint64_t &size) override {
      auto &p = planes[plane];

      VkFormatProperties format_props {};
      vk.get_format_properties(physical_device, shared_plane_vk_format(internal_format), &format_props);

      VkExternalMemoryImageCreateInfo external_info = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      const auto image_info = shared_plane_image_info(internal_format, width, height, format_props.optimalTilingFeatures, external_info);
      if (vk.create_image(device, &image_info, nullptr, &p.image) != VK_SUCCESS) {
        BOOST_LOG(error) << "Couldn't create Vulkan image for plane "sv << plane;
        return -1;
      }

      VkMemoryRequirements requirements;
      vk.get_image_memory_requirements(device, p.image, &requirements);

      VkPhysicalDeviceMemoryProperties memory_props;
      vk.get_memory_properties(physical_device, &memory_props);
      std::uint32_t type_index = 0;
      while (type_index < memory_props.memoryTypeCount &&
             !((requirements.memoryTypeBits & (1u << type_index)) && (memory_props.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) {
        ++type_index;
      }

      VkMemoryDedicatedAllocateInfo dedicated_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicated_info.image = p.image;
      VkExportMemoryAllocateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
      export_info.pNext = &dedicated_info;
      export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc_info.pNext = &export_info;
      alloc_info.allocationSize = requirements.size;
      alloc_info.memoryTypeIndex = type_index;
      if (type_index == memory_props.memoryTypeCount || vk.allocate_memory(device, &alloc_info, nullptr, &p.memory) != VK_SUCCESS || vk.bind_image_memory(device, p.image, p.memory, 0) != VK_SUCCESS) {
        BOOST_LOG(error) << "Couldn't allocate exportable memory for plane "sv << plane;
        return -1;
      }

      size = requirements.size;
      return 0;
    }

    void free_memory(std::size_t plane) override {
      auto &p = planes[plane];
      vk.destroy_image(device, p.image, nullptr);
      vk.free_memory(device, p.memory, nullptr);
      p.image = VK_NULL_HANDLE;
      p.memory = VK_NULL_HANDLE;
    }

    int export_fd(std::size_t plane, int &fd) override {
      VkMemoryGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
      fd_info.memory = planes[plane].memory;
      fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      if (get_memory_fd(device, &fd_info, &fd) != VK_SUCCESS) {
        BOOST_LOG(error) << "Couldn't export memory for plane "sv << plane;
        return -1;
      }

      return 0;
    }

    void close_fd(int fd) override {
      vk.close_fd(fd);
    }

    int import_gl(std::size_t plane, int fd, std::uint64_t size) override {
      auto &p = planes[plane];
      // Only this import's errors count: a failure here decides who owns the descriptor.
      gl_drain_errors;
      const GLint dedicated = GL_TRUE;
      gl::ctx.CreateMemoryObjectsEXT(1, &p.gl_memory);
      gl::ctx.MemoryObjectParameterivEXT(p.gl_memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
      gl::ctx.ImportMemoryFdEXT(p.gl_memory, size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
      if (auto err = gl::ctx.GetError(); err != GL_NO_ERROR) {
        BOOST_LOG(error) << "Couldn't import plane memory into GL: "sv << util::hex(err).to_string_view();
        return -1;
      }

      return 0;
    }

    int create_gl_storage(std::size_t plane, std::uint32_t internal_format, int width, int height) override {
      gl_drain_errors;
      gl::ctx.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, internal_format, width, height, planes[plane].gl_memory, 0);
      if (auto err = gl::ctx.GetError(); err != GL_NO_ERROR) {
        BOOST_LOG(error) << "Couldn't create GL texture on plane memory: "sv << util::hex(err).to_string_view();
        return -1;
      }

      return 0;
    }

    void release_gl(std::size_t plane) override {
      auto &p = planes[plane];
      if (p.gl_memory) {
        gl::ctx.DeleteMemoryObjectsEXT(1, &p.gl_memory);
        p.gl_memory = 0;
      }
    }

    int import_cuda(std::size_t plane, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC &desc) override {
      return cu_check(cu()->cuImportExternalMemory(&planes[plane].cu_memory, &desc), "Couldn't import plane memory into CUDA: "sv);
    }

    void release_cuda_memory(std::size_t plane) override {
      auto &p = planes[plane];
      if (p.cu_memory) {
        cu_check(cu()->cuDestroyExternalMemory(p.cu_memory), "Couldn't destroy CUDA external memory: "sv);
        p.cu_memory = nullptr;
      }
    }

    int map_array(std::size_t plane, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC &desc) override {
      auto &p = planes[plane];
      if (cu_check(cu()->cuExternalMemoryGetMappedMipmappedArray(&p.cu_mipmap, p.cu_memory, &desc), "Couldn't map plane memory as a CUDA array: "sv)) {
        return -1;
      }

      return cu_check(cu()->cuMipmappedArrayGetLevel(&p.cu_array, p.cu_mipmap, 0), "Couldn't get plane array: "sv);
    }

    void release_array(std::size_t plane) override {
      auto &p = planes[plane];
      if (p.cu_mipmap) {
        cu_check(cu()->cuMipmappedArrayDestroy(p.cu_mipmap), "Couldn't destroy CUDA mipmapped array: "sv);
        p.cu_mipmap = nullptr;
      }
      p.cu_array = nullptr;
    }

    CUarray array(std::size_t plane) const override {
      return planes[plane].cu_array;
    }

  private:
    /**
     * @brief The CUDA function table.
     *
     * @return The table.
     */
    const CudaFunctions *cu() const {
      return cu_functions();
    }

    /**
     * @brief Log a failed CUDA call.
     *
     * @param result Result of the call.
     * @param sv Message to log on failure.
     * @return 0 on success or -1 on failure.
     */
    int cu_check(CUresult result, const std::string_view &sv) const {
      return check(*cu(), result, sv);
    }

    /**
     * @brief Memory backing one plane, as seen by Vulkan, GL and CUDA.
     */
    struct plane_t {
      VkImage image = VK_NULL_HANDLE;  ///< Vulkan image that owns the allocation's layout.
      VkDeviceMemory memory = VK_NULL_HANDLE;  ///< Exportable Vulkan allocation.
      GLuint gl_memory = 0;  ///< GL memory object imported from the allocation.
      CUexternalMemory cu_memory = nullptr;  ///< CUDA external memory imported from the allocation.
      CUmipmappedArray cu_mipmap = nullptr;  ///< CUDA view of the allocation.
      CUarray cu_array = nullptr;  ///< Level 0 of `cu_mipmap`, the source of the copies.
    };

    /**
     * @brief Find the Vulkan physical device with a given UUID.
     *
     * @param uuid UUID of the CUDA device.
     * @return The matching device, or `VK_NULL_HANDLE`.
     */
    VkPhysicalDevice find_physical_device(const CUuuid &uuid) const {
      std::uint32_t count = 0;
      vk.enumerate_physical_devices(instance, &count, nullptr);
      std::vector<VkPhysicalDevice> devices(count);
      vk.enumerate_physical_devices(instance, &count, devices.data());

      for (auto dev : devices) {
        VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        vk.get_properties2(dev, &props);

        if (std::memcmp(id.deviceUUID, uuid.bytes, VK_UUID_SIZE) == 0) {
          return dev;
        }
      }

      return VK_NULL_HANDLE;
    }

    vulkan_calls_t vk;  ///< Vulkan entry points.
    cuda_functions_fn cu_functions;  ///< Returns the CUDA function table.
    VkInstance instance = VK_NULL_HANDLE;  ///< Vulkan instance owning `device`.
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;  ///< Vulkan view of the CUDA device.
    VkDevice device = VK_NULL_HANDLE;  ///< Vulkan device that allocates the shared memory.
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;  ///< `vkGetMemoryFdKHR`.
    std::array<plane_t, 3> planes {};  ///< Memory of each plane.
    CUevent copy_done = nullptr;  ///< Recorded after each frame's copies out of the planes.
  };
#endif

#ifdef SUNSHINE_BUILD_VULKAN
  /**
   * @brief The real Vulkan entry points.
   *
   * @return The entry points.
   */
  static vulkan_calls_t real_vulkan_calls() {
    return {
      .create_instance = vkCreateInstance,
      .destroy_instance = vkDestroyInstance,
      .enumerate_physical_devices = vkEnumeratePhysicalDevices,
      .get_properties2 = vkGetPhysicalDeviceProperties2,
      .get_format_properties = vkGetPhysicalDeviceFormatProperties,
      .get_memory_properties = vkGetPhysicalDeviceMemoryProperties,
      .create_device = vkCreateDevice,
      .destroy_device = vkDestroyDevice,
      .create_image = vkCreateImage,
      .destroy_image = vkDestroyImage,
      .get_image_memory_requirements = vkGetImageMemoryRequirements,
      .allocate_memory = vkAllocateMemory,
      .free_memory = vkFreeMemory,
      .bind_image_memory = vkBindImageMemory,
      .load_get_memory_fd = [](VkDevice device) {
        return (PFN_vkGetMemoryFdKHR) vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
      },
      .close_fd = close,
    };
  }

  /**
   * @brief Create a driver that shares plane memory through Vulkan.
   *
   * @param calls Vulkan entry points.
   * @param cuda_functions Returns the CUDA function table, which loads after construction.
   * @return The driver.
   */
  std::unique_ptr<shared_memory_driver_t> make_vulkan_shared_memory(const vulkan_calls_t &calls, cuda_functions_fn cuda_functions) {
    return std::make_unique<vulkan_shared_memory_t>(calls, std::move(cuda_functions));
  }
#endif

  /**
   * @brief Create the driver that shares plane memory on this build.
   *
   * @return The driver, or nullptr in a build without Vulkan.
   */
  std::unique_ptr<shared_memory_driver_t> make_shared_memory_driver() {
#ifdef SUNSHINE_BUILD_VULKAN
    return make_vulkan_shared_memory(real_vulkan_calls(), [] {
      return static_cast<const CudaFunctions *>(cdf.get());
    });
#else
    return nullptr;
#endif
  }

  /**
   * @brief Create the plane textures, sharing their memory with CUDA when possible.
   *
   * @param ops Steps of creating the textures.
   * @return 0 on success or -1 when neither way of creating them works.
   */
  int create_gl_cuda_target(const gl_cuda_target_ops_t &ops) {
    if (ops.init_shared() == 0) {
      if (ops.create_shared_target() == 0) {
        BOOST_LOG(info) << "Sharing GL->CUDA planes through external memory"sv;
        return 0;
      }
      BOOST_LOG(warning) << "Couldn't share GL->CUDA planes, falling back to mapping them every frame"sv;
    }

    ops.reset_shared();

    if (ops.create_mapped_target()) {
      return -1;
    }

    return ops.register_mapped_target();
  }

  /**
   * @brief Block until GL has finished the commands submitted so far on the current context.
   *
   * @return 0 on success or -1 on failure.
   */
  int wait_for_gl_commands() {
    GLsync fence = nullptr;
    return wait_for_gl_fence({
      .create = [&fence] {
        fence = gl::ctx.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        return fence != nullptr;
      },
      // Generous timeout: this only guards against a hung GPU.
      .client_wait = [&fence] {
        return gl::ctx.ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, std::chrono::nanoseconds(1s).count());
      },
      .destroy = [&fence] {
        gl::ctx.DeleteSync(fence);
      },
    });
  }

  /**
   * @brief Build the steps of handing a converted frame from GL to CUDA.
   *
   * @param copy What the copies act on.
   * @param render Renders the frame into the plane textures with GL.
   * @return The steps.
   */
  gl_cuda_handoff_ops_t make_gl_cuda_handoff_ops(const gl_cuda_frame_copy_t &copy, std::function<void()> render) {
    return {
      .wait_for_cuda = [copy] {
        return copy.shared.wait_for_cuda();
      },
      .render = std::move(render),
      .wait_for_gl = wait_for_gl_commands,
      .map = [copy] {
        auto resources = copy.resources;
        return check(copy.cu, copy.cu.cuGraphicsMapResources(copy.plane_count, resources.data(), copy.stream), "Couldn't map GL textures in CUDA: "sv);
      },
      // Copy from the GL textures to the target CUDA frame
      .copy = [copy](int plane) {
        CUDA_MEMCPY2D cpy = {};
        cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        if (copy.shared.is_active()) {
          cpy.srcArray = copy.shared.array(plane);
        } else if (check(copy.cu, copy.cu.cuGraphicsSubResourceGetMappedArray(&cpy.srcArray, copy.resources[plane], 0, 0), "Couldn't get mapped plane array: "sv)) {
          return -1;
        }

        cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.dstDevice = (CUdeviceptr) copy.frame.data[plane];
        cpy.dstPitch = copy.frame.linesize[plane];
        cpy.WidthInBytes = (copy.frame.width * copy.fmt_desc.comp[plane].step) >> (plane ? copy.fmt_desc.log2_chroma_w : 0);
        cpy.Height = copy.frame.height >> (plane ? copy.fmt_desc.log2_chroma_h : 0);

        check(copy.cu, copy.cu.cuMemcpy2DAsync(&cpy, copy.stream), "Couldn't copy texture to CUDA frame: "sv);
        return 0;
      },
      .unmap = [copy] {
        auto resources = copy.resources;
        return check(copy.cu, copy.cu.cuGraphicsUnmapResources(copy.plane_count, resources.data(), copy.stream), "Couldn't unmap GL textures from CUDA: "sv);
      },
      .record_copies = [copy] {
        return copy.shared.record_copies(copy.stream);
      },
    };
  }

  /**
   * @brief Register a plane's plain GL texture with CUDA, for mapping it every frame.
   *
   * @param cu CUDA function table.
   * @param resource Receives the graphics resource.
   * @param texture The texture.
   * @return 0 on success or -1 on failure.
   */
  int register_plane_texture(const CudaFunctions &cu, CUgraphicsResource *resource, std::uint32_t texture) {
    return check(cu, cu.cuGraphicsGLRegisterImage(resource, texture, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "Couldn't register plane texture: "sv);
  }

  /**
   * @brief OpenGL/CUDA interop resources used for GPU-side frame conversion.
   */
  class gl_cuda_vram_t: public platf::avcodec_encode_device_t {
  public:
    ~gl_cuda_vram_t() override {
      // Let queued copies finish before the shared planes they read are released.
      if (stream && cdf) {
        CU_CHECK_IGNORE(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize CUDA stream");
      }
    }

    /**
     * @brief Initialize the GL->CUDA encoding device.
     * @param in_width Width of captured frames.
     * @param in_height Height of captured frames.
     * @param in_offset_x Offset of content in captured frame.
     * @param in_offset_y Offset of content in captured frame.
     * @return 0 on success or -1 on failure.
     */
    int init(int in_width, int in_height, int in_offset_x, int in_offset_y) {
      // This must be non-zero to tell the video core that it's a hardware encoding device.
      hardware = true;

      // TODO: Support more than one CUDA device
      file = std::move(open_drm_fd_for_cuda_device(0));
      if (file.el < 0) {
        std::array<char, 1024> string;
        BOOST_LOG(error) << "Couldn't open DRM FD for CUDA device: "sv << strerror_r(errno, string.data(), string.size());
        return -1;
      }

      gbm.reset(gbm::create_device(file.el));
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

      width = in_width;
      height = in_height;

      sequence = 0;

      this->offset_x = in_offset_x;
      this->offset_y = in_offset_y;

      return 0;
    }

    /**
     * @brief Initialize color conversion into target CUDA frame.
     * @param new_frame Destination CUDA frame to write into.
     * @param hw_frames_ctx_buf FFmpeg hardware frame context.
     * @return 0 on success or -1 on failure.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(new_frame);
      this->frame = new_frame;

      auto hw_frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;

      if (hw_frames_ctx->sw_format != AV_PIX_FMT_NV12 && hw_frames_ctx->sw_format != AV_PIX_FMT_YUV444P && hw_frames_ctx->sw_format != AV_PIX_FMT_P010LE && hw_frames_ctx->sw_format != AV_PIX_FMT_YUV444P16LE) {
        BOOST_LOG(error) << "cuda::gl_cuda_vram_t doesn't support any format other than AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV444P and AV_PIX_FMT_YUV444P16LE"sv;
        return -1;
      }

      if (!new_frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx_buf, new_frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for NVENC_GL"sv;
          return -1;
        }
      }

      sw_format = hw_frames_ctx->sw_format;
      is_yuv444 = (sw_format == AV_PIX_FMT_YUV444P || sw_format == AV_PIX_FMT_YUV444P16LE);

      auto sws_opt = egl::sws_t::make(width, height, new_frame->width, new_frame->height, sw_format, is_yuv444);
      if (!sws_opt) {
        return -1;
      }

      this->sws = std::move(*sws_opt);

      auto cuda_ctx = (AVCUDADeviceContext *) hw_frames_ctx->device_ctx->hwctx;

      stream = make_stream();
      if (!stream) {
        return -1;
      }

      cuda_ctx->stream = stream.get();

      // Share the output planes through external memory when possible, so CUDA can
      // read them without a synchronizing map/unmap every frame.
      return create_gl_cuda_textures(textures, shared, frame->width, frame->height, sw_format, is_yuv444, [this](std::size_t plane, std::uint32_t texture) {
        return register_plane_texture(*cdf, &cu_res[plane], texture);
      });
    }

    /**
     * @brief Convert the captured image into the target CUDA frame.
     * @param img Captured screen image.
     * @return 0 on success or -1 on failure.
     */
    int convert(platf::img_t &img) override {
      auto &descriptor = (egl::img_descriptor_t &) img;

      if (descriptor.sequence == 0) {
        // For dummy images, use a blank RGB texture instead of importing a DMA-BUF
        rgb = egl::create_blank(img);
      } else if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;

        rgb = egl::rgb_t {};

        auto rgb_opt = egl::import_source(display.get(), descriptor.sd);

        if (!rgb_opt) {
          return -1;
        }

        rgb = std::move(*rgb_opt);
      }

      sws.load_vram(descriptor, offset_x, offset_y, rgb->tex[0], is_yuv444);

      const int plane_count = is_yuv444 ? 3 : 2;
      const gl_cuda_frame_copy_t copy {
        .cu = *cdf,
        .shared = shared,
        .stream = stream.get(),
        .frame = *frame,
        .fmt_desc = *av_pix_fmt_desc_get(sw_format),
        .resources = {{cu_res[0].get(), cu_res[1].get(), cu_res[2].get()}},
        .plane_count = plane_count,
      };

      // Perform the color conversion and scaling in GL, then copy the planes to the target CUDA frame
      return gl_cuda_handoff(shared.is_active(), plane_count, make_gl_cuda_handoff_ops(copy, [this] {
                               if (is_yuv444) {
                                 sws.convert_yuv444(textures.yuv444->buf);
                               } else {
                                 sws.convert_nv12(textures.nv12->buf);
                               }
                             }));
    }

    /**
     * @brief Configures shader parameters for the specified colorspace.
     */
    void apply_colorspace() override {
      sws.apply_colorspace(colorspace, is_yuv444);
    }

    file_t file;  ///< File descriptor for the imported DMA-BUF.
    gbm::gbm_t gbm;  ///< GBM device used for buffer allocation..
    egl::display_t display;  ///< EGL display used to import captured frames.
    egl::ctx_t ctx;  ///< EGL context used to import captured frames.

    // This must be destroyed before display_t
    stream_t stream;  ///< CUDA stream used for asynchronous conversion work.
    frame_t hwframe;  ///< FFmpeg hardware frame backed by CUDA resources.

    // Holds the plane textures' memory, so it must outlive textures
    gl_cuda_shared_planes_t shared {make_shared_memory_driver()};  ///< Output planes shared with CUDA, when external memory is available.

    egl::sws_t sws;  ///< Software scaler used for CUDA frame conversion fallback paths.
    gl_cuda_textures_t textures;  ///< GL textures that the color conversion renders into.
    AVPixelFormat sw_format;  ///< FFmpeg software pixel format produced by conversion.

    int height;  ///< Frame or display height in pixels.
    int width;  ///< Frame or display width in pixels.

    std::uint64_t sequence;  ///< Capture sequence number associated with the frame.
    egl::rgb_t rgb;  ///< Imported RGB source image used before CUDA conversion.

    std::array<registered_resource_t, 3> cu_res;  ///< CUDA graphics resource of each plane, when mapping the textures every frame.

    int offset_x;  ///< Horizontal offset in physical pixels.
    int offset_y;  ///< Vertical offset in physical pixels.

    bool is_yuv444;  ///< Whether the CUDA converter outputs YUV 4:4:4.
  };

  /**
   * @brief Create AVCodec encode device.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param vram Whether the image should use GPU memory instead of system memory.
   * @return Constructed AVCodec encode device object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram) {
    if (init()) {
      return nullptr;
    }

    std::unique_ptr<cuda_t> cuda;

    if (vram) {
      cuda = std::make_unique<cuda_vram_t>();
    } else {
      cuda = std::make_unique<cuda_ram_t>();
    }

    if (cuda->init(width, height)) {
      return nullptr;
    }

    return cuda;
  }

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param width Width of captured frames.
   * @param height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y) {
    if (init()) {
      return nullptr;
    }

    auto cuda = std::make_unique<gl_cuda_vram_t>();

    if (cuda->init(width, height, offset_x, offset_y)) {
      return nullptr;
    }

    return cuda;
  }

  namespace nvfbc {
    static PNVFBCCREATEINSTANCE createInstance {};
    static NVFBC_API_FUNCTION_LIST func {.dwVersion = NVFBC_VERSION};

    /**
     * @brief Convert a C++ boolean to the corresponding NvFBC flag.
     * @param b Boolean state to convert.
     * @return NVFBC_TRUE when b is true, otherwise NVFBC_FALSE.
     */
    static constexpr NVFBC_BOOL nv_bool(bool b) {
      return b ? NVFBC_TRUE : NVFBC_FALSE;
    }

    static dyn::library_t *library_handle {nullptr};  ///< Loaded NvFBC library retained while its function table is in use.

    /**
     * @brief Load NvFBC and create the CUDA capture helper.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!library_handle) {
        library_handle = dyn::handle({"libnvidia-fbc.so.1", "libnvidia-fbc.so"});
        if (!library_handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &createInstance, "NvFBCCreateInstance"},
      };

      if (dyn::load(library_handle, funcs)) {
        dlclose(library_handle);
        library_handle = nullptr;

        return -1;
      }

      auto status = cuda::nvfbc::createInstance(&cuda::nvfbc::func);
      if (status) {
        BOOST_LOG(error) << "Unable to create NvFBC instance"sv;

        dlclose(library_handle);
        library_handle = nullptr;
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    /**
     * @brief NvFBC CUDA context selected for a capture session.
     */
    class ctx_t {
    public:
      /**
       * @brief Explicitly create an NvFBC session context for a native capture handle.
       *
       * @param session_handle Native library or object handle used by the operation.
       */
      explicit ctx_t(NVFBC_SESSION_HANDLE session_handle) {
        NVFBC_BIND_CONTEXT_PARAMS params {NVFBC_BIND_CONTEXT_PARAMS_VER};

        if (func.nvFBCBindContext(session_handle, &params)) {
          BOOST_LOG(error) << "Couldn't bind NvFBC context to current thread: " << func.nvFBCGetLastErrorStr(session_handle);
        }

        this->handle = session_handle;
      }

      ~ctx_t() {
        NVFBC_RELEASE_CONTEXT_PARAMS params {NVFBC_RELEASE_CONTEXT_PARAMS_VER};
        if (func.nvFBCReleaseContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't release NvFBC context from current thread: " << func.nvFBCGetLastErrorStr(handle);
        }
      }

      NVFBC_SESSION_HANDLE handle;  ///< NVIDIA FBC capture session handle.
    };

    /**
     * @brief NvFBC dynamic-library handle and function table.
     */
    class handle_t {
      enum flag_e {
        SESSION_HANDLE,
        SESSION_CAPTURE,
        MAX_FLAGS,
      };

    public:
      handle_t() = default;

      /**
       * @brief Move an NvFBC API handle and its resolved function table.
       *
       * @param other Source object whose state is copied or moved into this object.
       */
      handle_t(handle_t &&other):
          handle_flags {other.handle_flags},
          handle {other.handle} {
        other.handle_flags.reset();
      }

      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @param other Source object whose state is copied or moved into this object.
       * @return Reference or value produced by the operator.
       */
      handle_t &operator=(handle_t &&other) {
        std::swap(handle_flags, other.handle_flags);
        std::swap(handle, other.handle);

        return *this;
      }

      /**
       * @brief Allocate the underlying object and wrap it in the owning handle.
       *
       * @return Created backend object, or null when creation fails.
       */
      static std::optional<handle_t> make() {
        NVFBC_CREATE_HANDLE_PARAMS params {.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER};

        // Set privateData to allow NvFBC on consumer NVIDIA GPUs.
        // Based on https://github.com/keylase/nvidia-patch/blob/3193b4b1cea91527bf09ea9b8db5aade6a3f3c0a/win/nvfbcwrp/nvfbcwrp_main.cpp#L23-L25 .
        const std::array<unsigned int, 4> MAGIC_PRIVATE_DATA = {0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA};
        params.privateData = MAGIC_PRIVATE_DATA.data();
        params.privateDataSize = sizeof(MAGIC_PRIVATE_DATA);

        handle_t handle;
        auto status = func.nvFBCCreateHandle(&handle.handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to create session: "sv << handle.last_error();

          return std::nullopt;
        }

        handle.handle_flags[SESSION_HANDLE] = true;

        return handle;
      }

      /**
       * @brief Read the last error string from the active NvFBC session.
       *
       * @return Human-readable NvFBC error string.
       */
      const char *last_error() {
        return func.nvFBCGetLastErrorStr(handle);
      }

      /**
       * @brief Return or update the current status value.
       *
       * @return Status status.
       */
      std::optional<NVFBC_GET_STATUS_PARAMS> status() {
        NVFBC_GET_STATUS_PARAMS params {.dwVersion = NVFBC_GET_STATUS_PARAMS_VER};

        auto status = func.nvFBCGetStatus(handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to get NvFBC status: "sv << last_error();

          return std::nullopt;
        }

        return params;
      }

      /**
       * @brief Run the capture loop for this backend.
       *
       * @param capture_params Capture params.
       * @return Capture status reported to the streaming pipeline.
       */
      int capture(NVFBC_CREATE_CAPTURE_SESSION_PARAMS &capture_params) {
        if (func.nvFBCCreateCaptureSession(handle, &capture_params)) {
          BOOST_LOG(error) << "Failed to start capture session: "sv << last_error();
          return -1;
        }

        handle_flags[SESSION_CAPTURE] = true;

        NVFBC_TOCUDA_SETUP_PARAMS setup_params {
          NVFBC_TOCUDA_SETUP_PARAMS_VER,
          NVFBC_BUFFER_FORMAT_BGRA,
        };

        if (func.nvFBCToCudaSetUp(handle, &setup_params)) {
          BOOST_LOG(error) << "Failed to setup cuda interop with nvFBC: "sv << last_error();
          return -1;
        }
        return 0;
      }

      /**
       * @brief Release the NvFBC capture session and wait for capture work to stop.
       *
       * @return Stop status.
       */
      int stop() {
        if (!handle_flags[SESSION_CAPTURE]) {
          return 0;
        }

        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params {NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER};

        if (func.nvFBCDestroyCaptureSession(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy capture session: "sv << last_error();

          return -1;
        }

        handle_flags[SESSION_CAPTURE] = false;

        return 0;
      }

      /**
       * @brief Reset the object to its initial empty state.
       *
       * @return Reset status.
       */
      int reset() {
        if (!handle_flags[SESSION_HANDLE]) {
          return 0;
        }

        stop();

        NVFBC_DESTROY_HANDLE_PARAMS params {NVFBC_DESTROY_HANDLE_PARAMS_VER};

        ctx_t ctx {handle};
        if (func.nvFBCDestroyHandle(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy session handle: "sv << func.nvFBCGetLastErrorStr(handle);
        }

        handle_flags[SESSION_HANDLE] = false;

        return 0;
      }

      ~handle_t() {
        reset();
      }

      std::bitset<MAX_FLAGS> handle_flags;  ///< Handle flags.

      NVFBC_SESSION_HANDLE handle;  ///< NVIDIA FBC capture session handle.
    };

    /**
     * @brief NvFBC display capture backend that produces CUDA frames.
     */
    class display_t: public platf::display_t {
    public:
      /**
       * @brief Initialize NvFBC capture for the selected display.
       *
       * @param display_name Display name.
       * @param config Configuration values to apply.
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init(const std::string_view &display_name, const ::video::config_t &config) {
        auto handle = handle_t::make();
        if (!handle) {
          return -1;
        }

        ctx_t ctx {handle->handle};

        auto status_params = handle->status();
        if (!status_params) {
          return -1;
        }

        int streamedMonitor = -1;
        if (!display_name.empty()) {
          if (status_params->bXRandRAvailable) {
            auto monitor_nr = util::from_view(display_name);

            if (monitor_nr < 0 || monitor_nr >= status_params->dwOutputNum) {
              BOOST_LOG(warning) << "Can't stream monitor ["sv << monitor_nr << "], valid output indices are [0, "sv << status_params->dwOutputNum << "), defaulting to virtual desktop"sv;
            } else {
              streamedMonitor = monitor_nr;
            }
          } else {
            BOOST_LOG(warning) << "XrandR not available, streaming entire virtual desktop"sv;
          }
        }

        delay = ::video::capture_frame_interval(config);

        capture_params = NVFBC_CREATE_CAPTURE_SESSION_PARAMS {.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER};

        capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
        capture_params.bDisableAutoModesetRecovery = nv_bool(true);

        capture_params.dwSamplingRateMs = 1000 /* ms */ / config.framerate;

        if (streamedMonitor != -1) {
          auto &output = status_params->outputs[streamedMonitor];

          width = output.trackedBox.w;
          height = output.trackedBox.h;
          offset_x = output.trackedBox.x;
          offset_y = output.trackedBox.y;

          capture_params.eTrackingType = NVFBC_TRACKING_OUTPUT;
          capture_params.dwOutputId = output.dwId;
        } else {
          capture_params.eTrackingType = NVFBC_TRACKING_SCREEN;

          width = status_params->screenSize.w;
          height = status_params->screenSize.h;
        }

        env_width = status_params->screenSize.w;
        env_height = status_params->screenSize.h;

        this->handle = std::move(*handle);
        return 0;
      }

      platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();

        {
          // We must create at least one texture on this thread before calling NvFBCToCudaSetUp()
          // Otherwise it fails with "Unable to register an OpenGL buffer to a CUDA resource (result: 201)" message
          std::shared_ptr<platf::img_t> img_dummy;
          pull_free_image_cb(img_dummy);
        }

        // Force display_t::capture to initialize handle_t::capture
        cursor_visible = !*cursor;

        ctx_t ctx {handle.handle};
        auto fg = util::fail_guard([&]() {
          handle.reset();
        });

        sleep_overshoot_logger.reset();

        while (true) {
          platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 150ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::capture_error:
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
              BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
              return status;
          }
        }

        return platf::capture_e::ok;
      }

      // Reinitialize the capture session.
      /**
       * @brief Reinitialize NvFBC capture after a recoverable failure.
       *
       * @param cursor Cursor image or visibility state to composite.
       * @return Reinit status.
       */
      platf::capture_e reinit(bool cursor) {
        if (handle.stop()) {
          return platf::capture_e::capture_error;
        }

        cursor_visible = cursor;
        if (cursor) {
          capture_params.bPushModel = nv_bool(false);
          capture_params.bWithCursor = nv_bool(true);
          capture_params.bAllowDirectCapture = nv_bool(false);
        } else {
          capture_params.bPushModel = nv_bool(true);
          capture_params.bWithCursor = nv_bool(false);
          capture_params.bAllowDirectCapture = nv_bool(true);
        }

        if (handle.capture(capture_params)) {
          return platf::capture_e::capture_error;
        }

        // If trying to capture directly, test if it actually does.
        if (capture_params.bAllowDirectCapture) {
          CUdeviceptr device_ptr;
          NVFBC_FRAME_GRAB_INFO grab_info;

          NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
            NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
            NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
            &device_ptr,
            &grab_info,
            0,
          };

          // Direct Capture may fail the first few times, even if it's possible
          for (int x = 0; x < 3; ++x) {
            if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
              if (status == NVFBC_ERR_MUST_RECREATE) {
                return platf::capture_e::reinit;
              }

              BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();

              return platf::capture_e::capture_error;
            }

            if (grab_info.bDirectCapture) {
              break;
            }

            BOOST_LOG(debug) << "Direct capture failed attempt ["sv << x << ']';
          }

          if (!grab_info.bDirectCapture) {
            BOOST_LOG(debug) << "Direct capture failed, trying the extra copy method"sv;
            // Direct capture failed
            capture_params.bPushModel = nv_bool(false);
            capture_params.bWithCursor = nv_bool(false);
            capture_params.bAllowDirectCapture = nv_bool(false);

            if (handle.stop() || handle.capture(capture_params)) {
              return platf::capture_e::capture_error;
            }
          }
        }

        return platf::capture_e::ok;
      }

      /**
       * @brief Capture a display frame into the provided image object.
       *
       * @param pull_free_image_cb Callback that provides an available image buffer.
       * @param img_out Captured CUDA image returned to the streaming pipeline.
       * @param timeout Maximum time to wait for the operation.
       * @param cursor Cursor image or visibility state to composite.
       * @return Capture status reported to the streaming pipeline.
       */
      platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
        if (cursor != cursor_visible) {
          auto status = reinit(cursor);
          if (status != platf::capture_e::ok) {
            return status;
          }
        }

        CUdeviceptr device_ptr;
        NVFBC_FRAME_GRAB_INFO grab_info;

        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
          NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
          NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
          &device_ptr,
          &grab_info,
          (std::uint32_t) timeout.count(),
        };

        if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
          if (status == NVFBC_ERR_MUST_RECREATE) {
            return platf::capture_e::reinit;
          }

          BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();
          return platf::capture_e::capture_error;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (img_t *) img_out.get();

        if (img->tex.copy((std::uint8_t *) device_ptr, img->height, img->row_pitch)) {
          return platf::capture_e::capture_error;
        }

        return platf::capture_e::ok;
      }

      /**
       * @brief Create AVCodec encode device.
       *
       * @param pix_fmt Sunshine pixel format to convert or allocate for.
       * @return Constructed AVCodec encode device object.
       */
      std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
        return ::cuda::make_avcodec_encode_device(width, height, true);
      }

      /**
       * @brief Allocate an image buffer compatible with this display backend.
       *
       * @return Allocated img object, or null when unavailable.
       */
      std::shared_ptr<platf::img_t> alloc_img() override {
        auto img = std::make_shared<cuda::img_t>();

        img->data = nullptr;
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->width * img->pixel_pitch;

        auto tex_opt = tex_t::make(height, width * img->pixel_pitch);
        if (!tex_opt) {
          return nullptr;
        }

        img->tex = std::move(*tex_opt);

        return img;
      };

      /**
       * @brief Populate a fallback image when real capture data is unavailable.
       *
       * @return Capture status reported to the streaming pipeline.
       */
      int dummy_img(platf::img_t *) override {
        return 0;
      }

      std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

      bool cursor_visible;  ///< Whether the cursor should be included in the capture.
      handle_t handle;  ///< NvFBC capture handle owning the active capture session.

      NVFBC_CREATE_CAPTURE_SESSION_PARAMS capture_params;  ///< NvFBC capture-session parameters used for frame grabs.
    };
  }  // namespace nvfbc
}  // namespace cuda

namespace platf {
  /**
   * @brief Create an NvFBC CUDA display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend, or nullptr when NvFBC/CUDA initialization fails.
   */
  std::shared_ptr<display_t> nvfbc_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize nvfbc display with the given hw device type"sv;
      return nullptr;
    }

    auto display = std::make_shared<cuda::nvfbc::display_t>();

    if (display->init(display_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate display names exposed by NvFBC.
   *
   * @return NvFBC display names, or an empty list when enumeration fails.
   */
  std::vector<std::string> nvfbc_display_names() {
    if (cuda::init() || cuda::nvfbc::init()) {
      return {};
    }

    std::vector<std::string> display_names;

    auto handle = cuda::nvfbc::handle_t::make();
    if (!handle) {
      return {};
    }

    auto status_params = handle->status();
    if (!status_params) {
      return {};
    }

    if (!status_params->bIsCapturePossible) {
      BOOST_LOG(error) << "NVidia driver doesn't support NvFBC screencasting"sv;
    }

    BOOST_LOG(info) << "Found ["sv << status_params->dwOutputNum << "] outputs"sv;
    BOOST_LOG(info) << "Virtual Desktop: "sv << status_params->screenSize.w << 'x' << status_params->screenSize.h;
    BOOST_LOG(info) << "XrandR: "sv << (status_params->bXRandRAvailable ? "available"sv : "unavailable"sv);

    for (auto x = 0; x < status_params->dwOutputNum; ++x) {
      auto &output = status_params->outputs[x];
      BOOST_LOG(info) << "-- Output --"sv;
      BOOST_LOG(debug) << "  ID: "sv << output.dwId;
      BOOST_LOG(debug) << "  Name: "sv << output.name;
      BOOST_LOG(info) << "  Resolution: "sv << output.trackedBox.w << 'x' << output.trackedBox.h;
      BOOST_LOG(info) << "  Offset: "sv << output.trackedBox.x << 'x' << output.trackedBox.y;
      display_names.emplace_back(std::to_string(x));
    }

    if (cuda::nvfbc::should_offer_virtual_desktop(status_params->dwOutputNum, status_params->bIsCapturePossible, status_params->screenSize.w, status_params->screenSize.h)) {
      BOOST_LOG(info) << "No NvFBC outputs, offering the virtual desktop"sv;
      display_names.emplace_back("0");
    }

    return display_names;
  }
}  // namespace platf
