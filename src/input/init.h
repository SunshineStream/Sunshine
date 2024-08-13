/**
 * @file src/input/init.h
 * @brief Declarations for common input initialization.
 */
#pragma once

namespace input {
  struct input_t;

  enum class batch_result_e {
    batched,  ///< Batched with the source entry
    not_batchable,  ///< Not eligible to batch but continue attempts to batch
    terminate_batch,  ///< Stop trying to batch
  };
}  // namespace input
