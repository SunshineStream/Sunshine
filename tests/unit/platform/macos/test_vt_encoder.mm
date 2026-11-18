/**
 * @file tests/unit/platform/macos/test_vt_encoder.mm
 * @brief Unit tests for src/platform/macos/vt_encoder.*.
 */
// Only compile these tests on macOS
#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <cstdint>
  #include <memory>
  #include <type_traits>
  #include <vector>

  // lib includes
  #import <CoreMedia/CoreMedia.h>
  #import <VideoToolbox/VideoToolbox.h>

  // local includes
  #include <src/platform/macos/vt_encoder.h>

namespace {
  /**
   * @brief Releases a CoreFoundation object owned by a unique pointer.
   */
  struct cf_release_t {
    /**
     * @brief Release the object.
     *
     * @param object CoreFoundation object to release.
     */
    void operator()(CFTypeRef object) const {
      CFRelease(object);
    }
  };

  /**
   * @brief Owning pointer for a CoreFoundation object.
   */
  template<class T>
  using cf_ptr = std::unique_ptr<std::remove_pointer_t<T>, cf_release_t>;

  using bytes_t = std::vector<uint8_t>;

  // Parameter sets of a 64x64 stream produced by the VideoToolbox hardware encoder.
  const bytes_t h264_sps = {0x27, 0x64, 0x00, 0x0b, 0xac, 0x56, 0x2c, 0x30, 0xde, 0x04, 0x18, 0x45};
  const bytes_t h264_pps = {0x28, 0xee, 0x3c, 0xb0};
  const bytes_t hevc_vps = {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x1e, 0x15, 0xc0, 0x90};
  const bytes_t hevc_sps = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x1e, 0xa0, 0x14, 0x20, 0x41, 0xc1, 0x8f, 0x88, 0x15, 0xee, 0x45, 0x95, 0x10};
  const bytes_t hevc_pps = {0x44, 0x01, 0xc0, 0x2c, 0xbc, 0x14, 0xc9};

  const bytes_t h264_idr_slice = {0x65, 0x88, 0x84, 0x00};
  const bytes_t h264_inter_slice = {0x41, 0x9a, 0x02};
  const bytes_t h264_sei = {0x06, 0x05, 0x01, 0x80};
  const bytes_t hevc_idr_slice = {0x26, 0x01, 0xaf, 0x13};

  /**
   * @brief Create an H.264 format description from parameter sets.
   *
   * @param parameter_sets Parameter sets in the order they are stored.
   * @param nal_length_size Size of the AVCC NAL length prefix in bytes.
   * @return Format description.
   */
  cf_ptr<CMFormatDescriptionRef> make_h264_format(const std::vector<bytes_t> &parameter_sets, int nal_length_size = 4) {
    std::vector<const uint8_t *> pointers;
    std::vector<size_t> sizes;
    for (const auto &parameter_set : parameter_sets) {
      pointers.push_back(parameter_set.data());
      sizes.push_back(parameter_set.size());
    }
    CMFormatDescriptionRef format = nullptr;
    EXPECT_EQ(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, pointers.size(), pointers.data(), sizes.data(), nal_length_size, &format), noErr);
    return cf_ptr<CMFormatDescriptionRef> {format};
  }

  /**
   * @brief Create an HEVC format description from the VPS, SPS and PPS fixtures.
   *
   * @return Format description.
   */
  cf_ptr<CMFormatDescriptionRef> make_hevc_format() {
    const uint8_t *pointers[] = {hevc_vps.data(), hevc_sps.data(), hevc_pps.data()};
    const size_t sizes[] = {hevc_vps.size(), hevc_sps.size(), hevc_pps.size()};
    CMFormatDescriptionRef format = nullptr;
    EXPECT_EQ(CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, pointers, sizes, 4, nullptr, &format), noErr);
    return cf_ptr<CMFormatDescriptionRef> {format};
  }

  /**
   * @brief Frame NAL units with big-endian length prefixes.
   *
   * @param nals NAL units to frame.
   * @param nal_length_size Size of each length prefix in bytes.
   * @return AVCC payload.
   */
  bytes_t avcc(const std::vector<bytes_t> &nals, int nal_length_size = 4) {
    bytes_t payload;
    for (const auto &nal : nals) {
      for (int shift = (nal_length_size - 1) * 8; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<uint8_t>(nal.size() >> shift));
      }
      payload.insert(payload.end(), nal.begin(), nal.end());
    }
    return payload;
  }

  /**
   * @brief Frame NAL units with Annex-B start codes.
   *
   * @param nals NAL units to frame.
   * @return Annex-B byte stream.
   */
  bytes_t annexb(const std::vector<bytes_t> &nals) {
    bytes_t stream;
    for (const auto &nal : nals) {
      stream.insert(stream.end(), {0, 0, 0, 1});
      stream.insert(stream.end(), nal.begin(), nal.end());
    }
    return stream;
  }

  /**
   * @brief Create a block buffer holding a copy of the given bytes.
   *
   * @param bytes Bytes to copy.
   * @return Block buffer.
   */
  cf_ptr<CMBlockBufferRef> make_block(const bytes_t &bytes) {
    CMBlockBufferRef block = nullptr;
    EXPECT_EQ(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes.size(), kCFAllocatorDefault, nullptr, 0, bytes.size(), 0, &block), kCMBlockBufferNoErr);
    EXPECT_EQ(CMBlockBufferReplaceDataBytes(bytes.data(), block, 0, bytes.size()), kCMBlockBufferNoErr);
    return cf_ptr<CMBlockBufferRef> {block};
  }

  /**
   * @brief Create a single-sample buffer as delivered by a compression session.
   *
   * @param format Format description, or null.
   * @param payload AVCC payload; no data buffer is attached when empty.
   * @param split When nonzero, store the payload in two memory blocks split at this offset.
   * @return Sample buffer.
   */
  cf_ptr<CMSampleBufferRef> make_sample(CMFormatDescriptionRef format, const bytes_t &payload, size_t split = 0) {
    cf_ptr<CMBlockBufferRef> block;
    if (split) {
      CMBlockBufferRef parent = nullptr;
      EXPECT_EQ(CMBlockBufferCreateEmpty(kCFAllocatorDefault, 2, 0, &parent), kCMBlockBufferNoErr);
      block.reset(parent);
      for (const auto &part : {bytes_t(payload.begin(), payload.begin() + split), bytes_t(payload.begin() + split, payload.end())}) {
        auto child = make_block(part);
        EXPECT_EQ(CMBlockBufferAppendBufferReference(parent, child.get(), 0, part.size(), 0), kCMBlockBufferNoErr);
      }
    } else if (!payload.empty()) {
      block = make_block(payload);
    }

    CMSampleBufferRef sample = nullptr;
    const size_t sample_size = payload.size();
    EXPECT_EQ(CMSampleBufferCreateReady(kCFAllocatorDefault, block.get(), format, block ? 1 : 0, 0, nullptr, block ? 1 : 0, block ? &sample_size : nullptr, &sample), noErr);
    return cf_ptr<CMSampleBufferRef> {sample};
  }
}  // namespace

TEST(VTEncoder, H264IdrPrependsParameterSets) {
  auto format = make_h264_format({h264_sps, h264_pps});
  auto sample = make_sample(format.get(), avcc({h264_sei, h264_idr_slice}));

  bytes_t out;
  ASSERT_TRUE(platf::vt::append_annexb(sample.get(), true, out));
  EXPECT_EQ(out, annexb({h264_sps, h264_pps, h264_sei, h264_idr_slice}));
}

TEST(VTEncoder, H264InterFrameOmitsParameterSets) {
  auto format = make_h264_format({h264_sps, h264_pps});
  auto sample = make_sample(format.get(), avcc({h264_inter_slice, h264_inter_slice}));

  bytes_t out;
  ASSERT_TRUE(platf::vt::append_annexb(sample.get(), false, out));
  EXPECT_EQ(out, annexb({h264_inter_slice, h264_inter_slice}));
}

TEST(VTEncoder, HevcIdrPrependsVpsSpsPps) {
  auto format = make_hevc_format();
  auto sample = make_sample(format.get(), avcc({hevc_idr_slice}));

  bytes_t out;
  ASSERT_TRUE(platf::vt::append_annexb(sample.get(), true, out));
  EXPECT_EQ(out, annexb({hevc_vps, hevc_sps, hevc_pps, hevc_idr_slice}));
}

TEST(VTEncoder, HonorsTwoByteNalLengths) {
  auto format = make_h264_format({h264_sps, h264_pps}, 2);
  auto sample = make_sample(format.get(), avcc({h264_sei, h264_inter_slice}, 2));

  bytes_t out;
  ASSERT_TRUE(platf::vt::append_annexb(sample.get(), false, out));
  EXPECT_EQ(out, annexb({h264_sei, h264_inter_slice}));
}

TEST(VTEncoder, ConvertsNoncontiguousBlockBuffers) {
  auto format = make_h264_format({h264_sps, h264_pps});
  const auto payload = avcc({h264_sei, h264_idr_slice});
  // Split inside the second length prefix so neither block holds a whole NAL unit.
  auto sample = make_sample(format.get(), payload, 4 + h264_sei.size() + 2);

  bytes_t out;
  ASSERT_TRUE(platf::vt::append_annexb(sample.get(), true, out));
  EXPECT_EQ(out, annexb({h264_sps, h264_pps, h264_sei, h264_idr_slice}));
}

TEST(VTEncoder, AppendsAfterExistingOutput) {
  auto format = make_h264_format({h264_sps, h264_pps});
  auto sample = make_sample(format.get(), avcc({h264_inter_slice}));

  bytes_t out = {0xaa, 0xbb};
  ASSERT_TRUE(platf::vt::append_annexb(sample.get(), false, out));
  auto expected = annexb({h264_inter_slice});
  expected.insert(expected.begin(), {0xaa, 0xbb});
  EXPECT_EQ(out, expected);
}

TEST(VTEncoder, RejectsTruncatedNal) {
  auto format = make_h264_format({h264_sps, h264_pps});
  // The second length prefix promises three bytes, but only two follow.
  auto payload = avcc({h264_inter_slice});
  payload.insert(payload.end(), {0, 0, 0, 3, 0x41, 0x9a});
  auto sample = make_sample(format.get(), payload);

  bytes_t out = {0xaa};
  EXPECT_FALSE(platf::vt::append_annexb(sample.get(), true, out));
  EXPECT_EQ(out, bytes_t {0xaa});
}

TEST(VTEncoder, RejectsZeroLengthNal) {
  auto format = make_h264_format({h264_sps, h264_pps});
  auto payload = avcc({h264_inter_slice});
  payload.insert(payload.end(), {0, 0, 0, 0});
  auto sample = make_sample(format.get(), payload);

  bytes_t out;
  EXPECT_FALSE(platf::vt::append_annexb(sample.get(), false, out));
  EXPECT_TRUE(out.empty());
}

TEST(VTEncoder, RejectsTrailingPartialLengthPrefix) {
  auto format = make_h264_format({h264_sps, h264_pps});
  auto payload = avcc({h264_inter_slice});
  payload.insert(payload.end(), {0, 0});
  auto sample = make_sample(format.get(), payload);

  bytes_t out;
  EXPECT_FALSE(platf::vt::append_annexb(sample.get(), false, out));
  EXPECT_TRUE(out.empty());
}

TEST(VTEncoder, RejectsSampleWithoutFormatDescription) {
  auto sample = make_sample(nullptr, avcc({h264_inter_slice}));

  bytes_t out;
  EXPECT_FALSE(platf::vt::append_annexb(sample.get(), false, out));
  EXPECT_TRUE(out.empty());
}

TEST(VTEncoder, RejectsFormatWithoutParameterSets) {
  CMVideoFormatDescriptionRef raw_format = nullptr;
  ASSERT_EQ(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_H264, 64, 64, nullptr, &raw_format), noErr);
  cf_ptr<CMFormatDescriptionRef> format {raw_format};
  auto sample = make_sample(format.get(), avcc({h264_inter_slice}));

  bytes_t out;
  EXPECT_FALSE(platf::vt::append_annexb(sample.get(), false, out));
  EXPECT_TRUE(out.empty());
}

TEST(VTEncoder, RejectsSampleWithoutDataBuffer) {
  auto format = make_h264_format({h264_sps, h264_pps});
  auto sample = make_sample(format.get(), {});

  bytes_t out;
  EXPECT_FALSE(platf::vt::append_annexb(sample.get(), true, out));
  EXPECT_TRUE(out.empty());
}

TEST(VTEncoder, TriesLowLatencyFirstExceptForHevcOnIntel) {
  EXPECT_TRUE(platf::vt::try_low_latency(kCMVideoCodecType_H264, true));
  EXPECT_TRUE(platf::vt::try_low_latency(kCMVideoCodecType_HEVC, true));
  EXPECT_TRUE(platf::vt::try_low_latency(kCMVideoCodecType_H264, false));
  EXPECT_FALSE(platf::vt::try_low_latency(kCMVideoCodecType_HEVC, false));
}

TEST(VTEncoder, CoderSettingMapsToH264EntropyMode) {
  EXPECT_EQ(platf::vt::h264_entropy_mode(1), kVTH264EntropyMode_CABAC);
  EXPECT_EQ(platf::vt::h264_entropy_mode(2), kVTH264EntropyMode_CAVLC);
  // auto parses to 0; the built-in default is -1.
  EXPECT_EQ(platf::vt::h264_entropy_mode(0), nullptr);
  EXPECT_EQ(platf::vt::h264_entropy_mode(-1), nullptr);
}

TEST(VTEncoder, H264ExpectedFrameRateIsCappedAtLevel52) {
  // 3840x2160 is 32400 macroblocks; level 5.2 allows 2073600 per second.
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 3840, 2160, 120), 64);
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 3840, 2160, 90), 64);
  // 2560x1440 is 14400 macroblocks.
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 2560, 1440, 240), 144);
}

TEST(VTEncoder, H264ExpectedFrameRateWithinLevel52IsUnchanged) {
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 3840, 2160, 60), 60);
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 1920, 1080, 120), 120);
  // Partial macroblocks round up: 1920x1080 is 120x68 macroblocks.
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 1920, 1080, 300), 254);
}

TEST(VTEncoder, OtherCodecsAndInvalidSizesAreNotCapped) {
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_HEVC, 3840, 2160, 120), 120);
  EXPECT_EQ(platf::vt::expected_frame_rate(kCMVideoCodecType_H264, 0, 2160, 120), 120);
}
#endif
