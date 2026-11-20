/**
 * @file tests/unit/test_nvhttp_probe_retry.cpp
 * @brief Test the encoder probe retry before a stream starts.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <vector>

// local includes
#include <src/nvhttp.h>

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief A fake clock that only the fake sleep moves forward, recording each pause.
   */
  struct fake_time_t {
    std::chrono::steady_clock::time_point now_point {};  ///< Current fake time.
    std::vector<std::chrono::milliseconds> sleeps;  ///< Pauses taken, in order.

    /**
     * @brief Run nvhttp::probe_until_ready() with this clock, 10 seconds of wait and 500 ms between attempts.
     *
     * @param probe Returns `0` once it succeeds.
     * @return The result of nvhttp::probe_until_ready().
     */
    int run(const std::function<int()> &probe) {
      return nvhttp::probe_until_ready(
        probe,
        10s,
        500ms,
        [this] {
          return now_point;
        },
        [this](std::chrono::milliseconds duration) {
          sleeps.push_back(duration);
          now_point += duration;
        }
      );
    }
  };
}  // namespace

// An encoder that works at once: no pause before the stream.
TEST(ProbeUntilReady, FirstSuccessDoesNotWait) {
  fake_time_t time;
  int calls = 0;
  const auto works = [&calls] {
    ++calls;
    return 0;
  };
  EXPECT_EQ(time.run(works), 0);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(time.sleeps.empty());
}

// The display lights up after a few attempts (woken by the request): the stream starts instead of a refusal.
TEST(ProbeUntilReady, SucceedsOnceADisplayLightsUp) {
  fake_time_t time;
  int calls = 0;
  const auto lights_up_on_the_fourth = [&calls] {
    return ++calls <= 3 ? -1 : 0;
  };
  EXPECT_EQ(time.run(lights_up_on_the_fourth), 0);
  EXPECT_EQ(calls, 4);
  EXPECT_EQ(time.sleeps, (std::vector<std::chrono::milliseconds>(3, 500ms)));
}

// No display ever: the refusal comes once the wait is over, after an attempt every interval.
TEST(ProbeUntilReady, GivesUpAfterTheWait) {
  fake_time_t time;
  int calls = 0;
  const auto never_works = [&calls] {
    ++calls;
    return -1;
  };
  EXPECT_EQ(time.run(never_works), -1);
  EXPECT_EQ(calls, 21);
  EXPECT_EQ(time.sleeps.size(), 20u);
}

namespace {
  struct probe_run_t {
    int calls = 0;
    int checks = 0;
    std::vector<bool> displays;  ///< What each display check answers.

    int probe_again_with(int first_result, int second_result) {
      return nvhttp::probe_again_if_display_appeared(
        [this, first_result, second_result] {
          return ++calls == 1 ? first_result : second_result;
        },
        [this] {
          return displays[checks++];
        }
      );
    }
  };
}  // namespace

TEST(ProbeAgainIfDisplayAppeared, ProbesOnceWithADisplayFromTheStart) {
  probe_run_t run {.displays = {true}};

  EXPECT_EQ(run.probe_again_with(0, 1), 0);
  EXPECT_EQ(run.calls, 1);
}

TEST(ProbeAgainIfDisplayAppeared, ProbesOnceWithNoDisplayAtAll) {
  probe_run_t run {.displays = {false, false}};

  EXPECT_EQ(run.probe_again_with(1, 0), 1);
  EXPECT_EQ(run.calls, 1);
}

TEST(ProbeAgainIfDisplayAppeared, ProbesAgainWhenADisplayAppearedDuringAProbeThatPassed) {
  probe_run_t run {.displays = {false, true}};

  EXPECT_EQ(run.probe_again_with(0, 0), 0);
  EXPECT_EQ(run.calls, 2);
}

TEST(ProbeAgainIfDisplayAppeared, ReturnsTheResultOfTheSecondProbe) {
  probe_run_t run {.displays = {false, true}};

  EXPECT_EQ(run.probe_again_with(1, 0), 0);
  EXPECT_EQ(run.calls, 2);
}
