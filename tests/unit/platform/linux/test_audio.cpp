/**
 * @file tests/unit/platform/linux/test_audio.cpp
 * @brief Tests for Linux audio sink handling and the capture channel map.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <vector>

  // lib includes
  #include <pulse/pulseaudio.h>

  // local includes
  #include "src/platform/common.h"
  #include "src/thread_safe.h"

  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/config.h"

namespace platf {
  bool fill_canonical_channel_map(int channels, pa_channel_map &pa_map);

  namespace pa {
    /**
     * @brief Forward a PulseAudio integer result to the caller's alarm.
     * @param value Result supplied by PulseAudio.
     * @param userdata Alarm receiving the result.
     */
    void cb_i(pa_context *, std::uint32_t value, void *userdata);

    /**
     * @brief Forward PulseAudio operation completion to the caller's alarm.
     * @param status Nonzero on success, zero on failure.
     * @param userdata Alarm receiving the translated result.
     */
    void success_cb(pa_context *, int status, void *userdata);
  }  // namespace pa
}  // namespace platf

namespace {
  /**
   * @brief Collect the speaker positions a channel map describes.
   *
   * @param pa_map Channel map to read.
   * @return The positions of its channels, in channel order.
   */
  std::vector<pa_channel_position_t> positions_of(const pa_channel_map &pa_map) {
    return {pa_map.map, pa_map.map + pa_map.channels};
  }
}  // namespace

/**
 * @brief Capture must use the canonical layout, whatever Opus mapping the client asked for.
 *
 * The Opus mapping is applied by the multistream encoder and undone by the client when it
 * decodes, so applying it to the captured PCM as well leaves the client rendering permuted
 * channels. The webOS client requests 0,1,4,5,2,3, which would put the centre channel on the
 * rear left speaker.
 */
TEST(LinuxAudioChannelMap, Surround51UsesCanonicalOrder) {
  pa_channel_map pa_map;
  ASSERT_TRUE(platf::fill_canonical_channel_map(6, pa_map));

  const std::vector<pa_channel_position_t> expected {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT
  };
  EXPECT_EQ(positions_of(pa_map), expected);
}

/**
 * @brief Stereo capture maps onto the two front channels.
 */
TEST(LinuxAudioChannelMap, StereoUsesCanonicalOrder) {
  pa_channel_map pa_map;
  ASSERT_TRUE(platf::fill_canonical_channel_map(2, pa_map));

  const std::vector<pa_channel_position_t> expected {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT
  };
  EXPECT_EQ(positions_of(pa_map), expected);
}

/**
 * @brief 7.1 capture adds the two side channels to the 5.1 layout.
 */
TEST(LinuxAudioChannelMap, Surround71UsesCanonicalOrder) {
  pa_channel_map pa_map;
  ASSERT_TRUE(platf::fill_canonical_channel_map(8, pa_map));

  const std::vector<pa_channel_position_t> expected {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT
  };
  EXPECT_EQ(positions_of(pa_map), expected);
}

/**
 * @brief The map the backend hands PulseAudio must be a valid one.
 */
TEST(LinuxAudioChannelMap, ProducesValidPulseAudioMaps) {
  for (const int channels : {2, 6, 8}) {
    pa_channel_map pa_map;
    ASSERT_TRUE(platf::fill_canonical_channel_map(channels, pa_map)) << "channels: " << channels;
    EXPECT_TRUE(pa_channel_map_valid(&pa_map)) << "channels: " << channels;
  }
}

/**
 * @brief Channel counts with no known speaker layout are rejected rather than read out of bounds.
 */
TEST(LinuxAudioChannelMap, RejectsUnsupportedChannelCounts) {
  pa_channel_map pa_map;
  EXPECT_FALSE(platf::fill_canonical_channel_map(0, pa_map));
  EXPECT_FALSE(platf::fill_canonical_channel_map(-1, pa_map));
  EXPECT_FALSE(platf::fill_canonical_channel_map(platf::speaker::MAX_SPEAKERS + 1, pa_map));
}

/**
 * @brief PulseAudio integer callbacks ring the supplied alarm without requiring a context.
 */
TEST(LinuxAudioCallbackTest, ForwardsIntegerResultWithoutContext) {
  safe::alarm_raw_t<int> alarm;
  platf::pa::cb_i(nullptr, 42, &alarm);

  ASSERT_TRUE(alarm.wait_for(std::chrono::milliseconds {0}));
  ASSERT_TRUE(alarm.status().has_value());
  EXPECT_EQ(*alarm.status(), 42);
}

/**
 * @brief PulseAudio success callbacks preserve the zero-success and one-failure alarm convention.
 */
TEST(LinuxAudioCallbackTest, TranslatesSuccessAndFailureWithoutContext) {
  for (const int status : {0, 1, -1}) {
    safe::alarm_raw_t<int> alarm;
    platf::pa::success_cb(nullptr, status, &alarm);

    ASSERT_TRUE(alarm.wait_for(std::chrono::milliseconds {0}));
    ASSERT_TRUE(alarm.status().has_value());
    EXPECT_EQ(*alarm.status(), status == 0 ? 1 : 0);
  }
}

namespace {
  /**
   * @brief Restore the keep_default_sink setting when a test scope ends.
   */
  class keep_default_sink_guard_t {
  public:
    /**
     * @brief Save the current keep_default_sink value.
     */
    keep_default_sink_guard_t():
        original_ {config::audio.keep_default_sink} {
    }

    /**
     * @brief Restore the saved keep_default_sink value.
     */
    ~keep_default_sink_guard_t() {
      config::audio.keep_default_sink = original_;
    }

  private:
    bool original_;  ///< Value present before the test changed the setting.
  };
}  // namespace

TEST(KeepDefaultSinkAudioTest, CaptureSinkKeepsHostDefaultSink) {
  keep_default_sink_guard_t guard;
  config::audio.keep_default_sink = true;

  auto control = platf::audio_control();
  if (!control) {
    GTEST_SKIP() << "PulseAudio is unavailable in this environment";
  }

  ASSERT_TRUE(control->supports_keep_default_sink());

  auto before = control->sink_info();
  ASSERT_TRUE(before.has_value());

  // A nonexistent sink proves the capture path never swaps: set_sink would fail with -1.
  EXPECT_EQ(control->capture_sink("sunshine-test-nonexistent-sink"), 0);

  auto after = control->sink_info();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->host, before->host);
}

TEST(NullSinkOwnershipTest, AdoptedSinksSurvivePeerTeardown) {
  auto first = platf::audio_control();
  if (!first) {
    GTEST_SKIP() << "PulseAudio is unavailable in this environment";
  }

  auto first_sink = first->sink_info();
  ASSERT_TRUE(first_sink.has_value());
  ASSERT_TRUE(first_sink->null.has_value()) << "Sunshine null sinks missing from the graph";

  // A second session adopts the same sinks; tearing it down must not unload
  // sinks it didn't create (previously this yanked routed streams back to
  // the host default on every disconnect).
  {
    auto second = platf::audio_control();
    ASSERT_TRUE(second) << "PulseAudio rejected a second control connection";
    auto second_sink = second->sink_info();
    ASSERT_TRUE(second_sink.has_value());
    ASSERT_TRUE(second_sink->null.has_value());
  }

  auto after = first->sink_info();
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after->null.has_value()) << "Peer teardown unloaded adopted null sinks";
}

#endif  // __linux__
