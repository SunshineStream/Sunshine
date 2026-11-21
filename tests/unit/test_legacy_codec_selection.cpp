/**
 * @file tests/unit/test_legacy_codec_selection.cpp
 * @brief Verify independent codec probing and shared host/client negotiation.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <string>
#include <vector>

// lib includes
extern "C" {
#include <moonlight-common-c/src/VideoFormats.h>
}

// local includes
#include <src/rtsp.h>
#include <src/video.h>

/**
 * @brief H.263+ software fallback does not displace an available hardware MPEG-2 backend.
 */
TEST(LegacyCodecSelectionTest, SelectsEachCodecIndependentlyWithoutRequiringH264) {
  video::encoder_t hardware {};
  video::encoder_t software {};
  const std::array candidates {&hardware, &software};
  std::vector<int> probes;
  const auto probe = [&](video::encoder_t &encoder, int format) {
    probes.push_back(format);
    return &encoder == &software || format == video::format_mpeg2;
  };
  EXPECT_EQ(video::select_legacy_encoder(candidates, video::format_mpeg2, probe), &hardware);
  EXPECT_EQ(video::select_legacy_encoder(candidates, video::format_h263p, probe), &software);
  EXPECT_FALSE(hardware.h264[video::encoder_t::PASSED]);
  EXPECT_FALSE(software.h264[video::encoder_t::PASSED]);
  EXPECT_EQ(probes, (std::vector<int> {video::format_mpeg2, video::format_h263p, video::format_h263p}));
}

/**
 * @brief Unsupported codecs and failed probes never return an unrelated backend.
 */
TEST(LegacyCodecSelectionTest, RejectsInvalidFormatsAndUnavailableBackends) {
  video::encoder_t backend {};
  const std::array candidates {static_cast<video::encoder_t *>(nullptr), &backend};
  int probes = 0;
  const auto reject = [&](auto &, int) {
    ++probes;
    return false;
  };
  EXPECT_EQ(video::select_legacy_encoder(candidates, video::format_h264, reject), nullptr);
  EXPECT_EQ(probes, 0);
  EXPECT_EQ(video::select_legacy_encoder(candidates, video::format_mpeg2, reject), nullptr);
  EXPECT_EQ(probes, 1);
  EXPECT_FALSE(video::validate_legacy_codec(backend, video::format_av1));
  EXPECT_FALSE(video::validate_legacy_codec(backend, video::format_mpeg2));
  EXPECT_FALSE(video::validate_legacy_codec(backend, video::format_h263p));
}

/**
 * @brief Any implementing client can negotiate an advertised legacy codec using shared wire values.
 */
TEST(LegacyCodecNegotiationTest, MatchesSunshineAdvertisementsAndStrictClientRequests) {
  constexpr uint32_t modes = SCM_H264 | SCM_HEVC | SCM_AV1_MAIN8 | SCM_MPEG2 | SCM_H263P;
  const auto sdp = rtsp_stream::describe_video_codecs(modes);
  EXPECT_EQ(chooseVideoFormat(VIDEO_FORMAT_H264 | VIDEO_FORMAT_AV1_MAIN8 | VIDEO_FORMAT_MPEG2, modes, sdp.c_str()), VIDEO_FORMAT_AV1_MAIN8);
  EXPECT_EQ(chooseVideoFormat(VIDEO_FORMAT_MPEG2, modes, sdp.c_str()), VIDEO_FORMAT_MPEG2);
  EXPECT_EQ(chooseVideoFormat(VIDEO_FORMAT_H263P, modes, sdp.c_str()), VIDEO_FORMAT_H263P);
  EXPECT_EQ(getVideoBitStreamFormat(VIDEO_FORMAT_MPEG2), video::format_mpeg2);
  EXPECT_EQ(getVideoBitStreamFormat(VIDEO_FORMAT_H263P), video::format_h263p);
  for (const auto mode : {SCM_MPEG2, SCM_H263P}) {
    const auto legacy_sdp = rtsp_stream::describe_video_codecs(mode);
    const auto supported = VIDEO_FORMAT_H264 | VIDEO_FORMAT_MPEG2 | VIDEO_FORMAT_H263P;
    const auto selected = chooseVideoFormat(supported, mode, legacy_sdp.c_str());
    EXPECT_EQ(selected, mode == SCM_MPEG2 ? VIDEO_FORMAT_MPEG2 : VIDEO_FORMAT_H263P);
  }
  EXPECT_TRUE(rtsp_stream::describe_video_codecs(0).empty());
  EXPECT_EQ(chooseVideoFormat(VIDEO_FORMAT_MPEG2, SCM_H264, rtsp_stream::describe_video_codecs(SCM_H264).c_str()), 0);
}
