/**
 * @file src/nvenc/win/impl/nvenc_d3d11_on_cuda.h
 * @brief Declarations for CUDA NVENC encoder with Direct3D11 input surfaces.
 */
#pragma once

#include "nvenc_d3d11_base.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
  #include <ffnvcodec/dynlink_cuda.h>
namespace nvenc {
#endif

  /**
   * @brief Interop Direct3D11 on CUDA NVENC encoder.
   *        Input surface is Direct3D11, encoding is performed by CUDA.
   */
  class nvenc_d3d11_on_cuda final: public nvenc_d3d11_base {
  public:
    /**
     * @param d3d_device Direct3D11 device that will create input surface texture.
     *                   CUDA encoding device will be derived from it.
     */
    nvenc_d3d11_on_cuda(ID3D11Device *d3d_device, shared_dll dll);
    ~nvenc_d3d11_on_cuda();

    ID3D11Texture2D *
    get_input_texture() override;

  private:
    bool
    init_library() override;

    bool
    create_and_register_input_buffer() override;

    bool
    synchronize_input_buffer() override;

    bool
    cuda_succeeded(CUresult result);

    bool
    cuda_failed(CUresult result);

    struct autopop_context {
      autopop_context(nvenc_d3d11_on_cuda &parent, CUcontext pushed_context):
          parent(parent),
          pushed_context(pushed_context) {
      }

      ~autopop_context();

      explicit
      operator bool() const {
        return pushed_context != nullptr;
      }

      nvenc_d3d11_on_cuda &parent;
      CUcontext pushed_context = nullptr;
    };

    autopop_context
    push_context();

    HMODULE dll = NULL;
    const ID3D11DevicePtr d3d_device;
    ID3D11Texture2DPtr d3d_input_texture;

    struct {
      tcuInit *cuInit;
      tcuD3D11GetDevice *cuD3D11GetDevice;
      tcuCtxCreate_v2 *cuCtxCreate;
      tcuCtxDestroy_v2 *cuCtxDestroy;
      tcuCtxPushCurrent_v2 *cuCtxPushCurrent;
      tcuCtxPopCurrent_v2 *cuCtxPopCurrent;
      tcuMemAllocPitch_v2 *cuMemAllocPitch;
      tcuMemFree_v2 *cuMemFree;
      tcuGraphicsD3D11RegisterResource *cuGraphicsD3D11RegisterResource;
      tcuGraphicsUnregisterResource *cuGraphicsUnregisterResource;
      tcuGraphicsMapResources *cuGraphicsMapResources;
      tcuGraphicsUnmapResources *cuGraphicsUnmapResources;
      tcuGraphicsSubResourceGetMappedArray *cuGraphicsSubResourceGetMappedArray;
      tcuMemcpy2D_v2 *cuMemcpy2D;
      shared_dll dll;
    } cuda_functions = {};

    CUresult last_cuda_error = CUDA_SUCCESS;
    CUcontext cuda_context = nullptr;
    CUgraphicsResource cuda_d3d_input_texture = nullptr;
    CUdeviceptr cuda_surface = 0;
    size_t cuda_surface_pitch = 0;
  };
}
