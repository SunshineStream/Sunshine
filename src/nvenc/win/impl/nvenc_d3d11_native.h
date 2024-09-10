/**
 * @file src/nvenc/win/impl/nvenc_d3d11_native.h
 * @brief Declarations for native Direct3D11 NVENC encoder.
 */
#pragma once

#include "nvenc_d3d11_base.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
namespace nvenc {
#endif

  /**
   * @brief Native Direct3D11 NVENC encoder.
   */
  class nvenc_d3d11_native final: public nvenc_d3d11_base {
  public:
    /**
     * @param d3d_device Direct3D11 device used for encoding.
     */
    nvenc_d3d11_native(ID3D11Device *d3d_device, shared_dll dll);
    ~nvenc_d3d11_native();

    ID3D11Texture2D *
    get_input_texture() override;

  private:
    bool
    create_and_register_input_buffer() override;

    const ID3D11DevicePtr d3d_device;
    ID3D11Texture2DPtr d3d_input_texture;
  };
}
