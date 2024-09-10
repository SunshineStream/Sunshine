/**
 * @file src/nvenc/win/impl/nvenc_d3d11_on_cuda.cpp
 * @brief Definitions for CUDA NVENC encoder with Direct3D11 input surfaces.
 */
#include "nvenc_d3d11_on_cuda.h"

#include "../../common_impl/nvenc_utils.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
namespace nvenc {
#endif

  nvenc_d3d11_on_cuda::nvenc_d3d11_on_cuda(ID3D11Device *d3d_device, shared_dll dll):
      nvenc_d3d11_base(NV_ENC_DEVICE_TYPE_CUDA, dll),
      d3d_device(d3d_device) {
  }

  nvenc_d3d11_on_cuda::~nvenc_d3d11_on_cuda() {
    if (encoder) destroy_encoder();

    if (cuda_context) {
      {
        auto autopop_context = push_context();

        if (cuda_d3d_input_texture) {
          if (cuda_failed(cuda_functions.cuGraphicsUnregisterResource(cuda_d3d_input_texture))) {
            BOOST_LOG(error) << "NvEnc: cuGraphicsUnregisterResource() failed: error " << last_cuda_error;
          }
          cuda_d3d_input_texture = nullptr;
        }

        if (cuda_surface) {
          if (cuda_failed(cuda_functions.cuMemFree(cuda_surface))) {
            BOOST_LOG(error) << "NvEnc: cuMemFree() failed: error " << last_cuda_error;
          }
          cuda_surface = 0;
        }
      }

      if (cuda_failed(cuda_functions.cuCtxDestroy(cuda_context))) {
        BOOST_LOG(error) << "NvEnc: cuCtxDestroy() failed: error " << last_cuda_error;
      }
      cuda_context = nullptr;
    }
  }

  ID3D11Texture2D *
  nvenc_d3d11_on_cuda::get_input_texture() {
    return d3d_input_texture.GetInterfacePtr();
  }

  bool
  nvenc_d3d11_on_cuda::init_library() {
    if (!nvenc_d3d11_base::init_library()) return false;

    constexpr auto dll_name = "nvcuda.dll";

    if ((cuda_functions.dll = make_shared_dll(LoadLibraryEx(dll_name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)))) {
      auto load_function = [&]<typename T>(T &location, auto symbol) -> bool {
        location = (T) GetProcAddress(cuda_functions.dll.get(), symbol);
        return location != nullptr;
      };
      if (!load_function(cuda_functions.cuInit, "cuInit") ||
          !load_function(cuda_functions.cuD3D11GetDevice, "cuD3D11GetDevice") ||
          !load_function(cuda_functions.cuCtxCreate, "cuCtxCreate_v2") ||
          !load_function(cuda_functions.cuCtxDestroy, "cuCtxDestroy_v2") ||
          !load_function(cuda_functions.cuCtxPushCurrent, "cuCtxPushCurrent_v2") ||
          !load_function(cuda_functions.cuCtxPopCurrent, "cuCtxPopCurrent_v2") ||
          !load_function(cuda_functions.cuMemAllocPitch, "cuMemAllocPitch_v2") ||
          !load_function(cuda_functions.cuMemFree, "cuMemFree_v2") ||
          !load_function(cuda_functions.cuGraphicsD3D11RegisterResource, "cuGraphicsD3D11RegisterResource") ||
          !load_function(cuda_functions.cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource") ||
          !load_function(cuda_functions.cuGraphicsMapResources, "cuGraphicsMapResources") ||
          !load_function(cuda_functions.cuGraphicsUnmapResources, "cuGraphicsUnmapResources") ||
          !load_function(cuda_functions.cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray") ||
          !load_function(cuda_functions.cuMemcpy2D, "cuMemcpy2D_v2")) {
        BOOST_LOG(error) << "NvEnc: missing CUDA functions in " << dll_name;
        cuda_functions = {};
      }
    }
    else {
      BOOST_LOG(debug) << "NvEnc: couldn't load CUDA dynamic library " << dll_name;
    }

    if (cuda_functions.dll) {
      IDXGIDevicePtr dxgi_device;
      IDXGIAdapterPtr dxgi_adapter;
      if (d3d_device &&
          SUCCEEDED(d3d_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
          SUCCEEDED(dxgi_device->GetAdapter(&dxgi_adapter))) {
        CUdevice cuda_device;
        if (cuda_succeeded(cuda_functions.cuInit(0)) &&
            cuda_succeeded(cuda_functions.cuD3D11GetDevice(&cuda_device, dxgi_adapter)) &&
            cuda_succeeded(cuda_functions.cuCtxCreate(&cuda_context, CU_CTX_SCHED_BLOCKING_SYNC, cuda_device)) &&
            cuda_succeeded(cuda_functions.cuCtxPopCurrent(&cuda_context))) {
          device = cuda_context;
        }
        else {
          BOOST_LOG(error) << "NvEnc: couldn't create CUDA interop context: error " << last_cuda_error;
        }
      }
      else {
        BOOST_LOG(error) << "NvEnc: couldn't get DXGI adapter for CUDA interop";
      }
    }

    return device != nullptr;
  }

  bool
  nvenc_d3d11_on_cuda::create_and_register_input_buffer() {
    if (encoder_params.buffer_format != NV_ENC_BUFFER_FORMAT_YUV444_10BIT) {
      BOOST_LOG(error) << "NvEnc: CUDA interop is expected to be used only for 10-bit 4:4:4 encoding";
      return false;
    }

    if (!d3d_input_texture) {
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = encoder_params.width;
      desc.Height = encoder_params.height * 3;  // Planar YUV
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = dxgi_format_from_nvenc_format(encoder_params.buffer_format);
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;

      if (d3d_device->CreateTexture2D(&desc, nullptr, &d3d_input_texture) != S_OK) {
        BOOST_LOG(error) << "NvEnc: couldn't create input texture";
        return false;
      }
    }

    {
      auto autopop_context = push_context();
      if (!autopop_context) return false;

      if (!cuda_d3d_input_texture) {
        if (cuda_failed(cuda_functions.cuGraphicsD3D11RegisterResource(
              &cuda_d3d_input_texture,
              d3d_input_texture,
              CU_GRAPHICS_REGISTER_FLAGS_NONE))) {
          BOOST_LOG(error) << "NvEnc: cuGraphicsD3D11RegisterResource() failed: error " << last_cuda_error;
          return false;
        }
      }

      if (!cuda_surface) {
        if (cuda_failed(cuda_functions.cuMemAllocPitch(
              &cuda_surface,
              &cuda_surface_pitch,
              // Planar 16-bit YUV
              encoder_params.width * 2,
              encoder_params.height * 3, 16))) {
          BOOST_LOG(error) << "NvEnc: cuMemAllocPitch() failed: error " << last_cuda_error;
          return false;
        }
      }
    }

    if (!registered_input_buffer) {
      NV_ENC_REGISTER_RESOURCE register_resource = { NV_ENC_REGISTER_RESOURCE_VER };
      register_resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
      register_resource.width = encoder_params.width;
      register_resource.height = encoder_params.height;
      register_resource.pitch = cuda_surface_pitch;
      register_resource.resourceToRegister = (void *) cuda_surface;
      register_resource.bufferFormat = encoder_params.buffer_format;
      register_resource.bufferUsage = NV_ENC_INPUT_IMAGE;

      if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &register_resource))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterResource() failed: " << last_nvenc_error_string;
        return false;
      }

      registered_input_buffer = register_resource.registeredResource;
    }

    return true;
  }

  bool
  nvenc_d3d11_on_cuda::synchronize_input_buffer() {
    auto autopop_context = push_context();
    if (!autopop_context) return false;

    if (cuda_failed(cuda_functions.cuGraphicsMapResources(1, &cuda_d3d_input_texture, 0))) {
      BOOST_LOG(error) << "NvEnc: cuGraphicsMapResources() failed: error " << last_cuda_error;
      return false;
    }

    auto unmap = [&]() -> bool {
      if (cuda_failed(cuda_functions.cuGraphicsUnmapResources(1, &cuda_d3d_input_texture, 0))) {
        BOOST_LOG(error) << "NvEnc: cuGraphicsUnmapResources() failed: error " << last_cuda_error;
        return false;
      }
      return true;
    };
    auto unmap_guard = util::fail_guard(unmap);

    CUarray input_texture_array;
    if (cuda_failed(cuda_functions.cuGraphicsSubResourceGetMappedArray(&input_texture_array, cuda_d3d_input_texture, 0, 0))) {
      BOOST_LOG(error) << "NvEnc: cuGraphicsSubResourceGetMappedArray() failed: error " << last_cuda_error;
      return false;
    }

    {
      CUDA_MEMCPY2D copy_params = {};
      copy_params.srcMemoryType = CU_MEMORYTYPE_ARRAY;
      copy_params.srcArray = input_texture_array;
      copy_params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy_params.dstDevice = cuda_surface;
      copy_params.dstPitch = cuda_surface_pitch;
      // Planar 16-bit YUV
      copy_params.WidthInBytes = encoder_params.width * 2;
      copy_params.Height = encoder_params.height * 3;

      if (cuda_failed(cuda_functions.cuMemcpy2D(&copy_params))) {
        BOOST_LOG(error) << "NvEnc: cuMemcpy2D() failed: error " << last_cuda_error;
        return false;
      }
    }

    unmap_guard.disable();
    return unmap();
  }

  bool
  nvenc_d3d11_on_cuda::cuda_succeeded(CUresult result) {
    last_cuda_error = result;
    return result == CUDA_SUCCESS;
  }

  bool
  nvenc_d3d11_on_cuda::cuda_failed(CUresult result) {
    last_cuda_error = result;
    return result != CUDA_SUCCESS;
  }

  nvenc_d3d11_on_cuda::autopop_context::~autopop_context() {
    if (pushed_context) {
      CUcontext popped_context;
      if (parent.cuda_failed(parent.cuda_functions.cuCtxPopCurrent(&popped_context))) {
        BOOST_LOG(error) << "NvEnc: cuCtxPopCurrent() failed: error " << parent.last_cuda_error;
      }
    }
  }

  nvenc_d3d11_on_cuda::autopop_context
  nvenc_d3d11_on_cuda::push_context() {
    if (cuda_context &&
        cuda_succeeded(cuda_functions.cuCtxPushCurrent(cuda_context))) {
      return { *this, cuda_context };
    }
    else {
      BOOST_LOG(error) << "NvEnc: cuCtxPushCurrent() failed: error " << last_cuda_error;
      return { *this, nullptr };
    }
  }
}
