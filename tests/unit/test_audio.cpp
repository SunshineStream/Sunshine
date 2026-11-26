/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */

// test includes
#include "../tests_common.h"
#include "audio_transport_test_helper.h"

// standard includes
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include <src/audio.h>
#include <src/rtsp.h>
#include <src/stream.h>

namespace audio {
  void encodeThread(std::shared_ptr<safe::queue_t<std::vector<float>>> samples, config_t config, stream::session_t *channel_data);
}

using namespace audio;

TEST(AudioLayoutTest, CustomMappingRemainsContiguousAndUsesAllEightChannels) {
  const stream_params_t params {8, 6, 2, {0, 1, 4, 5, 2, 3, 6, 7}};
  auto copy = params;
  EXPECT_EQ(copy.channelCount, 8);
  EXPECT_EQ(copy.streams, 6);
  EXPECT_EQ(copy.coupledStreams, 2);
  EXPECT_EQ(copy.mapping.size(), 8U);
  EXPECT_EQ(copy.mapping.back(), 7);
  EXPECT_EQ(copy.mapping.data() + 7, &copy.mapping[7]);
  copy.mapping[0] = 7;
  EXPECT_EQ(params.mapping[0], 0);
  EXPECT_EQ(stream_configs.size(), MAX_STREAM_CONFIG);
  EXPECT_EQ(stream_configs[HIGH_SURROUND71].channelCount, 8);
}

TEST(AudioDolbyBitrateTest, SelectsChannelAwareBitrates) {
  EXPECT_EQ(dolby_bitrate_kbps(false, false, 2), 128);
  EXPECT_EQ(dolby_bitrate_kbps(false, true, 2), 192);
  EXPECT_EQ(dolby_bitrate_kbps(true, false, 2), 128);
  EXPECT_EQ(dolby_bitrate_kbps(true, true, 2), 192);
  EXPECT_EQ(dolby_bitrate_kbps(false, false, 6), 384);
  EXPECT_EQ(dolby_bitrate_kbps(false, true, 6), 640);
  EXPECT_EQ(dolby_bitrate_kbps(true, false, 6), 192);
  EXPECT_EQ(dolby_bitrate_kbps(true, true, 6), 384);
  for (const bool eac3 : {false, true}) {
    for (const bool high_quality : {false, true}) {
      EXPECT_EQ(dolby_bitrate_kbps(eac3, high_quality, 1), 0);
      EXPECT_EQ(dolby_bitrate_kbps(eac3, high_quality, 8), 0);
    }
  }
}

TEST(AudioDolbyNegotiationTest, ConfiguresExactlyAcceptedAc3AndResponseLabel) {
  config_t config {.packetDuration = 5, .channels = 6, .mask = 0x3F};
  const auto codec = configure_dolby_audio(config, "AC3", "AC3", true, true, true);
  EXPECT_EQ(codec, dolby_codec_e::ac3);
  EXPECT_TRUE(config.use_ac3);
  EXPECT_FALSE(config.use_eac3);
  EXPECT_EQ(config.packetDuration, 32);
  EXPECT_TRUE(config.flags[config_t::HIGH_QUALITY]);
  EXPECT_FALSE(config.flags[config_t::CUSTOM_SURROUND_PARAMS]);
  EXPECT_TRUE(config.high_quality_dolby);
  EXPECT_EQ(dolby_bitrate_kbps(config.use_eac3, config.high_quality_dolby, config.channels), 640);
  EXPECT_EQ(dolby_codec_name(codec), "AC3");
}

TEST(AudioDolbyNegotiationTest, ConfiguresExactlyAcceptedEac3AndResponseLabel) {
  config_t config {.packetDuration = 5, .channels = 2, .mask = 0x3};
  const auto codec = configure_dolby_audio(config, "EAC3", "EAC3", true, true, false);
  EXPECT_EQ(codec, dolby_codec_e::eac3);
  EXPECT_FALSE(config.use_ac3);
  EXPECT_TRUE(config.use_eac3);
  EXPECT_EQ(config.packetDuration, 32);
  EXPECT_TRUE(config.flags[config_t::HIGH_QUALITY]);
  EXPECT_FALSE(config.flags[config_t::CUSTOM_SURROUND_PARAMS]);
  EXPECT_FALSE(config.high_quality_dolby);
  EXPECT_EQ(dolby_bitrate_kbps(config.use_eac3, config.high_quality_dolby, config.channels), 128);
  EXPECT_EQ(dolby_codec_name(codec), "EAC3");
}

TEST(AudioDolbyNegotiationTest, FallsBackToOpusForMissingOrMismatchedAcceptance) {
  const std::array negotiations {
    std::pair<std::string_view, std::string_view> {"AC3", ""},
    std::pair<std::string_view, std::string_view> {"AC3", "EAC3"},
    std::pair<std::string_view, std::string_view> {"EAC3", "AC3"},
    std::pair<std::string_view, std::string_view> {"", "AC3"}
  };
  for (const auto &[offered, accepted] : negotiations) {
    config_t config {.packetDuration = 5, .channels = 2, .mask = 0x3};
    const auto codec = configure_dolby_audio(config, offered, accepted, true, true, true);
    EXPECT_EQ(codec, dolby_codec_e::opus);
    EXPECT_FALSE(config.use_ac3);
    EXPECT_FALSE(config.use_eac3);
    EXPECT_EQ(config.packetDuration, 5);
    EXPECT_FALSE(config.high_quality_dolby);
    EXPECT_EQ(dolby_codec_name(codec), "Opus");
  }
}

TEST(AudioDolbyNegotiationTest, FallsBackToOpusWhenEncoderIsUnavailable) {
  config_t ac3_config {.packetDuration = 5, .channels = 2, .mask = 0x3};
  EXPECT_EQ(configure_dolby_audio(ac3_config, "AC3", "AC3", false, true, false), dolby_codec_e::opus);
  EXPECT_FALSE(ac3_config.use_ac3);
  config_t eac3_config {.packetDuration = 5, .channels = 6, .mask = 0x3F};
  EXPECT_EQ(configure_dolby_audio(eac3_config, "EAC3", "EAC3", true, false, false), dolby_codec_e::opus);
  EXPECT_FALSE(eac3_config.use_eac3);
}

TEST(AudioDolbyNegotiationTest, FallsBackToOpusForUnsupportedChannelCounts) {
  config_t config {.packetDuration = 5, .channels = 8, .mask = 0x63F};
  EXPECT_EQ(configure_dolby_audio(config, "AC3", "AC3", true, true, true), dolby_codec_e::opus);
  EXPECT_FALSE(config.use_ac3);
  EXPECT_FALSE(config.use_eac3);
  EXPECT_EQ(config.packetDuration, 5);
  EXPECT_EQ(dolby_codec_name(dolby_codec_e::opus), "Opus");
}
#ifdef SUNSHINE_TESTS
TEST(AudioDolbyEncoderTest, EncodesAllSupportedCodecsLayoutsAndQualityTiersWithoutCapture) {
  ASSERT_TRUE(ac3_available());
  ASSERT_TRUE(eac3_available());
  for (const bool eac3 : {false, true}) {
    for (const int channels : {2, 6}) {
      for (const bool high_quality : {false, true}) {
        SCOPED_TRACE(testing::Message() << "eac3=" << eac3 << ", channels=" << channels << ", high_quality=" << high_quality);
        const std::vector<float> pcm(1536U * static_cast<std::size_t>(channels), 0.0F);
        buffer_t output {MAX_DOLBY_PACKET_SIZE};
        EXPECT_GT(encode_dolby_frame_for_testing(eac3, high_quality, channels, pcm, output), 0);
      }
    }
  }
}

TEST(AudioDolbyEncoderTest, RejectsInvalidInputAndUnsupportedChannelCount) {
  ASSERT_TRUE(ac3_available());
  const std::vector<float> invalid_frame(1535U * 2U, 0.0F);
  buffer_t output {4096};
  EXPECT_LT(encode_dolby_frame_for_testing(false, false, 2, invalid_frame, output), 0);
  const std::vector<float> unsupported_layout(1536U * 8U, 0.0F);
  EXPECT_LT(encode_dolby_frame_for_testing(false, false, 8, unsupported_layout, output), 0);
}
#endif

TEST(AudioPacketTransportTest, ChecksPkcs7CiphertextCapacityAtBlockBoundaries) {
  EXPECT_TRUE(stream::audio_packet_fits(1400, true, 1408));
  EXPECT_TRUE(stream::audio_packet_fits(1401, true, 1408));
  EXPECT_FALSE(stream::audio_packet_fits(1408, true, 1408));

  constexpr std::size_t max_audio_payload = MAX_DOLBY_PACKET_SIZE;
  constexpr auto max_encrypted_payload = crypto::cipher::round_to_pkcs7_padded(max_audio_payload);
  static_assert(max_audio_payload == 2560);
  static_assert(max_encrypted_payload == 2576);

  EXPECT_TRUE(stream::audio_packet_fits(max_audio_payload, false, max_audio_payload));
  EXPECT_FALSE(stream::audio_packet_fits(max_audio_payload + 1, false, max_audio_payload));
  EXPECT_TRUE(stream::audio_packet_fits(max_audio_payload, true, max_encrypted_payload));
  EXPECT_FALSE(stream::audio_packet_fits(max_audio_payload, true, max_encrypted_payload - 1));
}

TEST(AudioPacketTransportTest, EncryptsAndWritesMaximumPayloadWithinExactShardCapacity) {
  constexpr std::size_t plaintext_size = MAX_DOLBY_PACKET_SIZE;
  constexpr auto encrypted_size = crypto::cipher::round_to_pkcs7_padded(plaintext_size);
  const audio::buffer_t plaintext(plaintext_size, 0x5A);

  for (const bool encrypted : {false, true}) {
    SCOPED_TRACE(testing::Message() << "encrypted=" << encrypted);
    const auto required_capacity = encrypted ? encrypted_size : plaintext_size;
    audio_transport_test::packet_writer writer(required_capacity);

    const auto bytes = writer.encode(encrypted, plaintext, writer.destination.size());

    EXPECT_EQ(bytes, static_cast<int>(required_capacity));
    if (encrypted) {
      EXPECT_FALSE(writer.is_filled_with(0xA5));
    } else {
      EXPECT_TRUE(std::equal(plaintext.begin(), plaintext.end(), writer.destination.begin()));
    }
  }
}

TEST(AudioPacketTransportTest, RejectsInsufficientShardCapacityBeforeWriting) {
  constexpr std::size_t plaintext_size = MAX_DOLBY_PACKET_SIZE;
  constexpr auto encrypted_size = crypto::cipher::round_to_pkcs7_padded(plaintext_size);
  const audio::buffer_t plaintext(plaintext_size, 0x5A);

  for (const bool encrypted : {false, true}) {
    SCOPED_TRACE(testing::Message() << "encrypted=" << encrypted);
    const auto required_capacity = encrypted ? encrypted_size : plaintext_size;
    audio_transport_test::packet_writer writer(required_capacity);

    const auto bytes = writer.encode(encrypted, plaintext, required_capacity - 1);

    EXPECT_EQ(bytes, -1);
    EXPECT_TRUE(writer.is_filled_with(0xA5));
  }
}

TEST(AudioPacketSessionTest, EncoderPreservesTheReceivingSession) {
  stream::config_t stream_config {};
  rtsp_stream::launch_session_t launch_session {};
  launch_session.gcm_key.resize(16);
  launch_session.iv.resize(16);
  auto session = stream::session::alloc(stream_config, launch_session);
  ASSERT_NE(session.get(), nullptr);
  config_t config {.packetDuration = 5, .channels = 2, .mask = 0x3};
  auto samples = std::make_shared<safe::queue_t<std::vector<float>>>();
  auto packets = mail::man->queue<packet_t>(mail::audio_packets);
  std::jthread worker([&samples, &config, &session] {
    audio::encodeThread(samples, config, session.get());
  });
  auto stop_worker = util::fail_guard([&samples] {
    samples->stop();
  });
  ASSERT_TRUE(samples->raise(std::vector<float>(480, 0.0F)));
  auto packet = packets->pop(2s);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->first, session.get());
  EXPECT_GT(packet->second.size(), 0U);
}

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    BaseTest::SetUp();
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  auto result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {.packetDuration = 5, .channels = 2, .mask = 0x3, .customStreamParams = {}, .flags = config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {.packetDuration = 5, .channels = 6, .mask = 0x3F, .customStreamParams = {}, .flags = config_flags()}),
    std::make_tuple("SURROUND71", config_t {.packetDuration = 5, .channels = 8, .mask = 0x63F, .customStreamParams = {}, .flags = config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)})
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(AudioTest, TestEncode) {
  std::jthread timer([this] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::jthread capture([this] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}
