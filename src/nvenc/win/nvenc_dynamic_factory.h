/**
 * @file src/nvenc/win/nvenc_dynamic_factory.h
 * @brief Declarations for Windows NVENC encoder factory.
 */
#pragma once

#include "nvenc_d3d11.h"

#include <memory>

namespace nvenc {

  /**
   * @brief Windows NVENC encoder factory.
   */
  class nvenc_dynamic_factory {
  public:
    virtual ~nvenc_dynamic_factory() = default;

    /**
     * @brief Initialize NVENC factory, depends on NVIDIA drivers present in the system.
     * @return `shared_ptr` containing factory on success, empty `shared_ptr` on error.
     */
    static std::shared_ptr<nvenc_dynamic_factory>
    get();

    /**
     * @brief Create native Direct3D11 NVENC encoder.
     * @param d3d_device Direct3D11 device.
     * @return `unique_ptr` containing encoder on success, empty `unique_ptr` on error.
     */
    virtual std::unique_ptr<nvenc_d3d11>
    create_nvenc_d3d11_native(ID3D11Device *d3d_device) = 0;

    /**
     * @brief Create CUDA NVENC encoder with Direct3D11 input surfaces.
     * @param d3d_device Direct3D11 device.
     * @return `unique_ptr` containing encoder on success, empty `unique_ptr` on error.
     */
    virtual std::unique_ptr<nvenc_d3d11>
    create_nvenc_d3d11_on_cuda(ID3D11Device *d3d_device) = 0;
  };

}  // namespace nvenc
