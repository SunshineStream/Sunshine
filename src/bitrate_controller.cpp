/**
 * @file src/bitrate_controller.cpp
 * @brief Definitions for the MACCA adaptive bitrate controller.
 */
// header include
#include "bitrate_controller.h"

// standard includes
#include <algorithm>
#include <cmath>

namespace bitrate {

  using std::chrono::milliseconds;

  bitrate_controller_t::bitrate_controller_t(int initial_kbps, const params_t &params):
      params {params},
      max_kbps {params.max_kbps > 0 ? std::min(params.max_kbps, initial_kbps) : initial_kbps},
      min_kbps {std::min(params.min_kbps, max_kbps)},
      current_kbps {max_kbps},
      last_known_good {max_kbps} {
  }

  std::optional<int> bitrate_controller_t::on_sample(std::chrono::steady_clock::time_point now, std::uint32_t rtt_ms) {
    const auto rtt = static_cast<std::int64_t>(rtt_ms);

    // Track the congested and clear streaks on every sample.
    if (rtt > params.rtt_threshold_ms) {
      if (!rtt_over_since) {
        rtt_over_since = now;
      }
    } else {
      rtt_over_since.reset();
    }

    if (rtt < params.rtt_threshold_ms - params.margin_ms) {
      if (!rtt_clear_since) {
        rtt_clear_since = now;
      }
    } else {
      rtt_clear_since.reset();
    }

    if (last_decision_at && now - *last_decision_at < milliseconds(params.decision_interval_ms)) {
      return std::nullopt;
    }
    last_decision_at = now;

    // Decrease after sustained congestion.
    if (rtt_over_since && now - *rtt_over_since >= milliseconds(params.hysteresis_ms)) {
      const int new_kbps = std::max(static_cast<int>(current_kbps * params.beta), min_kbps);
      if (new_kbps == current_kbps) {
        return std::nullopt;
      }

      if (params.fast_convergence && current_kbps < last_known_good) {
        last_known_good = static_cast<int>(current_kbps * (1.0 + params.beta) / 2.0);
      } else {
        last_known_good = current_kbps;
      }
      current_kbps = new_kbps;
      last_decrease_at = now;

      // A further decrease needs another full hysteresis window of congestion.
      rtt_over_since = now;
      return current_kbps;
    }

    // Increase along the cubic curve after a sustained clear path.
    if (last_decrease_at && rtt_clear_since && now - *rtt_clear_since >= milliseconds(params.stable_time_ms)) {
      const double t = std::chrono::duration<double>(now - *last_decrease_at).count();
      const double k = params.cubic_recovery_ms / 1000.0;
      const double c = (1.0 - params.beta) * last_known_good / (k * k * k);
      const double target = c * std::pow(t - k, 3) + last_known_good;
      // Clamp before converting: far from the last decrease the curve exceeds the range of int.
      const auto new_kbps = static_cast<int>(std::clamp(target, static_cast<double>(current_kbps), static_cast<double>(max_kbps)));
      if (new_kbps == current_kbps) {
        return std::nullopt;
      }

      current_kbps = new_kbps;

      // The next increase needs another full stable window.
      rtt_clear_since = now;
      return current_kbps;
    }

    return std::nullopt;
  }

}  // namespace bitrate
