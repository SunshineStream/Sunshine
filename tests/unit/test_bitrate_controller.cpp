/**
 * @file tests/unit/test_bitrate_controller.cpp
 * @brief Tests for the MACCA adaptive bitrate controller.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

// local includes
#include <src/bitrate_controller.h>

using namespace std::literals;

namespace {
  constexpr std::uint32_t congested_rtt = 150;  ///< Above the default 100 ms threshold.
  constexpr std::uint32_t clear_rtt = 10;  ///< Below the default threshold minus margin (80 ms).
  constexpr std::uint32_t grey_rtt = 90;  ///< Between threshold minus margin and threshold.

  /**
   * @brief Drives a controller with samples at a fixed frame interval.
   */
  class BitrateControllerTest: public testing::Test {
  protected:
    /**
     * @brief Create the controller under test.
     *
     * @param initial_kbps Negotiated bitrate.
     * @param params Policy parameters.
     */
    void make(int initial_kbps, const bitrate::params_t &params = {}) {
      controller_.emplace(initial_kbps, params);
    }

    /**
     * @brief Feed samples with a constant RTT for a duration, one every 16 ms.
     *
     * @param rtt_ms RTT of every sample.
     * @param duration Time to advance.
     * @return Every target change, in order.
     */
    std::vector<int> feed(std::uint32_t rtt_ms, std::chrono::milliseconds duration) {
      std::vector<int> changes;
      const auto end = now_ + duration;
      while (now_ < end) {
        now_ += 16ms;
        if (auto target = controller_->on_sample(now_, rtt_ms); target.has_value()) {
          changes.push_back(*target);
          last_change_at_ = now_;
        }
      }
      return changes;
    }

    /**
     * @brief Feed congested samples until the first decrease.
     */
    void congest_once() {
      const auto changes = feed(congested_rtt, 2100ms);
      ASSERT_EQ(changes.size(), 1);
    }

    /**
     * @brief Get the controller under test.
     *
     * @return Controller created by `make`.
     */
    bitrate::bitrate_controller_t &controller() {
      return *controller_;
    }

    /**
     * @brief Get the simulated clock.
     *
     * @return Time of the most recent sample.
     */
    std::chrono::steady_clock::time_point now() const {
      return now_;
    }

    /**
     * @brief Get the time of the most recent target change.
     *
     * @return Time of the sample that changed the target.
     */
    std::chrono::steady_clock::time_point last_change_at() const {
      return last_change_at_;
    }

  private:
    std::chrono::steady_clock::time_point now_ {};  ///< Simulated clock.
    std::chrono::steady_clock::time_point last_change_at_ {};  ///< Time of the most recent target change.
    std::optional<bitrate::bitrate_controller_t> controller_;  ///< Controller under test.
  };
}  // namespace

TEST_F(BitrateControllerTest, KeepsTheNegotiatedBitrateOnAClearPath) {
  make(20000);
  EXPECT_TRUE(feed(clear_rtt, 60s).empty());
  EXPECT_EQ(controller().current_target_kbps(), 20000);
}

TEST_F(BitrateControllerTest, DecreasesOnlyAfterTheHysteresisWindow) {
  make(20000);
  EXPECT_TRUE(feed(congested_rtt, 1900ms).empty());

  const auto changes = feed(congested_rtt, 1200ms);
  ASSERT_EQ(changes.size(), 1);
  EXPECT_EQ(changes[0], 14000);
  EXPECT_EQ(controller().recovery_target_kbps(), 20000);
}

TEST_F(BitrateControllerTest, ShortSpikesDoNotTriggerADecrease) {
  make(20000);
  for (int i = 0; i < 10; ++i) {
    feed(congested_rtt, 1500ms);
    feed(clear_rtt, 100ms);
  }
  EXPECT_EQ(controller().current_target_kbps(), 20000);
}

TEST_F(BitrateControllerTest, CascadedDecreasesWaitForAFullWindowAndStopAtTheFloor) {
  bitrate::params_t params;
  params.min_kbps = 5000;
  make(20000, params);

  const auto changes = feed(congested_rtt, 30s);
  ASSERT_EQ(changes.size(), 4);
  EXPECT_EQ(changes[0], 14000);
  EXPECT_EQ(changes[1], 9800);
  EXPECT_EQ(changes[2], 6860);
  EXPECT_EQ(changes[3], 5000);
}

TEST_F(BitrateControllerTest, AppliesFastConvergenceOnACascadedDecrease) {
  make(20000);
  feed(congested_rtt, 4100ms);  // two decreases: 20000 -> 14000 -> 9800
  EXPECT_EQ(controller().current_target_kbps(), 9800);
  EXPECT_EQ(controller().recovery_target_kbps(), 11900);  // 14000 * (1 + 0.7) / 2
}

TEST_F(BitrateControllerTest, KeepsTheRecoveryTargetWithoutFastConvergence) {
  bitrate::params_t params;
  params.fast_convergence = false;
  make(20000, params);
  feed(congested_rtt, 4100ms);
  EXPECT_EQ(controller().current_target_kbps(), 9800);
  EXPECT_EQ(controller().recovery_target_kbps(), 14000);
}

TEST_F(BitrateControllerTest, RecoversAlongTheCubicCurve) {
  make(20000);
  congest_once();
  ASSERT_EQ(controller().current_target_kbps(), 14000);
  const auto decrease_at = last_change_at();

  constexpr double k = 15.0;
  constexpr double c = 0.3 * 20000 / (k * k * k);
  int previous = controller().current_target_kbps();
  int increases = 0;
  while (now() - decrease_at < 15s) {
    for (const int target : feed(clear_rtt, 100ms)) {
      const double t = std::chrono::duration<double>(last_change_at() - decrease_at).count();
      EXPECT_GT(target, previous);
      EXPECT_EQ(target, static_cast<int>(c * std::pow(t - k, 3) + 20000)) << "t = " << t;
      previous = target;
      ++increases;
    }
  }

  // One increase per second, ending just below the rate held before the decrease
  EXPECT_GE(increases, 12);
  EXPECT_LE(increases, 14);
  EXPECT_LT(controller().current_target_kbps(), 20000);
  EXPECT_GT(controller().current_target_kbps(), 19990);
}

TEST_F(BitrateControllerTest, ProbesPastTheRecoveryTargetUpToTheCeiling) {
  bitrate::params_t params;
  params.fast_convergence = false;
  make(20000, params);
  feed(congested_rtt, 4100ms);  // 20000 -> 14000 -> 9800; recovery target 14000
  feed(clear_rtt, 40s);
  EXPECT_EQ(controller().current_target_kbps(), 20000);
}

TEST_F(BitrateControllerTest, DoesNotOverflowLongAfterTheLastDecrease) {
  make(500000);
  congest_once();
  feed(grey_rtt, 3h);
  feed(clear_rtt, 5s);
  EXPECT_EQ(controller().current_target_kbps(), 500000);
}

TEST_F(BitrateControllerTest, NeitherIncreasesNorDecreasesInsideTheMargin) {
  make(20000);
  congest_once();
  ASSERT_EQ(controller().current_target_kbps(), 14000);
  EXPECT_TRUE(feed(grey_rtt, 30s).empty());
}

TEST_F(BitrateControllerTest, ChangesTheTargetAtMostOncePerDecisionInterval) {
  make(20000);
  congest_once();
  const auto changes = feed(clear_rtt, 10s);
  EXPECT_GE(changes.size(), 8);
  EXPECT_LE(changes.size(), 10);
}

TEST_F(BitrateControllerTest, RespectsAConfiguredCeilingBelowTheNegotiatedBitrate) {
  bitrate::params_t params;
  params.max_kbps = 10000;
  make(20000, params);
  EXPECT_EQ(controller().current_target_kbps(), 10000);
  congest_once();
  EXPECT_EQ(controller().current_target_kbps(), 7000);
  feed(clear_rtt, 60s);
  EXPECT_EQ(controller().current_target_kbps(), 10000);
}

TEST_F(BitrateControllerTest, NeverExceedsTheNegotiatedBitrate) {
  bitrate::params_t params;
  params.max_kbps = 50000;
  make(20000, params);
  congest_once();
  feed(clear_rtt, 60s);
  EXPECT_EQ(controller().current_target_kbps(), 20000);
}

TEST_F(BitrateControllerTest, KeepsAFloorAboveTheCeilingFromBlockingDecreases) {
  bitrate::params_t params;
  params.min_kbps = 30000;
  make(20000, params);
  EXPECT_TRUE(feed(congested_rtt, 10s).empty());
  EXPECT_EQ(controller().current_target_kbps(), 20000);
}
