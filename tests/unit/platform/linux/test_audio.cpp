/**
 * @file tests/unit/platform/linux/test_audio.cpp
 * @brief Tests for the Linux audio capture channel map.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <functional>
  #include <vector>

  // lib includes
  #include <pulse/pulseaudio.h>

  // local includes
  #include "src/platform/common.h"
  #include "src/thread_safe.h"

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

    /**
     * @brief Make sure one of Sunshine's virtual sinks exists, loading it if it isn't listed.
     * @param found Whether the sink is in the sink list.
     * @param module Index of the module that owns the sink; set when the sink is loaded here.
     * @param load Loads the sink and returns its module index, or PA_INVALID_INDEX on failure.
     * @return `true` when the sink exists afterwards.
     */
    bool ensure_null_sink(bool found, std::uint32_t &module, const std::function<std::uint32_t()> &load);
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

/**
 * @brief A listed sink with no owner module (a native PipeWire node) is used as is.
 *
 * Loading it again would create a second sink with the same name, and Sunshine would fall back
 * to capturing the host sink's monitor.
 */
TEST(LinuxAudioNullSinkTest, UsesListedSinkWithoutOwnerModule) {
  std::uint32_t module = PA_INVALID_INDEX;
  int loads = 0;

  EXPECT_TRUE(platf::pa::ensure_null_sink(true, module, [&loads]() {
    ++loads;
    return 7u;
  }));
  EXPECT_EQ(loads, 0);
  EXPECT_EQ(module, PA_INVALID_INDEX);
}

/**
 * @brief A listed sink owned by a module keeps that module, so it is still unloaded on shutdown.
 */
TEST(LinuxAudioNullSinkTest, KeepsOwnerModuleOfListedSink) {
  std::uint32_t module = 42;
  int loads = 0;

  EXPECT_TRUE(platf::pa::ensure_null_sink(true, module, [&loads]() {
    ++loads;
    return 7u;
  }));
  EXPECT_EQ(loads, 0);
  EXPECT_EQ(module, 42u);
}

/**
 * @brief A sink that isn't listed is loaded once, and its module is kept for unloading.
 */
TEST(LinuxAudioNullSinkTest, LoadsMissingSink) {
  std::uint32_t module = PA_INVALID_INDEX;
  int loads = 0;

  EXPECT_TRUE(platf::pa::ensure_null_sink(false, module, [&loads]() {
    ++loads;
    return 7u;
  }));
  EXPECT_EQ(loads, 1);
  EXPECT_EQ(module, 7u);
}

/**
 * @brief A sink that fails to load is reported missing.
 */
TEST(LinuxAudioNullSinkTest, ReportsSinkThatFailsToLoad) {
  std::uint32_t module = PA_INVALID_INDEX;

  EXPECT_FALSE(platf::pa::ensure_null_sink(false, module, []() {
    return static_cast<std::uint32_t>(PA_INVALID_INDEX);
  }));
  EXPECT_EQ(module, PA_INVALID_INDEX);
}

#endif  // __linux__
