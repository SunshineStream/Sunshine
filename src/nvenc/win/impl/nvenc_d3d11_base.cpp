/**
 * @file src/nvenc/win/impl/nvenc_d3d11_base.cpp
 * @brief Definitions for abstract Direct3D11 NVENC encoder.
 */
#include "nvenc_d3d11_base.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
namespace nvenc {
#endif

  bool
  nvenc_d3d11_base::init_library() {
    if (nvenc) return true;
    if (!dll) return false;

    if (auto create_instance = (decltype(NvEncodeAPICreateInstance) *) GetProcAddress(dll.get(), "NvEncodeAPICreateInstance")) {
      auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
      new_nvenc->version = NV_ENCODE_API_FUNCTION_LIST_VER;
      if (nvenc_failed(create_instance(new_nvenc.get()))) {
        BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
      }
      else {
        nvenc = std::move(new_nvenc);
        return true;
      }
    }
    else {
      BOOST_LOG(error) << "NvEnc: No NvEncodeAPICreateInstance() in dynamic library";
    }

    return false;
  }
}
