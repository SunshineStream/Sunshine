/**
 * @file src/platform/linux/cuda_color.h
 * @brief Sample conversions shared by the CUDA color conversion kernels and their tests.
 */
#pragma once

// standard includes
#include <cstdint>

#if defined(__CUDACC__)
  #define SUNSHINE_CUDA_HOST_DEVICE __host__ __device__
#else
  #define SUNSHINE_CUDA_HOST_DEVICE
#endif

namespace cuda {
  /**
   * @brief Convert a sample in 8-bit code values to a byte.
   *
   * Values at or below 0, and NaN, give 0. Values of 255 or more give 255. Other values are
   * truncated. Storing a float of 256 or more directly into a `std::uint8_t` wraps on the GPU.
   *
   * @param value The sample in 8-bit code values.
   * @return The sample clamped to [0, 255].
   */
  SUNSHINE_CUDA_HOST_DEVICE inline std::uint8_t clamp_to_uint8(float value) {
    if (!(value > 0.0f)) {
      return 0;
    }
    if (value >= 255.0f) {
      return 255;
    }
    return static_cast<std::uint8_t>(value);
  }

  /**
   * @brief Quantize an NV12 luma sample from the normalized color matrix.
   *
   * The matrix already applies the selected full or limited range.
   *
   * @param y Normalized luma.
   * @return The rounded and clamped 8-bit luma code value.
   */
  SUNSHINE_CUDA_HOST_DEVICE inline std::uint8_t nv12_luma_sample(float y) {
    return clamp_to_uint8(y * 255.0f + 0.5f);
  }

  /**
   * @brief Compute the NV12 chroma sample of a 2x2 pixel block.
   *
   * The kernel scales chroma by 256. At full range, the chroma of a pure primary is slightly
   * above 1.0, so the scaled average can exceed 255 and is clamped.
   *
   * @param lt Chroma (U or V) of the left-top pixel, as computed from the unorm color matrix.
   * @param rt Chroma of the right-top pixel.
   * @param lb Chroma of the left-bottom pixel.
   * @param rb Chroma of the right-bottom pixel.
   * @return The 8-bit chroma sample of the block.
   */
  SUNSHINE_CUDA_HOST_DEVICE inline std::uint8_t nv12_chroma_sample(float lt, float rt, float lb, float rb) {
    return clamp_to_uint8((lt * 256.0f + lb * 256.0f + rt * 256.0f + rb * 256.0f) * 0.25f);
  }
}  // namespace cuda
