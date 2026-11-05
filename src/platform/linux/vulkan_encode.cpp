/**
 * @file src/platform/linux/vulkan_encode.cpp
 * @brief Vulkan-native encoder: DMA-BUF -> Vulkan compute (RGB->YUV) -> Vulkan Video encode.
 *        No EGL/GL dependency — all GPU work stays in a single Vulkan queue.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <drm_fourcc.h>
#include <map>
#include <optional>
#include <sys/stat.h>
#if defined(__FreeBSD__)
  #include <sys/types.h>
#else
  #include <sys/sysmacros.h>
#endif
#include <vector>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/version.h>
}

#include "graphics.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/video_colorspace.h"
#include "vulkan_encode.h"

// SPIR-V data generated at build time
static const std::vector<uint32_t> rgb2yuv_comp_spv_data
#include "shaders/rgb2yuv.spv.inc"
  ;
static const size_t rgb2yuv_comp_spv_size = rgb2yuv_comp_spv_data.size() * sizeof(uint32_t);

using namespace std::literals;

namespace vk {

  /**
   * @brief Find Vulkan physical device matching a render node path.
   *
   * @param devs List of Vulkan physical devices.
   * @param render_path Path to render node (e.g. /dev/dri/renderD128).
   * @return Matching device, or devs[0] if no match found.
   */
  static VkPhysicalDevice find_device_by_render_node(const std::vector<VkPhysicalDevice> &devs, const std::string &render_path) {
    if (render_path.empty() || render_path[0] != '/') {
      return devs[0];
    }

    struct stat node_stat;
    if (stat(render_path.c_str(), &node_stat) != 0) {
      return devs[0];
    }

    auto target_major = major(node_stat.st_rdev);
    auto target_minor = minor(node_stat.st_rdev);

    for (const auto &dev : devs) {
      VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props2.pNext = &drm;
      vkGetPhysicalDeviceProperties2(dev, &props2);

      if (drm.hasRender && drm.renderMajor == (int64_t) target_major && drm.renderMinor == (int64_t) target_minor) {
        return dev;
      }
    }

    return devs[0];
  }

  /**
   * @brief Query supported modifiers for a single format.
   *
   * @param phys_dev Vulkan physical device.
   * @param vk_fmt Vulkan format to query.
   * @return Vector of supported modifiers, or empty if none.
   */
  static std::vector<std::uint64_t> query_format_modifiers(VkPhysicalDevice phys_dev, VkFormat vk_fmt) {
    VkDrmFormatModifierPropertiesListEXT mod_list = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fmt_props2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    fmt_props2.pNext = &mod_list;

    vkGetPhysicalDeviceFormatProperties2(phys_dev, vk_fmt, &fmt_props2);

    if (mod_list.drmFormatModifierCount == 0) {
      return {};
    }

    std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(mod_list.drmFormatModifierCount);
    mod_list.pDrmFormatModifierProperties = mod_props.data();
    vkGetPhysicalDeviceFormatProperties2(phys_dev, vk_fmt, &fmt_props2);

    std::vector<std::uint64_t> modifiers;
    modifiers.reserve(mod_props.size());
    for (const auto &mp : mod_props) {
      // Only include modifiers that support sampled images (needed for compute shader input)
      if (mp.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        modifiers.push_back(mp.drmFormatModifier);
      }
    }

    return modifiers;
  }

  /**
   * @brief Query DRM format modifiers supported by the Vulkan driver for common capture formats.
   *
   * Creates a temporary Vulkan instance and physical device to query modifier support
   * via VK_EXT_image_drm_format_modifier extension.
   *
   * @return Map of DRM format to supported modifiers, or empty map if query fails.
   */
  std::map<std::uint32_t, std::vector<std::uint64_t>> get_supported_capture_modifiers() {
    std::map<std::uint32_t, std::vector<std::uint64_t>> result;

    // Create temporary Vulkan instance
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
      BOOST_LOG(warning) << "[vulkan] Failed to create instance for modifier query"sv;
      return result;
    }

    // Get physical devices
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    if (count == 0) {
      vkDestroyInstance(inst, nullptr);
      return result;
    }

    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(inst, &count, devs.data());

    // Find the device matching the render node that will be used for encoding
    VkPhysicalDevice phys_dev = find_device_by_render_node(devs, platf::resolve_render_device());

    // Common capture formats (ARGB/XRGB variants)
    static const std::array<std::pair<uint32_t, VkFormat>, 4> formats_to_query = {{
      {DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM},
      {DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM},
      {DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM},
      {DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM},
    }};

    for (const auto &[drm_fmt, vk_fmt] : formats_to_query) {
      auto modifiers = query_format_modifiers(phys_dev, vk_fmt);
      if (!modifiers.empty()) {
        BOOST_LOG(debug) << "[vulkan] Format 0x"sv << std::hex << drm_fmt << std::dec
                         << " has "sv << modifiers.size() << " supported modifiers"sv;
        result[drm_fmt] = std::move(modifiers);
      }
    }

    vkDestroyInstance(inst, nullptr);
    return result;
  }

  /**
   * @brief Query and validate encoder quality level for a given codec.
   *
   * This uses FFmpeg's Vulkan context to query maxQualityLevels and validate
   * the requested level using vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR.
   * Codec-specific quality properties are chained to the output as required by Vulkan.
   *
   * @param codec_id FFmpeg codec ID (AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1).
   * @param vk_ctx FFmpeg's AVVulkanDeviceContext with initialized Vulkan handles.
   * @param requested_level Requested quality level to validate (0 = speed, max = quality).
   * @param[out] max_level Highest zero-based quality level supported by the driver.
   * @return Validated quality level, or std::nullopt when the query fails.
   */
  static std::optional<uint32_t> query_and_validate_quality_level(AVCodecID codec_id, AVVulkanDeviceContext *vk_ctx, uint32_t requested_level, uint32_t *max_level) {
    *max_level = 0;

    if (!vk_ctx || !vk_ctx->inst || !vk_ctx->phys_dev || !vk_ctx->get_proc_addr) {
      return std::nullopt;
    }

    // Only a device FFmpeg created with video encode support can be queried. The loader resolves
    // vkGetPhysicalDeviceVideoCapabilitiesKHR for any instance, but on a device whose driver lacks video
    // encode (Mesa's lavapipe, the only Vulkan device on GPUs without a Vulkan driver) the call reaches a
    // null driver entry point and crashes Sunshine during encoder probing.
    const auto *extensions_begin = vk_ctx->enabled_dev_extensions;
    const auto *extensions_end = extensions_begin + std::max(vk_ctx->nb_enabled_dev_extensions, 0);
    if (!extensions_begin || std::none_of(extensions_begin, extensions_end, [](const char *extension) {
          return extension && std::strcmp(extension, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) == 0;
        })) {
      BOOST_LOG(debug) << "[vulkan] Device has no video encode support; keeping the driver's default quality"sv;
      return std::nullopt;
    }

    auto vkGetPhysicalDeviceVideoCapabilitiesKHR_fn = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)
                                                        vk_ctx->get_proc_addr(vk_ctx->inst, "vkGetPhysicalDeviceVideoCapabilitiesKHR");

    if (!vkGetPhysicalDeviceVideoCapabilitiesKHR_fn) {
      return std::nullopt;
    }

    // Codec-specific profile extensions (zero-initialized)
    VkVideoEncodeH264ProfileInfoKHR h264_profile = {};
    h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
    h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;

    VkVideoEncodeH265ProfileInfoKHR h265_profile = {};
    h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR;
    h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;

    VkVideoEncodeAV1ProfileInfoKHR av1_profile = {};
    av1_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR;
    av1_profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;

    // Codec-specific capabilities extensions (zero-initialized)
    VkVideoEncodeH264CapabilitiesKHR h264_caps = {};
    h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;

    VkVideoEncodeH265CapabilitiesKHR h265_caps = {};
    h265_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR;

    VkVideoEncodeAV1CapabilitiesKHR av1_caps = {};
    av1_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR;

    VkVideoEncodeH264QualityLevelPropertiesKHR h264_quality = {};
    h264_quality.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR;

    VkVideoEncodeH265QualityLevelPropertiesKHR h265_quality = {};
    h265_quality.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR;

    VkVideoEncodeAV1QualityLevelPropertiesKHR av1_quality = {};
    av1_quality.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUALITY_LEVEL_PROPERTIES_KHR;

    VkVideoEncodeQualityLevelPropertiesKHR quality_props = {};
    quality_props.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR;

    // Usage info chain (follows FFmpeg's pattern)
    VkVideoEncodeUsageInfoKHR usage_info = {};
    usage_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR;
    usage_info.videoUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR;
    usage_info.videoContentHints = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR;
    usage_info.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR;

    // Profile info
    VkVideoProfileInfoKHR profile = {};
    profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile.pNext = &usage_info;
    profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    // Encode capabilities output
    VkVideoEncodeCapabilitiesKHR enc_caps = {};
    enc_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;

    // Set codec operation and chain codec-specific profile, capabilities, and quality properties.
    switch (codec_id) {
      case AV_CODEC_ID_H264:
        profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        usage_info.pNext = &h264_profile;
        enc_caps.pNext = &h264_caps;
        quality_props.pNext = &h264_quality;
        break;
      case AV_CODEC_ID_HEVC:
        profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        usage_info.pNext = &h265_profile;
        enc_caps.pNext = &h265_caps;
        quality_props.pNext = &h265_quality;
        break;
      case AV_CODEC_ID_AV1:
        profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
        usage_info.pNext = &av1_profile;
        enc_caps.pNext = &av1_caps;
        quality_props.pNext = &av1_quality;
        break;
      default:
        return std::nullopt;
    }

    // Main capabilities output
    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps.pNext = &enc_caps;

    VkResult result = vkGetPhysicalDeviceVideoCapabilitiesKHR_fn(vk_ctx->phys_dev, &profile, &caps);
    if (result != VK_SUCCESS || enc_caps.maxQualityLevels == 0) {
      return std::nullopt;
    }

    *max_level = enc_caps.maxQualityLevels - 1;
    BOOST_LOG(debug) << "[vulkan] Driver max quality level: "sv << *max_level;

    // If requested level is 0 or within range, try to validate it
    if (requested_level > *max_level) {
      BOOST_LOG(warning) << "[vulkan] Requested quality level "sv << requested_level
                         << " exceeds max "sv << *max_level << ", using max"sv;
      requested_level = *max_level;
    }

    // Try to validate the requested level
    auto vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR_fn = (PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR)
                                                                        vk_ctx->get_proc_addr(vk_ctx->inst, "vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR");

    if (vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR_fn) {
      VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR quality_level_info = {};
      quality_level_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
      quality_level_info.pVideoProfile = &profile;
      quality_level_info.qualityLevel = requested_level;

      result = vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR_fn(vk_ctx->phys_dev, &quality_level_info, &quality_props);
      if (result != VK_SUCCESS) {
        return std::nullopt;
      }
    }

    return requested_level;
  }

  // Match a DRI render node path to a Vulkan device index via VK_EXT_physical_device_drm.
  // Returns the index as a string (e.g. "1"), or empty string if no match.
  // Result is cached since render device doesn't change during runtime.
  static std::string find_vulkan_index_for_render_node(const char *render_path) {
    // Cache result to avoid repeated Vulkan instance creation (~10-20ms overhead)
    static std::string cached_result;
    static std::string cached_path;
    static bool cached = false;

    if (cached && cached_path == render_path) {
      return cached_result;
    }

    struct stat node_stat;
    if (stat(render_path, &node_stat) < 0) {
      return {};
    }

    auto target_major = major(node_stat.st_rdev);
    auto target_minor = minor(node_stat.st_rdev);

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;

    static const std::array<const char *, 1> instance_exts = {VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = instance_exts.size();
    ci.ppEnabledExtensionNames = instance_exts.data();
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
      // Retry without the extension for loaders that don't support it
      ci.enabledExtensionCount = 0;
      ci.ppEnabledExtensionNames = nullptr;
      if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
        return {};
      }
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(inst, &count, devs.data());

    std::string result;
    for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props2.pNext = &drm;
      vkGetPhysicalDeviceProperties2(devs[i], &props2);
      if (drm.hasRender && drm.renderMajor == (int64_t) target_major && drm.renderMinor == (int64_t) target_minor) {
        result = std::to_string(i);
        break;
      }
    }
    vkDestroyInstance(inst, nullptr);

    // Cache the result
    cached_path = render_path;
    cached_result = result;
    cached = true;

    return result;
  }

  static int create_vulkan_hwdevice(AVBufferRef **hw_device_buf) {
    // Resolve render device path to Vulkan device index
    if (auto render_path = platf::resolve_render_device(); render_path[0] == '/') {
      if (auto idx = find_vulkan_index_for_render_node(render_path.c_str()); !idx.empty() && av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, idx.c_str(), nullptr, 0) >= 0) {
        return 0;
      }
    } else {
      // Non-path: treat as device name substring or numeric index
      if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, render_path.c_str(), nullptr, 0) >= 0) {
        return 0;
      }
    }
    // Final fallback: let FFmpeg pick default
    if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) >= 0) {
      return 0;
    }
    return -1;
  }

  /**
   * @brief Vulkan shader constants used by the conversion pass.
   */
  struct PushConstants {
    std::array<float, 4> color_vec_y;  ///< Color vec y.
    std::array<float, 4> color_vec_u;  ///< Color vec u.
    std::array<float, 4> color_vec_v;  ///< Color vec v.
    std::array<float, 2> range_y;  ///< Range y.
    std::array<float, 2> range_uv;  ///< Range uv.
    std::array<int32_t, 2> src_offset;  ///< Src offset.
    std::array<int32_t, 2> src_size;  ///< Src size.
    std::array<int32_t, 2> dst_offset;  ///< Dst offset.
    std::array<int32_t, 2> dst_size;  ///< Dst size.
    std::array<int32_t, 2> dst_full_size;  ///< Dst full size.
    std::array<int32_t, 2> cursor_pos;  ///< Cursor pos.
    std::array<int32_t, 2> cursor_size;  ///< Cursor size.
    int32_t y_invert;  ///< Y invert.
  };

// Helper to check VkResult
/**
 * @def VK_CHECK(expr)
 * @brief Macro for VK CHECK.
 */
#define VK_CHECK(expr) \
  do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
      BOOST_LOG(error) << #expr << " failed: " << _r; \
      return -1; \
    } \
  } while (0)
/**
 * @def VK_CHECK_BOOL(expr)
 * @brief Macro for VK CHECK BOOL.
 */
#define VK_CHECK_BOOL(expr) \
  do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
      BOOST_LOG(error) << #expr << " failed: " << _r; \
      return false; \
    } \
  } while (0)

  /**
   * @brief Vulkan encode device that keeps converted frames in GPU memory.
   */
  class vk_vram_t: public platf::avcodec_encode_device_t {
  public:
    ~vk_vram_t() override {
      cleanup_pipeline();
    }

    /**
     * @brief Initialize Vulkan encode device and conversion resources.
     *
     * @param in_width In width.
     * @param in_height In height.
     * @param in_offset_x In offset x.
     * @param in_offset_y In offset y.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(int in_width, int in_height, int in_offset_x = 0, int in_offset_y = 0) {
      width = in_width;
      height = in_height;
      offset_x = in_offset_x;
      offset_y = in_offset_y;
      hardware = true;
      avcodec_encode_device_t::init_hw_device = &init_hw_device;
      return 0;
    }

    /**
     * @brief Configure rate control and validated Vulkan encoder quality options.
     * @note Failed quality queries leave the encoder's default quality unchanged.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param options Request options or socket options to apply.
     */
    void init_codec_options(AVCodecContext *ctx, AVDictionary **options) override {
      // When VBR mode is selected (rc_mode=4), don't pin rc_min_rate to the target bitrate.
      // Having rc_min_rate == rc_max_rate == bit_rate in VBR mode prevents the encoder from
      // undershooting on simple frames, which builds up headroom that causes large overshoots
      // on complex frames.
      if (config::video.vk.rc_mode == 4) {
        ctx->rc_min_rate = 0;
      }

      // Map quality preset to driver's quality range (same abstraction as VAAPI)
      // 1 = speed, 2 = balanced (default), 3 = quality
      // Note: Vulkan quality is 0 = fastest, higher = slower/better quality
      int quality_preset = config::video.vk.quality;
      if (quality_preset > 0 && ctx->hw_frames_ctx) {
        // Reuse FFmpeg's Vulkan instance and device instead of creating new ones
        auto *frames_ctx = (AVHWFramesContext *) ctx->hw_frames_ctx->data;
        auto *dev_ctx = (AVHWDeviceContext *) frames_ctx->device_ref->data;
        auto *vk_ctx = (AVVulkanDeviceContext *) dev_ctx->hwctx;

        // Calculate target quality based on preset
        uint32_t max_quality = 0;
        uint32_t target_quality = 0;
        const char *preset_name = "speed";

        // First pass: get max quality to calculate target
        if (query_and_validate_quality_level(ctx->codec_id, vk_ctx, 0, &max_quality).has_value()) {
          switch (quality_preset) {
            default:
            case 1:  // speed (level 0 = fastest)
              target_quality = 0;
              preset_name = "speed";
              break;
            case 2:  // balanced (middle level)
              target_quality = max_quality / 2;
              preset_name = "balanced";
              break;
            case 3:  // quality (max level = best quality)
              target_quality = max_quality;
              preset_name = "quality";
              break;
          }

          // Validate the calculated target quality level
          auto validated_quality = query_and_validate_quality_level(ctx->codec_id, vk_ctx, target_quality, &max_quality);

          if (validated_quality && *validated_quality == target_quality) {
            av_dict_set_int(options, "quality", *validated_quality, 0);
            BOOST_LOG(info) << "[vulkan] Encoder quality set to "sv << *validated_quality
                            << " ("sv << preset_name << "), driver range: 0-"sv << max_quality;
          } else {
            BOOST_LOG(warning) << "[vulkan] Quality level "sv << target_quality
                               << " is not supported by the driver; using default"sv;
          }
        }
      }
    }

    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     *
     * @param new_frame Frame to attach.
     * @param hw_frames_ctx_buf Hardware frames context buffer.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(new_frame);
      this->frame = new_frame;
      this->hw_frames_ctx = hw_frames_ctx_buf;

      auto *frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;
      auto *dev_ctx = (AVHWDeviceContext *) frames_ctx->device_ref->data;
      vk_dev.ctx = (AVVulkanDeviceContext *) dev_ctx->hwctx;
      vk_dev.dev = vk_dev.ctx->act_dev;
      vk_dev.phys_dev = vk_dev.ctx->phys_dev;
      is_10bit = (frames_ctx->sw_format == AV_PIX_FMT_P010);

      {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(vk_dev.phys_dev, &p);
        BOOST_LOG(info) << "Vulkan encode using GPU: " << p.deviceName;
      }

      // Find a compute-capable queue family from FFmpeg's context
      vk_dev.compute_qf = -1;
      for (int i = 0; i < vk_dev.ctx->nb_qf; i++) {
        if (vk_dev.ctx->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
          vk_dev.compute_qf = vk_dev.ctx->qf[i].idx;
          break;
        }
      }
      if (vk_dev.compute_qf < 0) {
        BOOST_LOG(error) << "No compute queue family in Vulkan device"sv;
        return -1;
      }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100)
      // FFmpeg 9.0+ may create its queues with VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR
      // (when VK_KHR_internally_synchronized_queues is available). The Vulkan spec requires
      // vkGetDeviceQueue2 to retrieve such queues; plain vkGetDeviceQueue would return an
      // incompatible queue handle, breaking synchronization with the encoder.
      VkDeviceQueueInfo2 queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
      queue_info.flags = vk_dev.ctx->queue_flags;
      queue_info.queueFamilyIndex = vk_dev.compute_qf;
      queue_info.queueIndex = 0;
      vkGetDeviceQueue2(vk_dev.dev, &queue_info, &vk_dev.compute_queue);
#else
      vkGetDeviceQueue(vk_dev.dev, vk_dev.compute_qf, 0, &vk_dev.compute_queue);
#endif

      // Load extension functions
      vk_dev.getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(vk_dev.dev, "vkGetMemoryFdPropertiesKHR");

      if (!create_compute_pipeline()) {
        return -1;
      }
      if (!create_command_resources()) {
        return -1;
      }

      return 0;
    }

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     */
    void apply_colorspace() override {
      auto *colors = video::color_vectors_from_colorspace(colorspace, true);
      if (colors) {
        push.color_vec_y = colors->color_vec_y;
        push.color_vec_u = colors->color_vec_u;
        push.color_vec_v = colors->color_vec_v;
        push.range_y = colors->range_y;
        push.range_uv = colors->range_uv;
      }
    }

    /**
     * @brief Configure FFmpeg Vulkan hardware frames for video encode input.
     * @note FFmpeg adds video encode usage only when the device and format support it.
     *
     * @param frames FFmpeg hardware frames context to initialize.
     */
    void init_hwframes(AVHWFramesContext *frames) override {
      frames->initial_pool_size = 4;
      auto *vk_frames = (AVVulkanFramesContext *) frames->hwctx;
      vk_frames->tiling = VK_IMAGE_TILING_OPTIMAL;
      vk_frames->usage = (VkImageUsageFlagBits) (VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    /**
     * @brief Convert a captured frame into a Vulkan hardware frame.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      auto &descriptor = (egl::img_descriptor_t &) img;

      // Get encoder target frame
      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0) < 0) {
          BOOST_LOG(error) << "Failed to get Vulkan frame"sv;
          return -1;
        }
      }

      // Import new DMA-BUF as VkImage when capture sequence changes
      if (descriptor.sequence == 0) {
        // Dummy frame — clear the target
        return 0;
      }

      if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;
        if (!import_dmabuf(descriptor.sd)) {
          BOOST_LOG(error) << "Failed to import DMA-BUF"sv;
          return -1;
        }
        descriptors_dirty = true;
      }

      if (src.image == VK_NULL_HANDLE) {
        return -1;
      }

      // Setup Y/UV image views for the encoder target (once)
      if (!target.views_created) {
        if (!create_target_views()) {
          return -1;
        }
        target.views_created = true;
        descriptors_dirty = true;
      }

      // Update descriptor set only when source or target changed
      if (descriptors_dirty) {
        update_descriptors();
        descriptors_dirty = false;
      }

      if (descriptor.data && descriptor.serial != cursor_serial) {
        cursor_serial = descriptor.serial;
        if (!create_cursor_image(descriptor.src_w, descriptor.src_h, descriptor.data)) {
          return -1;
        }
        update_descriptors();
        descriptors_dirty = false;
      }

      // Preserve aspect ratio: fit src into dst, center with black bars.
      // UV plane is subsampled 2x, so keep effective size and offset even.
      float scalar = std::min((float) frame->width / width, (float) frame->height / height);
      int32_t eff_w = std::min<int32_t>(((int32_t) (width * scalar)) & ~1, frame->width & ~1);
      int32_t eff_h = std::min<int32_t>(((int32_t) (height * scalar)) & ~1, frame->height & ~1);
      int32_t dst_off_x = ((frame->width - eff_w) / 2) & ~1;
      int32_t dst_off_y = ((frame->height - eff_h) / 2) & ~1;
      eff_w = std::min(eff_w, (frame->width - dst_off_x) & ~1);
      eff_h = std::min(eff_h, (frame->height - dst_off_y) & ~1);

      // Fill push constants
      push.src_offset[0] = offset_x;
      push.src_offset[1] = offset_y;
      push.src_size[0] = width;
      push.src_size[1] = height;
      push.dst_offset[0] = dst_off_x;
      push.dst_offset[1] = dst_off_y;
      push.dst_size[0] = eff_w;
      push.dst_size[1] = eff_h;
      push.dst_full_size[0] = frame->width;
      push.dst_full_size[1] = frame->height;
      push.y_invert = descriptor.y_invert ? 1 : 0;

      if (descriptor.data) {
        float scale_x = (float) eff_w / width;
        float scale_y = (float) eff_h / height;
        push.cursor_pos[0] = (int32_t) ((descriptor.x - offset_x) * scale_x) + dst_off_x;
        push.cursor_pos[1] = (int32_t) ((descriptor.y - offset_y) * scale_y) + dst_off_y;
        push.cursor_size[0] = (int32_t) (descriptor.width * scale_x);
        push.cursor_size[1] = (int32_t) (descriptor.height * scale_y);
      } else {
        push.cursor_size[0] = 0;
      }

      // Record and submit compute dispatch
      return dispatch_compute();
    }

  private:
    bool create_compute_pipeline() {
      // Shader module
      VkShaderModuleCreateInfo shader_ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shader_ci.codeSize = rgb2yuv_comp_spv_size;
      shader_ci.pCode = rgb2yuv_comp_spv_data.data();
      VK_CHECK_BOOL(vkCreateShaderModule(vk_dev.dev, &shader_ci, nullptr, &compute.shader_module));

      // Descriptor set layout: binding 0=sampler, 1=Y storage, 2=UV storage, 3=cursor sampler
      std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};
      bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

      VkDescriptorSetLayoutCreateInfo ds_layout_ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      ds_layout_ci.bindingCount = bindings.size();
      ds_layout_ci.pBindings = bindings.data();
      VK_CHECK_BOOL(vkCreateDescriptorSetLayout(vk_dev.dev, &ds_layout_ci, nullptr, &compute.ds_layout));

      // Push constant range
      VkPushConstantRange pc_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};

      VkPipelineLayoutCreateInfo pl_ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl_ci.setLayoutCount = 1;
      pl_ci.pSetLayouts = &compute.ds_layout;
      pl_ci.pushConstantRangeCount = 1;
      pl_ci.pPushConstantRanges = &pc_range;
      VK_CHECK_BOOL(vkCreatePipelineLayout(vk_dev.dev, &pl_ci, nullptr, &compute.pipeline_layout));

      // Compute pipeline
      VkComputePipelineCreateInfo comp_ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      comp_ci.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      comp_ci.stage.module = compute.shader_module;
      comp_ci.stage.pName = "main";
      comp_ci.layout = compute.pipeline_layout;
      VK_CHECK_BOOL(vkCreateComputePipelines(vk_dev.dev, VK_NULL_HANDLE, 1, &comp_ci, nullptr, &compute.pipeline));

      // Descriptor pool
      std::array<VkDescriptorPoolSize, 2> pool_sizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
      }};
      VkDescriptorPoolCreateInfo pool_ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool_ci.maxSets = 1;
      pool_ci.poolSizeCount = pool_sizes.size();
      pool_ci.pPoolSizes = pool_sizes.data();
      VK_CHECK_BOOL(vkCreateDescriptorPool(vk_dev.dev, &pool_ci, nullptr, &compute.desc_pool));

      VkDescriptorSetAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc_info.descriptorPool = compute.desc_pool;
      alloc_info.descriptorSetCount = 1;
      alloc_info.pSetLayouts = &compute.ds_layout;
      VK_CHECK_BOOL(vkAllocateDescriptorSets(vk_dev.dev, &alloc_info, &compute.desc_set));

      // Sampler for source image
      VkSamplerCreateInfo sampler_ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sampler_ci.magFilter = VK_FILTER_LINEAR;
      sampler_ci.minFilter = VK_FILTER_LINEAR;
      sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      VK_CHECK_BOOL(vkCreateSampler(vk_dev.dev, &sampler_ci, nullptr, &compute.sampler));

      if (!create_cursor_image(1, 1, nullptr)) {
        return false;
      }

      return true;
    }

    bool create_command_resources() {
      VkCommandPoolCreateInfo pool_ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_ci.queueFamilyIndex = vk_dev.compute_qf;
      pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      VK_CHECK_BOOL(vkCreateCommandPool(vk_dev.dev, &pool_ci, nullptr, &cmd.pool));

      VkCommandBufferAllocateInfo alloc_ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc_ci.commandPool = cmd.pool;
      alloc_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc_ci.commandBufferCount = CMD_RING_SIZE;
      VK_CHECK_BOOL(vkAllocateCommandBuffers(vk_dev.dev, &alloc_ci, cmd.ring.data()));

      return true;
    }

    struct drm_format_info {
      VkFormat format;
      VkComponentMapping swizzle;
    };

    static drm_format_info drm_fourcc_to_vk_format(uint32_t fourcc) {
      static constexpr VkComponentMapping identity = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
      };
      static constexpr VkComponentMapping bgr_swap = {
        VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_A,
      };

      switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
          return {VK_FORMAT_B8G8R8A8_UNORM, identity};
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
          return {VK_FORMAT_R8G8B8A8_UNORM, identity};
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
          return {VK_FORMAT_A2R10G10B10_UNORM_PACK32, identity};
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
          return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, identity};
        case DRM_FORMAT_XBGR16161616:
        case DRM_FORMAT_ABGR16161616:
          return {VK_FORMAT_R16G16B16A16_UNORM, identity};
        case DRM_FORMAT_XRGB16161616:
        case DRM_FORMAT_ARGB16161616:
          return {VK_FORMAT_R16G16B16A16_UNORM, bgr_swap};
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_ABGR16161616F:
          return {VK_FORMAT_R16G16B16A16_SFLOAT, identity};
        case DRM_FORMAT_XRGB16161616F:
        case DRM_FORMAT_ARGB16161616F:
          return {VK_FORMAT_R16G16B16A16_SFLOAT, bgr_swap};
        default:
          BOOST_LOG(warning) << "Unknown DRM fourcc 0x" << std::hex << fourcc << std::dec << ", assuming B8G8R8A8";
          return {VK_FORMAT_B8G8R8A8_UNORM, identity};
      }
    }

    /**
     * @brief Query the driver-expected plane count for a format+modifier pair.
     * @return Expected plane count, or 0 if unknown.
     */
    int query_modifier_plane_count(VkFormat format, uint64_t modifier) {
      VkDrmFormatModifierPropertiesListEXT mod_list = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 fmt_props2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      fmt_props2.pNext = &mod_list;
      vkGetPhysicalDeviceFormatProperties2(vk_dev.phys_dev, format, &fmt_props2);
      std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(mod_list.drmFormatModifierCount);
      mod_list.pDrmFormatModifierProperties = mod_props.data();
      vkGetPhysicalDeviceFormatProperties2(vk_dev.phys_dev, format, &fmt_props2);
      for (const auto &mp : mod_props) {
        if (mp.drmFormatModifier == modifier) {
          return mp.drmFormatModifierPlaneCount;
        }
      }
      return 0;
    }

    bool import_dmabuf(const egl::surface_descriptor_t &sd) {
      destroy_src_image();

      int fd = dup(sd.fds[0]);
      if (fd < 0) {
        return false;
      }

      // Query memory requirements for this DMA-BUF
      VkMemoryFdPropertiesKHR fd_props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
      if (vk_dev.getMemoryFdProperties) {
        vk_dev.getMemoryFdProperties(vk_dev.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props);
      }

      // Create VkImage for the DMA-BUF
      VkExternalMemoryImageCreateInfo ext_ci = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      ext_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

      std::array<VkSubresourceLayout, 4> drm_layouts = {};
      VkImageDrmFormatModifierExplicitCreateInfoEXT drm_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      };
      VkImageTiling tiling;

      auto [vk_format, vk_swizzle] = drm_fourcc_to_vk_format(sd.fourcc);

      if (sd.modifier != DRM_FORMAT_MOD_INVALID) {
        int dmabuf_planes = 0;
        for (int i = 0; i < 4 && sd.fds[i] >= 0; ++i) {
          dmabuf_planes++;
        }

        // Query driver for the expected plane count for this format+modifier.
        // DMA-BUF exports may include extra metadata planes (e.g. AMD DCC).
        int expected = query_modifier_plane_count(vk_format, sd.modifier);
        int plane_count = (expected > 0 && expected <= dmabuf_planes) ? expected : dmabuf_planes;

        for (int i = 0; i < plane_count; ++i) {
          drm_layouts[i].offset = sd.offsets[i];
          drm_layouts[i].rowPitch = sd.pitches[i];
        }
        drm_ci.drmFormatModifier = sd.modifier;
        drm_ci.drmFormatModifierPlaneCount = plane_count;
        drm_ci.pPlaneLayouts = drm_layouts.data();
        ext_ci.pNext = &drm_ci;
        tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      } else {
        tiling = VK_IMAGE_TILING_LINEAR;
      }

      VkImageCreateInfo img_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      img_ci.pNext = &ext_ci;
      img_ci.imageType = VK_IMAGE_TYPE_2D;
      img_ci.format = vk_format;
      img_ci.extent = {(uint32_t) sd.width, (uint32_t) sd.height, 1};
      img_ci.mipLevels = 1;
      img_ci.arrayLayers = 1;
      img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
      img_ci.tiling = tiling;
      img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      auto res = vkCreateImage(vk_dev.dev, &img_ci, nullptr, &src.image);
      if (res != VK_SUCCESS) {
        close(fd);
        BOOST_LOG(error) << "vkCreateImage for DMA-BUF failed: " << res
                         << " (modifier=0x" << std::hex << sd.modifier << std::dec
                         << ", pitch=" << sd.pitches[0] << ", offset=" << sd.offsets[0] << ")";
        return false;
      }

      // Bind imported DMA-BUF memory
      VkMemoryRequirements mem_req;
      vkGetImageMemoryRequirements(vk_dev.dev, src.image, &mem_req);

      VkImportMemoryFdInfoKHR import_fd = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      import_fd.fd = fd;  // Vulkan takes ownership

      VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc_info.pNext = &import_fd;
      alloc_info.allocationSize = mem_req.size;
      alloc_info.memoryTypeIndex = find_memory_type(
        fd_props.memoryTypeBits ? fd_props.memoryTypeBits : mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      );

      VkDeviceMemory src_mem = VK_NULL_HANDLE;
      res = vkAllocateMemory(vk_dev.dev, &alloc_info, nullptr, &src_mem);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkAllocateMemory for DMA-BUF failed: " << res;
        vkDestroyImage(vk_dev.dev, src.image, nullptr);
        src.image = VK_NULL_HANDLE;
        return false;
      }

      vkBindImageMemory(vk_dev.dev, src.image, src_mem, 0);

      // Create image view
      VkImageViewCreateInfo view_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_ci.image = src.image;
      view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_ci.format = vk_format;
      view_ci.components = vk_swizzle;
      view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &src.view));

      src.mem = src_mem;
      return true;
    }

    bool create_cursor_image(int w, int h, const uint8_t *pixels) {
      destroy_cursor_image();

      VkImageCreateInfo img_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      img_ci.imageType = VK_IMAGE_TYPE_2D;
      img_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
      img_ci.extent = {(uint32_t) w, (uint32_t) h, 1};
      img_ci.mipLevels = 1;
      img_ci.arrayLayers = 1;
      img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
      img_ci.tiling = VK_IMAGE_TILING_LINEAR;
      img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      img_ci.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
      VK_CHECK_BOOL(vkCreateImage(vk_dev.dev, &img_ci, nullptr, &cursor.image));

      VkMemoryRequirements mem_req;
      vkGetImageMemoryRequirements(vk_dev.dev, cursor.image, &mem_req);
      VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc.allocationSize = mem_req.size;
      alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VK_CHECK_BOOL(vkAllocateMemory(vk_dev.dev, &alloc, nullptr, &cursor.mem));
      VK_CHECK_BOOL(vkBindImageMemory(vk_dev.dev, cursor.image, cursor.mem, 0));

      if (pixels) {
        void *mapped;
        VK_CHECK_BOOL(vkMapMemory(vk_dev.dev, cursor.mem, 0, VK_WHOLE_SIZE, 0, &mapped));
        VkImageSubresource subres = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(vk_dev.dev, cursor.image, &subres, &layout);
        for (int y = 0; y < h; y++) {
          memcpy((uint8_t *) mapped + layout.offset + y * layout.rowPitch, pixels + y * w * 4, w * 4);
        }
        vkUnmapMemory(vk_dev.dev, cursor.mem);
      }

      VkImageViewCreateInfo view_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_ci.image = cursor.image;
      view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
      view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &cursor.view));

      cursor.needs_transition = true;
      descriptors_dirty = true;
      return true;
    }

    void destroy_cursor_image() {
      if (cursor.view) {
        vkDestroyImageView(vk_dev.dev, cursor.view, nullptr);
        cursor.view = VK_NULL_HANDLE;
      }
      if (cursor.image) {
        vkDestroyImage(vk_dev.dev, cursor.image, nullptr);
        cursor.image = VK_NULL_HANDLE;
      }
      if (cursor.mem) {
        vkFreeMemory(vk_dev.dev, cursor.mem, nullptr);
        cursor.mem = VK_NULL_HANDLE;
      }
    }

    bool create_target_views() {
      auto *vk_frame = (AVVkFrame *) frame->data[0];
      if (!vk_frame) {
        return false;
      }

      auto y_fmt = is_10bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
      auto uv_fmt = is_10bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;

      // Detect multiplane vs multi-image layout
      int num_imgs = 0;
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        num_imgs++;
      }

      if (num_imgs == 1) {
        // Single multiplane image — create plane views
        VkImageViewCreateInfo view_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image = vk_frame->img[0];
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;

        // Y plane
        view_ci.format = y_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 1, 0, 1};
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.y_view));

        // UV plane
        view_ci.format = uv_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.uv_view));
      } else {
        // Separate images per plane
        VkImageViewCreateInfo view_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        view_ci.image = vk_frame->img[0];
        view_ci.format = y_fmt;
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.y_view));

        view_ci.image = vk_frame->img[1];
        view_ci.format = uv_fmt;
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.uv_view));
      }
      return true;
    }

    void update_descriptors() {
      VkDescriptorImageInfo src_info = {compute.sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo y_info = {VK_NULL_HANDLE, target.y_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo uv_info = {VK_NULL_HANDLE, target.uv_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo cursor_info = {compute.sampler, cursor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

      std::array<VkWriteDescriptorSet, 4> writes = {};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_info, nullptr, nullptr};
      writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &y_info, nullptr, nullptr};
      writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &uv_info, nullptr, nullptr};
      writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_set, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cursor_info, nullptr, nullptr};
      vkUpdateDescriptorSets(vk_dev.dev, writes.size(), writes.data(), 0, nullptr);
    }

    int dispatch_compute() {
      auto *vk_frame = (AVVkFrame *) frame->data[0];
      int num_imgs = 0;
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        num_imgs++;
      }

      // Rotate to next command buffer. With CMD_RING_SIZE slots, the buffer
      // we're about to reuse was submitted CMD_RING_SIZE frames ago.
      // At 60fps that's ~50ms for a <1ms compute dispatch — always complete.
      // No fences, no semaphore waits, no CPU blocking.
      auto cmd_buf = cmd.ring[cmd.ring_idx];
      cmd.ring_idx = (cmd.ring_idx + 1) % CMD_RING_SIZE;

      VkCommandBufferBeginInfo begin_ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin_ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      VK_CHECK(vkBeginCommandBuffer(cmd_buf, &begin_ci));

      // Transition source image to SHADER_READ_ONLY
      VkImageMemoryBarrier src_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      src_barrier.srcAccessMask = 0;
      src_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      src_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      src_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      src_barrier.image = src.image;
      src_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      src_barrier.dstQueueFamilyIndex = vk_dev.compute_qf;

      vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &src_barrier);

      // Transition cursor image if needed
      if (cursor.needs_transition) {
        VkImageMemoryBarrier cursor_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        cursor_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        cursor_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        cursor_barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        cursor_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cursor_barrier.image = cursor.image;
        cursor_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cursor_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cursor_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cursor_barrier);
        cursor.needs_transition = false;
      }

      // Transition target planes to GENERAL for storage writes
      std::array<VkImageMemoryBarrier, 2> dst_barriers = {};
      int num_dst_barriers = (num_imgs == 1) ? 1 : 2;
      for (int i = 0; i < num_dst_barriers; i++) {
        dst_barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        dst_barriers[i].srcAccessMask = target.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        dst_barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        dst_barriers[i].oldLayout = target.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        dst_barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst_barriers[i].image = vk_frame->img[num_imgs == 1 ? 0 : i];
        dst_barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dst_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dst_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      }

      vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, num_dst_barriers, dst_barriers.data());

      // Bind pipeline and dispatch
      vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline);
      vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline_layout, 0, 1, &compute.desc_set, 0, nullptr);
      vkCmdPushConstants(cmd_buf, compute.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &push);

      uint32_t gx = (frame->width + 15) / 16;
      uint32_t gy = (frame->height + 15) / 16;
      vkCmdDispatch(cmd_buf, gx, gy, 1);

      VK_CHECK(vkEndCommandBuffer(cmd_buf));

      // Submit with timeline semaphore signaling for FFmpeg
      VkTimelineSemaphoreSubmitInfo timeline_info = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      std::array<VkSemaphore, AV_NUM_DATA_POINTERS> wait_sems = {};
      std::array<VkSemaphore, AV_NUM_DATA_POINTERS> signal_sems = {};
      std::array<uint64_t, AV_NUM_DATA_POINTERS> wait_vals = {};
      std::array<uint64_t, AV_NUM_DATA_POINTERS> signal_vals = {};
      std::array<VkPipelineStageFlags, AV_NUM_DATA_POINTERS> wait_stages = {};
      int sem_count = 0;

      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->sem[i]; i++) {
        wait_sems[sem_count] = vk_frame->sem[i];
        wait_vals[sem_count] = vk_frame->sem_value[i];
        wait_stages[sem_count] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        signal_sems[sem_count] = vk_frame->sem[i];
        signal_vals[sem_count] = vk_frame->sem_value[i] + 1;
        vk_frame->sem_value[i]++;
        sem_count++;
      }

      timeline_info.waitSemaphoreValueCount = sem_count;
      timeline_info.pWaitSemaphoreValues = wait_vals.data();
      timeline_info.signalSemaphoreValueCount = sem_count;
      timeline_info.pSignalSemaphoreValues = signal_vals.data();

      VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.pNext = &timeline_info;
      submit.waitSemaphoreCount = sem_count;
      submit.pWaitSemaphores = wait_sems.data();
      submit.pWaitDstStageMask = wait_stages.data();
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd_buf;
      submit.signalSemaphoreCount = sem_count;
      submit.pSignalSemaphores = signal_sems.data();

      auto res = vkQueueSubmit(vk_dev.compute_queue, 1, &submit, VK_NULL_HANDLE);

      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkQueueSubmit failed: " << res;
        return -1;
      }

      // Update frame layouts for FFmpeg
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        vk_frame->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
        vk_frame->access[i] = VK_ACCESS_SHADER_WRITE_BIT;
        vk_frame->queue_family[i] = vk_dev.compute_qf;
      }

      target.initialized = true;

      return 0;
    }

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) {
      VkPhysicalDeviceMemoryProperties mem_props;
      vkGetPhysicalDeviceMemoryProperties(vk_dev.phys_dev, &mem_props);
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
          return i;
        }
      }
      // Fallback: any matching type bit
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if (type_bits & (1 << i)) {
          return i;
        }
      }
      return 0;
    }

    void destroy_src_image() {
      if (src.image) {
        // Defer destruction — the GPU may still be using this image.
        // By the time we wrap around (4 frames later), it's guaranteed done.
        auto &slot = defer_ring[defer_idx];
        if (slot.view) {
          vkDestroyImageView(vk_dev.dev, slot.view, nullptr);
        }
        if (slot.image) {
          vkDestroyImage(vk_dev.dev, slot.image, nullptr);
        }
        if (slot.mem) {
          vkFreeMemory(vk_dev.dev, slot.mem, nullptr);
        }
        slot = src;
        defer_idx = (defer_idx + 1) % DEFER_RING_SIZE;
      }
      src = {};
    }

    void cleanup_pipeline() {
      if (!vk_dev.dev) {
        return;
      }
      vkDeviceWaitIdle(vk_dev.dev);
      destroy_src_image();
      // Flush deferred destroys
      for (auto &slot : defer_ring) {
        if (slot.view) {
          vkDestroyImageView(vk_dev.dev, slot.view, nullptr);
        }
        if (slot.image) {
          vkDestroyImage(vk_dev.dev, slot.image, nullptr);
        }
        if (slot.mem) {
          vkFreeMemory(vk_dev.dev, slot.mem, nullptr);
        }
        slot = {};
      }
      if (target.y_view) {
        vkDestroyImageView(vk_dev.dev, target.y_view, nullptr);
      }
      if (target.uv_view) {
        vkDestroyImageView(vk_dev.dev, target.uv_view, nullptr);
      }
      destroy_cursor_image();
      if (cmd.pool) {
        vkDestroyCommandPool(vk_dev.dev, cmd.pool, nullptr);
      }
      if (compute.sampler) {
        vkDestroySampler(vk_dev.dev, compute.sampler, nullptr);
      }
      if (compute.desc_pool) {
        vkDestroyDescriptorPool(vk_dev.dev, compute.desc_pool, nullptr);
      }
      if (compute.pipeline) {
        vkDestroyPipeline(vk_dev.dev, compute.pipeline, nullptr);
      }
      if (compute.pipeline_layout) {
        vkDestroyPipelineLayout(vk_dev.dev, compute.pipeline_layout, nullptr);
      }
      if (compute.ds_layout) {
        vkDestroyDescriptorSetLayout(vk_dev.dev, compute.ds_layout, nullptr);
      }
      if (compute.shader_module) {
        vkDestroyShaderModule(vk_dev.dev, compute.shader_module, nullptr);
      }
    }

    static int init_hw_device(platf::avcodec_encode_device_t *, AVBufferRef **hw_device_buf) {
      return create_vulkan_hwdevice(hw_device_buf);
    }

    // Dimensions
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    bool is_10bit = false;
    AVBufferRef *hw_frames_ctx = nullptr;
    frame_t hwframe;
    std::uint64_t sequence = 0;

    // Vulkan device (from FFmpeg)
    struct vk_device_t {
      VkDevice dev = VK_NULL_HANDLE;
      VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
      AVVulkanDeviceContext *ctx = nullptr;
      int compute_qf = -1;
      VkQueue compute_queue = VK_NULL_HANDLE;
      PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
    };

    vk_device_t vk_dev = {};

    // Compute pipeline
    struct compute_pipeline_t {
      VkShaderModule shader_module = VK_NULL_HANDLE;
      VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
      VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;
      VkDescriptorPool desc_pool = VK_NULL_HANDLE;
      VkDescriptorSet desc_set = VK_NULL_HANDLE;
      VkSampler sampler = VK_NULL_HANDLE;
    };

    compute_pipeline_t compute = {};

    // Command submission — ring of buffers to avoid reuse while in-flight.
    // No CPU waits: by the time we wrap around, the old submission is long done.
    static constexpr int CMD_RING_SIZE = 3;

    struct cmd_submission_t {
      VkCommandPool pool = VK_NULL_HANDLE;
      std::array<VkCommandBuffer, CMD_RING_SIZE> ring = {};
      int ring_idx = 0;
    };

    cmd_submission_t cmd = {};

    // Source DMA-BUF image with deferred destruction
    struct src_image_t {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
    };

    src_image_t src = {};
    static constexpr int DEFER_RING_SIZE = 4;
    std::array<src_image_t, DEFER_RING_SIZE> defer_ring = {};
    int defer_idx = 0;

    // Target NV12 plane views
    struct target_state_t {
      VkImageView y_view = VK_NULL_HANDLE;
      VkImageView uv_view = VK_NULL_HANDLE;
      bool views_created = false;
      bool initialized = false;
    };

    target_state_t target = {};

    bool descriptors_dirty = false;

    // Cursor image
    struct {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
      bool needs_transition = false;
    } cursor = {};

    unsigned long cursor_serial = 0;

    // Push constants (color matrix)
    PushConstants push = {};
  };

  // Free functions

  int vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *, AVBufferRef **hw_device_buf) {
    return create_vulkan_hwdevice(hw_device_buf);
  }

  bool validate() {
    if (!avcodec_find_encoder_by_name("h264_vulkan") && !avcodec_find_encoder_by_name("hevc_vulkan")) {
      return false;
    }
    AVBufferRef *dev = nullptr;
    if (create_vulkan_hwdevice(&dev) < 0) {
      return false;
    }
    av_buffer_unref(&dev);
    return true;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_vram(int w, int h, int offset_x, int offset_y) {
    auto dev = std::make_unique<vk_vram_t>();
    if (dev->init(w, h, offset_x, offset_y) < 0) {
      return nullptr;
    }
    return dev;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_ram(int, int) {
    return nullptr;
  }

}  // namespace vk
