/**
 * @file src/platform/linux/cuda.h
 * @brief Definitions for CUDA implementation.
 */
#pragma once

// standard includes
#include <cstdint>

#if defined(SUNSHINE_BUILD_CUDA)
  // standard includes
  #include <array>
  #include <cmath>
  #include <cstddef>
  #include <cstdint>
  #include <functional>
  #include <memory>
  #include <optional>
  #include <string>
  #include <utility>
  #include <vector>

  // lib includes
  #include <ffnvcodec/dynlink_cuda.h>
  #ifdef SUNSHINE_BUILD_VULKAN
    #include <vulkan/vulkan.h>
  #endif

  // local includes
  #include "src/video_colorspace.h"

struct CudaFunctions;
#endif

namespace cuda::nvfbc {
  /**
   * @brief Decide whether NvFBC should offer the whole virtual desktop as a display.
   *
   * NvFBC enumerates zero outputs when the X screen drives no display head.
   * Capture can still use `NVFBC_TRACKING_SCREEN` for the virtual desktop.
   *
   * @param output_count Number of outputs NvFBC enumerated.
   * @param capture_possible Whether NvFBC reported that capture is possible.
   * @param screen_width Virtual desktop width reported by NvFBC.
   * @param screen_height Virtual desktop height reported by NvFBC.
   * @return `true` when the virtual desktop should be offered as a display.
   */
  constexpr bool should_offer_virtual_desktop(std::uint32_t output_count, bool capture_possible, std::uint32_t screen_width, std::uint32_t screen_height) {
    return output_count == 0 && capture_possible && screen_width > 0 && screen_height > 0;
  }
}  // namespace cuda::nvfbc

#if defined(SUNSHINE_BUILD_CUDA)
namespace platf {
  struct avcodec_encode_device_t;
  struct img_t;
}  // namespace platf

namespace cuda {

  namespace nvfbc {
    std::vector<std::string> display_names();
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram);

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param in_width Width of captured frames.
   * @param in_height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y);

  /**
   * @brief Functions used to query the PCI bus id of a CUDA device.
   */
  struct cuda_pci_accessors_t {
    bool (*is_initialized)();  ///< Report whether the CUDA function table is loaded.
    int (*initialize)();  ///< Load the CUDA function table, returning 0 on success.
    tcuDeviceGet *get_device;  ///< Resolve a CUDA device index, returning CUDA_SUCCESS on success.
    tcuDeviceGetPCIBusId *get_pci_bus_id;  ///< Write the device bus id, returning CUDA_SUCCESS on success.
  };

  /**
   * @brief Query the PCI bus id of a CUDA device.
   * @param index CUDA device index to query.
   * @return Lowercase PCI bus id (e.g. "00000000:0f:00.0"), or no value when
   *         CUDA is unavailable or the query fails.
   */
  std::optional<std::string> device_pci_bus_id(int index);

  /**
   * @brief Query the PCI bus id of a CUDA device with injected driver operations.
   * @param index CUDA device index to query.
   * @param accessors Driver operations used to initialize CUDA and query the device.
   * @return Lowercase PCI bus id (e.g. "00000000:0f:00.0"), or no value when
   *         CUDA is unavailable or the query fails.
   */
  std::optional<std::string> device_pci_bus_id(int index, const cuda_pci_accessors_t &accessors);

  /**
   * @brief GL fence calls used to wait for GL on the CPU, all acting on the same fence.
   */
  struct gl_fence_ops_t {
    std::function<bool()> create;  ///< Insert a fence after the submitted GL commands, returning false on failure.
    std::function<std::uint32_t()> client_wait;  ///< Wait for the fence, returning the GL wait status.
    std::function<void()> destroy;  ///< Delete the fence.
  };

  /**
   * @brief Block until GL has finished the commands submitted so far.
   *
   * @param ops GL fence calls.
   * @return 0 on success or -1 when the fence couldn't be created, timed out or failed.
   */
  int wait_for_gl_fence(const gl_fence_ops_t &ops);

  /**
   * @brief Steps of handing one converted frame from GL to CUDA.
   */
  struct gl_cuda_handoff_ops_t {
    std::function<int()> wait_for_cuda;  ///< Shared planes: block until the previous frame's copies are done.
    std::function<void()> render;  ///< Render the frame into the plane textures with GL.
    std::function<int()> wait_for_gl;  ///< Shared planes: block until GL has finished rendering.
    std::function<int()> map;  ///< Mapped planes: map the plane textures into CUDA.
    std::function<int(int plane)> copy;  ///< Queue the copy of one plane into the frame.
    std::function<int()> unmap;  ///< Mapped planes: unmap the plane textures.
    std::function<int()> record_copies;  ///< Shared planes: record the queued copies for the next wait_for_cuda.
  };

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
  int gl_cuda_handoff(bool shared_planes, int plane_count, const gl_cuda_handoff_ops_t &ops);

  /**
   * @brief Block until GL has finished the commands submitted so far on the current context.
   *
   * @return 0 on success or -1 on failure.
   */
  int wait_for_gl_commands();

  /**
   * @brief How a shared plane's format appears to GL and CUDA.
   */
  struct shared_plane_format_t {
    std::uint32_t gl_format;  ///< Sized GL internal format.
    CUarray_format cu_format;  ///< CUDA array format.
    unsigned int channels;  ///< Channels per pixel.
  };

  /**
   * @brief Find the GL, CUDA and channel layout of a shared plane's format.
   *
   * @param internal_format Sized GL internal format of the plane.
   * @return The plane format, or no value when it can't be shared.
   */
  std::optional<shared_plane_format_t> shared_plane_format(std::uint32_t internal_format);

  #ifdef SUNSHINE_BUILD_VULKAN
  /**
   * @brief Find the Vulkan format of a shared plane.
   *
   * @param internal_format Sized GL internal format of the plane.
   * @return The Vulkan format, or `VK_FORMAT_UNDEFINED` when it can't be shared.
   */
  VkFormat shared_plane_vk_format(std::uint32_t internal_format);

  /**
   * @brief Describe the Vulkan image behind a shared plane.
   *
   * @param internal_format Sized GL internal format of the plane, one shared_plane_format() accepts.
   * @param width Plane width.
   * @param height Plane height.
   * @param optimal_tiling_features Features the plane's Vulkan format has with optimal tiling.
   * @param external_info Export info to chain into the image info. Must outlive the result.
   * @return The image's creation info.
   */
  VkImageCreateInfo shared_plane_image_info(std::uint32_t internal_format, int width, int height, VkFormatFeatureFlags optimal_tiling_features, const VkExternalMemoryImageCreateInfo &external_info);
  #endif

  /**
   * @brief Driver calls that share plane memory between GL and CUDA.
   *
   * Release calls may run after their acquiring call failed, so they tolerate
   * whatever that call left behind.
   */
  class shared_memory_driver_t {
  public:
    virtual ~shared_memory_driver_t() = default;

    virtual bool supported() = 0;  ///< Whether GL and CUDA can import shared memory.
    virtual int create_device() = 0;  ///< Create the device that allocates the memory. Returns 0 on success.
    virtual void destroy_device() = 0;  ///< Destroy the device.
    virtual int create_event() = 0;  ///< Create the event recorded after each frame's copies. Returns 0 on success.
    virtual void destroy_event() = 0;  ///< Destroy the event.
    virtual int wait_event() = 0;  ///< Block until the last recorded copies are done. Returns 0 on success.
    virtual int record_event(CUstream stream) = 0;  ///< Record the event after the copies queued on `stream`. Returns 0 on success.
    virtual int allocate_memory(std::size_t plane, std::uint32_t internal_format, int width, int height, std::uint64_t &size) = 0;  ///< Allocate exportable memory, setting `size`. Returns 0 on success.
    virtual void free_memory(std::size_t plane) = 0;  ///< Free a plane's memory.
    virtual int export_fd(std::size_t plane, int &fd) = 0;  ///< Export a new descriptor for a plane's memory. Returns 0 on success.
    virtual void close_fd(int fd) = 0;  ///< Close a descriptor that wasn't imported.
    virtual int import_gl(std::size_t plane, int fd, std::uint64_t size) = 0;  ///< Import into GL, which owns `fd` on success. Returns 0 on success.
    virtual int create_gl_storage(std::size_t plane, std::uint32_t internal_format, int width, int height) = 0;  ///< Back the texture bound to `GL_TEXTURE_2D` with the imported memory. Returns 0 on success.
    virtual void release_gl(std::size_t plane) = 0;  ///< Release a plane's GL memory object.
    virtual int import_cuda(std::size_t plane, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC &desc) = 0;  ///< Import into CUDA, which owns the descriptor on success. Returns 0 on success.
    virtual void release_cuda_memory(std::size_t plane) = 0;  ///< Release a plane's CUDA external memory.
    virtual int map_array(std::size_t plane, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC &desc) = 0;  ///< Map the external memory as an array. Returns 0 on success.
    virtual void release_array(std::size_t plane) = 0;  ///< Release a plane's CUDA array.
    virtual CUarray array(std::size_t plane) const = 0;  ///< CUDA array backing a plane.
  };

  /**
   * @brief Create the driver that shares plane memory on this build.
   *
   * @return The driver, or nullptr in a build without Vulkan.
   */
  std::unique_ptr<shared_memory_driver_t> make_shared_memory_driver();

  #ifdef SUNSHINE_BUILD_VULKAN
  /**
   * @brief Vulkan entry points used to allocate shared memory.
   */
  struct vulkan_calls_t {
    PFN_vkCreateInstance create_instance;  ///< `vkCreateInstance`.
    PFN_vkDestroyInstance destroy_instance;  ///< `vkDestroyInstance`.
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;  ///< `vkEnumeratePhysicalDevices`.
    PFN_vkGetPhysicalDeviceProperties2 get_properties2;  ///< `vkGetPhysicalDeviceProperties2`.
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties;  ///< `vkGetPhysicalDeviceFormatProperties`.
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;  ///< `vkGetPhysicalDeviceMemoryProperties`.
    PFN_vkCreateDevice create_device;  ///< `vkCreateDevice`.
    PFN_vkDestroyDevice destroy_device;  ///< `vkDestroyDevice`.
    PFN_vkCreateImage create_image;  ///< `vkCreateImage`.
    PFN_vkDestroyImage destroy_image;  ///< `vkDestroyImage`.
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements;  ///< `vkGetImageMemoryRequirements`.
    PFN_vkAllocateMemory allocate_memory;  ///< `vkAllocateMemory`.
    PFN_vkFreeMemory free_memory;  ///< `vkFreeMemory`.
    PFN_vkBindImageMemory bind_image_memory;  ///< `vkBindImageMemory`.
    PFN_vkGetMemoryFdKHR (*load_get_memory_fd)(VkDevice device);  ///< Load `vkGetMemoryFdKHR` for a device.
    int (*close_fd)(int fd);  ///< `close`.
  };

  /**
   * @brief Function returning the CUDA function table.
   */
  using cuda_functions_fn = std::function<const ::CudaFunctions *()>;

  /**
   * @brief Create a driver that shares plane memory through Vulkan.
   *
   * @param calls Vulkan entry points.
   * @param cuda_functions Returns the CUDA function table, which loads after construction.
   * @return The driver.
   */
  std::unique_ptr<shared_memory_driver_t> make_vulkan_shared_memory(const vulkan_calls_t &calls, cuda_functions_fn cuda_functions);
  #endif

  /**
   * @brief Output plane textures shared by GL and CUDA through external memory.
   *
   * Vulkan allocates exportable memory for each plane. GL and CUDA both import it,
   * so CUDA reads what GL renders without mapping the textures.
   *
   * Each handoff waits on the CPU: a GL fence before the copies, and a CUDA event
   * before GL renders into the planes again. Handing off on the GPU instead, by
   * mapping or with external semaphores, makes GL and CUDA wait on each other inside
   * the GPU, which stalls other GPU work such as a game.
   *
   * Without Vulkan there is no driver, so init() fails and the caller maps the textures every frame.
   */
  class gl_cuda_shared_planes_t {
  public:
    /**
     * @brief Construct the planes on top of a driver.
     *
     * @param memory_driver Driver calls, or nullptr when this build can't share memory.
     */
    explicit gl_cuda_shared_planes_t(std::unique_ptr<shared_memory_driver_t> memory_driver):
        driver {std::move(memory_driver)} {
    }

    gl_cuda_shared_planes_t(const gl_cuda_shared_planes_t &) = delete;
    gl_cuda_shared_planes_t &operator=(const gl_cuda_shared_planes_t &) = delete;

    ~gl_cuda_shared_planes_t() {
      reset();
    }

    /**
     * @brief Prepare to share memory on the current CUDA device.
     *
     * @return 0 on success or -1 when GL, CUDA or Vulkan can't share memory here.
     */
    int init() {
      if (!driver || !driver->supported()) {
        return -1;
      }

      device_started = true;
      if (driver->create_device()) {
        return -1;
      }

      event_started = true;
      if (driver->create_event()) {
        return -1;
      }

      active = true;
      return 0;
    }

    /**
     * @brief Back the texture bound to `GL_TEXTURE_2D` with exportable memory and import it into CUDA.
     *
     * Matches egl::target_storage_fn. On failure, what was acquired stays held until reset().
     *
     * @param plane Plane index.
     * @param internal_format Sized GL internal format of the plane.
     * @param width Plane width.
     * @param height Plane height.
     * @return 0 on success or -1 on failure.
     */
    int allocate(std::size_t plane, std::uint32_t internal_format, int width, int height) {
      const auto format = shared_plane_format(internal_format);
      if (!format) {
        return -1;
      }

      auto &started = planes[plane];

      std::uint64_t size = 0;
      started.memory = true;
      if (driver->allocate_memory(plane, internal_format, width, height, size)) {
        return -1;
      }

      // GL and CUDA each import their own fd, and take ownership of it on success.
      int gl_fd = -1;
      int cu_fd = -1;
      if (driver->export_fd(plane, gl_fd)) {
        return -1;
      }
      if (driver->export_fd(plane, cu_fd)) {
        driver->close_fd(gl_fd);
        return -1;
      }

      started.gl_memory = true;
      if (driver->import_gl(plane, gl_fd, size)) {
        driver->close_fd(gl_fd);
        driver->close_fd(cu_fd);
        return -1;
      }
      if (driver->create_gl_storage(plane, internal_format, width, height)) {
        driver->close_fd(cu_fd);
        return -1;
      }

      CUDA_EXTERNAL_MEMORY_HANDLE_DESC memory_desc {};
      memory_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
      memory_desc.handle.fd = cu_fd;
      memory_desc.size = size;
      memory_desc.flags = cu_external_memory_dedicated;
      started.cu_memory = true;
      if (driver->import_cuda(plane, memory_desc)) {
        driver->close_fd(cu_fd);
        return -1;
      }

      CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC array_desc {};
      array_desc.arrayDesc.Width = width;
      array_desc.arrayDesc.Height = height;
      array_desc.arrayDesc.Format = format->cu_format;
      array_desc.arrayDesc.NumChannels = format->channels;
      array_desc.arrayDesc.Flags = cu_array3d_color_attachment;
      array_desc.numLevels = 1;
      started.array = true;
      return driver->map_array(plane, array_desc);
    }

    /**
     * @brief Block until CUDA has copied the previous frame out of the planes.
     *
     * Call before GL renders into the planes.
     *
     * @return 0 on success or -1 on failure.
     */
    int wait_for_cuda() {
      if (!copies_pending) {
        return 0;
      }

      copies_pending = false;
      return driver->wait_event();
    }

    /**
     * @brief Note that this frame's copies out of the planes are queued, so wait_for_cuda() waits for them.
     *
     * @param stream CUDA stream the copies were queued on.
     * @return 0 on success or -1 on failure.
     */
    int record_copies(CUstream stream) {
      if (driver->record_event(stream)) {
        return -1;
      }

      copies_pending = true;
      return 0;
    }

    /**
     * @brief CUDA array backing a plane.
     *
     * @param plane Plane index.
     * @return The plane's CUDA array.
     */
    CUarray array(std::size_t plane) const {
      return driver->array(plane);
    }

    /**
     * @brief Whether the planes are shared and the handoff methods apply.
     *
     * @return True after a successful init() until reset().
     */
    bool is_active() const {
      return active;
    }

    /**
     * @brief Release everything init() and allocate() started. The plane textures must already be deleted.
     */
    void reset() {
      for (std::size_t plane = 0; plane < max_planes; ++plane) {
        const auto started = planes[plane];
        planes[plane] = {};

        if (started.array) {
          driver->release_array(plane);
        }
        if (started.cu_memory) {
          driver->release_cuda_memory(plane);
        }
        if (started.gl_memory) {
          driver->release_gl(plane);
        }
        if (started.memory) {
          driver->free_memory(plane);
        }
      }

      if (event_started) {
        driver->destroy_event();
      }
      if (device_started) {
        driver->destroy_device();
      }

      event_started = false;
      device_started = false;
      copies_pending = false;
      active = false;
    }

  private:
    /**
     * @brief Which steps were started for one plane, and so must be released.
     */
    struct plane_steps_t {
      bool memory = false;  ///< allocate_memory() was called.
      bool gl_memory = false;  ///< import_gl() was called.
      bool cu_memory = false;  ///< import_cuda() was called.
      bool array = false;  ///< map_array() was called.
    };

    static constexpr std::size_t max_planes = 3;  ///< YUV 4:4:4 has the most planes.
    static constexpr unsigned int cu_external_memory_dedicated = 0x1;  ///< `CUDA_EXTERNAL_MEMORY_DEDICATED`, missing from ffnvcodec.
    static constexpr unsigned int cu_array3d_color_attachment = 0x20;  ///< `CUDA_ARRAY3D_COLOR_ATTACHMENT`, missing from ffnvcodec.

    std::unique_ptr<shared_memory_driver_t> driver;  ///< Driver calls.
    std::array<plane_steps_t, max_planes> planes {};  ///< Steps started for each plane.
    bool device_started = false;  ///< create_device() was called.
    bool event_started = false;  ///< create_event() was called.
    bool copies_pending = false;  ///< Whether the event was recorded since wait_for_cuda() last waited on it.
    bool active = false;  ///< Whether init() succeeded.
  };

  /**
   * @brief Steps of creating the GL plane textures.
   */
  struct gl_cuda_target_ops_t {
    std::function<int()> init_shared;  ///< Prepare to share memory.
    std::function<int()> create_shared_target;  ///< Create the textures on shared memory.
    std::function<void()> reset_shared;  ///< Release the shared memory.
    std::function<int()> create_mapped_target;  ///< Create plain GL textures.
    std::function<int()> register_mapped_target;  ///< Register the plain textures with CUDA.
  };

  /**
   * @brief Create the plane textures, sharing their memory with CUDA when possible.
   *
   * Falls back to textures mapped every frame, after releasing anything the shared attempt
   * left behind. A failed attempt must have deleted its textures already, because they can't
   * outlive the memory backing them.
   *
   * @param ops Steps of creating the textures.
   * @return 0 on success or -1 when neither way of creating them works.
   */
  int create_gl_cuda_target(const gl_cuda_target_ops_t &ops);

  int init();

  /**
   * @brief Fill a CUDA function table the way `cuda_load_functions` does.
   */
  using function_table_loader_t = std::function<int(CudaFunctions **table)>;

  /**
   * @brief Load a CUDA function table unless it is already loaded, and initialize CUDA through it.
   *
   * `init()` passes the table every encode device shares and the real loader.
   *
   * @param table Table to load.
   * @param load Function that fills the table, returning 0 on success.
   * @return 0 on success, -1 on failure.
   */
  int init(CudaFunctions **table, const function_table_loader_t &load);
}  // namespace cuda

/**
 * @brief Opaque CUDA array handle shared by host and CUDA compiler declarations.
 */
using cudaArray_t = struct cudaArray *;

  #if !defined(__CUDACC__)
/**
 * @brief Opaque CUDA stream handle for host compilation.
 */
using cudaStream_t = struct CUstream_st *;
/**
 * @brief CUDA texture object identifier for host compilation.
 */
using cudaTextureObject_t = unsigned long long;
  #else /* defined(__CUDACC__) */
typedef __location__(device_builtin) struct CUstream_st *cudaStream_t;
typedef __location__(device_builtin) unsigned long long cudaTextureObject_t;
  #endif /* !defined(__CUDACC__) */

namespace cuda {

  class freeCudaPtr_t {
  public:
    void operator()(void *ptr);
  };

  class freeCudaStream_t {
  public:
    void operator()(cudaStream_t ptr);
  };

  using ptr_t = std::unique_ptr<void, freeCudaPtr_t>;
  using stream_t = std::unique_ptr<CUstream_st, freeCudaStream_t>;

  stream_t make_stream(int flags = 0);

  struct viewport_t {
    int width;
    int height;
    int offsetX;
    int offsetY;
  };

  /**
   * @brief Fit a captured image into an output frame, keeping its aspect ratio.
   *
   * The offsets are rounded down to even values. NV12 stores one interleaved U,V pair for every
   * two pixels, and the conversion kernel writes each pair at the pixel's x plus `offsetX`, so an
   * odd offset would place every pair one byte off and swap U and V (red and blue).
   *
   * @param in_width Width of the captured image in pixels.
   * @param in_height Height of the captured image in pixels.
   * @param out_width Width of the output frame in pixels.
   * @param out_height Height of the output frame in pixels.
   * @param scale Set to the number of captured pixels per output pixel.
   * @return The area of the output frame that the captured image covers.
   */
  inline viewport_t make_viewport(int in_width, int in_height, int out_width, int out_height, float &scale) {
    // Ensure aspect ratio is maintained
    const auto scalar = std::fminf(out_width / (float) in_width, out_height / (float) in_height);
    const auto out_width_f = in_width * scalar;
    const auto out_height_f = in_height * scalar;

    // result is always positive
    const auto offsetX_f = (out_width - out_width_f) / 2;
    const auto offsetY_f = (out_height - out_height_f) / 2;

    viewport_t viewport;
    viewport.width = out_width_f;
    viewport.height = out_height_f;
    viewport.offsetX = static_cast<int>(offsetX_f) & ~1;
    viewport.offsetY = static_cast<int>(offsetY_f) & ~1;

    scale = 1.0f / scalar;
    return viewport;
  }

  class tex_t {
  public:
    static std::optional<tex_t> make(int height, int pitch);

    tex_t();
    tex_t(tex_t &&) noexcept;

    tex_t &operator=(tex_t &&other) noexcept;

    ~tex_t();

    int copy(std::uint8_t *src, int height, int pitch);

    cudaArray_t array;

    /**
     * @brief CUDA texture objects for point and linear sampling of the same image.
     */
    struct texture_t {
      cudaTextureObject_t point;
      cudaTextureObject_t linear;
    };

    texture_t texture;  ///< Point and linear sampling textures for the CUDA array.
  };

  class sws_t {
  public:
    sws_t() = default;
    sws_t(int in_width, int in_height, int out_width, int out_height, int pitch, int threadsPerBlock, ptr_t &&color_matrix);

    /**
     * in_width, in_height -- The width and height of the captured image in pixels
     * out_width, out_height -- the width and height of the NV12 image in pixels
     *
     * pitch -- The size of a single row of pixels in bytes
     */
    static std::optional<sws_t> make(int in_width, int in_height, int out_width, int out_height, int pitch);

    // Converts loaded image into a CUDevicePtr
    int convert_nv12(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream);
    int convert_nv12(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport);
    int convert_yuv444(std::uint8_t *Y, std::uint8_t *U, std::uint8_t *V, std::uint32_t pitch, cudaTextureObject_t texture, stream_t::pointer stream);
    int convert_yuv444(std::uint8_t *Y, std::uint8_t *U, std::uint8_t *V, std::uint32_t pitch, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport);

    void apply_colorspace(const video::sunshine_colorspace_t &colorspace);

    int load_ram(platf::img_t &img, cudaArray_t array);

    ptr_t color_matrix;

    int threadsPerBlock;

    viewport_t viewport;

    float scale;
  };

  #ifdef SUNSHINE_TESTS
  namespace test {
    /**
     * @brief NV12 planes copied back from the GPU.
     */
    struct nv12_planes_t {
      std::vector<std::uint8_t> y;  ///< Luma plane, one byte per pixel, rows packed.
      std::vector<std::uint8_t> uv;  ///< Chroma plane, U then V for each 2x2 block, rows packed.
    };

    /**
     * @brief Report whether a CUDA device is available to run the kernels.
     * @return True when at least one CUDA device can be used.
     */
    bool has_device();

    /**
     * @brief Convert a BGRA frame to NV12 with the production NV12 kernel.
     * @param bgra Frame pixels, 4 bytes per pixel in B, G, R, A order, rows packed.
     * @param width Frame width in pixels. Must be even.
     * @param height Frame height in pixels. Must be even.
     * @param colorspace Colorspace to apply before the conversion.
     * @return The converted planes, or `std::nullopt` when the input is invalid or a CUDA call fails.
     */
    std::optional<nv12_planes_t> convert_to_nv12(const std::vector<std::uint8_t> &bgra, int width, int height, const video::sunshine_colorspace_t &colorspace);
  }  // namespace test
  #endif
}  // namespace cuda

#endif
