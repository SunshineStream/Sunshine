/**
 * @file src/nvenc/common_impl/nvenc_utils.h
 * @brief Declarations for NVENC utilities.
 */
#pragma once

#ifdef _WIN32
  #include <dxgiformat.h>
#endif

#include "src/platform/common.h"
#include "src/video_colorspace.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
  #include <ffnvcodec/nvEncodeAPI.h>
namespace nvenc {
#endif

  /**
   * @brief YUV colorspace and color range.
   */
  struct nvenc_colorspace_t {
    NV_ENC_VUI_COLOR_PRIMARIES primaries;
    NV_ENC_VUI_TRANSFER_CHARACTERISTIC tranfer_function;
    NV_ENC_VUI_MATRIX_COEFFS matrix;
    bool full_range;
  };

#ifdef _WIN32
  DXGI_FORMAT
  dxgi_format_from_nvenc_format(NV_ENC_BUFFER_FORMAT format);
#endif

  NV_ENC_BUFFER_FORMAT
  nvenc_format_from_sunshine_format(platf::pix_fmt_e format);

  nvenc_colorspace_t
  nvenc_colorspace_from_sunshine_colorspace(const video::sunshine_colorspace_t &sunshine_colorspace);
}
