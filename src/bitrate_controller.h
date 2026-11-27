/**
 * @file src/bitrate_controller.h
 * @brief Declarations for the MACCA adaptive bitrate controller.
 *
 * MACCA (Multi-state AIMD with Cubic Convergence Adaptation) is a server-side
 * adaptive bitrate policy that only needs the ENet round-trip time Sunshine
 * already measures for every client, so it works with unmodified Moonlight clients.
 *
 * - Decrease: when the RTT stays above `rtt_threshold_ms` for `hysteresis_ms`,
 *   the target is cut to `max(beta * target, min_kbps)`.
 * - Increase: when the RTT stays below `rtt_threshold_ms - margin_ms` for
 *   `stable_time_ms`, the target follows the CUBIC recovery curve
 *   `B(t) = C * (t - K)^3 + L`, where `t` is the time since the last decrease,
 *   `L` is the rate held before that decrease, `K = cubic_recovery_ms` and
 *   `C = (1 - beta) * L / K^3`. The curve recovers quickly towards `L`, levels
 *   off around it, then probes beyond it.
 * - Fast convergence: when a decrease happens before the rate has recovered to
 *   `L`, `L` is lowered to `target * (1 + beta) / 2`, so the next recovery aims
 *   closer to the capacity actually available.
 *
 * Decisions are taken at most once every `decision_interval_ms`, independently of the frame rate.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>

namespace bitrate {

  /**
   * @brief Tunable parameters of the MACCA policy.
   *        Times are in milliseconds and bitrates in kbps.
   */
  struct params_t {
    int rtt_threshold_ms = 100;  ///< RTT above this value is treated as congestion.
    int hysteresis_ms = 2000;  ///< Continuous congested time before a decrease.
    int margin_ms = 20;  ///< The RTT must be below `rtt_threshold_ms - margin_ms` to count as clear.
    int stable_time_ms = 1000;  ///< Continuous clear time before each increase.
    double beta = 0.7;  ///< Multiplicative decrease factor, in (0, 1).
    int cubic_recovery_ms = 15000;  ///< Time the cubic curve takes to return to the rate held before the last decrease (K).
    bool fast_convergence = true;  ///< Lower the recovery target when a decrease happens before recovery completes.
    int min_kbps = 1000;  ///< Bitrate floor.
    int max_kbps = 0;  ///< Bitrate ceiling; 0 or a value above the negotiated bitrate means the negotiated bitrate.
    int decision_interval_ms = 1000;  ///< Minimum interval between two decisions.
  };

  /**
   * @brief Per-session MACCA controller.
   *
   * The controller only holds the policy. The caller feeds it one RTT sample per
   * sent frame and forwards any new target to the encoder.
   */
  class bitrate_controller_t {
  public:
    /**
     * @brief Construct a controller that starts at the negotiated bitrate.
     *
     * @param initial_kbps Bitrate negotiated with the client, in kbps.
     * @param params Policy parameters.
     */
    bitrate_controller_t(int initial_kbps, const params_t &params);

    /**
     * @brief Feed one RTT observation.
     *
     * @param now Time of the observation.
     * @param rtt_ms Smoothed round-trip time to the client, in milliseconds.
     * @return The new target bitrate in kbps when it changes, otherwise `std::nullopt`.
     */
    std::optional<int> on_sample(std::chrono::steady_clock::time_point now, std::uint32_t rtt_ms);

    /**
     * @brief Get the bitrate currently requested from the encoder.
     *
     * @return Target bitrate in kbps.
     */
    int current_target_kbps() const {
      return current_kbps;
    }

    /**
     * @brief Get the rate the cubic recovery currently aims at.
     *
     * @return Recovery target `L` in kbps.
     */
    int recovery_target_kbps() const {
      return last_known_good;
    }

  private:
    params_t params;  ///< Policy parameters.
    int max_kbps;  ///< Effective ceiling.
    int min_kbps;  ///< Effective floor, never above the ceiling.
    int current_kbps;  ///< Current target.
    int last_known_good;  ///< Rate held before the most recent decrease (L).

    std::optional<std::chrono::steady_clock::time_point> rtt_over_since;  ///< Start of the current congested streak.
    std::optional<std::chrono::steady_clock::time_point> rtt_clear_since;  ///< Start of the current clear streak.
    std::optional<std::chrono::steady_clock::time_point> last_decision_at;  ///< Last decision time.
    std::optional<std::chrono::steady_clock::time_point> last_decrease_at;  ///< Origin of the cubic recovery curve.
  };

}  // namespace bitrate
