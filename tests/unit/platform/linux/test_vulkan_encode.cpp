/**
 * @file tests/unit/platform/linux/test_vulkan_encode.cpp
 * @brief Tests for Vulkan encoder frame configuration and quality queries.
 */

#ifdef SUNSHINE_BUILD_VULKAN

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <array>
  #include <bit>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <vector>

// lib includes
extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavutil/hwcontext.h>
  #include <libavutil/hwcontext_vulkan.h>
}

  // local includes
  #include "src/platform/linux/vulkan_encode.h"

namespace {
  /**
   * @brief Codec-specific structures required by the Vulkan quality query API.
   */
  struct quality_codec_t {
    AVCodecID codec;  ///< FFmpeg codec identifier.
    VkVideoCodecOperationFlagBitsKHR operation;  ///< Vulkan encode operation.
    VkStructureType profile;  ///< Required codec-specific profile structure type.
    VkStructureType capabilities;  ///< Required codec-specific capability structure type.
    VkStructureType quality;  ///< Required codec-specific quality structure type.
  };

  constexpr std::array quality_codecs {
    quality_codec_t {AV_CODEC_ID_H264, VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR},
    quality_codec_t {AV_CODEC_ID_HEVC, VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR},
    quality_codec_t {AV_CODEC_ID_AV1, VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR, VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUALITY_LEVEL_PROPERTIES_KHR},
  };  ///< Supported codecs and their Vulkan API requirements.

  /**
   * @brief Fake Vulkan driver that checks query chains without requiring a GPU.
   */
  struct quality_driver_t {
    quality_codec_t codec;  ///< Expected codec and required structures.
    std::vector<uint32_t> quality_counts {4};  ///< Quality counts returned by successive capability queries.
    std::vector<uint32_t> queried_levels;  ///< Quality indices submitted to the fake driver.
    size_t capability_calls = 0;  ///< Number of capability queries received.
    size_t fail_quality_call = 0;  ///< Quality query to fail, or zero for no failure.
    VkResult capability_result = VK_SUCCESS;  ///< Result returned by capability queries.
    bool capabilities_available = true;  ///< Whether the capabilities entry point is available.
    bool quality_available = true;  ///< Whether the quality entry point is available.
    bool valid_chains = true;  ///< Whether all query structures satisfy the Vulkan contract.

    /**
     * @brief Resolve fake driver entry points from the instance's test state.
     * @param instance Fake instance carrying the driver state.
     * @param name Requested Vulkan function name.
     * @return Fake function pointer, or nullptr when unavailable.
     */
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc_addr(VkInstance instance, const char *name) {
      auto &driver = *std::bit_cast<quality_driver_t *>(instance);
      if (std::string_view(name) == "vkGetPhysicalDeviceVideoCapabilitiesKHR") {
        return driver.capabilities_available ? std::bit_cast<PFN_vkVoidFunction>(&get_capabilities) : nullptr;
      }
      return driver.quality_available ? std::bit_cast<PFN_vkVoidFunction>(&get_quality) : nullptr;
    }

    /**
     * @brief Check codec-specific input and capability chains and return a quality count.
     * @param physical_device Fake physical device carrying the driver state.
     * @param profile Profile whose structures are checked.
     * @param capabilities Capability output chain to populate.
     * @return Configured driver result.
     */
    static VKAPI_ATTR VkResult VKAPI_CALL get_capabilities(VkPhysicalDevice physical_device, const VkVideoProfileInfoKHR *profile, VkVideoCapabilitiesKHR *capabilities) {
      auto &driver = *std::bit_cast<quality_driver_t *>(physical_device);
      auto *usage = static_cast<const VkVideoEncodeUsageInfoKHR *>(profile->pNext);
      auto *codec_profile = static_cast<const VkBaseInStructure *>(usage->pNext);
      auto *encode_caps = static_cast<VkVideoEncodeCapabilitiesKHR *>(capabilities->pNext);
      auto *codec_caps = static_cast<VkBaseOutStructure *>(encode_caps->pNext);
      driver.valid_chains &= profile->sType == VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR &&
                             profile->videoCodecOperation == driver.codec.operation &&
                             usage->sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR &&
                             codec_profile->sType == driver.codec.profile &&
                             capabilities->sType == VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR &&
                             encode_caps->sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR &&
                             codec_caps->sType == driver.codec.capabilities;
      encode_caps->maxQualityLevels = driver.quality_counts[std::min(driver.capability_calls, driver.quality_counts.size() - 1)];
      ++driver.capability_calls;
      return driver.capability_result;
    }

    /**
     * @brief Require codec-specific output storage and a valid zero-based quality index.
     * @param physical_device Fake physical device carrying the driver state.
     * @param quality_info Profile and quality index to validate.
     * @param properties Quality output chain to validate and populate.
     * @return Success, or an error for invalid structures or an injected failure.
     */
    static VKAPI_ATTR VkResult VKAPI_CALL get_quality(VkPhysicalDevice physical_device, const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR *quality_info, VkVideoEncodeQualityLevelPropertiesKHR *properties) {
      auto &driver = *std::bit_cast<quality_driver_t *>(physical_device);
      driver.queried_levels.push_back(quality_info->qualityLevel);
      const auto *codec_quality = static_cast<const VkBaseOutStructure *>(properties->pNext);
      const auto count = driver.quality_counts[std::min(driver.capability_calls - 1, driver.quality_counts.size() - 1)];
      driver.valid_chains &= quality_info->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR &&
                             quality_info->pVideoProfile->videoCodecOperation == driver.codec.operation &&
                             properties->sType == VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR &&
                             codec_quality && codec_quality->sType == driver.codec.quality &&
                             quality_info->qualityLevel < count;
      if (!driver.valid_chains || driver.queried_levels.size() == driver.fail_quality_call) {
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      // Write through the codec-specific output just as a real driver does.
      switch (driver.codec.codec) {
        case AV_CODEC_ID_H264:
          static_cast<VkVideoEncodeH264QualityLevelPropertiesKHR *>(properties->pNext)->preferredGopFrameCount = 60;
          break;
        case AV_CODEC_ID_HEVC:
          static_cast<VkVideoEncodeH265QualityLevelPropertiesKHR *>(properties->pNext)->preferredGopFrameCount = 60;
          break;
        default:
          static_cast<VkVideoEncodeAV1QualityLevelPropertiesKHR *>(properties->pNext)->preferredGopFrameCount = 60;
          break;
      }
      return VK_SUCCESS;
    }
  };

  /**
   * @brief Supply FFmpeg's device/frame context chain while preserving global Vulkan settings.
   */
  struct quality_context_t {
    decltype(config::video.vk) saved_settings = config::video.vk;  ///< Settings restored after each scenario.
    quality_driver_t driver;  ///< Fake driver and query observations.
    std::array<const char *, 1> video_encode_extensions {VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME};  ///< Device extensions FFmpeg enabled.
    AVVulkanDeviceContext vulkan {};  ///< Vulkan handles and fake entry point resolver.
    AVHWDeviceContext hardware {};  ///< FFmpeg hardware device wrapping Vulkan.
    AVBufferRef device_buffer {};  ///< Non-owning reference to the hardware device.
    AVHWFramesContext frames {};  ///< FFmpeg hardware frame pool.
    AVBufferRef frames_buffer {};  ///< Non-owning reference to the frame pool.
    AVCodecContext codec {};  ///< Codec configuration passed to the real encode device.
    AVDictionary *options = nullptr;  ///< Options produced by the real encode device.
    std::unique_ptr<platf::avcodec_encode_device_t> device = vk::make_avcodec_encode_device_vram(1920, 1080, 0, 0);  ///< Production Vulkan encode device under test.

    /**
     * @brief Configure the requested codec and quality preset using fake Vulkan handles.
     * @param codec_case Codec-specific Vulkan structures expected by the fake driver.
     * @param preset Sunshine quality preset.
     * @note Buffer data pointers carry typed context addresses without byte-oriented access.
     */
    explicit quality_context_t(const quality_codec_t &codec_case, int preset = 2):
        driver {codec_case} {
      config::video.vk.quality = preset;
      config::video.vk.rc_mode = 0;
      vulkan.inst = std::bit_cast<VkInstance>(&driver);
      vulkan.phys_dev = std::bit_cast<VkPhysicalDevice>(&driver);
      vulkan.get_proc_addr = &quality_driver_t::get_proc_addr;
      vulkan.enabled_dev_extensions = video_encode_extensions.data();
      vulkan.nb_enabled_dev_extensions = static_cast<int>(video_encode_extensions.size());
      hardware.hwctx = &vulkan;
      device_buffer.data = static_cast<decltype(device_buffer.data)>(static_cast<void *>(&hardware));
      frames.device_ref = &device_buffer;
      frames_buffer.data = static_cast<decltype(frames_buffer.data)>(static_cast<void *>(&frames));
      codec.hw_frames_ctx = &frames_buffer;
      codec.codec_id = codec_case.codec;
    }

    /**
     * @brief Release generated codec options and restore the original global settings.
     */
    ~quality_context_t() {
      av_dict_free(&options);
      config::video.vk = saved_settings;
    }

    /**
     * @brief Query the production device's selected codec quality option.
     * @return Quality level, or std::nullopt when the driver default is retained.
     */
    std::optional<int> initialize() {
      device->init_codec_options(&codec, &options);
      const auto *entry = av_dict_get(options, "quality", nullptr, 0);
      return entry ? std::make_optional(std::stoi(entry->value)) : std::nullopt;
    }
  };
}  // namespace

/**
 * @brief Leave optional video encode image usage to FFmpeg's capability checks.
 */
TEST(VulkanEncodeDeviceTest, DefersVideoEncodeImageUsageUntilDeviceSupportIsKnown) {
  auto device = vk::make_avcodec_encode_device_vram(1920, 1080, 0, 0);
  ASSERT_NE(device, nullptr);

  AVVulkanFramesContext vulkan_frames {};
  AVHWFramesContext frames {};
  frames.hwctx = &vulkan_frames;

  device->init_hwframes(&frames);

  EXPECT_EQ(vulkan_frames.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, 0);
  EXPECT_EQ(vulkan_frames.usage & VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_USAGE_STORAGE_BIT);
  EXPECT_EQ(vulkan_frames.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  EXPECT_EQ(vulkan_frames.usage & VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_USAGE_SAMPLED_BIT);
  EXPECT_EQ(vulkan_frames.tiling, VK_IMAGE_TILING_OPTIMAL);
  EXPECT_EQ(frames.initial_pool_size, 4);
}

/**
 * @brief Quality queries provide writable codec-specific storage for every supported codec.
 */
TEST(VulkanEncodeDeviceTest, ChainsQualityPropertiesAndUsesZeroBasedPresetLevels) {
  constexpr std::array expected_levels {0, 1, 3, 0};
  for (const auto &codec : quality_codecs) {
    for (int preset = 1; preset <= 4; ++preset) {
      SCOPED_TRACE(std::to_string(codec.codec) + ":" + std::to_string(preset));
      quality_context_t context {codec, preset};
      EXPECT_EQ(context.initialize(), expected_levels[preset - 1]);
      EXPECT_TRUE(context.driver.valid_chains);
      EXPECT_EQ(context.driver.queried_levels, (std::vector<uint32_t> {0, static_cast<uint32_t>(expected_levels[preset - 1])}));
    }
  }
}

/**
 * @brief A device with a single quality level still supports the valid level zero.
 */
TEST(VulkanEncodeDeviceTest, AcceptsLevelZeroWhenTheDriverOffersOnlyOneLevel) {
  for (const auto &codec : quality_codecs) {
    for (int preset = 1; preset <= 3; ++preset) {
      quality_context_t context {codec, preset};
      context.driver.quality_counts = {1};
      EXPECT_EQ(context.initialize(), 0);
      EXPECT_TRUE(context.driver.valid_chains);
      EXPECT_EQ(context.driver.queried_levels, (std::vector<uint32_t> {0, 0}));
    }
  }
}

/**
 * @brief Failed quality queries must not be mistaken for a successful level-zero query.
 */
TEST(VulkanEncodeDeviceTest, RetainsDriverDefaultWhenEitherQualityQueryFails) {
  for (size_t failed_call : {1, 2}) {
    quality_context_t context {quality_codecs[0], 1};
    context.driver.fail_quality_call = failed_call;
    EXPECT_EQ(context.initialize(), std::nullopt);
    EXPECT_EQ(context.driver.queried_levels.size(), failed_call);
  }
}

/**
 * @brief Unsupported profiles and failed or empty capability queries preserve driver defaults.
 */
TEST(VulkanEncodeDeviceTest, RetainsDriverDefaultWhenCapabilitiesAreUnavailable) {
  for (int scenario = 0; scenario < 4; ++scenario) {
    SCOPED_TRACE(scenario);
    quality_context_t context {quality_codecs[0]};
    switch (scenario) {
      case 0:
        context.driver.capability_result = VK_ERROR_INITIALIZATION_FAILED;
        break;
      case 1:
        context.driver.quality_counts = {0};
        break;
      case 2:
        context.driver.capabilities_available = false;
        break;
      default:
        context.codec.codec_id = AV_CODEC_ID_MPEG2VIDEO;
        break;
    }
    EXPECT_EQ(context.initialize(), std::nullopt);
    EXPECT_TRUE(context.driver.queried_levels.empty());
  }
}

/**
 * @brief Missing Vulkan context handles prevent any driver query.
 */
TEST(VulkanEncodeDeviceTest, RetainsDriverDefaultForIncompleteVulkanContexts) {
  for (int scenario = 0; scenario < 4; ++scenario) {
    quality_context_t context {quality_codecs[0]};
    switch (scenario) {
      case 0:
        context.hardware.hwctx = nullptr;
        break;
      case 1:
        context.vulkan.inst = VK_NULL_HANDLE;
        break;
      case 2:
        context.vulkan.phys_dev = VK_NULL_HANDLE;
        break;
      default:
        context.vulkan.get_proc_addr = nullptr;
        break;
    }
    EXPECT_EQ(context.initialize(), std::nullopt);
    EXPECT_EQ(context.driver.capability_calls, 0);
  }
}

/**
 * @brief Capability-based selection remains available without the optional quality entry point.
 */
TEST(VulkanEncodeDeviceTest, SelectsQualityWithoutTheOptionalQualityQueryEntryPoint) {
  quality_context_t context {quality_codecs[0], 3};
  context.driver.quality_available = false;
  EXPECT_EQ(context.initialize(), 3);
  EXPECT_TRUE(context.driver.queried_levels.empty());
}

/**
 * @brief A changing capability range never causes an out-of-range query or option.
 */
TEST(VulkanEncodeDeviceTest, ClampsQueriesWhenTheQualityRangeChanges) {
  quality_context_t context {quality_codecs[0], 3};
  context.driver.quality_counts = {4, 1};
  EXPECT_EQ(context.initialize(), std::nullopt);
  EXPECT_TRUE(context.driver.valid_chains);
  EXPECT_EQ(context.driver.queried_levels, (std::vector<uint32_t> {0, 0}));
}

/**
 * @brief Disabled presets and absent hardware frames skip quality selection.
 */
TEST(VulkanEncodeDeviceTest, SkipsQualityQueriesWhenDisabledOrHardwareFramesAreAbsent) {
  for (bool have_frames : {false, true}) {
    quality_context_t context {quality_codecs[0], have_frames ? 0 : 2};
    context.codec.hw_frames_ctx = have_frames ? &context.frames_buffer : nullptr;
    EXPECT_EQ(context.initialize(), std::nullopt);
    EXPECT_EQ(context.driver.capability_calls, 0);
  }
}

/**
 * @brief VBR mode permits the encoder bitrate to fall below the configured target.
 */
TEST(VulkanEncodeDeviceTest, ClearsMinimumBitrateOnlyForVbr) {
  for (int rc_mode : {0, 4}) {
    quality_context_t context {quality_codecs[0], 0};
    config::video.vk.rc_mode = rc_mode;
    context.codec.rc_min_rate = 1000000;
    EXPECT_EQ(context.initialize(), std::nullopt);
    EXPECT_EQ(context.codec.rc_min_rate, rc_mode == 4 ? 0 : 1000000);
  }
}

/**
 * @brief A device without video encode support (Mesa's lavapipe) is never asked for video capabilities.
 *
 * The loader resolves the capability entry point for any instance; calling it on such a device reached a null
 * driver function and crashed Sunshine during encoder probing.
 */
TEST(VulkanEncodeDeviceTest, SkipsQualityQueryWithoutVideoEncodeSupport) {
  for (const auto &codec : quality_codecs) {
    quality_context_t context {codec, 2};
    context.vulkan.enabled_dev_extensions = nullptr;
    context.vulkan.nb_enabled_dev_extensions = 0;
    EXPECT_EQ(context.initialize(), std::nullopt);
    EXPECT_EQ(context.driver.capability_calls, 0);
    EXPECT_TRUE(context.driver.queried_levels.empty());
  }
}
#endif  // SUNSHINE_BUILD_VULKAN
