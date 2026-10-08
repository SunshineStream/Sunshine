/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */

// test includes
#include "../tests_common.h"

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

// The routing policy is pure: resolvable at compile time, no I/O, no globals.
static_assert(!resolve_sink_routing_policy(true, true).swap_at_startup);
static_assert(!resolve_sink_routing_policy(true, true).restore_at_teardown);
static_assert(resolve_sink_routing_policy(true, false).swap_at_startup);
static_assert(resolve_sink_routing_policy(true, false).restore_at_teardown);

TEST(SinkRoutingPolicyTest, KeepsDefaultOnlyWhenRequestedAndSupported) {
  const auto keep = resolve_sink_routing_policy(true, true);
  EXPECT_FALSE(keep.swap_at_startup);
  EXPECT_FALSE(keep.restore_at_teardown);

  // Unsupported backends keep legacy swap-and-restore even when requested:
  // the (true, false) row is the MUST-1 regression pin (startup swapped,
  // teardown skipped, host default stranded).
  for (const auto [requested, supported] : {std::pair {false, false}, {false, true}, {true, false}}) {
    const auto legacy = resolve_sink_routing_policy(requested, supported);
    EXPECT_TRUE(legacy.swap_at_startup);
    EXPECT_TRUE(legacy.restore_at_teardown);
  }
}

TEST(SinkRoutingPolicyTest, StartupAndTeardownNeverDiverge) {
  for (const auto requested : {false, true}) {
    for (const auto supported : {false, true}) {
      const auto policy = resolve_sink_routing_policy(requested, supported);
      EXPECT_EQ(policy.swap_at_startup, policy.restore_at_teardown);
    }
  }
}

namespace {
  /**
   * @brief Minimal backend that never learned the keep-default behavior.
   */
  class legacy_audio_control_t: public platf::audio_control_t {
  public:
    int set_sink(const std::string &) override {
      set_sink_called = true;
      return 0;
    }

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t, bool, bool) override {
      return nullptr;
    }

    bool is_sink_available(const std::string &) override {
      return true;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }

    bool set_sink_called = false;  ///< Whether the legacy swap path ran.
  };
}  // namespace

TEST(AudioControlAdapterTest, LegacyBackendKeepsSwapBehavior) {
  legacy_audio_control_t control;
  EXPECT_FALSE(control.supports_keep_default_sink());
  EXPECT_EQ(control.capture_sink("any-sink"), 0);
  EXPECT_TRUE(control.set_sink_called);
}
