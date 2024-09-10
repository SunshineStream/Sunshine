/**
 * @file src/nvenc/win/nvenc_d3d11.h
 * @brief Declarations for Direct3D11 NVENC encoder interface.
 */
#pragma once

#include "../nvenc_encoder.h"

#include <d3d11.h>

namespace nvenc {

  /**
   * @brief Direct3D11 NVENC encoder interface.
   */
  class nvenc_d3d11: virtual public nvenc_encoder {
  public:
    virtual ~nvenc_d3d11() = default;

    /**
     * @brief Get input surface texture.
     * @return Input surface texture.
     */
    virtual ID3D11Texture2D *
    get_input_texture() = 0;
  };

}  // namespace nvenc
