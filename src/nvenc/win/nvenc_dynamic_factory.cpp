/**
 * @file src/nvenc/win/nvenc_dynamic_factory.cpp
 * @brief Definitions for Windows NVENC encoder factory.
 */
#include "nvenc_dynamic_factory.h"

#include "impl/nvenc_dynamic_factory_1100.h"
#include "impl/nvenc_dynamic_factory_1200.h"
#include "impl/nvenc_dynamic_factory_1202.h"

#include "impl/nvenc_shared_dll.h"

#include "src/logging.h"

#include <windows.h>

#include <array>
#include <tuple>

uint32_t
NvEncodeAPIGetMaxSupportedVersion(uint32_t *version);

namespace {
  using namespace nvenc;

  constexpr std::array factory_priorities = {
    std::tuple(&nvenc_dynamic_factory_1202::get, 1202),
    std::tuple(&nvenc_dynamic_factory_1200::get, 1200),
    std::tuple(&nvenc_dynamic_factory_1100::get, 1100),
  };
  constexpr auto min_driver_version = "456.71";

#ifdef _WIN64
  constexpr auto dll_name = "nvEncodeAPI64.dll";
#else
  constexpr auto dll_name = "nvEncodeAPI.dll";
#endif

  std::tuple<shared_dll, uint32_t>
  load_dll() {
    auto dll = make_shared_dll(LoadLibraryEx(dll_name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!dll) {
      BOOST_LOG(debug) << "NvEnc: Couldn't load NvEnc library " << dll_name;
      return {};
    }

    auto get_max_version = (decltype(NvEncodeAPIGetMaxSupportedVersion) *) GetProcAddress(dll.get(), "NvEncodeAPIGetMaxSupportedVersion");
    if (!get_max_version) {
      BOOST_LOG(error) << "NvEnc: No NvEncodeAPIGetMaxSupportedVersion() in " << dll_name;
      return {};
    }

    uint32_t max_version = 0;
    if (get_max_version(&max_version) != 0) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPIGetMaxSupportedVersion() failed";
      return {};
    }
    max_version = (max_version >> 4) * 100 + (max_version & 0xf);

    return { dll, max_version };
  }

}  // namespace

namespace nvenc {

  std::shared_ptr<nvenc_dynamic_factory>
  nvenc_dynamic_factory::get() {
    auto [dll, max_version] = load_dll();
    if (!dll) return {};

    for (const auto &[factory_init, version] : factory_priorities) {
      if (max_version >= version) {
        return factory_init(dll);
      }
    }

    BOOST_LOG(error) << "NvEnc: minimum required driver version is " << min_driver_version;
    return {};
  }

}  // namespace nvenc

#ifdef SUNSHINE_TESTS
  #include "tests/tests_common.h"

  #include <comdef.h>
  #include <d3d11.h>

namespace {
  _COM_SMARTPTR_TYPEDEF(IDXGIFactory1, IID_IDXGIFactory1);
  _COM_SMARTPTR_TYPEDEF(IDXGIAdapter, IID_IDXGIAdapter);
  _COM_SMARTPTR_TYPEDEF(ID3D11Device, IID_ID3D11Device);
}  // namespace

struct NvencVersionTests: testing::TestWithParam<decltype(factory_priorities)::value_type> {
  static void
  SetUpTestSuite() {
    std::tie(suite.dll, suite.max_version) = load_dll();
    if (!suite.dll) {
      GTEST_SKIP() << "Can't load " << dll_name;
    }

    IDXGIFactory1Ptr dxgi_factory;
    ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)));

    IDXGIAdapterPtr dxgi_adapter;
    for (UINT i = 0; dxgi_factory->EnumAdapters(i, &dxgi_adapter) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC desc;
      ASSERT_HRESULT_SUCCEEDED(dxgi_adapter->GetDesc(&desc));
      if (desc.VendorId == 0x10de) break;
    }
    if (!dxgi_adapter) GTEST_SKIP();

    ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(dxgi_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
      nullptr, 0, D3D11_SDK_VERSION, &suite.device, nullptr, nullptr));
  }

  static void
  TearDownTestSuite() {
    suite = {};
  }

  inline static struct {
    nvenc::shared_dll dll;
    uint32_t max_version;
    ID3D11DevicePtr device;
  } suite = {};
};

TEST_P(NvencVersionTests, CreateAndEncode) {
  auto [factory_init, version] = GetParam();
  if (version > suite.max_version) {
    GTEST_SKIP() << "Need dll version " << version << ", have " << suite.max_version;
  }

  auto factory = factory_init(suite.dll);
  ASSERT_TRUE(factory);

  auto nvenc = factory->create_nvenc_d3d11_native(suite.device);
  ASSERT_TRUE(nvenc);

  video::config_t config = {
    .width = 1920,
    .height = 1080,
    .framerate = 60,
    .bitrate = 10 * 1000,
  };
  video::sunshine_colorspace_t colorspace = {
    .colorspace = video::colorspace_e::rec601,
    .bit_depth = 8,
  };
  ASSERT_TRUE(nvenc->create_encoder({}, config, colorspace, platf::pix_fmt_e::nv12));
  ASSERT_FALSE(nvenc->encode_frame(0, false).data.empty());
}

INSTANTIATE_TEST_SUITE_P(NvencFactoryTestsPrivate, NvencVersionTests, testing::ValuesIn(factory_priorities),
  [](const auto &info) { return std::to_string(std::get<1>(info.param)); });

#endif
