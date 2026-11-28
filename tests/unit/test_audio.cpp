/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

// The target match is pure: resolvable at compile time, no I/O, no globals.
static_assert(platf::capture_target_matches(platf::audio_capture_intent_e::host_and_client, 1234, platf::audio_capture_intent_e::host_and_client, 1234));
static_assert(!platf::capture_target_matches(platf::audio_capture_intent_e::host_and_client, 1234, platf::audio_capture_intent_e::host_and_client, 5678));
static_assert(!platf::capture_target_matches(platf::audio_capture_intent_e::host_and_client, 1234, platf::audio_capture_intent_e::client_only, 1234));

/**
 * @brief The exit clear must only release a target that still matches.
 */
TEST(CaptureTargetMatchTest, MatchesOnlyWhenIntentAndPidAgree) {
  using intent = platf::audio_capture_intent_e;
  EXPECT_TRUE(platf::capture_target_matches(intent::host_and_client, 1234, intent::host_and_client, 1234));
  EXPECT_TRUE(platf::capture_target_matches(intent::client_only, 0, intent::client_only, 0));
  EXPECT_FALSE(platf::capture_target_matches(intent::host_and_client, 1234, intent::host_and_client, 5678));
  EXPECT_FALSE(platf::capture_target_matches(intent::host_and_client, 1234, intent::client_only, 1234));
  EXPECT_FALSE(platf::capture_target_matches(intent::host_and_client, 1234, intent::host_and_client, 0));
  EXPECT_FALSE(platf::capture_target_matches(intent::client_only, 0, intent::host_and_client, 1234));
  EXPECT_FALSE(platf::capture_target_matches(intent::pinned_sink, 1234, intent::host_and_client, 1234));
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

    // Keep the params-bundle overload visible: the positional override below would otherwise hide it.
    using platf::audio_control_t::microphone;

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

/**
 * @brief A backend that never learned keep-default must keep swap-and-restore.
 *
 * Fail-without-fix: if the capture_sink() default stops delegating to
 * set_sink(), legacy backends silently capture nothing (returning success
 * while selecting no sink) instead of taking the historical swap path.
 */
TEST(AudioControlAdapterTest, LegacyBackendKeepsSwapBehavior) {
  legacy_audio_control_t control;
  EXPECT_FALSE(control.supports_keep_default_sink());
  EXPECT_EQ(control.capture_sink("any-sink"), 0);
  EXPECT_TRUE(control.set_sink_called);
}

/**
 * @brief Recording a capture target must be a safe no-op on legacy backends.
 *
 * Fail-without-fix: if the set_capture_target() default gains behavior (or
 * crashes on backends without target storage), every legacy session breaks,
 * since the guard refreshes unconditionally before microphone creation.
 */
TEST(AudioControlAdapterTest, DefaultCaptureTargetSetterIsNoOp) {
  legacy_audio_control_t control;
  EXPECT_FALSE(control.supports_process_capture());
  control.set_capture_target(platf::audio_capture_intent_e::host_and_client, platf::process_identity_t {1234, 0});
  control.set_capture_target(platf::audio_capture_intent_e::client_only, platf::process_identity_t {0, 0});
  EXPECT_FALSE(control.set_sink_called);
}

/**
 * @brief Releasing a capture target must be a safe no-op on legacy backends.
 *
 * Fail-without-fix: if the clear_capture_target() default gains behavior (or
 * crashes on backends without target storage), every legacy session teardown
 * breaks, since the guard releases unconditionally when a target was recorded.
 */
TEST(AudioControlAdapterTest, DefaultCaptureTargetClearIsNoOp) {
  legacy_audio_control_t control;
  control.clear_capture_target(platf::audio_capture_intent_e::host_and_client, 1234);
  control.clear_capture_target(platf::audio_capture_intent_e::client_only, 0);
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

    // Keep the params-bundle overload visible: the positional override below would otherwise hide it.
    using platf::audio_control_t::microphone;

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
    audio_config_guard_t() = default;
    audio_config_guard_t(const audio_config_guard_t &) = delete;
    audio_config_guard_t &operator=(const audio_config_guard_t &) = delete;
    audio_config_guard_t(audio_config_guard_t &&) = delete;
    audio_config_guard_t &operator=(audio_config_guard_t &&) = delete;

    bool keep_default_sink = config::audio.keep_default_sink;  ///< Saved keep_default_sink value.
    std::string sink = config::audio.sink;  ///< Saved audio_sink value.

    ~audio_config_guard_t() noexcept {
      try {
        config::audio.keep_default_sink = keep_default_sink;
        config::audio.sink = sink;
      } catch (...) {
        // Teardown restore must not throw during stack unwinding: report via noexcept fputs
        // (Boost.Log allocates/may throw, so it cannot be used in this noexcept destructor).
        std::fputs("audio_config_guard_t: failed to restore audio config\n", stderr);
      }
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

namespace {
  /**
   * @brief Microphone stub that never blocks the capture loop.
   */
  class stub_mic_t: public platf::mic_t {
  public:
    /**
     * @brief Report a timeout instead of delivering samples.
     * @param frame_buffer Destination for captured samples, left untouched.
     * @return Timeout status so the capture loop keeps polling.
     */
    platf::capture_e sample(std::vector<float> &frame_buffer) override {
      (void) frame_buffer;
      return platf::capture_e::timeout;
    }
  };

  /**
   * @brief Backend that records the capture() orchestration calls for verification.
   */
  class recording_audio_control_t: public platf::audio_control_t {
  public:
    int set_sink(const std::string &sink) override {
      calls.push_back("set_sink:" + sink);
      return set_sink_status;
    }

    bool supports_keep_default_sink() const override {
      return supports_keep;
    }

    bool supports_process_capture() const override {
      return supports_process;
    }

    int capture_sink(const std::string &sink) override {
      calls.push_back("capture_sink:" + sink);
      return capture_sink_status;
    }

    /**
     * @brief Record the refreshed capture target.
     * @param intent Capture intent resolved from the audio configuration.
     * @param identity Operating-system identity to capture, or {0, 0} for endpoint capture.
     */
    void set_capture_target(platf::audio_capture_intent_e intent, platf::process_identity_t identity) override {
      calls.emplace_back("set_capture_target");
      set_intent = intent;
      set_process_id = identity.process_id;
      set_creation_time = identity.creation_time;
      record_capture_target(intent, identity.process_id);
    }

    /**
     * @brief Record the released capture target.
     * @param intent Capture intent recorded by the session being torn down.
     * @param process_id PID recorded by the session being torn down, or 0 for endpoint capture.
     */
    void clear_capture_target(platf::audio_capture_intent_e intent, std::uint32_t process_id) override {
      calls.emplace_back("clear_capture_target");
      clear_intent = intent;
      clear_process_id = process_id;
    }

    // Keep the params-bundle overload visible: the positional override below would otherwise hide it.
    using platf::audio_control_t::microphone;

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, bool host_audio_enabled) override {
      calls.emplace_back("microphone");
      mic_mapping = mapping;
      mic_channels = channels;
      mic_sample_rate = sample_rate;
      mic_frame_size = frame_size;
      mic_continuous_audio = continuous_audio;
      mic_host_audio_enabled = host_audio_enabled;
      if (fail_microphone) {
        return nullptr;
      }
      return std::make_unique<stub_mic_t>();
    }

    bool is_sink_available(const std::string &) override {
      return true;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }

    /**
     * @brief Find the position of a recorded call in the call sequence.
     * @param prefix Call name prefix to search for.
     * @return Index of the first matching call, or calls.size() when absent.
     */
    std::size_t call_index(std::string_view prefix) const {
      for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].compare(0, prefix.size(), prefix) == 0) {
          return i;
        }
      }
      return calls.size();
    }

    bool supports_keep = true;  ///< Whether capture_sink() is supported without swapping.
    bool supports_process = true;  ///< Whether set_capture_target() may select per-process capture.
    int set_sink_status = 0;  ///< Status returned by set_sink().
    int capture_sink_status = 0;  ///< Status returned by capture_sink().
    bool fail_microphone = false;  ///< Whether microphone() returns nullptr.
    std::vector<std::string> calls;  ///< Recorded call sequence in invocation order.
    platf::audio_capture_intent_e set_intent = platf::audio_capture_intent_e::client_only;  ///< Intent from the last refresh.
    std::uint32_t set_process_id = 0;  ///< PID from the last refresh.
    std::uint64_t set_creation_time = 0;  ///< Creation marker from the last refresh.
    platf::audio_capture_intent_e clear_intent = platf::audio_capture_intent_e::client_only;  ///< Intent from the last release.
    std::uint32_t clear_process_id = 0;  ///< PID from the last release.
    const std::uint8_t *mic_mapping = nullptr;  ///< Mapping from the last microphone() call.
    int mic_channels = 0;  ///< Channels from the last microphone() call.
    std::uint32_t mic_sample_rate = 0;  ///< Sample rate from the last microphone() call.
    std::uint32_t mic_frame_size = 0;  ///< Frame size from the last microphone() call.
    bool mic_continuous_audio = false;  ///< Continuous flag from the last microphone() call.
    bool mic_host_audio_enabled = false;  ///< Host-audio flag from the last microphone() call.
  };

  /**
   * @brief Build a capture-only routing that never swaps the host default.
   * @return Startup plan targeting the saved host sink.
   */
  capture_routing_t make_capture_routing() {
    return resolve_capture_routing(true, true, false, 2, "hw-default", "", std::nullopt);
  }

  /**
   * @brief Build a swap routing that swaps to the virtual sink and restores it.
   * @return Startup plan targeting the virtual stereo sink.
   */
  capture_routing_t make_swap_routing() {
    return resolve_capture_routing(false, true, false, 2, "hw-default", "", test_null_sinks());
  }

  /**
   * @brief Build a swap routing with nothing to restore.
   * @return Startup plan targeting the host sink it already points at.
   */
  capture_routing_t make_swap_without_restore_routing() {
    return resolve_capture_routing(false, true, false, 2, "hw-default", "", std::nullopt);
  }

  /**
   * @brief Build microphone parameters matching the stereo test layout.
   * @return Capture parameters for a 5 ms stereo frame at 48 kHz.
   */
  platf::microphone_params_t make_mic_params() {
    platf::microphone_params_t params {};
    params.mapping = nullptr;
    params.channels = 2;
    params.sample_rate = 48000;
    params.frame_size = 240;
    params.continuous_audio = false;
    params.host_audio_enabled = false;
    return params;
  }
}  // namespace

/**
 * @brief The capture-only path must capture without swapping the host default.
 *
 * Fail-without-fix: if the dispatch branches swap, a keep-default session
 * calls set_sink() and steals the host default it promised to leave alone;
 * the missing capture_sink() call below is that regression.
 */
TEST(CaptureSinkDispatchTest, CapturePathUsesCaptureSink) {
  recording_audio_control_t control;
  const auto routing = make_capture_routing();
  ASSERT_FALSE(routing.swap_at_startup);

  EXPECT_TRUE(select_audio_capture_sink(control, routing));
  EXPECT_EQ(control.call_index("capture_sink:hw-default"), 0U);
  EXPECT_EQ(control.call_index("set_sink"), control.calls.size());
}

/**
 * @brief A failed capture-only selection must abort before any refresh.
 *
 * Fail-without-fix: if select_audio_capture_sink() returns true on a
 * capture_sink() failure, capture() proceeds to refresh and microphone
 * creation against a sink that was never selected, capturing silence (or the
 * wrong endpoint) instead of aborting startup.
 */
TEST(CaptureSinkDispatchTest, CaptureFailureAbortsStartup) {
  recording_audio_control_t control;
  control.capture_sink_status = -1;

  EXPECT_FALSE(select_audio_capture_sink(control, make_capture_routing()));
  EXPECT_EQ(control.call_index("set_sink"), control.calls.size());
  EXPECT_EQ(control.call_index("set_capture_target"), control.calls.size());
}

/**
 * @brief The swap path must swap the host default instead of capturing.
 *
 * Fail-without-fix: if the dispatch branches swap, a quiet-streaming session
 * calls capture_sink() and never moves applications onto the virtual sink, so
 * the host keeps playing aloud while the client captures the wrong endpoint.
 */
TEST(CaptureSinkDispatchTest, SwapPathUsesSetSink) {
  recording_audio_control_t control;
  const auto routing = make_swap_routing();
  ASSERT_TRUE(routing.swap_at_startup);
  ASSERT_TRUE(routing.restore_at_teardown);

  EXPECT_TRUE(select_audio_capture_sink(control, routing));
  EXPECT_EQ(control.call_index("set_sink:" + routing.target), 0U);
  EXPECT_EQ(control.call_index("capture_sink"), control.calls.size());
}

/**
 * @brief A failed swap must abort before any refresh.
 *
 * Fail-without-fix: if select_audio_capture_sink() returns true on a
 * set_sink() failure, capture() records a restore flag for a swap that never
 * happened and then captures the unswapped default, stranding teardown
 * symmetry and streaming the wrong source.
 */
TEST(CaptureSinkDispatchTest, SwapFailureAbortsStartup) {
  recording_audio_control_t control;
  control.set_sink_status = -1;

  EXPECT_FALSE(select_audio_capture_sink(control, make_swap_routing()));
  EXPECT_EQ(control.call_index("capture_sink"), control.calls.size());
  EXPECT_EQ(control.call_index("set_capture_target"), control.calls.size());
}

/**
 * @brief A swap with nothing to restore must skip the sink call entirely.
 *
 * Fail-without-fix: if the no-restore skip is dropped, startup reasserts the
 * host default as a swap even though the target already is the host sink,
 * needlessly disturbing routing (and any default-device watchers) for a no-op.
 */
TEST(CaptureSinkDispatchTest, SwapWithoutRestoreSkipsSinkCall) {
  recording_audio_control_t control;
  const auto routing = make_swap_without_restore_routing();
  ASSERT_TRUE(routing.swap_at_startup);
  ASSERT_FALSE(routing.restore_at_teardown);

  EXPECT_TRUE(select_audio_capture_sink(control, routing));
  EXPECT_TRUE(control.calls.empty());
}

/**
 * @brief The endpoint target must be recorded ahead of microphone creation.
 *
 * Fail-without-fix: if the guard skips the refresh (or creates the microphone
 * first), the backend captures a stale target from a previous session instead
 * of this session's intent, and the set-before-microphone ordering below breaks.
 */
TEST(CaptureRefreshTest, EndpointTargetIsRecordedBeforeMicrophone) {
  recording_audio_control_t control;
  std::mutex mutex;
  session_capture_target_t target {control, platf::audio_capture_intent_e::host_and_client, mutex};

  auto mic = target.create_microphone(make_mic_params());

  EXPECT_TRUE(static_cast<bool>(mic));
  EXPECT_TRUE(target.recorded());
  EXPECT_EQ(control.set_intent, platf::audio_capture_intent_e::host_and_client);
  EXPECT_EQ(control.set_process_id, 0u);
  EXPECT_LT(control.call_index("set_capture_target"), control.call_index("microphone"));
}

/**
 * @brief Endpoint capture must record PID 0 for every intent.
 *
 * Fail-without-fix: if the guard records anything but the endpoint target,
 * a session captures a process it never selected instead of the endpoint,
 * defeating silence or the configured sink pin.
 */
TEST(CaptureRefreshTest, EndpointCasesRecordZeroPid) {
  using intent = platf::audio_capture_intent_e;

  for (const auto case_intent : {intent::host_and_client, intent::client_only, intent::pinned_sink}) {
    recording_audio_control_t control;
    std::mutex mutex;
    session_capture_target_t target {control, case_intent, mutex};

    auto mic = target.create_microphone(make_mic_params());

    EXPECT_TRUE(static_cast<bool>(mic));
    EXPECT_TRUE(target.recorded());
    EXPECT_EQ(control.set_process_id, 0u);
    EXPECT_EQ(control.set_intent, case_intent);
    EXPECT_LT(control.call_index("set_capture_target"), control.call_index("microphone"));
  }
}

/**
 * @brief A microphone failure must still leave the refreshed target behind for teardown.
 *
 * Fail-without-fix: if the guard only records the target on microphone
 * success, a failed creation leaks the shared target (no teardown release
 * ever runs for it) and the next session inherits a target this session
 * recorded but never owned.
 */
TEST(CaptureRefreshTest, MicrophoneFailureStillRecordsTarget) {
  recording_audio_control_t control;
  control.fail_microphone = true;
  std::mutex mutex;

  {
    session_capture_target_t target {control, platf::audio_capture_intent_e::host_and_client, mutex};

    EXPECT_FALSE(static_cast<bool>(target.create_microphone(make_mic_params())));
    EXPECT_TRUE(target.recorded());
    EXPECT_EQ(control.set_process_id, 0u);
  }

  // The failed refresh still owns a target, so scope exit must release it.
  EXPECT_EQ(control.clear_intent, platf::audio_capture_intent_e::host_and_client);
  EXPECT_EQ(control.clear_process_id, 0u);
}

/**
 * @brief The restore flag must follow the routing decision in both modes.
 *
 * capture() assigns routing.restore_at_teardown directly, so this pins the
 * decision values that flow into that assignment, including the
 * swap-without-restore edge.
 *
 * Fail-without-fix: if the capture-only routing ever reports restore=true,
 * teardown restores a sink that was never swapped, yanking the user's live
 * default; if the swap routing reports restore=false, teardown strands the
 * virtual sink as the host default after the stream ends.
 */
TEST(CaptureRestoreTest, RecordsRoutingRestoreDecision) {
  audio_ctx_t ctx {};
  ctx.restore_sink = true;
  ctx.restore_sink = make_capture_routing().restore_at_teardown;
  EXPECT_FALSE(ctx.restore_sink);

  ctx.restore_sink = make_swap_routing().restore_at_teardown;
  EXPECT_TRUE(ctx.restore_sink);

  ctx.restore_sink = make_swap_without_restore_routing().restore_at_teardown;
  EXPECT_FALSE(ctx.restore_sink);
}

/**
 * @brief Teardown without a refresh must leave the shared target untouched.
 *
 * Fail-without-fix: if the guard clears unconditionally, a session that
 * aborted before any refresh (for example a failed sink selection) wipes a
 * survivor session's live target on its way out.
 */
TEST(CaptureReleaseTest, SkipsClearWhenNothingRecorded) {
  recording_audio_control_t control;
  std::mutex mutex;

  {
    session_capture_target_t target {control, platf::audio_capture_intent_e::host_and_client, mutex};
    EXPECT_FALSE(target.recorded());
  }

  EXPECT_TRUE(control.calls.empty());
}

/**
 * @brief Teardown after a refresh must release exactly the recorded target.
 *
 * Fail-without-fix: if the guard releases a stale target instead of the last
 * recorded one, the backend keeps the dead session's target (leaking capture
 * state) or wipes a survivor's newer target.
 */
TEST(CaptureReleaseTest, ClearsRecordedTarget) {
  recording_audio_control_t control;
  std::mutex mutex;

  {
    session_capture_target_t target {control, platf::audio_capture_intent_e::host_and_client, mutex};
    auto mic = target.create_microphone(make_mic_params());
    ASSERT_TRUE(static_cast<bool>(mic));
    // Release lands on scope exit, so only the refresh and creation run here.
    ASSERT_EQ(control.calls.size(), 2U);
  }

  ASSERT_EQ(control.calls.size(), 3U);
  EXPECT_EQ(control.calls[2], "clear_capture_target");
  EXPECT_EQ(control.clear_intent, platf::audio_capture_intent_e::host_and_client);
  EXPECT_EQ(control.clear_process_id, 0u);
}

/**
 * @brief The startup sequence must dispatch, refresh, create, and release in order.
 *
 * Fail-without-fix: a dispatch-branch swap shows up as the wrong sink call
 * here, a dropped refresh as a missing set_capture_target before microphone(),
 * and a leaked guard as a missing trailing clear_capture_target.
 */
TEST(CaptureOrchestrationTest, StartupSequenceRunsDispatchRefreshCreateAndRelease) {
  for (const auto &routing : {make_capture_routing(), make_swap_routing()}) {
    recording_audio_control_t control;
    std::mutex mutex;
    audio_ctx_t ctx {};

    {
      session_capture_target_t target {control, platf::audio_capture_intent_e::host_and_client, mutex};

      ctx.restore_sink = routing.restore_at_teardown;
      EXPECT_EQ(ctx.restore_sink, routing.restore_at_teardown);
      ASSERT_TRUE(select_audio_capture_sink(control, routing));
      auto mic = target.create_microphone(make_mic_params());
      ASSERT_TRUE(static_cast<bool>(mic));
      ASSERT_EQ(control.calls.size(), 3U);
    }

    const std::string expected_sink_call = routing.swap_at_startup ? "set_sink:" + routing.target : "capture_sink:" + routing.target;
    ASSERT_EQ(control.calls.size(), 4U);
    EXPECT_EQ(control.calls[0], expected_sink_call);
    EXPECT_EQ(control.calls[1], "set_capture_target");
    EXPECT_EQ(control.calls[2], "microphone");
    EXPECT_EQ(control.calls[3], "clear_capture_target");
    EXPECT_EQ(control.clear_process_id, 0u);
  }
}

/**
 * @brief The parameter-bundle microphone() must forward every field to the positional override.
 *
 * Fail-without-fix: if the forwarding overload drops, reorders, or mistypes a
 * field, backends capture the wrong layout (channels, rate, frame size) or the
 * wrong mode (continuous, host audio) while the call still succeeds.
 */
TEST(AudioControlAdapterTest, StructMicrophoneForwardsEveryField) {
  recording_audio_control_t control;
  platf::audio_control_t &base = control;
  const std::array<std::uint8_t, 6> mapping {0, 1, 4, 5, 2, 3};
  const platf::microphone_params_t params {mapping.data(), 6, 48000, 480, true, true};

  auto mic = base.microphone(params);

  EXPECT_TRUE(static_cast<bool>(mic));
  ASSERT_EQ(control.calls.size(), 1U);
  EXPECT_EQ(control.calls[0], "microphone");
  EXPECT_EQ(control.mic_mapping, mapping.data());
  EXPECT_EQ(control.mic_channels, 6);
  EXPECT_EQ(control.mic_sample_rate, 48000u);
  EXPECT_EQ(control.mic_frame_size, 480u);
  EXPECT_TRUE(control.mic_continuous_audio);
  EXPECT_TRUE(control.mic_host_audio_enabled);
}

namespace {
  /**
   * @brief Minimal backend that stores through the shared base record.
   *
   * Drives the protected opt-in helpers through the production virtuals so
   * the centralized sink and capture-target storage is pinned without
   * platform hardware. Models the Linux call pattern (record-only
   * `capture_sink()`, record-on-success `set_sink()`) and the Windows
   * intent/PID bookkeeping through the one shared store.
   */
  class recorded_audio_control_t: public platf::audio_control_t {
  public:
    int set_sink(const std::string &sink) override {
      record_capture_sink(sink);
      return 0;
    }

    bool supports_keep_default_sink() const override {
      return true;
    }

    bool supports_process_capture() const override {
      return true;
    }

    int capture_sink(const std::string &sink) override {
      return record_keep_default_capture_sink(sink);
    }

    /**
     * @brief Record the refreshed capture target in the shared base record.
     * @param intent Capture intent resolved from the audio configuration.
     * @param identity Operating-system identity to capture, or {0, 0} for endpoint capture.
     */
    void set_capture_target(platf::audio_capture_intent_e intent, platf::process_identity_t identity) override {
      record_capture_target(intent, identity.process_id);
    }

    /**
     * @brief Release the shared base record when it still belongs to the caller.
     * @param intent Capture intent recorded by the session being torn down.
     * @param process_id PID recorded by the session being torn down, or 0 for endpoint capture.
     */
    void clear_capture_target(platf::audio_capture_intent_e intent, std::uint32_t process_id) override {
      clear_recorded_capture_target_if_matches(intent, process_id);
    }

    // Keep the params-bundle overload visible: the positional override below would otherwise hide it.
    using platf::audio_control_t::microphone;

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t, bool, bool) override {
      return std::make_unique<stub_mic_t>();
    }

    bool is_sink_available(const std::string &) override {
      return true;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }

    using platf::audio_control_t::snapshot_recorded_capture_target;
    using platf::audio_control_t::snapshot_recorded_sink;
  };

  /**
   * @brief Backend modeling the macOS no-op capture contract.
   *
   * Mirrors `macos_audio_control_t`: `capture_sink()` never records, and the
   * inherited `set_capture_target()`/`clear_capture_target()` no-ops leave
   * the shared base record alone.
   */
  class macos_like_audio_control_t: public platf::audio_control_t {
  public:
    int set_sink(const std::string &) override {
      return 0;
    }

    bool supports_keep_default_sink() const override {
      return true;
    }

    int capture_sink(const std::string &) override {
      return 0;
    }

    // Keep the params-bundle overload visible: the positional override below would otherwise hide it.
    using platf::audio_control_t::microphone;

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t, bool, bool) override {
      return nullptr;
    }

    bool is_sink_available(const std::string &) override {
      return true;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }

    using platf::audio_control_t::snapshot_recorded_capture_target;
    using platf::audio_control_t::snapshot_recorded_sink;
  };
}  // namespace

/**
 * @brief The shared sink record must start empty before any session records.
 *
 * Fail-without-fix: if the base record is not value-initialized, the first
 * microphone() consumes a garbage sink instead of falling back to the
 * configured or default sink.
 */
TEST(BaseRecordTest, RecordedSinkStartsEmpty) {
  recorded_audio_control_t control;
  EXPECT_TRUE(control.snapshot_recorded_sink().empty());
}

/**
 * @brief A recorded sink snapshot must stay stable while the shared value changes.
 *
 * Fail-without-fix: if snapshots alias the shared string, the
 * default-endpoint callback reasserts a sink another session rewrote
 * concurrently instead of the copy taken at install time.
 */
TEST(BaseRecordTest, CaptureSinkRecordIsStableAcrossRewrite) {
  recorded_audio_control_t control;
  EXPECT_EQ(control.capture_sink("sink-a"), 0);
  const auto snapshot = control.snapshot_recorded_sink();
  EXPECT_EQ(control.capture_sink("sink-b"), 0);
  EXPECT_EQ(snapshot, "sink-a");
  EXPECT_EQ(control.snapshot_recorded_sink(), "sink-b");
}

/**
 * @brief The swap path must overwrite the same shared record the capture path reads.
 *
 * Fail-without-fix: if `set_sink()` keeps a separate store, a swap session
 * captures the previously recorded sink instead of the swapped one.
 */
TEST(BaseRecordTest, SetSinkOverwritesRecordedSink) {
  recorded_audio_control_t control;
  EXPECT_EQ(control.capture_sink("capture-sink"), 0);
  EXPECT_EQ(control.set_sink("swapped-sink"), 0);
  EXPECT_EQ(control.snapshot_recorded_sink(), "swapped-sink");
}

/**
 * @brief The record matcher must agree only when both intent and PID agree.
 *
 * Fail-without-fix: if `matches()` drops either comparison, an exiting
 * session wipes a survivor's target that shares only one field with its own.
 */
TEST(BaseRecordTest, CaptureTargetRecordMatchesIntentAndPid) {
  using intent = platf::audio_capture_intent_e;
  const platf::capture_target_record_t record {intent::host_and_client, 1234};
  EXPECT_TRUE(record.matches(intent::host_and_client, 1234));
  EXPECT_FALSE(record.matches(intent::host_and_client, 5678));
  EXPECT_FALSE(record.matches(intent::host_and_client, 0));
  EXPECT_FALSE(record.matches(intent::client_only, 1234));
  EXPECT_FALSE(record.matches(intent::pinned_sink, 1234));
  const platf::capture_target_record_t endpoint {};
  EXPECT_EQ(endpoint.intent, intent::client_only);
  EXPECT_EQ(endpoint.process_id, 0u);
  EXPECT_TRUE(endpoint.matches(intent::client_only, 0));
  EXPECT_FALSE(endpoint.matches(intent::host_and_client, 1234));
}

/**
 * @brief Recording a capture target must store the intent and PID together.
 *
 * Fail-without-fix: if the setter stores only one field, microphone()
 * captures with a mismatched intent/PID pair from two sessions.
 */
TEST(BaseRecordTest, CaptureTargetSetterStoresIntentAndPid) {
  recorded_audio_control_t control;
  control.set_capture_target(platf::audio_capture_intent_e::host_and_client, platf::process_identity_t {1234, 0});
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, platf::audio_capture_intent_e::host_and_client);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 1234u);
  control.set_capture_target(platf::audio_capture_intent_e::client_only, platf::process_identity_t {0, 0});
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, platf::audio_capture_intent_e::client_only);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 0u);
}

/**
 * @brief Clearing a matching target must reset the record to endpoint capture.
 *
 * Fail-without-fix: if the clear-if-match keeps the dead session's target,
 * the next session inherits process capture it never requested.
 */
TEST(BaseRecordTest, ConditionalClearResetsMatchingTarget) {
  using intent = platf::audio_capture_intent_e;
  recorded_audio_control_t control;
  control.set_capture_target(intent::host_and_client, platf::process_identity_t {1234, 0});
  control.clear_capture_target(intent::host_and_client, 1234);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::client_only);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 0u);
}

/**
 * @brief Clearing a mismatched target must preserve the survivor's record.
 *
 * Fail-without-fix: if the clear runs unconditionally, an exiting session
 * wipes a survivor session's live target on its way out.
 */
TEST(BaseRecordTest, ConditionalClearPreservesMismatchedTarget) {
  using intent = platf::audio_capture_intent_e;
  recorded_audio_control_t control;
  control.set_capture_target(intent::host_and_client, platf::process_identity_t {1234, 0});
  // A survivor that recorded a different PID keeps its target.
  control.clear_capture_target(intent::host_and_client, 5678);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::host_and_client);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 1234u);
  // A survivor that recorded a different intent keeps its target.
  control.clear_capture_target(intent::client_only, 1234);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::host_and_client);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 1234u);
  // An endpoint clear cannot wipe a process target and vice versa.
  control.clear_capture_target(intent::host_and_client, 0);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 1234u);
  control.set_capture_target(intent::client_only, platf::process_identity_t {0, 0});
  control.clear_capture_target(intent::host_and_client, 1234);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::client_only);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 0u);
}

/**
 * @brief An exiting session must not wipe a survivor's recorded target.
 *
 * Drives the real shared base record: the second refresh overwrites the
 * first, so releasing the stale target preserves the survivor, while
 * releasing the current target resets to endpoint capture.
 */
TEST(BaseRecordTest, ExitingSessionPreservesSurvivorTarget) {
  using intent = platf::audio_capture_intent_e;
  recorded_audio_control_t control;
  control.set_capture_target(intent::host_and_client, platf::process_identity_t {1111, 0});
  control.set_capture_target(intent::host_and_client, platf::process_identity_t {2222, 0});
  control.clear_capture_target(intent::host_and_client, 1111);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::host_and_client);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 2222u);
  control.clear_capture_target(intent::host_and_client, 2222);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::client_only);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 0u);
}

/**
 * @brief Concurrent sessions must always snapshot a consistent record.
 *
 * Fail-without-fix: without the base mutex, concurrent record and snapshot
 * calls tear the sink string or mix an intent from one session with a PID
 * from another. Completing with every snapshot reading a written value is
 * the regression pin. The loop also writes through `set_sink()`, the exact
 * production call the default-endpoint callback makes, so the
 * callback-to-record path is pinned under the same lock.
 */
TEST(BaseRecordTest, ConcurrentControlAccessStaysConsistent) {
  using enum platf::audio_capture_intent_e;
  recorded_audio_control_t control;
  std::atomic consistent {true};
  const auto worker = [&](std::uint32_t pid) {
    for (int i = 0; i < 50 && consistent.load(std::memory_order_relaxed); ++i) {
      control.capture_sink("sink-" + std::to_string(pid));
      if (control.snapshot_recorded_sink().compare(0, 5, "sink-") != 0) {
        consistent.store(false, std::memory_order_relaxed);
      }
      if (control.set_sink("sink-" + std::to_string(pid)) != 0) {
        consistent.store(false, std::memory_order_relaxed);
      }
      if (control.snapshot_recorded_sink().compare(0, 5, "sink-") != 0) {
        consistent.store(false, std::memory_order_relaxed);
      }
      control.set_capture_target(host_and_client, platf::process_identity_t {pid, 0});
      const auto target = control.snapshot_recorded_capture_target();
      const bool valid_target = (target.intent == host_and_client && target.process_id >= 1 && target.process_id <= 4) ||
                                (target.intent == client_only && target.process_id == 0);
      if (!valid_target) {
        consistent.store(false, std::memory_order_relaxed);
      }
      control.clear_capture_target(host_and_client, pid);
    }
  };
  {
    std::vector<std::jthread> threads;
    threads.reserve(4);
    for (std::uint32_t pid = 1; pid <= 4; ++pid) {
      threads.emplace_back(worker, pid);
    }
  }
  EXPECT_TRUE(consistent.load(std::memory_order_relaxed));
}

/**
 * @brief A macOS-style no-op backend must leave the shared record untouched.
 *
 * Fail-without-fix: if the base virtual defaults gain behavior, the by-design
 * macOS no-op (which never calls the opt-in helpers) starts recording a
 * target that microphone() never consumes, stranding state for the next
 * session.
 */
TEST(BaseRecordTest, MacOsLikeNoOpLeavesRecordUntouched) {
  macos_like_audio_control_t control;
  EXPECT_EQ(control.capture_sink("any-sink"), 0);
  control.set_capture_target(platf::audio_capture_intent_e::host_and_client, platf::process_identity_t {1234, 999});
  control.clear_capture_target(platf::audio_capture_intent_e::host_and_client, 1234);
  EXPECT_TRUE(control.snapshot_recorded_sink().empty());
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, platf::audio_capture_intent_e::client_only);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 0u);
}

/**
 * @brief Backend stub pairing a fake identity with the shared base record.
 *
 * Models the Windows `on_capture_target_cleared()` override: the hook resets
 * the paired identity exactly when the shared record resets.
 */
class hook_audio_control_t: public recorded_audio_control_t {
public:
  int set_sink(const std::string &) override {
    return 0;
  }

  // Stays at the base default: these tests pin record/hook pairing, not capture support.
  bool supports_process_capture() const override {
    return false;
  }

  /**
   * @brief Record the capture target and mark the fake identity as paired.
   *
   * Stores the intent, PID, and paired identity under one `capture_state_mutex_`
   * hold, mirroring the production Windows setter, so the stub never exposes a
   * split write.
   * @param intent Capture intent resolved from the audio configuration.
   * @param identity Operating-system identity to capture, or {0, 0} for endpoint capture.
   */
  void set_capture_target(platf::audio_capture_intent_e intent, platf::process_identity_t identity) override {
    std::lock_guard lock {capture_state_mutex_};
    recorded_target_.intent = intent;
    recorded_target_.process_id = identity.process_id;
    paired_identity = identity.process_id;
  }

protected:
  /**
   * @brief Reset the paired fake identity when the shared record resets.
   *
   * Runs under `capture_state_mutex_` via the base clearer: plain field
   * resets only, no base-helper calls, so the hook stays non-reentrant.
   */
  void on_capture_target_cleared() override {
    paired_identity = 0;
    hook_fired = true;
  }

public:
  using platf::audio_control_t::snapshot_recorded_capture_target;
  using platf::audio_control_t::snapshot_recorded_sink;

  bool hook_fired = false;  ///< Whether `on_capture_target_cleared()` ran.
  std::uint32_t paired_identity = 0;  ///< Fake backend-side identity paired with the recorded target.
};

/**
 * @brief The shared keep-default helper must record the sink and report success.
 *
 * Fail-without-fix: if `capture_sink()` stops routing through the shared
 * helper, backend copies drift and one backend records a sink another path
 * cannot see.
 */
TEST(BaseRecordTest, KeepDefaultHelperRecordsAndReturnsZero) {
  recorded_audio_control_t control;
  EXPECT_EQ(control.capture_sink("keep-default-sink"), 0);
  EXPECT_EQ(control.snapshot_recorded_sink(), "keep-default-sink");
}

/**
 * @brief A matching clear must reset the shared record and fire the hook.
 *
 * Fail-without-fix: if the clearer skips the hook, paired backend state
 * (Windows creation-time identity) survives the reset and the next session
 * revalidates against a stale identity.
 */
TEST(BaseRecordTest, ClearMatchResetsTripleAndFiresHook) {
  using intent = platf::audio_capture_intent_e;
  hook_audio_control_t control;
  control.set_capture_target(intent::host_and_client, platf::process_identity_t {1234, 0});
  EXPECT_EQ(control.paired_identity, 1234u);
  control.clear_capture_target(intent::host_and_client, 1234);
  EXPECT_TRUE(control.hook_fired);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::client_only);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 0u);
  EXPECT_EQ(control.paired_identity, 0u);
}

/**
 * @brief A mismatched clear must preserve the record and skip the hook.
 *
 * Fail-without-fix: if the clearer fires the hook unconditionally, an
 * exiting session wipes a survivor's paired backend state while keeping the
 * shared record, leaving the two out of sync.
 */
TEST(BaseRecordTest, ClearMismatchPreservesTripleAndSkipsHook) {
  using intent = platf::audio_capture_intent_e;
  hook_audio_control_t control;
  control.set_capture_target(intent::host_and_client, platf::process_identity_t {1234, 0});
  control.clear_capture_target(intent::host_and_client, 5678);
  EXPECT_FALSE(control.hook_fired);
  EXPECT_EQ(control.snapshot_recorded_capture_target().process_id, 1234u);
  EXPECT_EQ(control.paired_identity, 1234u);
  control.clear_capture_target(intent::client_only, 1234);
  EXPECT_FALSE(control.hook_fired);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::host_and_client);
  EXPECT_EQ(control.paired_identity, 1234u);
  control.set_capture_target(intent::client_only, platf::process_identity_t {0, 0});
  control.clear_capture_target(intent::host_and_client, 1234);
  EXPECT_FALSE(control.hook_fired);
  EXPECT_EQ(control.snapshot_recorded_capture_target().intent, intent::client_only);
  EXPECT_EQ(control.paired_identity, 0u);
}
