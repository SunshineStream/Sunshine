/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */

// test includes
#include "../tests_common.h"

// local includes
#include <src/audio.h>
#include <src/config.h>
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
  for (const auto &[requested, supported] : {std::pair {false, false}, {false, true}, {true, false}}) {
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
   * @brief Virtual sink names shared by the capture-routing tests.
   */
  std::optional<platf::sink_t::null_t> test_null_sinks() {
    return platf::sink_t::null_t {"virtual-stereo", "virtual-surround51", "virtual-surround71"};
  }
}  // namespace

/**
 * @brief Capture-only startup must target the host sink when nothing is configured.
 *
 * Regression test: without a swap, applications stay on the host default, so
 * capturing the virtual sink's monitor would yield silence.
 */
TEST(CaptureRoutingTest, CaptureOnlyUsesHostSinkWhenNothingConfigured) {
  for (const auto host_audio : {false, true}) {
    const auto routing = resolve_capture_routing(true, true, host_audio, 2, "hw-default", "", test_null_sinks());
    EXPECT_FALSE(routing.swap_at_startup) << "host_audio=" << host_audio;
    EXPECT_FALSE(routing.restore_at_teardown) << "host_audio=" << host_audio;
    EXPECT_EQ(routing.target, "hw-default") << "host_audio=" << host_audio;
  }
}

/**
 * @brief Capture-only startup must target the configured sink when one is set.
 */
TEST(CaptureRoutingTest, CaptureOnlyUsesConfiguredSinkWhenSet) {
  for (const auto host_audio : {false, true}) {
    const auto routing = resolve_capture_routing(true, true, host_audio, 2, "hw-default", "eq-sink", test_null_sinks());
    EXPECT_FALSE(routing.swap_at_startup) << "host_audio=" << host_audio;
    EXPECT_FALSE(routing.restore_at_teardown) << "host_audio=" << host_audio;
    EXPECT_EQ(routing.target, "eq-sink") << "host_audio=" << host_audio;
  }
}

/**
 * @brief Swap-path startup must preserve the historical virtual sink selection.
 */
TEST(CaptureRoutingTest, SwapPathKeepsVirtualSinkSelection) {
  const auto stereo = resolve_capture_routing(false, true, false, 2, "hw-default", "", test_null_sinks());
  EXPECT_TRUE(stereo.swap_at_startup);
  EXPECT_TRUE(stereo.restore_at_teardown);
  EXPECT_EQ(stereo.target, "virtual-stereo");

  const auto surround = resolve_capture_routing(false, true, false, 6, "hw-default", "", test_null_sinks());
  EXPECT_TRUE(surround.swap_at_startup);
  EXPECT_EQ(surround.target, "virtual-surround51");

  const auto configured = resolve_capture_routing(false, true, true, 2, "hw-default", "eq-sink", test_null_sinks());
  EXPECT_TRUE(configured.swap_at_startup);
  EXPECT_TRUE(configured.restore_at_teardown);
  EXPECT_EQ(configured.target, "eq-sink");

  // An explicit audio_sink pin beats the virtual sink even when host playback is off.
  const auto configured_muted = resolve_capture_routing(false, true, false, 2, "hw-default", "eq-sink", test_null_sinks());
  EXPECT_TRUE(configured_muted.swap_at_startup);
  EXPECT_TRUE(configured_muted.restore_at_teardown);
  EXPECT_EQ(configured_muted.target, "eq-sink");

  // Legacy backends take the swap path even when keep-default is requested.
  const auto legacy = resolve_capture_routing(true, false, false, 2, "hw-default", "", test_null_sinks());
  EXPECT_TRUE(legacy.swap_at_startup);
  EXPECT_TRUE(legacy.restore_at_teardown);
  EXPECT_EQ(legacy.target, "virtual-stereo");
}

/**
 * @brief Startup without virtual sinks must fall back to the host sink.
 */
TEST(CaptureRoutingTest, FallsBackToHostWithoutVirtualSinks) {
  const auto routing = resolve_capture_routing(true, true, false, 2, "hw-default", "", std::nullopt);
  EXPECT_FALSE(routing.swap_at_startup);
  EXPECT_FALSE(routing.restore_at_teardown);
  EXPECT_EQ(routing.target, "hw-default");

  // Swap path with no alternative: the target is already the host, so there is nothing to restore.
  const auto swap = resolve_capture_routing(false, true, false, 2, "hw-default", "", std::nullopt);
  EXPECT_TRUE(swap.swap_at_startup);
  EXPECT_FALSE(swap.restore_at_teardown);
  EXPECT_EQ(swap.target, "hw-default");
}

/**
 * @brief The shared intent must map the three UI choices onto config state.
 */
TEST(AudioCaptureIntentTest, ResolvesIntentFromConfig) {
  using intent = platf::audio_capture_intent_e;
  EXPECT_EQ(resolve_audio_capture_intent(false, ""), intent::client_only);
  EXPECT_EQ(resolve_audio_capture_intent(true, ""), intent::host_and_client);
  EXPECT_EQ(resolve_audio_capture_intent(false, "eq-sink"), intent::pinned_sink);
  EXPECT_EQ(resolve_audio_capture_intent(true, "eq-sink"), intent::pinned_sink);
}

/**
 * @brief Intent names must stay stable for the capture-mode log line.
 */
TEST(AudioCaptureIntentTest, NamesEveryIntent) {
  using intent = platf::audio_capture_intent_e;
  EXPECT_EQ(audio_capture_intent_name(intent::client_only), "client_only");
  EXPECT_EQ(audio_capture_intent_name(intent::host_and_client), "host_and_client");
  EXPECT_EQ(audio_capture_intent_name(intent::pinned_sink), "pinned_sink");
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

TEST(AudioControlAdapterTest, DefaultCaptureTargetSetterIsNoOp) {
  legacy_audio_control_t control;
  // Recording a capture target must be a safe no-op on legacy backends.
  control.set_capture_target(platf::audio_capture_intent_e::host_and_client, 1234);
  control.set_capture_target(platf::audio_capture_intent_e::client_only, 0);
  EXPECT_FALSE(control.set_sink_called);
}

namespace {
  /**
   * @brief Backend that records which sink the availability probe checks.
   */
  class probe_audio_control_t: public platf::audio_control_t {
  public:
    /**
     * @brief Create a probe backend with a controlled keep-default capability.
     * @param supports_keep Whether capture_sink() is supported without swapping.
     */
    explicit probe_audio_control_t(bool supports_keep):
        supports_keep {supports_keep} {
    }

    int set_sink(const std::string &) override {
      return 0;
    }

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t, bool, bool) override {
      return nullptr;
    }

    bool is_sink_available(const std::string &sink) override {
      probed_sink = sink;
      return true;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }

    bool supports_keep_default_sink() const override {
      return supports_keep;
    }

    bool supports_keep;  ///< Whether this backend supports keep-default capture.
    std::string probed_sink;  ///< Sink most recently passed to is_sink_available().
  };

  /**
   * @brief Build an audio context whose host sink differs from the configured sink.
   * @param supports_keep Whether the fake backend supports keep-default capture.
   * @param probed Receives the fake backend so tests can inspect the probed sink.
   * @return Audio context with host sink "hw-default".
   */
  audio_ctx_t make_probe_ctx(bool supports_keep, probe_audio_control_t *&probed) {
    auto control = std::make_unique<probe_audio_control_t>(supports_keep);
    probed = control.get();

    audio_ctx_t ctx {};
    ctx.control = std::move(control);
    ctx.restore_sink = false;
    ctx.sink.host = "hw-default";
    return ctx;
  }

  /**
   * @brief Preserve the global audio config while a probe test rewrites it.
   */
  struct audio_config_guard_t {
    bool keep_default_sink = config::audio.keep_default_sink;  ///< Saved keep_default_sink value.
    std::string sink = config::audio.sink;  ///< Saved audio_sink value.

    ~audio_config_guard_t() {
      config::audio.keep_default_sink = keep_default_sink;
      config::audio.sink = sink;
    }
  };
}  // namespace

TEST(SinkAvailabilityProbeTest, KeepDefaultProbesConfiguredSink) {
  const audio_config_guard_t guard;
  config::audio.keep_default_sink = true;
  config::audio.sink = "configured-sink";

  probe_audio_control_t *probed = nullptr;
  auto ctx = make_probe_ctx(true, probed);

  EXPECT_TRUE(is_audio_ctx_sink_available(ctx));
  EXPECT_EQ(probed->probed_sink, "configured-sink");
}

TEST(SinkAvailabilityProbeTest, KeepDefaultWithoutConfiguredSinkProbesHostSink) {
  const audio_config_guard_t guard;
  config::audio.keep_default_sink = true;
  config::audio.sink.clear();

  probe_audio_control_t *probed = nullptr;
  auto ctx = make_probe_ctx(true, probed);

  EXPECT_TRUE(is_audio_ctx_sink_available(ctx));
  EXPECT_EQ(probed->probed_sink, "hw-default");
}

TEST(SinkAvailabilityProbeTest, SwapModeProbesHostSink) {
  const audio_config_guard_t guard;
  config::audio.keep_default_sink = false;
  config::audio.sink = "configured-sink";

  probe_audio_control_t *probed = nullptr;
  auto ctx = make_probe_ctx(true, probed);

  EXPECT_TRUE(is_audio_ctx_sink_available(ctx));
  EXPECT_EQ(probed->probed_sink, "hw-default");
}

TEST(SinkAvailabilityProbeTest, UnsupportedBackendProbesHostSink) {
  const audio_config_guard_t guard;
  config::audio.keep_default_sink = true;
  config::audio.sink = "configured-sink";

  probe_audio_control_t *probed = nullptr;
  auto ctx = make_probe_ctx(false, probed);

  EXPECT_TRUE(is_audio_ctx_sink_available(ctx));
  EXPECT_EQ(probed->probed_sink, "hw-default");
}
