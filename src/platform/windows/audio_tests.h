/**
 * @file src/platform/windows/audio_tests.h
 * @brief Test-only seams for Windows audio capture.
 *
 * Included only from SUNSHINE_TESTS translation units: the backend
 * implementation and the Windows audio unit tests. Production builds never
 * include this header.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

// platform includes
#include <mmdeviceapi.h>

// local includes
#include "src/platform/common.h"

namespace platf::audio::tests {
  /**
   * @brief Input and output bundle for microphone_attempts_loopback_for_target().
   *
   * The input fields describe the capture the test wants to exercise; the
   * seam fills the output fields with the attempt evidence. Call sites use
   * designated initializers so each input is named and outputs default to
   * "no attempt".
   */
  struct microphone_loopback_probe_t {
    std::string assigned_sink;  ///< Sink selected by the shared audio context.
    std::string configured_sink;  ///< Sink configured by the user, possibly empty.
    platf::audio_capture_intent_e intent {platf::audio_capture_intent_e::client_only};  ///< Capture intent recorded for the next microphone() call.
    std::uint32_t process_id {};  ///< PID recorded for the next microphone() call, or 0 for endpoint capture.
    std::uint64_t creation_time {};  ///< Creation marker recorded with the PID, or 0 for an unverifiable baseline.
    IMMDeviceEnumerator *device_enum {};  ///< Device enumerator supplied by the test.
    bool keep_default {};  ///< keep_default_sink value tested with.
    std::uint32_t attempted_process_id {};  ///< Receives the PID passed to the activator, or 0 when loopback was never attempted.
    bool microphone_returned {};  ///< Receives whether microphone() returned a microphone.
  };

  /**
   * @brief Exercise microphone() end to end with a recorded capture target and a failing test activator.
   *
   * Records the given capture target through the production setter, installs
   * a counting activator that returns no client, and runs the production
   * microphone() via the parameter-bundle overload. A valid target proves
   * the loopback-attempt branch ran when the activator was invoked; the
   * failing activator then proves the activation-failure fallback by
   * resolving the endpoint path afterwards. The current process identity is
   * the deterministic valid target: its PID and creation time are read from
   * the owned handle, while PID 0, an unverifiable marker, a mismatched
   * marker, or a mismatched intent must never attempt loopback.
   *
   * @param probe Capture probe: sinks, intent, identity, enumerator, and keep-default flag in; attempt evidence out.
   * @return True when process-loopback activation was attempted.
   */
  bool microphone_attempts_loopback_for_target(microphone_loopback_probe_t &probe);

  /**
   * @brief Exercise microphone() end to end through a succeeding loopback activation.
   *
   * Records the given capture target through the production setter, installs
   * an activator that initializes a fake client, and runs the production
   * microphone(). The endpoint fallback path creates no fake clients, so a
   * returned microphone paired with exactly one live fake client proves the
   * loopback branch — including post-activation revalidation — returned it
   * rather than the fallback. The current process identity is the
   * deterministic valid target: its PID and creation time are read from the
   * owned handle.
   *
   * @param probe Capture probe: sinks, intent, identity, enumerator, and keep-default flag in; attempt evidence out.
   * @return True when loopback activation succeeded and its microphone was returned without leaking.
   */
  bool microphone_returns_loopback_for_valid_target(microphone_loopback_probe_t &probe);
}  // namespace platf::audio::tests
