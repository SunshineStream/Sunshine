/**
 * @file tests/unit/test_video_pyrowave.cpp
 * @brief Regression coverage for PyroWave profiles, chroma and HDR precision, framing, and failures.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstring>
#include <limits>
#include <string_view>

// lib includes
#ifdef __APPLE__
  #include <CoreVideo/CoreVideo.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#ifdef SUNSHINE_BUILD_VULKAN
  #include <libavutil/hwcontext_vulkan.h>
#endif
}

// local includes
#include <src/config.h>
#include <src/video.h>

namespace video {
  /**
   * @brief Dispatch a converted frame through the common packet queue.
   * @param frame_nr Sequence number.
   * @param session Active encoder.
   * @param packets Destination queue.
   * @param channel_data Owning stream.
   * @param frame_timestamp Capture timestamp.
   * @return Zero on success.
   */
  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp);

  /**
   * @brief Create an encoder session through the capture pipeline.
   * @param disp Capture display.
   * @param encoder Capture backend descriptor.
   * @param config Negotiated configuration.
   * @param width Captured width.
   * @param height Captured height.
   * @param encode_device Capture conversion device.
   * @return Initialized session, or null on failure.
   */
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device);

  /**
   * @brief Create a PyroWave capture session with injectable codec entry points.
   * @param encoder Capture backend descriptor.
   * @param config Negotiated configuration.
   * @param width Captured width.
   * @param height Captured height.
   * @param device Capture conversion device.
   * @param api Codec entry points.
   * @return Initialized session, or null on failure.
   */
  std::unique_ptr<encode_session_t> make_pyrowave_encode_session(const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::avcodec_encode_device_t> device, const pyrowave::api_t &api);

  /**
   * @brief Request a capture converter for the negotiated format.
   * @param disp Capture display.
   * @param encoder Capture backend descriptor.
   * @param config Negotiated configuration.
   * @return Capture device, or null when unsupported.
   */
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &disp, const encoder_t &encoder, const config_t &config);

  /**
   * @brief Refresh available PyroWave profiles.
   * @param encoder Capture backend descriptor.
   */
  void probe_pyrowave(const encoder_t &encoder);
}  // namespace video

namespace nvhttp {
  /**
   * @brief Obtain the same capability mask used in server-info responses.
   * @return Probed codec support bits.
   */
  uint32_t get_codec_mode_flags();
}  // namespace nvhttp

TEST(PyrowaveConfigTest, ServerInfoReportsEachProfileIndependently) {
  const auto previous = video::active_pyrowave_profiles;
  const auto restore = util::fail_guard([previous]() {
    video::active_pyrowave_profiles = previous;
  });
  video::active_pyrowave_profiles = 0;
  const auto baseline = nvhttp::get_codec_mode_flags();
  EXPECT_EQ(baseline & video::pyrowave::scm_mask, 0);
  for (uint32_t profiles = 0; profiles < 16; ++profiles) {
    video::active_pyrowave_profiles = profiles << 23;
    EXPECT_EQ(nvhttp::get_codec_mode_flags(), baseline | (profiles << 23));
  }
  video::active_pyrowave_profiles = ~video::pyrowave::scm_mask;
  EXPECT_EQ(nvhttp::get_codec_mode_flags(), baseline);
}

namespace {
  /**
   * @brief Return a small, valid PyroWave configuration for deterministic tests.
   * @return 64x48 SDR 4:2:0 at 60 FPS.
   */
  video::config_t pyrowave_config() {
    return {64, 48, 60, 6000, 200000, 1, 0, 3, video::pyrowave::video_format, 0, 0, 0};
  }

  /**
   * @brief Build native v1 records with known block indices and payload lengths.
   *
   * @param blocks Block index and total byte length for each record.
   * @return Sequence header followed by the requested block records.
   */
  std::vector<uint8_t> pyrowave_records(std::initializer_list<std::pair<uint32_t, size_t>> blocks) {
    std::vector<uint8_t> data(8);
    AV_WL32(data.data(), 0x80000000u | 63 | (47 << 14));
    AV_WL32(data.data() + 4, blocks.size());
    for (auto [index, size] : blocks) {
      const auto offset = data.size();
      data.resize(offset + size, uint8_t(index));
      AV_WL32(data.data() + offset, (size / 4) << 16);
      AV_WL32(data.data() + offset + 4, index << 8);
    }
    return data;
  }

  /**
   * @brief Record converter requests without requiring a display or video encoder.
   */
  class pyrowave_test_display_t: public platf::display_t {
  public:
    /**
     * @copydoc platf::display_t::capture
     */
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    /**
     * @copydoc platf::display_t::alloc_img
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      return {};
    }

    /**
     * @copydoc platf::display_t::dummy_img
     */
    int dummy_img(platf::img_t *) override {
      return -1;
    }

    /**
     * @copydoc platf::display_t::make_avcodec_encode_device
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e format) override {
      requested = format;
      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    platf::pix_fmt_e requested = platf::pix_fmt_e::unknown;  ///< Last requested conversion format.
  };
}  // namespace

/**
 * @brief Match legacy clients and record clients, including short and unaligned RTP payloads.
 */
TEST(PyrowaveFramingTest, MatchesLegacyAndRecordWireFormats) {
  const auto data = pyrowave_records({{0, 12}, {12, 12}});
  const std::array packets {pyrowave_packet {0, data.size()}};
  auto config = pyrowave_config();
  auto legacy = video::pyrowave::frame_packets(packets, data, config, 12, 42);
  ASSERT_NE(legacy, nullptr);
  std::vector<uint8_t> expected {1, 0, 0, 0, 32, 0, 0, 0};
  expected.insert(expected.end(), data.begin(), data.end());
  EXPECT_EQ(std::vector(legacy->data(), legacy->data() + legacy->data_size()), expected);
  EXPECT_TRUE(legacy->is_idr());
  EXPECT_EQ(legacy->frame_index(), 42);
  EXPECT_EQ(legacy->pyrowave_critical_packets, 0);
  EXPECT_TRUE(legacy->pyrowave_record_starts.empty());

  const auto large = pyrowave_records({{0, 2048}});
  const std::array large_packets {pyrowave_packet {0, 8}, pyrowave_packet {8, 2048}};
  auto oversized = video::pyrowave::frame_packets(large_packets, large, config, 12, 42);
  ASSERT_NE(oversized, nullptr);
  EXPECT_EQ(AV_RL32(oversized->data()), 2);
  EXPECT_EQ(AV_RL32(oversized->data() + 16), 2048);
  EXPECT_EQ(oversized->data_size(), large.size() + 12);

  config.pyrowaveRecordFraming = true;
  config.packetSize = 40;  // 24 data bytes per RTP packet; the first includes an eight-byte frame header.
  auto records = video::pyrowave::frame_packets(packets, data, config, 12, 43);
  ASSERT_NE(records, nullptr);
  ASSERT_EQ(records->data_size(), 52);
  EXPECT_EQ(records->pyrowave_critical_packets, 2);
  EXPECT_EQ(records->pyrowave_record_starts, (std::vector<bool> {true, true, true}));
  EXPECT_EQ(AV_RL32(records->data()), AV_RL32(data.data()));
  EXPECT_EQ(AV_RL32(records->data() + 8), 0xffffffffu);
  EXPECT_EQ(AV_RL32(records->data() + 12), 0);
  EXPECT_EQ(AV_RL32(records->data() + 28), 0xffffffffu);
  EXPECT_EQ(AV_RL32(records->data() + 32), 1);
  EXPECT_EQ(AV_RL32(records->data() + 44) >> 8, 12);

  for (int packet_size : {36, 42}) {
    config.packetSize = packet_size;
    auto unaligned = video::pyrowave::frame_packets(packets, data, config, 12, 44);
    ASSERT_NE(unaligned, nullptr);
    EXPECT_EQ(std::vector(unaligned->data(), unaligned->data() + unaligned->data_size()), data);
    EXPECT_EQ(unaligned->pyrowave_critical_packets, 0);
    EXPECT_TRUE(unaligned->pyrowave_record_starts.empty());
  }
}

/**
 * @brief Preserve native records while aligning recoverable detail and critical prefix boundaries.
 */
TEST(PyrowaveFramingTest, AlignsOversizedRecordsAndMarksRecoveryBoundaries) {
  auto config = pyrowave_config();
  config.pyrowaveRecordFraming = true;
  config.packetSize = 64;
  for (const auto &data : {
         pyrowave_records({{0, 12}, {1, 84}, {12, 12}, {13, 88}}),
         pyrowave_records({{0, 76}, {12, 12}}),
         pyrowave_records({{0, 12}}),
         pyrowave_records({})
       }) {
    const std::array packets {pyrowave_packet {0, data.size()}};
    auto frame = video::pyrowave::frame_packets(packets, data, config, 12, 1);
    ASSERT_NE(frame, nullptr);
    std::vector<bool> starts((frame->data_size() + 8 + 47) / 48);
    std::vector<uint32_t> indices;
    size_t critical_end = 8;
    for (size_t offset = 0; offset < frame->data_size();) {
      ASSERT_GE(frame->data_size() - offset, 8);
      const auto word = AV_RL32(frame->data() + offset);
      const auto info = AV_RL32(frame->data() + offset + 4);
      if (offset == 0 || (offset + 8) % 48 == 0) {
        starts[(offset + 8) / 48] = true;
      }
      const auto size = word == 0xffffffffu ? 8 + size_t(info) * 4 : word & 0x80000000u ? 8 :
                                                                                          size_t((word >> 16) & 0xfff) * 4;
      ASSERT_GE(size, 8);
      ASSERT_LE(size, frame->data_size() - offset);
      if (!(word & 0x80000000u)) {
        const auto index = info >> 8;
        indices.push_back(index);
        if (index < 12) {
          critical_end = offset + size;
        }
        if (size <= 40) {
          EXPECT_EQ((offset + 8) / 48, (offset + 8 + size - 1) / 48);
        }
      }
      offset += size;
    }
    EXPECT_EQ(frame->pyrowave_record_starts, starts);
    EXPECT_EQ(frame->pyrowave_critical_packets, (critical_end + 8 + 47) / 48);
    if (indices.size() == 4) {
      EXPECT_EQ(indices, (std::vector<uint32_t> {1, 0, 13, 12}));
    }
  }
}

/**
 * @brief Reject malformed native packets and frames that exceed transport or allocation limits.
 */
TEST(PyrowaveFramingTest, RejectsInvalidPacketRangesRecordsAndFrameSizes) {
  auto config = pyrowave_config();
  for (bool records : {false, true}) {
    config.pyrowaveRecordFraming = records;
    auto data = pyrowave_records({{0, 12}});
    for (auto packet : {pyrowave_packet {0, 0}, {0, 3}, {0, 20000}, {data.size() + 1, 8}, {data.size() - 4, 8}}) {
      EXPECT_EQ(video::pyrowave::frame_packets(std::span(&packet, 1), data, config, 12, 1), nullptr);
    }
    EXPECT_EQ(video::pyrowave::frame_packets({}, data, config, 12, 1), nullptr);
    for (int size : {-1, 0, 16, 65536}) {
      config.packetSize = size;
      const std::array packets {pyrowave_packet {0, data.size()}};
      EXPECT_EQ(video::pyrowave::frame_packets(packets, data, config, 12, 1), nullptr);
    }
    config.packetSize = 1392;
  }
  for (int failure = 0; failure < 5; ++failure) {
    auto data = pyrowave_records({{0, 12}});
    if (failure == 0) {
      data.resize(4);
    } else if (failure == 1) {
      AV_WL32(data.data(), 2 << 16);
    } else if (failure == 2) {
      AV_WL32(data.data() + 8, 0x80000000u);
    } else {
      AV_WL32(data.data() + 8, (failure == 3 ? 1 : 4) << 16);
    }
    const std::array packets {pyrowave_packet {0, data.size()}};
    EXPECT_EQ(video::pyrowave::frame_packets(packets, data, config, 12, 1), nullptr) << failure;
  }
  const auto data = pyrowave_records({{12, 12}});
  std::vector<pyrowave_packet> packets(9000, {8, 12});
  packets.front() = {0, 8};
  config.packetSize = 40;
  EXPECT_EQ(video::pyrowave::frame_packets(packets, data, config, 12, 1), nullptr);
  const auto large_record = pyrowave_records({{0, 16380}});
  config.packetSize = 65532;
  for (size_t count : {1001, 1101}) {
    packets.assign(count, {8, 16380});
    packets.front() = {0, 8};
    EXPECT_EQ(video::pyrowave::frame_packets(packets, large_record, config, 12, 1), nullptr);
  }
  config.pyrowaveRecordFraming = false;
  std::vector<uint8_t> large_packet(video::pyrowave::packet_boundary);
  packets.assign(video::pyrowave::max_frame_size / large_packet.size(), {0, large_packet.size()});
  EXPECT_EQ(video::pyrowave::frame_packets(packets, large_packet, config, 0, 1), nullptr);
}

TEST(PyrowaveConfigTest, NegotiatesOnlySuccessfullyProbedProfiles) {
  auto config = pyrowave_config();
  const uint32_t flags[] {video::pyrowave::scm_sdr, video::pyrowave::scm_sdr_444, video::pyrowave::scm_hdr, video::pyrowave::scm_hdr_444};
  for (uint32_t profiles = 0; profiles < 16; ++profiles) {
    std::array<int, 4> calls {};
    const auto probed = video::pyrowave::probe_profiles([&](const video::config_t &probe) {
      EXPECT_TRUE(video::pyrowave::valid_config(probe));
      const auto profile = probe.dynamicRange * 2 + probe.chromaSamplingType;
      ++calls.at(profile);
      return (profiles & (1 << profile)) != 0;
    });
    EXPECT_EQ(probed, profiles << 23);
    EXPECT_EQ(calls, (std::array {1, 1, 1, 1}));
    for (int profile = 0; profile < 4; ++profile) {
      config.chromaSamplingType = profile % 2;
      config.dynamicRange = profile / 2;
      EXPECT_EQ(video::pyrowave::supported(config, probed), (profiles & (1 << profile)) != 0);
      EXPECT_TRUE(video::pyrowave::supported(config, flags[profile]));
    }
  }
}

TEST(PyrowaveConfigTest, RejectsMalformedOrUnsupportedRequests) {
  const auto valid = pyrowave_config();
  EXPECT_TRUE(video::pyrowave::valid_config(valid));
  const std::vector<std::pair<int video::config_t::*, int>> invalid {
    {&video::config_t::videoFormat, -1},
    {&video::config_t::videoFormat, 2},
    {&video::config_t::width, 0},
    {&video::config_t::width, -2},
    {&video::config_t::width, 63},
    {&video::config_t::width, 16386},
    {&video::config_t::height, 0},
    {&video::config_t::height, -2},
    {&video::config_t::height, 47},
    {&video::config_t::height, 16386},
    {&video::config_t::packetSize, 0},
    {&video::config_t::packetSize, -1},
    {&video::config_t::packetSize, 65536},
    {&video::config_t::dynamicRange, -1},
    {&video::config_t::dynamicRange, 2},
    {&video::config_t::chromaSamplingType, -1},
    {&video::config_t::chromaSamplingType, 2},
    {&video::config_t::bitrate, 0},
    {&video::config_t::bitrate, -1},
    {&video::config_t::bitrate, std::numeric_limits<int>::max()},
    {&video::config_t::framerate, 0},
    {&video::config_t::framerate, -1},
    {&video::config_t::framerateX100, -1},
  };
  for (const auto &[member, value] : invalid) {
    auto config = valid;
    config.*member = value;
    EXPECT_FALSE(video::pyrowave::valid_config(config)) << value;
    EXPECT_FALSE(video::pyrowave::supported(config, video::pyrowave::scm_mask)) << value;
  }
  auto config = valid;
  config.chromaSamplingType = 1;
  config.width = 63;
  config.height = 47;
  EXPECT_TRUE(video::pyrowave::valid_config(config));
}

/**
 * @brief Select capture layouts independently of the hardware video encoder's codec support.
 */
TEST(PyrowaveConfigTest, SelectsCaptureFormatsForEachProfile) {
  video::encoder_t native {
    .platform_formats = std::make_unique<video::encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv,
      platf::pix_fmt_e::yuv444p16
    )
  };
  video::encoder_platform_formats_avcodec cuda {
    AV_HWDEVICE_TYPE_CUDA,
    AV_HWDEVICE_TYPE_NONE,
    AV_PIX_FMT_CUDA,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV444P16,
    {}
  };
  const AVPixelFormat software_formats[] {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV444P10};
  const AVPixelFormat dxgi_formats[] {AV_PIX_FMT_NV12, AV_PIX_FMT_VUYX, AV_PIX_FMT_P010, AV_PIX_FMT_XV30};
  const AVPixelFormat cuda_formats[] {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV444P, AV_PIX_FMT_P010, AV_PIX_FMT_YUV444P16};
  pyrowave_test_display_t display;
  auto config = pyrowave_config();
  for (int profile = 0; profile < 4; ++profile) {
    config.chromaSamplingType = profile % 2;
    config.dynamicRange = profile / 2;
    EXPECT_EQ(video::pyrowave::capture_format(*video::software.platform_formats, config), software_formats[profile]);
    EXPECT_EQ(video::pyrowave::capture_format(*native.platform_formats, config), dxgi_formats[profile]);
    EXPECT_EQ(video::pyrowave::capture_format(cuda, config), cuda_formats[profile]);
    ASSERT_NE(video::make_encode_device(display, native, config), nullptr);
    EXPECT_EQ(display.requested, video::map_pix_fmt(dxgi_formats[profile]));
  }
  video::encoder_platform_formats_t unknown {};
  unknown.dev_type = platf::mem_type_e::system;
  EXPECT_EQ(video::pyrowave::capture_format(unknown, config), AV_PIX_FMT_NONE);
  cuda.avcodec_pix_fmt_yuv444_8bit = AV_PIX_FMT_NONE;
  cuda.avcodec_pix_fmt_yuv444_10bit = AV_PIX_FMT_NONE;
  EXPECT_EQ(video::pyrowave::capture_format(cuda, config), AV_PIX_FMT_NONE);
  video::encoder_t unavailable {
    .platform_formats = std::make_unique<video::encoder_platform_formats_avcodec>(cuda)
  };
  display.requested = platf::pix_fmt_e::unknown;
  EXPECT_EQ(video::make_encode_device(display, unavailable, config), nullptr);
  EXPECT_EQ(display.requested, platf::pix_fmt_e::unknown);
  config.dynamicRange = 0;
  EXPECT_EQ(video::make_encode_session(nullptr, unavailable, config, 64, 48, std::make_unique<platf::avcodec_encode_device_t>()), nullptr);
}

/**
 * @brief Map planar, interleaved, and packed GPU storage while preserving chroma order and HDR range.
 */
TEST(PyrowaveConfigTest, MapsGpuImagesWithoutRepackingPixels) {
  struct layout_t {
    AVPixelFormat format;  ///< Capture pixel layout.
    VkFormat combined;  ///< Multiplanar Vulkan image format.
    std::array<VkFormat, 3> planes;  ///< Separate Vulkan plane formats.
    float scale;  ///< Normalization for separate images.
  };

  const layout_t layouts[] {
    {AV_PIX_FMT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM}, 1.0f},
    {AV_PIX_FMT_P010, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM}, 65535.0f / 65472.0f},
    {AV_PIX_FMT_YUV420P, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM}, 1.0f},
    {AV_PIX_FMT_YUV444P, VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM, {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM}, 1.0f},
    {AV_PIX_FMT_YUV420P10, VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM, {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM}, 65535.0f / 1023.0f},
    {AV_PIX_FMT_YUV444P10, VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM, {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM}, 65535.0f / 1023.0f},
    {AV_PIX_FMT_YUV444P16, VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM, {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM}, 1.0f},
  };
  pyrowave_gpu_buffers buffers {};
  float scale;
  for (const auto &layout : layouts) {
    SCOPED_TRACE(av_get_pix_fmt_name(layout.format));
    const auto desc = av_pix_fmt_desc_get(layout.format);
    const auto planes = av_pix_fmt_count_planes(layout.format);
    for (bool combined : {false, true}) {
      std::vector<pyrowave_image_view> images(combined ? 1 : planes);
      for (size_t i = 0; i < images.size(); ++i) {
        images[i].image = reinterpret_cast<VkImage>(i + 1);
        images[i].image_format = combined ? layout.combined : layout.planes[i];
        images[i].width = 64 >> (i ? desc->log2_chroma_w : 0);
        images[i].height = 48 >> (i ? desc->log2_chroma_h : 0);
        images[i].layout = VK_IMAGE_LAYOUT_GENERAL;
      }
      ASSERT_TRUE(video::pyrowave::map_gpu_frame(layout.format, images, buffers, scale));
      EXPECT_FLOAT_EQ(scale, combined && layout.format == AV_PIX_FMT_P010 ? 1.0f : layout.scale);
      for (int component = 0; component < 3; ++component) {
        const auto plane = planes == 2 && component ? 1 : component;
        const auto &view = buffers.planes[component];
        EXPECT_EQ(view.image, images[combined ? 0 : plane].image);
        EXPECT_EQ(view.aspect, combined ? VkImageAspectFlagBits(VK_IMAGE_ASPECT_PLANE_0_BIT << plane) : VK_IMAGE_ASPECT_COLOR_BIT);
        EXPECT_EQ(view.swizzle, planes == 2 && component == 2 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_R);
        EXPECT_EQ(view.width, images[combined ? 0 : plane].width);
        EXPECT_EQ(view.height, images[combined ? 0 : plane].height);
        EXPECT_EQ(view.layout, VK_IMAGE_LAYOUT_GENERAL);
        EXPECT_EQ(view.view_format, combined && layout.format == AV_PIX_FMT_P010 ? (plane ? VK_FORMAT_R10X6G10X6_UNORM_2PACK16 : VK_FORMAT_R10X6_UNORM_PACK16) : layout.planes[plane]);
      }
      if (!combined) {
        --images[1].width;
        EXPECT_FALSE(video::pyrowave::map_gpu_frame(layout.format, images, buffers, scale));
        ++images[1].width;
        --images[1].height;
        EXPECT_FALSE(video::pyrowave::map_gpu_frame(layout.format, images, buffers, scale));
        ++images[1].height;
        auto extra = images;
        extra.push_back(images[0]);
        EXPECT_FALSE(video::pyrowave::map_gpu_frame(layout.format, extra, buffers, scale));
      }
      images[0].width = 0;
      EXPECT_FALSE(video::pyrowave::map_gpu_frame(layout.format, images, buffers, scale));
      images[0].width = 64;
      images[0].height = 0;
      EXPECT_FALSE(video::pyrowave::map_gpu_frame(layout.format, images, buffers, scale));
      images[0].height = 48;
      images[0].image_format = VK_FORMAT_UNDEFINED;
      EXPECT_FALSE(video::pyrowave::map_gpu_frame(layout.format, images, buffers, scale));
    }
  }
  const std::pair<VkFormat, std::array<VkComponentSwizzle, 3>> packed[] {
    {VK_FORMAT_R8G8B8A8_UNORM, {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R}},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_B}},
    {VK_FORMAT_A2R10G10B10_UNORM_PACK32, {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_R}},
  };
  for (const auto &[format, swizzles] : packed) {
    pyrowave_image_view image {};
    image.image = reinterpret_cast<VkImage>(1);
    image.width = 64;
    image.height = 48;
    image.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    image.image_format = format;
    const auto pixel_format = format == VK_FORMAT_R8G8B8A8_UNORM ? AV_PIX_FMT_VUYX : AV_PIX_FMT_XV30;
    ASSERT_TRUE(video::pyrowave::map_gpu_frame(pixel_format, std::span(&image, 1), buffers, scale));
    EXPECT_FLOAT_EQ(scale, 1.0f);
    for (int component = 0; component < 3; ++component) {
      EXPECT_EQ(buffers.planes[component].swizzle, swizzles[component]);
      EXPECT_EQ(buffers.planes[component].view_format, format);
    }
    const std::array duplicate {image, image};
    EXPECT_FALSE(video::pyrowave::map_gpu_frame(pixel_format, duplicate, buffers, scale));
    image.image_format = VK_FORMAT_UNDEFINED;
    EXPECT_FALSE(video::pyrowave::map_gpu_frame(pixel_format, std::span(&image, 1), buffers, scale));
    EXPECT_FALSE(video::pyrowave::map_gpu_frame(AV_PIX_FMT_RGB24, std::span(&image, 1), buffers, scale));
    image.image = VK_NULL_HANDLE;
    EXPECT_FALSE(video::pyrowave::map_gpu_frame(pixel_format, std::span(&image, 1), buffers, scale));
  }
  EXPECT_FALSE(video::pyrowave::map_gpu_frame(AV_PIX_FMT_NV12, {}, buffers, scale));
}

/**
 * @brief Respect host bitrate limits and fractional rates without integer overflow.
 */
TEST(PyrowaveConfigTest, BudgetsCappedAndFractionalRatesWithoutIntegerOverflow) {
  const auto previous = config::video.max_bitrate;
  const auto restore = util::fail_guard([previous]() {
    config::video.max_bitrate = previous;
  });
  config::video.max_bitrate = 0;
  auto config = pyrowave_config();
  EXPECT_EQ(video::pyrowave::frame_budget(config), 416666);
  config.framerateX100 = 5994;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 417083);
  config.framerateX100 = 0;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 416666);
  config.bitrate = 1;
  config.framerate = 1000;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 0);
  config.bitrate = std::numeric_limits<int>::max();
  config.framerate = std::numeric_limits<int>::max();
  EXPECT_EQ(video::pyrowave::frame_budget(config), 125);

  config = pyrowave_config();
  config::video.max_bitrate = 5000;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 10416);
  config.framerateX100 = 5994;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 10427);
  config.framerateX100 = 0;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 10416);
  config.bitrate = 2000;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 4166);
  config.bitrate = 5000;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 10416);
  config = pyrowave_config();
  config::video.max_bitrate = -1;
  EXPECT_EQ(video::pyrowave::frame_budget(config), 416666);
  for (int packet_size : {-1, 0, 16, 65536}) {
    config.packetSize = packet_size;
    EXPECT_EQ(video::pyrowave::frame_budget(config), 0);
  }
}

/**
 * @brief Fit below-budget native frames within the negotiated transport, including record padding.
 */
TEST(PyrowaveConfigTest, BoundsAcceptedBudgetsByPacketCapacity) {
  const auto previous = config::video.max_bitrate;
  const auto restore = util::fail_guard([previous]() {
    config::video.max_bitrate = previous;
  });
  config::video.max_bitrate = 0;
  auto config = pyrowave_config();
  config.width = 3840;
  config.height = 2160;
  for (bool records : {false, true}) {
    config.pyrowaveRecordFraming = records;
    for (int packet_size : {config::PACKETSIZE_MIN, 201, 1392, 36, 65532}) {
      SCOPED_TRACE(testing::Message() << "records " << records << ", packet size " << packet_size);
      config.packetSize = packet_size;
      config.bitrate = packet_size == 65532 ? 8000000 : 1000000;
      ASSERT_TRUE(video::pyrowave::valid_config(config));
      const auto budget = video::pyrowave::frame_budget(config);
      const auto capacity = 4092 * (size_t(packet_size) - 16) - 8;
      if (packet_size == config::PACKETSIZE_MIN) {
        EXPECT_EQ(capacity, 752920);
        // The reported 1,024,008-byte frame must no longer be below the accepted budget.
        EXPECT_LT(budget, 1024008);
      } else if (packet_size == 1392) {
        EXPECT_EQ(budget, 2083333);
      }
      // Cover tiny records, padding on either side of a shard boundary, and records spanning shards.
      for (size_t record_size : {8, 88, 164, 176, 180, 184, 1024, 16380}) {
        SCOPED_TRACE(record_size);
        std::vector<uint8_t> native(8);
        std::vector<pyrowave_packet> packets {{0, 8}};
        for (uint32_t index = 0; native.size() + record_size <= budget; ++index) {
          const auto offset = native.size();
          native.resize(offset + record_size);
          AV_WL32(native.data() + offset, (record_size / 4) << 16);
          AV_WL32(native.data() + offset + 4, index << 8);
          packets.push_back({offset, record_size});
        }
        AV_WL32(native.data(), 0x80000000u | (config.width - 1) | ((config.height - 1) << 14));
        AV_WL32(native.data() + 4, packets.size() - 1);
        ASSERT_LE(native.size(), budget);
        const auto frame = video::pyrowave::frame_packets(packets, native, config, 12, 1);
        ASSERT_NE(frame, nullptr);
        EXPECT_LE(frame->data_size(), capacity);
        EXPECT_LE(frame->data_size(), video::pyrowave::max_frame_size);
        EXPECT_LE(frame->pyrowave_record_starts.size(), 4092);
      }
    }
  }
}

TEST(PyrowaveConfigTest, PreservesHdrWithoutConsultingUnrelatedCodecCapabilities) {
  auto config = pyrowave_config();
  config.dynamicRange = 1;
  video::encoder_t encoder {};
  for (int chroma : {0, 1}) {
    config.chromaSamplingType = chroma;
    EXPECT_EQ(video::resolve_dynamic_range(encoder, config).dynamicRange, 1);
  }
}

TEST(PyrowaveConfigTest, DisablingTheCodecClearsPreviouslyProbedProfiles) {
  const auto previous_profiles = video::active_pyrowave_profiles;
  const auto previous_enabled = config::video.pyrowave;
  const auto restore = util::fail_guard([&]() {
    video::active_pyrowave_profiles = previous_profiles;
    config::video.pyrowave = previous_enabled;
  });
  video::active_pyrowave_profiles = video::pyrowave::scm_mask;
  config::video.pyrowave = false;
  video::probe_pyrowave({});
  EXPECT_EQ(video::active_pyrowave_profiles, 0);
}

TEST(PyrowaveConfigTest, RejectsInvalidCaptureDevicesAndHdrFromRam) {
  auto config = pyrowave_config();
  EXPECT_EQ(video::make_encode_session(nullptr, video::software, config, 64, 48, nullptr), nullptr);
  EXPECT_EQ(video::make_encode_session(nullptr, {}, config, 64, 48, std::make_unique<platf::avcodec_encode_device_t>()), nullptr);
  EXPECT_EQ(video::make_encode_session(nullptr, video::software, config, 0, 48, std::make_unique<platf::avcodec_encode_device_t>()), nullptr);
  EXPECT_EQ(video::make_encode_session(nullptr, video::software, config, 64, 0, std::make_unique<platf::avcodec_encode_device_t>()), nullptr);
  config.width = 63;
  EXPECT_EQ(video::make_encode_session(nullptr, video::software, config, 64, 48, std::make_unique<platf::avcodec_encode_device_t>()), nullptr);
  config.width = 64;
  config.dynamicRange = 1;
  EXPECT_EQ(video::make_encode_session(nullptr, video::software, config, 64, 48, std::make_unique<platf::avcodec_encode_device_t>()), nullptr);
}

namespace {
  /**
   * @brief Converter supplying padded planar or packed YUV frames without display hardware.
   */
  class pyrowave_test_converter_t: public platf::avcodec_encode_device_t {
  public:
    /**
     * @brief Fill alternating luma and checkerboard chroma with full-range endpoint values.
     * @param format Pixel storage layout supplied by the capture converter.
     * @param negative_stride Store rows in reverse order within each plane.
     */
    explicit pyrowave_test_converter_t(AVPixelFormat format, bool negative_stride = false) {
      owned.reset(av_frame_alloc());
      frame = owned.get();
      frame->format = format;
      frame->width = 64;
      frame->height = 48;
      colorspace = {video::colorspace_e::bt2020, true, 10};
      frame->colorspace = AVCOL_SPC_BT2020_NCL;
      frame->color_primaries = AVCOL_PRI_BT2020;
      frame->color_trc = AVCOL_TRC_SMPTE2084;
      frame->color_range = AVCOL_RANGE_JPEG;
      if (av_frame_get_buffer(frame, 128) < 0) {
        throw std::bad_alloc();
      }
      for (auto buffer : frame->buf) {
        if (buffer) {
          std::memset(buffer->data, 0, buffer->size);
        }
      }
      const auto desc = av_pix_fmt_desc_get(format);
      if (negative_stride) {
        for (int plane = 0; plane < av_pix_fmt_count_planes(format); ++plane) {
          const auto height = 48 >> (plane ? desc->log2_chroma_h : 0);
          frame->data[plane] += (height - 1) * frame->linesize[plane];
          frame->linesize[plane] *= -1;
        }
      }
      const auto maximum = desc->comp[0].depth > 8 ? 1023 : 255;
      for (int component = 0; component < 3; ++component) {
        const auto width = 64 >> (component ? desc->log2_chroma_w : 0);
        const auto height = 48 >> (component ? desc->log2_chroma_h : 0);
        std::array<uint16_t, 64> row {};
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            const auto value = (x + (component ? y : 0) + (component == 2)) % 2 ? maximum : 1;
            row[x] = uint16_t(desc->comp[0].depth == 16 ? value * 65535 / 1023 : value);
          }
          av_write_image_line(row.data(), frame->data, frame->linesize, desc, 0, y, component, width);
        }
      }
    }

    /**
     * @brief Observe destruction before releasing the converter's frame.
     */
    ~pyrowave_test_converter_t() override {
      if (on_destroy) {
        on_destroy();
      }
    }

    /**
     * @brief Supply the prepared pixels or simulate a conversion error.
     * @param img Unused captured image.
     * @return Configured conversion status.
     */
    int convert(platf::img_t &img) override {
      return status;
    }

    int status = 0;  ///< Simulated conversion result.
    video::avcodec_frame_t owned;  ///< Owner of the input frame.
    std::function<void()> on_destroy;  ///< Optional lifetime assertion.
  };

  /**
   * @brief Mock codec state with explicit control over each failure boundary.
   */
  struct pyrowave_api_state_t {
    uint32_t major = 1;  ///< Reported ABI major version.
    pyrowave_result device_result = PYROWAVE_SUCCESS;  ///< Device creation status.
    pyrowave_result encoder_result = PYROWAVE_SUCCESS;  ///< Encoder creation status.
    pyrowave_result encode_result = PYROWAVE_SUCCESS;  ///< Encode status.
    pyrowave_result count_result = PYROWAVE_SUCCESS;  ///< Packet count status.
    pyrowave_result storage_result = PYROWAVE_SUCCESS;  ///< Native storage allocation query status.
    pyrowave_result packet_result = PYROWAVE_SUCCESS;  ///< Packetization status.
    pyrowave_result blocks_result = PYROWAVE_SUCCESS;  ///< Critical band query status.
    size_t count = 2;  ///< Packet allocation requested from the caller.
    size_t storage_count = 1;  ///< Large native packets required for safe backing storage.
    size_t output_count = 2;  ///< Packet count actually written.
    size_t packet_offset = 4;  ///< First packet offset in the backing buffer.
    size_t packet_size = 20;  ///< First packet length.
    size_t boundary = video::pyrowave::packet_boundary;  ///< Expected native packetization boundary.
    int encoded = 0;  ///< Number of codec calls, including repeated frames.
    int gpu_encoded = 0;  ///< Number of GPU encoding calls.
    pyrowave_result share_result = PYROWAVE_SUCCESS;  ///< Shared device initialization result.
    pyrowave_device_create_info shared_info {};  ///< Borrowed Vulkan device metadata.
    VkCommandBuffer command_buffer {};  ///< Command buffer currently receiving encoding commands.
    pyrowave_gpu_buffers gpu_input {};  ///< Last GPU component views.
    float scaling_factor = 0.0f;  ///< Last GPU sample normalization.
    size_t budget = 0;  ///< Last requested bitrate budget.
    pyrowave_cpu_buffer input {};  ///< Last codec input layout and borrowed plane addresses.
    pyrowave_chroma_subsampling chroma {};  ///< Chroma sampling used to create the encoder.
    std::optional<std::array<uint16_t, 3>> solid_color;  ///< Expected uniform YUV values instead of the checkerboard.
    std::array<uint16_t, 4> samples {};  ///< First two luma samples and first Cb/Cr samples.
    std::vector<std::string> destroyed;  ///< Resource destruction order.
  };

  thread_local pyrowave_api_state_t *api_state;  ///< State for the current synchronous fake API test.

  /**
   * @brief Build deterministic C API callbacks without a Vulkan driver.
   * @return Injectable C API table.
   */
  video::pyrowave::api_t fake_pyrowave_api() {
    video::pyrowave::api_t api;
    api.share_device = nullptr;
    api.match_device = nullptr;
    api.get_api_version = [](uint32_t *major, uint32_t *minor, uint32_t *patch) {
      *major = api_state->major;
      *minor = PYROWAVE_API_VERSION_MINOR;
      *patch = PYROWAVE_API_VERSION_PATCH;
    };
    api.create_device = [](pyrowave_device *device) {
      if (api_state->device_result == PYROWAVE_SUCCESS) {
        *device = reinterpret_cast<pyrowave_device>(api_state);
      }
      return api_state->device_result;
    };
    api.destroy_device = [](pyrowave_device) {
      api_state->destroyed.emplace_back("device");
    };
    api.create_encoder = [](const pyrowave_encoder_create_info *info, pyrowave_encoder *encoder) {
      EXPECT_EQ(info->width, 64);
      EXPECT_EQ(info->height, 48);
      api_state->chroma = info->chroma;
      if (api_state->encoder_result == PYROWAVE_SUCCESS) {
        *encoder = reinterpret_cast<pyrowave_encoder>(api_state);
      }
      return api_state->encoder_result;
    };
    api.destroy_encoder = [](pyrowave_encoder) {
      api_state->destroyed.emplace_back("encoder");
    };
    api.encode_cpu = [](pyrowave_encoder, const pyrowave_cpu_buffer *input, const pyrowave_rate_control *rate) {
      ++api_state->encoded;
      api_state->budget = rate->maximum_bitstream_size;
      api_state->input = *input;
      EXPECT_EQ(input->width, 64);
      EXPECT_EQ(input->height, 48);
      const bool hdr = input->format == PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10 || input->format == PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10;
      const bool nv12 = input->format == PYROWAVE_CPU_BUFFER_FORMAT_NV12;
      const bool subsampled = api_state->chroma == PYROWAVE_CHROMA_SUBSAMPLING_420;
      for (int component = 0; component < 3; ++component) {
        const auto plane = nv12 && component ? 1 : component;
        const auto width = component && subsampled ? 32 : 64;
        const auto height = component && subsampled ? 24 : 48;
        EXPECT_GE(input->row_stride_in_bytes[plane], width * (hdr || (nv12 && component) ? 2 : 1));
        EXPECT_EQ(input->plane_size_in_bytes[plane], input->row_stride_in_bytes[plane] * height);
        for (int y = 0; y < height; ++y) {
          const auto row = static_cast<uint8_t *>(input->data[plane]) + y * input->row_stride_in_bytes[plane];
          for (int x = 0; x < width; ++x) {
            const auto value = hdr ? reinterpret_cast<const uint16_t *>(row)[x] : row[nv12 && component ? 2 * x + (component == 2) : x];
            const auto expected = api_state->solid_color ? (*api_state->solid_color)[component] :
                                                           ((x + (component ? y : 0) + (component == 2)) % 2 ? (hdr ? 1023 : 255) : 1);
            EXPECT_EQ(value, expected) << "plane " << plane << ", x " << x << ", y " << y;
          }
        }
      }
      if (nv12) {
        EXPECT_EQ(input->data[2], nullptr);
        EXPECT_EQ(input->row_stride_in_bytes[2], 0);
        EXPECT_EQ(input->plane_size_in_bytes[2], 0);
        api_state->samples = {static_cast<uint8_t *>(input->data[0])[0], static_cast<uint8_t *>(input->data[0])[1], static_cast<uint8_t *>(input->data[1])[0], static_cast<uint8_t *>(input->data[1])[1]};
      } else if (hdr) {
        api_state->samples = {static_cast<uint16_t *>(input->data[0])[0], static_cast<uint16_t *>(input->data[0])[1], static_cast<uint16_t *>(input->data[1])[0], static_cast<uint16_t *>(input->data[2])[0]};
      } else {
        api_state->samples = {static_cast<uint8_t *>(input->data[0])[0], static_cast<uint8_t *>(input->data[0])[1], static_cast<uint8_t *>(input->data[1])[0], static_cast<uint8_t *>(input->data[2])[0]};
      }
      return api_state->encode_result;
    };
    api.num_packets = [](pyrowave_encoder, size_t boundary, size_t *count) {
      if (api_state->boundary == video::pyrowave::packet_boundary && boundary == video::pyrowave::record_packet_boundary) {
        *count = api_state->storage_count;
        return api_state->storage_result;
      }
      EXPECT_EQ(boundary, api_state->boundary);
      *count = api_state->count;
      return api_state->count_result;
    };
    api.packetize = [](pyrowave_encoder, pyrowave_packet *packets, size_t, size_t *count, void *data, size_t size) {
      EXPECT_GE(size, video::pyrowave::record_packet_boundary);
      packets[0] = {api_state->packet_offset, api_state->packet_size};
      if (api_state->count > 1) {
        packets[1] = {32, 12};
      }
      auto bytes = static_cast<uint8_t *>(data);
      AV_WL32(bytes + 4, 0x80000000u | 63 | (47 << 14));
      AV_WL32(bytes + 8, 2 | (api_state->chroma == PYROWAVE_CHROMA_SUBSAMPLING_444 ? 1 << 26 : 0));
      AV_WL32(bytes + 12, 3 << 16);
      AV_WL32(bytes + 16, 0);
      AV_WL32(bytes + 20, 0x11223344);
      AV_WL32(bytes + 32, 3 << 16);
      AV_WL32(bytes + 36, 12 << 8);
      AV_WL32(bytes + 40, 0x55667788);
      *count = api_state->output_count;
      return api_state->packet_result;
    };
    api.active_blocks = [](pyrowave_encoder, int bands, size_t *blocks) {
      EXPECT_EQ(bands, 2);
      *blocks = 12;
      return api_state->blocks_result;
    };
    return api;
  }

#ifdef SUNSHINE_BUILD_VULKAN
  /**
   * @brief Replace only codec commands while exercising real Vulkan frame submission and synchronization.
   * @return Codec entry points supporting shared Vulkan inputs.
   */
  video::pyrowave::api_t fake_gpu_pyrowave_api() {
    auto api = fake_pyrowave_api();
    api.share_device = [](const pyrowave_device_create_info *info, pyrowave_device *device) {
      api_state->shared_info = *info;
      if (api_state->share_result == PYROWAVE_SUCCESS) {
        *device = reinterpret_cast<pyrowave_device>(api_state);
      }
      return api_state->share_result;
    };
    api.set_queue_type = [](pyrowave_device, VkQueueFlagBits queue) {
      EXPECT_EQ(queue, VK_QUEUE_COMPUTE_BIT);
      return PYROWAVE_SUCCESS;
    };
    api.set_command_buffer = [](pyrowave_device, VkCommandBuffer command) {
      api_state->command_buffer = command;
    };
    api.set_scaling_factor = [](pyrowave_encoder, float factor) {
      api_state->scaling_factor = factor;
    };
    api.encode_gpu = [](pyrowave_encoder, const pyrowave_gpu_sync_operation *acquire, const pyrowave_gpu_sync_operation *release, const pyrowave_gpu_buffers *buffers, const pyrowave_rate_control *rate) {
      EXPECT_EQ(acquire, nullptr);
      EXPECT_EQ(release, nullptr);
      EXPECT_NE(api_state->command_buffer, VK_NULL_HANDLE);
      api_state->gpu_input = *buffers;
      api_state->budget = rate->maximum_bitstream_size;
      ++api_state->gpu_encoded;
      return api_state->encode_result;
    };
    return api;
  }
#endif
}  // namespace

/**
 * @brief Reset fake C API state between independent encoder tests.
 */
class PyrowaveSessionTest: public testing::Test {
protected:
  /**
   * @brief Bind this test's state to the fake C callbacks.
   */
  void SetUp() override {
    api_state = &state;
  }

  /**
   * @brief Clear the callback state after each test.
   */
  void TearDown() override {
    api_state = nullptr;
  }

  pyrowave_api_state_t state;  ///< Per-test codec status and observations.
};

TEST_F(PyrowaveSessionTest, PreservesFrameTimingAndStreamOwnershipInThePacketQueue) {
  auto session = video::pyrowave::make_session(pyrowave_config(), std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12), fake_pyrowave_api());
  ASSERT_NE(session, nullptr);
  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto packets = mailbox->queue<video::packet_t>("pyrowave-test");
  auto stream = reinterpret_cast<stream::session_t *>(&state);
  const auto timestamp = std::chrono::steady_clock::now();
  EXPECT_EQ(video::encode(1, *session, packets, stream, timestamp), -1);
  EXPECT_FALSE(packets->peek());
  platf::img_t image {};
  ASSERT_EQ(session->convert(image), 0);
  ASSERT_EQ(video::encode(42, *session, packets, stream, timestamp), 0);
  ASSERT_TRUE(packets->peek());
  auto packet = packets->pop();
  EXPECT_EQ(packet->frame_index(), 42);
  EXPECT_EQ(packet->channel_data, stream);
  EXPECT_EQ(packet->frame_timestamp, timestamp);
  ASSERT_EQ(video::encode(43, *session, packets, nullptr, {}), 0);
  ASSERT_TRUE(packets->peek());
  packet = packets->pop();
  EXPECT_FALSE(packet->frame_timestamp.has_value());
  EXPECT_EQ(packet->channel_data, nullptr);
}

/**
 * @brief Attach missing VideoToolbox pool references while preserving existing references and lifetimes.
 */
TEST_F(PyrowaveSessionTest, RetainsTheBorrowedHardwarePoolUntilAfterConverterDestruction) {
  const std::pair<AVPixelFormat, bool> cases[] {
    {AV_PIX_FMT_NV12, false},
    {AV_PIX_FMT_VIDEOTOOLBOX, false},
    {AV_PIX_FMT_VIDEOTOOLBOX, true},
  };
  for (const auto &[format, has_pool] : cases) {
    bool released = false;
    AVHWFramesContext pool {};
    pool.sw_format = AV_PIX_FMT_NV12;
    video::avcodec_buffer_t frames {av_buffer_create(reinterpret_cast<uint8_t *>(&pool), sizeof(pool), [](void *opaque, uint8_t *) {
      *static_cast<bool *>(opaque) = true;
    },
                                                     &released,
                                                     0)};
    ASSERT_NE(frames, nullptr);
    auto converter = std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12);
    const auto frame = converter->frame;
    frame->format = format;
    if (has_pool) {
      frame->hw_frames_ctx = av_buffer_ref(frames.get());
      ASSERT_NE(frame->hw_frames_ctx, nullptr);
    }
    const auto original_pool = frame->hw_frames_ctx;
    const auto pool_buffer = frames->buffer;
    converter->on_destroy = [&]() {
      EXPECT_FALSE(released);
    };
    auto session = video::pyrowave::make_session(pyrowave_config(), std::move(converter), fake_pyrowave_api(), std::move(frames));
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(released);
    if (format == AV_PIX_FMT_VIDEOTOOLBOX) {
      ASSERT_NE(frame->hw_frames_ctx, nullptr);
      EXPECT_EQ(frame->hw_frames_ctx->buffer, pool_buffer);
      EXPECT_EQ(av_buffer_get_ref_count(frame->hw_frames_ctx), 2);
      if (has_pool) {
        EXPECT_EQ(frame->hw_frames_ctx, original_pool);
      }
    } else {
      EXPECT_EQ(frame->hw_frames_ctx, nullptr);
      platf::img_t image {};
      EXPECT_EQ(session->convert(image), 0);
    }
    EXPECT_FALSE(released);
    session.reset();
    EXPECT_TRUE(released);
  }
}

/**
 * @brief Reuse compatible planes, retain repeated frames, and convert other layouts without losing samples.
 */
TEST_F(PyrowaveSessionTest, EncodesSdrAndHdrWithExactPlaneValuesAndFraming) {
  const AVPixelFormat formats[] {AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P16, AV_PIX_FMT_VUYX, AV_PIX_FMT_XV30};
  for (auto format : formats) {
    for (int storage = 0; storage < 3; ++storage) {
      SCOPED_TRACE(av_get_pix_fmt_name(format));
      SCOPED_TRACE(storage);
      const auto desc = av_pix_fmt_desc_get(format);
      const bool hdr = desc->comp[0].depth > 8;
      const bool yuv444 = desc->log2_chroma_w == 0;
      auto config = pyrowave_config();
      config.dynamicRange = hdr;
      config.chromaSamplingType = yuv444;
      auto converter = std::make_unique<pyrowave_test_converter_t>(format, storage == 1);
      const auto raw = converter.get();
      video::avcodec_frame_t owner;
      if (storage == 2) {
        owner.reset(av_frame_clone(raw->frame));
        ASSERT_NE(owner, nullptr);
        for (auto &buffer : raw->frame->buf) {
          av_buffer_unref(&buffer);
        }
      }
      auto session = video::pyrowave::make_session(config, std::move(converter), fake_pyrowave_api());
      ASSERT_NE(session, nullptr);
      EXPECT_EQ(session->encode(1), nullptr);
      platf::img_t image {};
      ASSERT_EQ(session->convert(image), 0);
      session->request_idr_frame();
      session->request_normal_frame();
      session->invalidate_ref_frames(1, 2);
      auto packet = session->encode(42);
      ASSERT_NE(packet, nullptr);
      EXPECT_TRUE(packet->is_idr());
      EXPECT_EQ(packet->frame_index(), 42);
      ASSERT_EQ(packet->data_size(), 44);
      EXPECT_EQ(AV_RL32(packet->data()), 2);
      EXPECT_EQ(AV_RL32(packet->data() + 4), 20);
      EXPECT_EQ(AV_RL32(packet->data() + 8), 0x80000000u | 63 | (47 << 14));
      EXPECT_EQ(AV_RL32(packet->data() + 24), 0x11223344);
      EXPECT_EQ(AV_RL32(packet->data() + 28), 12);
      EXPECT_EQ(AV_RL32(packet->data() + 40), 0x55667788);
      EXPECT_EQ(packet->pyrowave_critical_packets, 0);
      EXPECT_TRUE(packet->pyrowave_record_starts.empty());
      EXPECT_EQ(state.budget, 416666);
      EXPECT_EQ(state.chroma, yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420);
      EXPECT_EQ(state.input.format, format == AV_PIX_FMT_NV12 ? PYROWAVE_CPU_BUFFER_FORMAT_NV12 : yuv444 ? (hdr ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV444P) :
                                                                                                           (hdr ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P));
      const bool direct = storage == 0 && (format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUV420P10 || format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUV444P10);
      if (direct) {
        for (int plane = 0; plane < av_pix_fmt_count_planes(format); ++plane) {
          EXPECT_EQ(state.input.data[plane], raw->frame->data[plane]);
          EXPECT_EQ(state.input.row_stride_in_bytes[plane], raw->frame->linesize[plane]);
        }
        EXPECT_EQ(av_buffer_get_ref_count(raw->frame->buf[0]), 2);
      } else {
        EXPECT_NE(state.input.data[0], raw->frame->data[0]);
      }
      EXPECT_EQ(state.samples, (std::array<uint16_t, 4> {1, uint16_t(hdr ? 1023 : 255), 1, uint16_t(hdr ? 1023 : 255)}));
      // Repeated capture content must be encoded again so the codec sequence advances.
      ASSERT_EQ(session->convert(image), 0);
      raw->owned.reset();
      owner.reset();
      EXPECT_NE(session->encode(43), nullptr);
      session.reset();
      EXPECT_EQ(state.destroyed, (std::vector<std::string> {"encoder", "device"}));
      state.destroyed.clear();
    }
  }
  EXPECT_EQ(state.encoded, 6 * std::size(formats));
}

/**
 * @brief Keep the codec's cached upload layout stable when a frame needs scaling.
 */
TEST_F(PyrowaveSessionTest, KeepsTheInputLayoutWhenScalingFrames) {
  state.solid_color = std::array<uint16_t, 3> {128, 128, 128};
  for (auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV444P}) {
    SCOPED_TRACE(av_get_pix_fmt_name(format));
    auto converter = std::make_unique<pyrowave_test_converter_t>(format);
    const auto frame = converter->frame;
    std::memset(frame->buf[0]->data, 128, frame->buf[0]->size);
    auto config = pyrowave_config();
    config.chromaSamplingType = format == AV_PIX_FMT_YUV444P;
    auto session = video::pyrowave::make_session(config, std::move(converter), fake_pyrowave_api());
    ASSERT_NE(session, nullptr);
    platf::img_t image {};
    for (int scale : {1, 2, 1}) {
      frame->width = 64 / scale;
      frame->height = 48 / scale;
      ASSERT_EQ(session->convert(image), 0);
      ASSERT_NE(session->encode(scale), nullptr);
      EXPECT_EQ(state.input.format, format == AV_PIX_FMT_NV12 ? PYROWAVE_CPU_BUFFER_FORMAT_NV12 : format == AV_PIX_FMT_YUV420P ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P :
                                                                                                                                 PYROWAVE_CPU_BUFFER_FORMAT_YUV444P);
      EXPECT_EQ(state.input.data[0] == frame->data[0], scale == 1);
    }
  }
}

/**
 * @brief Preserve SDR/HDR samples when reading Vulkan frames or borrowed macOS capture buffers.
 */
TEST_F(PyrowaveSessionTest, DownloadsHardwareFramesWithoutLosingTenBitPrecision) {
#ifdef __APPLE__
  constexpr auto device_type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
  constexpr auto hardware_format = AV_PIX_FMT_VIDEOTOOLBOX;
  const AVPixelFormat formats[] {AV_PIX_FMT_NV12, AV_PIX_FMT_P010};
#else
  constexpr auto device_type = AV_HWDEVICE_TYPE_VULKAN;
  constexpr auto hardware_format = AV_PIX_FMT_VULKAN;
  const AVPixelFormat formats[] {AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P16};
#endif
  AVBufferRef *raw_device = nullptr;
  if (av_hwdevice_ctx_create(&raw_device, device_type, nullptr, nullptr, 0) < 0) {
    GTEST_SKIP() << "An FFmpeg-compatible " << av_hwdevice_get_type_name(device_type) << " device is unavailable";
  }
  video::avcodec_buffer_t device {raw_device};
  for (auto format : formats) {
    SCOPED_TRACE(av_get_pix_fmt_name(format));
    const auto desc = av_pix_fmt_desc_get(format);
    const bool hdr = desc->comp[0].depth > 8;
    auto config = pyrowave_config();
    config.dynamicRange = hdr;
    config.chromaSamplingType = desc->log2_chroma_w == 0;
    auto converter = std::make_unique<pyrowave_test_converter_t>(format);
    auto host = std::move(converter->owned);
    converter->owned.reset(av_frame_alloc());
    converter->frame = converter->owned.get();
    ASSERT_NE(converter->frame, nullptr);
    video::avcodec_buffer_t frames {av_hwframe_ctx_alloc(device.get())};
    ASSERT_NE(frames, nullptr);
    auto pool = reinterpret_cast<AVHWFramesContext *>(frames->data);
    pool->format = hardware_format;
    pool->sw_format = format;
    pool->width = 64;
    pool->height = 48;
    ASSERT_EQ(av_hwframe_ctx_init(frames.get()), 0);
#ifdef __APPLE__
    CVPixelBufferRef pixel_buffer = nullptr;
    ASSERT_EQ(CVPixelBufferCreate(kCFAllocatorDefault, 64, 48, hdr ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pixel_buffer), kCVReturnSuccess);
    auto release_buffer = util::fail_guard([&]() {
      CVPixelBufferRelease(pixel_buffer);
    });
    converter->frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(pixel_buffer), 0, [](void *, uint8_t *data) {
      CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data));
    },
                                                nullptr,
                                                0);
    ASSERT_NE(converter->frame->buf[0], nullptr);
    release_buffer.disable();
    converter->frame->data[3] = reinterpret_cast<uint8_t *>(pixel_buffer);
    converter->frame->format = hardware_format;
    converter->frame->width = 64;
    converter->frame->height = 48;
    converter->frame->hw_frames_ctx = av_buffer_ref(frames.get());
    ASSERT_NE(converter->frame->hw_frames_ctx, nullptr);
#else
    ASSERT_EQ(av_hwframe_get_buffer(frames.get(), converter->frame, 0), 0);
#endif
    ASSERT_EQ(av_hwframe_transfer_data(converter->frame, host.get(), 0), 0);
    ASSERT_EQ(av_frame_copy_props(converter->frame, host.get()), 0);
#ifdef __APPLE__
    // Captured CVPixelBuffers arrive without an FFmpeg frame-pool reference.
    av_buffer_unref(&converter->frame->hw_frames_ctx);
#endif
    auto session = video::pyrowave::make_session(config, std::move(converter), fake_pyrowave_api(), std::move(frames));
    ASSERT_NE(session, nullptr);
    platf::img_t image {};
    for (int frame = 1; frame <= 2; ++frame) {
      ASSERT_EQ(session->convert(image), 0);
      ASSERT_NE(session->encode(frame), nullptr);
      EXPECT_EQ(state.samples, (std::array<uint16_t, 4> {1, uint16_t(hdr ? 1023 : 255), 1, uint16_t(hdr ? 1023 : 255)}));
    }
    pool->sw_format = AV_PIX_FMT_NONE;
    EXPECT_EQ(session->convert(image), -1);
    EXPECT_EQ(session->encode(3), nullptr);
    pool->sw_format = format;
  }
}

TEST_F(PyrowaveSessionTest, ClearsReadinessAfterConversionFailureAndRejectsWrongDepthOrChroma) {
  auto converter = std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12);
  auto raw = converter.get();
  auto session = video::pyrowave::make_session(pyrowave_config(), std::move(converter), fake_pyrowave_api());
  ASSERT_NE(session, nullptr);
  platf::img_t image {};
  ASSERT_EQ(session->convert(image), 0);
  raw->status = -1;
  EXPECT_EQ(session->convert(image), -1);
  EXPECT_EQ(session->encode(1), nullptr);
  raw->status = 0;
  for (auto format : {AV_PIX_FMT_RGB24, AV_PIX_FMT_P010, AV_PIX_FMT_YUV444P}) {
    raw->frame->format = format;
    EXPECT_EQ(session->convert(image), -1);
    EXPECT_EQ(session->encode(1), nullptr);
  }
  raw->frame->format = AV_PIX_FMT_NV12;
  raw->frame->width = 0;
  EXPECT_EQ(session->convert(image), -1);
  EXPECT_EQ(session->encode(1), nullptr);
  raw->frame->width = 64;
  const auto restore_allocations = util::fail_guard([]() {
    av_max_alloc(std::numeric_limits<int>::max());
  });
  av_max_alloc(0);
  EXPECT_EQ(session->convert(image), -1);
  EXPECT_EQ(session->encode(1), nullptr);
  EXPECT_EQ(state.encoded, 0);
}

/**
 * @brief Reject invalid frames and missing or unretainable VideoToolbox pools before creating a codec.
 */
TEST_F(PyrowaveSessionTest, RejectsInvalidInputsBeforeCallingTheCodec) {
  EXPECT_EQ(video::pyrowave::make_session(pyrowave_config(), nullptr, fake_pyrowave_api()), nullptr);
  EXPECT_EQ(video::pyrowave::make_session(pyrowave_config(), std::make_unique<platf::avcodec_encode_device_t>(), fake_pyrowave_api()), nullptr);
  for (int dimension : {32, 96}) {
    auto converter = std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12);
    converter->frame->width = dimension;
    EXPECT_EQ(video::pyrowave::make_session(pyrowave_config(), std::move(converter), fake_pyrowave_api()), nullptr);
  }
  auto config = pyrowave_config();
  config.chromaSamplingType = 2;
  EXPECT_EQ(video::pyrowave::make_session(config, std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12), fake_pyrowave_api()), nullptr);

  auto converter = std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12);
  converter->frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
  EXPECT_EQ(video::pyrowave::make_session(pyrowave_config(), std::move(converter), fake_pyrowave_api()), nullptr);

  converter = std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12);
  converter->frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
  video::avcodec_buffer_t frames {av_buffer_alloc(1)};
  ASSERT_NE(frames, nullptr);
  const auto restore_allocations = util::fail_guard([]() {
    av_max_alloc(std::numeric_limits<int>::max());
  });
  av_max_alloc(0);
  EXPECT_EQ(video::pyrowave::make_session(pyrowave_config(), std::move(converter), fake_pyrowave_api(), std::move(frames)), nullptr);
  EXPECT_TRUE(state.destroyed.empty());
}

TEST_F(PyrowaveSessionTest, CleansUpPartialInitializationAndRejectsAbiMismatch) {
  for (int failure = 0; failure < 3; ++failure) {
    state = {};
    if (failure == 0) {
      state.major = 99;
    } else if (failure == 1) {
      state.device_result = PYROWAVE_ERROR_NO_VULKAN;
    } else {
      state.encoder_result = PYROWAVE_ERROR_GENERIC;
    }
    EXPECT_EQ(video::pyrowave::make_session(pyrowave_config(), std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12), fake_pyrowave_api()), nullptr);
    EXPECT_EQ(state.destroyed, failure == 2 ? std::vector<std::string> {"device"} : std::vector<std::string> {});
  }
}

TEST_F(PyrowaveSessionTest, FailsClosedOnEncodeAndPacketizationErrors) {
  auto session = video::pyrowave::make_session(pyrowave_config(), std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12), fake_pyrowave_api());
  ASSERT_NE(session, nullptr);
  platf::img_t image {};
  ASSERT_EQ(session->convert(image), 0);
  for (int failure = 0; failure < 15; ++failure) {
    state = {};
    switch (failure) {
      case 0:
        state.encode_result = PYROWAVE_ERROR_GENERIC;
        break;
      case 1:
        state.count_result = PYROWAVE_ERROR_GENERIC;
        break;
      case 2:
        state.count = 0;
        break;
      case 3:
        state.count = 65536;
        break;
      case 4:
        state.count = video::pyrowave::max_frame_size / video::pyrowave::packet_boundary + 1;
        break;
      case 5:
        state.packet_result = PYROWAVE_ERROR_GENERIC;
        break;
      case 6:
        state.output_count = 0;
        break;
      case 7:
        state.output_count = 3;
        break;
      case 8:
        state.packet_size = 0;
        break;
      case 9:
        state.packet_size = video::pyrowave::record_packet_boundary + 4;
        break;
      case 10:
        state.packet_offset = std::numeric_limits<size_t>::max();
        break;
      case 11:
        state.packet_offset = video::pyrowave::record_packet_boundary - 4;
        break;
      case 12:
        state.storage_result = PYROWAVE_ERROR_GENERIC;
        break;
      case 13:
        state.storage_count = 0;
        break;
      case 14:
        state.storage_count = video::pyrowave::max_frame_size / video::pyrowave::record_packet_boundary + 1;
        break;
    }
    EXPECT_EQ(session->encode(failure), nullptr) << failure;
  }
  auto config = pyrowave_config();
  config.pyrowaveRecordFraming = true;
  state = {};
  state.boundary = video::pyrowave::record_packet_boundary;
  session = video::pyrowave::make_session(config, std::make_unique<pyrowave_test_converter_t>(AV_PIX_FMT_NV12), fake_pyrowave_api());
  ASSERT_NE(session, nullptr);
  ASSERT_EQ(session->convert(image), 0);
  state.blocks_result = PYROWAVE_ERROR_GENERIC;
  EXPECT_EQ(session->encode(42), nullptr);
  state.blocks_result = PYROWAVE_SUCCESS;
  auto records = session->encode(43);
  ASSERT_NE(records, nullptr);
  EXPECT_EQ(AV_RL32(records->data()), 0x80000000u | 63 | (47 << 14));
  EXPECT_EQ(records->pyrowave_critical_packets, 1);
  EXPECT_EQ(records->pyrowave_record_starts, (std::vector<bool> {true, true}));
}

/**
 * @brief Run integration tests only when the real device supports PyroWave's encoder shaders.
 */
class PyrowaveRoundTripTest: public testing::TestWithParam<std::pair<AVHWDeviceType, AVPixelFormat>> {
protected:
  /**
   * @brief Check shader capabilities, which are stricter than Vulkan device creation.
   */
  void SetUp() override {
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
      GTEST_SKIP() << "A PyroWave-compatible Vulkan device is unavailable";
    }
    pyrowave_encoder encoder {};
    const pyrowave_encoder_create_info info {device, 64, 48, PYROWAVE_CHROMA_SUBSAMPLING_420};
    if (pyrowave_encoder_create(&info, &encoder) != PYROWAVE_SUCCESS) {
      GTEST_SKIP() << "The Vulkan device does not support PyroWave's encoder shaders";
    }
    pyrowave_encoder_destroy(encoder);
  }

  /**
   * @brief Release the codec device after the test or a skipped capability probe.
   */
  void TearDown() override {
    if (device) {
      pyrowave_device_destroy(device);
    }
  }

  pyrowave_device device {};  ///< Real Vulkan codec device retained for decoder tests.
};

TEST_F(PyrowaveSessionTest, ConvertsCapturedRgbThroughTheSoftwareBackend) {
  state.solid_color = std::array<uint16_t, 3> {127, 128, 128};
  std::vector<uint8_t> pixels(128 * 96 * 4, 127);
  platf::img_t image {};
  image.data = pixels.data();
  image.width = 128;
  image.height = 96;
  image.pixel_pitch = 4;
  image.row_pitch = 128 * 4;
  for (int chroma : {0, 1}) {
    auto config = pyrowave_config();
    config.chromaSamplingType = chroma;
    auto converter = std::make_unique<platf::avcodec_encode_device_t>();
    converter->colorspace = video::colorspace_from_client_config(config, false);
    auto session = video::make_pyrowave_encode_session(video::software, config, 128, 96, std::move(converter), fake_pyrowave_api());
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->convert(image), 0);
    auto pyrowave_session = dynamic_cast<video::pyrowave::session_t *>(session.get());
    ASSERT_NE(pyrowave_session, nullptr);
    EXPECT_NE(pyrowave_session->encode(1), nullptr);
    EXPECT_EQ(state.chroma, chroma ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420);
  }
}

#if defined(SUNSHINE_BUILD_VULKAN) || defined(_WIN32)
namespace {
  /**
   * @brief Upload prepared pixels through a borrowed hardware frame pool, like platform converters.
   */
  class pyrowave_test_upload_t: public pyrowave_test_converter_t {
  public:
    /**
     * @brief Prepare SDR or HDR host pixels for upload.
     * @param format Input pixel layout.
     */
    explicit pyrowave_test_upload_t(AVPixelFormat format):
        pyrowave_test_converter_t(format) {
      hardware = true;
    }

    /**
     * @brief Request the same image usage as the Vulkan video converter.
     * @param frames Hardware frame configuration.
     */
    void init_hwframes(AVHWFramesContext *frames) override {
  #ifdef SUNSHINE_BUILD_VULKAN
      if (frames->format == AV_PIX_FMT_VULKAN) {
        auto vk = static_cast<AVVulkanFramesContext *>(frames->hwctx);
        vk->usage = static_cast<VkImageUsageFlagBits>(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
      }
  #endif
    }

    /**
     * @brief Take ownership of the converted frame and borrow its pool.
     * @param new_frame Empty encoder input frame.
     * @param hw_frames_ctx Retained by the session.
     * @return Zero on success.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) override {
      input = std::move(owned);
      owned.reset(new_frame);
      frame = owned.get();
      frames = hw_frames_ctx;
      EXPECT_EQ(frame->color_trc, AVCOL_TRC_SMPTE2084);
  #ifdef SUNSHINE_BUILD_VULKAN
      const auto pool = reinterpret_cast<AVHWFramesContext *>(frames->data);
      if (pool->format == AV_PIX_FMT_VULKAN) {
        const auto vk = static_cast<AVVulkanFramesContext *>(pool->hwctx);
        EXPECT_TRUE(vk->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        EXPECT_FALSE(vk->usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
      }
  #endif
      return 0;
    }

    /**
     * @brief Allocate lazily from the borrowed pool and upload the prepared image.
     * @param img Unused captured image.
     * @return FFmpeg transfer status.
     */
    int convert(platf::img_t &img) override {
      if (!frame->buf[0] && av_hwframe_get_buffer(frames, frame, 0) < 0) {
        return -1;
      }
      return av_hwframe_transfer_data(frame, input.get(), 0);
    }

  private:
    video::avcodec_frame_t input;  ///< Prepared host pixels.
    AVBufferRef *frames = nullptr;  ///< Borrowed pool used after session creation returns.
  };
}  // namespace
#endif

#ifdef SUNSHINE_BUILD_VULKAN
TEST_F(PyrowaveSessionTest, CreatesSessionsWithoutHardwareVideoEncodeUsage) {
  AVBufferRef *raw_device = nullptr;
  if (av_hwdevice_ctx_create(&raw_device, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) < 0) {
    GTEST_SKIP() << "An FFmpeg-compatible Vulkan device is unavailable";
  }
  video::avcodec_buffer_t device {raw_device};
  video::encoder_t encoder {
    .platform_formats = std::make_unique<video::encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VULKAN,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VULKAN,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P16,
      [&](platf::avcodec_encode_device_t *) -> util::Either<video::avcodec_buffer_t, int> {
        return video::avcodec_buffer_t {av_buffer_ref(device.get())};
      }
    )
  };
  for (auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P16}) {
    SCOPED_TRACE(av_get_pix_fmt_name(format));
    const auto desc = av_pix_fmt_desc_get(format);
    auto config = pyrowave_config();
    config.dynamicRange = desc->comp[0].depth > 8;
    config.chromaSamplingType = desc->log2_chroma_w == 0;
    for (const std::string_view mode : {"cpu", "gpu", "shared Vulkan device", "device failure", "encoder failure", "frame fallback"}) {
      SCOPED_TRACE(mode);
      state = {};
      auto api = mode == "cpu" ? fake_pyrowave_api() : fake_gpu_pyrowave_api();
      const bool gpu = mode == "gpu" || mode == "shared Vulkan device";
      if (mode == "shared Vulkan device") {
        api.share_device = [](const pyrowave_device_create_info *info, pyrowave_device *device) {
          api_state->shared_info = *info;
          return pyrowave_create_device(info, device);
        };
        api.create_device = [](pyrowave_device *) {
          return PYROWAVE_ERROR_GENERIC;
        };
        api.destroy_device = [](pyrowave_device device) {
          pyrowave_device_destroy(device);
          api_state->destroyed.emplace_back("device");
        };
        api.set_queue_type = pyrowave_device_set_queue_type;
      } else if (mode == "device failure") {
        state.share_result = PYROWAVE_ERROR_NO_VULKAN;
      } else if (mode == "encoder failure") {
        state.encoder_result = PYROWAVE_ERROR_GENERIC;
        api.create_encoder = [](const pyrowave_encoder_create_info *info, pyrowave_encoder *encoder) {
          const auto result = fake_pyrowave_api().create_encoder(info, encoder);
          *encoder = reinterpret_cast<pyrowave_encoder>(api_state);
          api_state->encoder_result = PYROWAVE_SUCCESS;
          return result;
        };
      }
      auto converter = std::make_unique<pyrowave_test_upload_t>(format);
      const auto upload = converter.get();
      auto session = video::make_pyrowave_encode_session(encoder, config, 64, 48, std::move(converter), api);
      ASSERT_NE(session, nullptr);
      if (gpu) {
        const auto context = reinterpret_cast<AVHWDeviceContext *>(device->data);
        EXPECT_EQ(state.shared_info.device, static_cast<AVVulkanDeviceContext *>(context->hwctx)->act_dev);
      }
      platf::img_t image {};
      if (mode == "frame fallback") {
        ASSERT_EQ(upload->convert(image), 0);
        auto pool = reinterpret_cast<AVHWFramesContext *>(upload->frame->hw_frames_ctx->data);
        auto vk_frames = static_cast<AVVulkanFramesContext *>(pool->hwctx);
        vk_frames->usage = VkImageUsageFlagBits(vk_frames->usage & ~VK_IMAGE_USAGE_SAMPLED_BIT);
      }
      ASSERT_EQ(session->convert(image), 0);
      auto pyrowave_session = dynamic_cast<video::pyrowave::session_t *>(session.get());
      ASSERT_NE(pyrowave_session, nullptr);
      auto pixels = reinterpret_cast<AVVkFrame *>(upload->frame->data[0]);
      const auto initial_timeline = pixels->sem_value[0];
      for (int i = 1; i <= 2; ++i) {
        EXPECT_NE(pyrowave_session->encode(i), nullptr);
        if (gpu) {
          EXPECT_EQ(state.encoded, 0);
          EXPECT_EQ(state.gpu_encoded, i);
          EXPECT_EQ(state.command_buffer, VK_NULL_HANDLE);
          EXPECT_EQ(state.gpu_input.planes[0].image, pixels->img[0]);
          EXPECT_EQ(pixels->sem_value[0], initial_timeline + i);
          for (int plane = 0; plane < AV_NUM_DATA_POINTERS && pixels->img[plane]; ++plane) {
            EXPECT_EQ(pixels->layout[plane], VK_IMAGE_LAYOUT_GENERAL);
            uint64_t completed = 0;
            EXPECT_EQ(vkGetSemaphoreCounterValue(state.shared_info.device, pixels->sem[plane], &completed), VK_SUCCESS);
            EXPECT_EQ(completed, pixels->sem_value[plane]);
          }
        } else {
          EXPECT_EQ(state.encoded, i);
          EXPECT_EQ(state.gpu_encoded, 0);
        }
      }
      EXPECT_EQ(state.chroma, config.chromaSamplingType ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420);
      if (gpu) {
        const auto timeline = pixels->sem_value[0];
        const auto semaphore = pixels->sem[0];
        const auto calls = state.gpu_encoded;
        pixels->sem[0] = VK_NULL_HANDLE;
        EXPECT_EQ(pyrowave_session->encode(3), nullptr);
        EXPECT_EQ(state.gpu_encoded, calls);
        pixels->sem[0] = semaphore;
        state.encode_result = PYROWAVE_ERROR_GENERIC;
        EXPECT_EQ(pyrowave_session->encode(3), nullptr);
        EXPECT_EQ(state.command_buffer, VK_NULL_HANDLE);
        EXPECT_EQ(pixels->sem_value[0], timeline);
        state.encode_result = PYROWAVE_SUCCESS;
        ASSERT_EQ(session->convert(image), 0);
        EXPECT_NE(pyrowave_session->encode(4), nullptr);
        EXPECT_GT(pixels->sem_value[0], timeline);
      }
      session.reset();
      ASSERT_GE(state.destroyed.size(), 2);
      EXPECT_EQ(state.destroyed[state.destroyed.size() - 2], "encoder");
      EXPECT_EQ(state.destroyed.back(), "device");
    }
  }
}
#endif

/**
 * @brief Exercise real encoding, imported image views, and decoding when a suitable device is present.
 */
TEST_P(PyrowaveRoundTripTest, DecodesSdrAndHdrFrameContainersWithTheUpstreamLibrary) {
  bool decode_pixels = true;
  video::avcodec_buffer_t hardware_device;
  const auto [type, hardware_format] = GetParam();
  if (type != AV_HWDEVICE_TYPE_NONE) {
    AVBufferRef *raw_device = nullptr;
    if (av_hwdevice_ctx_create(&raw_device, type, nullptr, nullptr, 0) < 0) {
      GTEST_SKIP() << "A hardware frame-sharing device is unavailable";
    }
    hardware_device.reset(raw_device);
  }
#ifdef SUNSHINE_BUILD_VULKAN
  VkPhysicalDevice physical_device;
  pyrowave_device_get_vk_device_handles(device, nullptr, &physical_device, nullptr);
  VkPhysicalDeviceProperties properties {};
  vkGetPhysicalDeviceProperties(physical_device, &properties);
  // Lavapipe's decoder shader crash also reproduces in PyroWave's own tests.
  // Still validate native frame containers on software Vulkan devices.
  decode_pixels = properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
#endif
  for (int profile = 0; profile < 8; ++profile) {
    const bool hdr = (profile % 4) / 2;
    const bool yuv444 = profile % 2;
    if (type == AV_HWDEVICE_TYPE_VAAPI && yuv444) {
      continue;  // The VAAPI capture converter only exposes 4:2:0.
    }
    pyrowave_decoder decoder {};
    const pyrowave_decoder_create_info info {device, 64, 48, yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420, false};
    ASSERT_EQ(pyrowave_decoder_create(&info, &decoder), PYROWAVE_SUCCESS);
    const auto destroy_decoder = util::fail_guard([&]() {
      pyrowave_decoder_destroy(decoder);
    });
    auto config = pyrowave_config();
    config.pyrowaveRecordFraming = profile >= 4;
    config.dynamicRange = hdr;
    config.chromaSamplingType = yuv444;
    auto format = yuv444 ? (hdr ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P) : (hdr ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12);
    std::unique_ptr<video::pyrowave::session_t> session;
    static thread_local int gpu_calls;  ///< GPU submissions observed by the real codec wrapper.
    gpu_calls = 0;
#ifdef _WIN32
    static thread_local int rejected_views;  ///< Imported views rejected to verify bridge initialization failure.
    rejected_views = 0;
#endif
#if defined(SUNSHINE_BUILD_VULKAN) || defined(_WIN32)
    if (hardware_device) {
      if (hardware_format == AV_PIX_FMT_D3D11 && yuv444) {
        format = hdr ? AV_PIX_FMT_XV30 : AV_PIX_FMT_VUYX;
      } else if (hardware_format == AV_PIX_FMT_CUDA && yuv444 && hdr) {
        format = AV_PIX_FMT_YUV444P16;
      }
      video::avcodec_buffer_t frames {av_hwframe_ctx_alloc(hardware_device.get())};
      ASSERT_NE(frames, nullptr);
      auto pool = reinterpret_cast<AVHWFramesContext *>(frames->data);
      pool->format = hardware_format;
      pool->sw_format = format;
      pool->width = 64;
      pool->height = 48;
      if (av_hwframe_ctx_init(frames.get()) < 0) {
        GTEST_SKIP() << "The hardware frame pool does not support " << av_get_pix_fmt_name(format);
      }
      auto converter = std::make_unique<pyrowave_test_upload_t>(format);
      auto frame = av_frame_alloc();
      ASSERT_NE(frame, nullptr);
      ASSERT_GE(av_frame_copy_props(frame, converter->frame), 0);
      frame->format = hardware_format;
      frame->width = 64;
      frame->height = 48;
      ASSERT_EQ(converter->set_frame(frame, frames.get()), 0);
      video::pyrowave::api_t api;
      api.encode_gpu = [](pyrowave_encoder encoder, const pyrowave_gpu_sync_operation *acquire, const pyrowave_gpu_sync_operation *release, const pyrowave_gpu_buffers *buffers, const pyrowave_rate_control *rate) {
        ++gpu_calls;
        return pyrowave_encoder_encode_gpu(encoder, acquire, release, buffers, rate);
      };
  #ifdef _WIN32
      if (hardware_format == AV_PIX_FMT_D3D11) {
        auto unavailable_views = api;
        unavailable_views.image_view = [](pyrowave_image, VkImageAspectFlagBits, VkImageUsageFlagBits, pyrowave_image_view *) {
          ++rejected_views;
          return PYROWAVE_ERROR_INVALID_ARGUMENT;
        };
        EXPECT_EQ(video::pyrowave::make_d3d11_input(pool, unavailable_views), nullptr);
      }
  #endif
      session = video::pyrowave::make_session(config, std::move(converter), api, std::move(frames));
    } else
#endif
    {
      session = video::pyrowave::make_session(config, std::make_unique<pyrowave_test_converter_t>(format));
    }
    ASSERT_NE(session, nullptr);
    platf::img_t image {};
    ASSERT_EQ(session->convert(image), 0);
    auto packet = session->encode(1);
    ASSERT_NE(packet, nullptr);
    if (hardware_device && !gpu_calls) {
      GTEST_SKIP() << "The device cannot share this frame layout with PyroWave: " << av_get_pix_fmt_name(format);
    }
#ifdef _WIN32
    if (hardware_format == AV_PIX_FMT_D3D11) {
      EXPECT_EQ(rejected_views, 1);
    }
#endif
    if (hardware_device) {
      EXPECT_NE(session->encode(2), nullptr);
      ASSERT_EQ(session->convert(image), 0);
      EXPECT_NE(session->encode(3), nullptr);
      EXPECT_EQ(gpu_calls, 3);
    }
    const auto data = packet->data();
    ASSERT_GE(packet->data_size(), 4);
    const auto count = config.pyrowaveRecordFraming ? std::numeric_limits<uint32_t>::max() : AV_RL32(data);
    size_t offset = config.pyrowaveRecordFraming ? 0 : 4;
    for (uint32_t i = 0; i < count && offset < packet->data_size(); ++i) {
      ASSERT_LE(offset + 4, packet->data_size());
      const auto word = AV_RL32(data + offset);
      if (config.pyrowaveRecordFraming && word == 0xffffffffu) {
        ASSERT_LE(offset + 8, packet->data_size());
        offset += 8 + size_t(AV_RL32(data + offset + 4)) * 4;
        continue;
      }
      const auto size = config.pyrowaveRecordFraming ? (word & 0x80000000u ? 8 : ((word >> 16) & 0xfff) * 4) : word;
      if (!config.pyrowaveRecordFraming) {
        offset += 4;
      }
      ASSERT_LE(size, packet->data_size() - offset);
      ASSERT_EQ(pyrowave_decoder_push_packet(decoder, data + offset, size), PYROWAVE_SUCCESS);
      offset += size;
    }
    EXPECT_EQ(offset, packet->data_size());
    ASSERT_TRUE(pyrowave_decoder_decode_is_ready(decoder, false));
    if (!decode_pixels) {
      continue;
    }
    std::array<std::vector<uint16_t>, 3> output;
    pyrowave_cpu_buffer decoded {};
    decoded.width = 64;
    decoded.height = 48;
    decoded.format = yuv444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10;
    for (int plane = 0; plane < 3; ++plane) {
      output[plane].resize(plane && !yuv444 ? 32 * 24 : 64 * 48);
      decoded.data[plane] = output[plane].data();
      decoded.row_stride_in_bytes[plane] = (plane && !yuv444 ? 32 : 64) * sizeof(uint16_t);
      decoded.plane_size_in_bytes[plane] = output[plane].size() * sizeof(uint16_t);
    }
    ASSERT_EQ(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &decoded), PYROWAVE_SUCCESS);
    EXPECT_NEAR(output[0][0], hdr ? 1 : 4, 8);
    EXPECT_NEAR(output[0][1], 1023, 8);
    EXPECT_NEAR(output[1][0], hdr ? 1 : 4, 8);
    EXPECT_NEAR(output[2][0], 1023, 8);
    EXPECT_NEAR(output[1][1], 1023, 8);
    EXPECT_NEAR(output[2][1], hdr ? 1 : 4, 8);
  }
  if (!decode_pixels) {
    GTEST_SKIP() << "SDR/HDR packet validation passed; pixel decoding requires a hardware Vulkan device";
  }
}

/**
 * @brief Exercise CPU input and each hardware sharing API supported by this build.
 */
constexpr std::pair<AVHWDeviceType, AVPixelFormat> pyrowave_round_trip_inputs[] {
  {AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_NONE},
#ifdef _WIN32
  {AV_HWDEVICE_TYPE_D3D11VA, AV_PIX_FMT_D3D11},
#endif
#ifdef SUNSHINE_BUILD_VULKAN
  {AV_HWDEVICE_TYPE_VULKAN, AV_PIX_FMT_VULKAN},
  {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA},
  {AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI},
#endif
};

INSTANTIATE_TEST_SUITE_P(CpuAndGpu, PyrowaveRoundTripTest, testing::ValuesIn(pyrowave_round_trip_inputs));
