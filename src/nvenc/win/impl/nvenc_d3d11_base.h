/**
 * @file src/nvenc/win/impl/nvenc_d3d11_base.h
 * @brief Declarations for abstract Direct3D11 NVENC encoder.
 */
#pragma once

#include "../../common_impl/nvenc_base.h"
#include "../nvenc_d3d11.h"
#include "nvenc_shared_dll.h"

#include <comdef.h>
#include <d3d11.h>

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
namespace nvenc {
#endif

  _COM_SMARTPTR_TYPEDEF(ID3D11Device, IID_ID3D11Device);
  _COM_SMARTPTR_TYPEDEF(ID3D11Texture2D, IID_ID3D11Texture2D);
  _COM_SMARTPTR_TYPEDEF(IDXGIDevice, IID_IDXGIDevice);
  _COM_SMARTPTR_TYPEDEF(IDXGIAdapter, IID_IDXGIAdapter);

  /**
   * @brief Abstract Direct3D11 NVENC encoder.
   *        Encapsulates common code used by native and interop implementations.
   */
  class nvenc_d3d11_base: public nvenc_base, virtual public nvenc_d3d11 {
  public:
    nvenc_d3d11_base(NV_ENC_DEVICE_TYPE device_type, shared_dll dll):
        nvenc_base(device_type),
        dll(dll) {}

  protected:
    bool
    init_library() override;

  private:
    shared_dll dll;
  };
}
