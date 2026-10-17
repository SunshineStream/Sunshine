/**
 * @file tests/unit/platform/windows/test_audio.cpp
 * @brief Tests for Windows audio sink selection and endpoint-change handling.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <array>
  #include <cstdlib>
  #include <cstring>
  #include <functional>
  #include <tuple>
  #include <type_traits>

  // platform includes
  #include <mmdeviceapi.h>
  #include <mmreg.h>
  #include <propsys.h>

  // local includes
  #include "src/config.h"
  #include "src/platform/common.h"

namespace platf::audio::tests {
  std::vector<WAVEFORMATEXTENSIBLE> virtual_sink_waveformats(WORD channel_count);
  int initialize_audio_control(const std::function<std::remove_pointer_t<decltype(&CoCreateInstance)>> &create_instance);
  std::optional<sink_t> configured_sink_info();
  int set_external_sink(const std::string &sink);
  bool keep_default_supported();
  std::string capture_assigned_sink(const std::string &sink);
  bool installs_default_callback(const std::string &sink, bool keep_default, bool external_audio);
  bool resets_default_device(bool keep_default, bool external_audio);
  bool installs_steam_drivers(bool keep_default, bool external_audio, bool install_requested);
  void reset_with_keep_default();
  bool sink_device_available(const std::string &sink, IMMDeviceEnumerator *device_enum);
  bool microphone_available(const std::string &assigned_sink, const std::string &configured_sink, IMMDeviceEnumerator *device_enum);
  bool capture_follows_default_device(IMMDeviceEnumerator *device_enum, IMMDevice *capture_device);
  capture_e simulate_default_device_change(bool follows_default_device, bool install_callback, bool render_device_changed, int &callback_count);
}  // namespace platf::audio::tests

namespace {
  class fake_property_store_t final: public IPropertyStore {
  public:
    explicit fake_property_store_t(std::wstring friendly_name):
        friendly_name {std::move(friendly_name)} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD *property_count) override {
      *property_count = friendly_name.empty() ? 0 : 1;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY *) override {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY, PROPVARIANT *value) override {
      if (friendly_name.empty()) {
        return E_NOTIMPL;
      }

      const auto byte_count = (friendly_name.size() + 1) * sizeof(wchar_t);
      value->pwszVal = static_cast<wchar_t *>(CoTaskMemAlloc(byte_count));
      if (!value->pwszVal) {
        return E_OUTOFMEMORY;
      }

      std::memcpy(value->pwszVal, friendly_name.c_str(), byte_count);
      value->vt = VT_LPWSTR;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit() override {
      return E_NOTIMPL;
    }

    std::wstring friendly_name;
  };

  class fake_device_t final: public IMMDevice {
  public:
    fake_device_t(std::wstring id, std::wstring friendly_name):
        id {std::move(id)},
        properties {std::move(friendly_name)} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT *, void **) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore **property_store) override {
      properties.AddRef();
      *property_store = &properties;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *device_id) override {
      const auto byte_count = (id.size() + 1) * sizeof(wchar_t);
      *device_id = static_cast<wchar_t *>(CoTaskMemAlloc(byte_count));
      if (!*device_id) {
        return E_OUTOFMEMORY;
      }

      std::memcpy(*device_id, id.c_str(), byte_count);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState(DWORD *device_state) override {
      if (FAILED(state_status)) {
        return state_status;
      }

      *device_state = state;
      return S_OK;
    }

    std::wstring id;
    fake_property_store_t properties;
    HRESULT state_status = S_OK;
    DWORD state = DEVICE_STATE_ACTIVE;
  };

  class fake_device_collection_t final: public IMMDeviceCollection {
  public:
    explicit fake_device_collection_t(fake_device_t &device):
        device {device} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE GetCount(UINT *device_count) override {
      *device_count = 1;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice **item) override {
      if (index != 0) {
        return E_INVALIDARG;
      }

      device.AddRef();
      *item = &device;
      return S_OK;
    }

    fake_device_t &device;
  };

  class fake_device_enumerator_t final: public IMMDeviceEnumerator {
  public:
    explicit fake_device_enumerator_t(std::wstring id, std::wstring friendly_name = {}):
        device {std::move(id), std::move(friendly_name)},
        collection {device} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection **devices) override {
      if (FAILED(enumeration_status)) {
        return enumeration_status;
      }

      collection.AddRef();
      *devices = &collection;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice **resolved_device) override {
      ++get_default_device_calls;
      device.AddRef();
      *resolved_device = &device;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR device_id, IMMDevice **resolved_device) override {
      ++get_device_calls;
      last_requested_id = device_id;
      if (FAILED(get_device_status)) {
        return get_device_status;
      }
      if (last_requested_id != device.id) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
      }

      device.AddRef();
      *resolved_device = &device;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *) override {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *) override {
      return S_OK;
    }

    fake_device_t device;
    fake_device_collection_t collection;
    HRESULT enumeration_status = S_OK;
    HRESULT get_device_status = S_OK;
    int get_device_calls = 0;
    int get_default_device_calls = 0;
    std::wstring last_requested_id;
  };
}  // namespace

TEST(WindowsAudioTest, PreservesVirtualSpeakerFormatPreferenceAndChannelMasks) {
  constexpr DWORD stereo_mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  constexpr DWORD surround_back_mask = stereo_mask | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
  constexpr DWORD surround_side_mask = stereo_mask | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
  constexpr DWORD surround71_mask = surround_back_mask | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
  const std::array stereo_samples {
    std::tuple {32, 24, WAVE_FORMAT_PCM},
    std::tuple {24, 24, WAVE_FORMAT_PCM},
    std::tuple {16, 16, WAVE_FORMAT_PCM},
    std::tuple {32, 32, WAVE_FORMAT_IEEE_FLOAT},
    std::tuple {32, 32, WAVE_FORMAT_PCM},
  };
  const std::array surround_samples {
    stereo_samples[3],
    stereo_samples[4],
    stereo_samples[0],
    stereo_samples[1],
    stereo_samples[2],
  };

  for (WORD channels : {2, 6, 8}) {
    const auto formats = platf::audio::tests::virtual_sink_waveformats(channels);
    const auto &samples = channels == 2 ? stereo_samples : surround_samples;
    ASSERT_EQ(formats.size(), channels == 6 ? 10 : 5);
    for (std::size_t i = 0; i < formats.size(); ++i) {
      const auto &[storage_bits, valid_bits, subformat] = samples[channels == 6 ? i / 2 : i];
      const auto &format = formats[i];
      EXPECT_EQ(format.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
      EXPECT_EQ(format.Format.nChannels, channels);
      EXPECT_EQ(format.Format.nSamplesPerSec, 48000);
      EXPECT_EQ(format.Format.wBitsPerSample, storage_bits);
      EXPECT_EQ(format.Samples.wValidBitsPerSample, valid_bits);
      EXPECT_EQ(format.SubFormat.Data1, subformat);
      EXPECT_EQ(format.Format.nBlockAlign, channels * storage_bits / 8);
      EXPECT_EQ(format.Format.nAvgBytesPerSec, 48000 * format.Format.nBlockAlign);
      EXPECT_EQ(format.dwChannelMask, channels == 2 ? stereo_mask : channels == 8 ? surround71_mask :
                                                                  i % 2 == 0      ? surround_back_mask :
                                                                                    surround_side_mask);
    }
  }
  EXPECT_TRUE(platf::audio::tests::virtual_sink_waveformats(4).empty());
}

TEST(WindowsAudioTest, AssignedSinkTakesPriorityOverConfiguredSink) {
  fake_device_enumerator_t enumerator {L"assigned-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available("assigned-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"assigned-id");
}

TEST(WindowsAudioTest, ConfiguredSinkIsUsedWhenNoSinkWasAssigned) {
  fake_device_enumerator_t enumerator {L"configured-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"configured-id");
}

TEST(WindowsAudioTest, DefaultDeviceIsUsedWhenNoSinkWasRequested) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  platf::audio::tests::microphone_available({}, {}, &enumerator);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

/**
 * @brief Preserve global audio configuration while exercising capture-only mode.
 */
class ExternalAudioTest: public testing::Test {
protected:
  void SetUp() override {
    previous = config::audio;
    config::audio.external_audio = true;
    config::audio.sink = "configured-id";
    config::audio.virtual_sink = "ignored-virtual-sink";
    policy_requests = 0;
    enumerator_requests = 0;
    enumerator_status = S_OK;
  }

  void TearDown() override {
    config::audio = previous;
  }

  /**
   * @brief Get the number of policy activation requests.
   * @return Requests observed by the test factory.
   */
  static int policy_request_count() {
    return policy_requests;
  }

  /**
   * @brief Get the number of endpoint enumeration activation requests.
   * @return Requests observed by the test factory.
   */
  static int enumerator_request_count() {
    return enumerator_requests;
  }

  /**
   * @brief Simulate failure to initialize endpoint enumeration.
   */
  static void fail_enumerator_initialization() {
    enumerator_status = E_FAIL;
  }

  /**
   * @brief Supply endpoint enumeration while simulating an unavailable policy interface.
   * @param class_id Requested COM class.
   * @param outer Unused aggregation pointer.
   * @param context Unused activation context.
   * @param interface_id Unused interface identifier.
   * @param object Receives the enumerator on success.
   * @return Simulated COM activation status.
   */
  static HRESULT WINAPI create_capture_interface(REFCLSID class_id, [[maybe_unused]] LPUNKNOWN outer, [[maybe_unused]] DWORD context, [[maybe_unused]] REFIID interface_id, LPVOID *object) {
    *object = nullptr;
    if (class_id != CLSID_MMDeviceEnumerator) {
      ++policy_requests;
      return REGDB_E_CLASSNOTREG;
    }
    ++enumerator_requests;
    if (FAILED(enumerator_status)) {
      return enumerator_status;
    }
    static fake_device_enumerator_t enumerator {L"configured-id"};
    enumerator.AddRef();
    *object = static_cast<IMMDeviceEnumerator *>(&enumerator);
    return S_OK;
  }

private:
  config::audio_t previous;  ///< Configuration restored after each test.
  inline static int policy_requests = 0;  ///< Requests for the unavailable policy interface.
  inline static int enumerator_requests = 0;  ///< Requests for endpoint enumeration.
  inline static HRESULT enumerator_status = S_OK;  ///< Simulated enumeration initialization result.
};

TEST_F(ExternalAudioTest, InitializesWithoutPolicyInterface) {
  EXPECT_EQ(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 0);
  EXPECT_EQ(enumerator_request_count(), 1);
}

TEST_F(ExternalAudioTest, InitializationRequiresEndpointEnumerator) {
  fail_enumerator_initialization();
  EXPECT_NE(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 0);
  EXPECT_EQ(enumerator_request_count(), 1);
}

TEST_F(ExternalAudioTest, DisabledModeStillRequiresPolicyInterface) {
  config::audio.external_audio = false;
  EXPECT_NE(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 1);
  EXPECT_EQ(enumerator_request_count(), 0);
}

TEST_F(ExternalAudioTest, RequiresExplicitSink) {
  config::audio.sink.clear();
  EXPECT_EXIT(std::exit(platf::audio::tests::configured_sink_info() ? 1 : 0), testing::ExitedWithCode(0), "");
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, {}, &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST_F(ExternalAudioTest, SkipsDefaultAndVirtualDeviceDiscovery) {
  EXPECT_EXIT(
    {
      const auto sinks = platf::audio::tests::configured_sink_info();
      std::exit(sinks && sinks->host == "configured-id" && !sinks->null ? 0 : 1);
    },
    testing::ExitedWithCode(0),
    ""
  );
}

TEST_F(ExternalAudioTest, SinkChangesDoNotAccessPolicyOrFormatInterfaces) {
  EXPECT_EXIT(std::exit(platf::audio::tests::set_external_sink("virtual-Stereoendpoint-id")), testing::ExitedWithCode(0), "");
  EXPECT_EXIT(std::exit(platf::audio::tests::set_external_sink("other-endpoint")), testing::ExitedWithCode(0), "");
}

TEST_F(ExternalAudioTest, ConfiguredSinkWinsOverAssignedSink) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("virtual-Stereoother-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.last_requested_id, L"configured-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, MissingSinkDoesNotFallBackToDefault) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "missing-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, DisconnectedSinkDoesNotFallBackToDefault) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.device.state = DEVICE_STATE_UNPLUGGED;
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, DisabledModePreservesAssignedSinkPriority) {
  config::audio.external_audio = false;
  fake_device_enumerator_t enumerator {L"assigned-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("assigned-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.last_requested_id, L"assigned-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, DisabledModePreservesDefaultFallback) {
  config::audio.external_audio = false;
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_TRUE(platf::audio::tests::capture_follows_default_device(&enumerator, nullptr));
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST_F(ExternalAudioTest, ReactivatedSinkCanBeResolvedAgain) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.device.state = DEVICE_STATE_UNPLUGGED;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("configured-id", &enumerator));
  enumerator.device.state = DEVICE_STATE_ACTIVE;
  EXPECT_TRUE(platf::audio::tests::sink_device_available("configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST(WindowsAudioTest, DefaultCaptureSelectsDefaultEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_TRUE(platf::audio::tests::capture_follows_default_device(&enumerator, nullptr));
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
}

TEST(WindowsAudioTest, PinnedCaptureKeepsExplicitEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_FALSE(platf::audio::tests::capture_follows_default_device(&enumerator, &enumerator.device));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST(WindowsAudioTest, MicrophoneRejectsUnresolvedSink) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available("unknown", {}, &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, ResolvesVirtualSinkDescriptorToActiveEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_TRUE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
}

TEST(WindowsAudioTest, ResolvesDeviceIdentifiersAndFriendlyNames) {
  fake_device_enumerator_t id_enumerator {L"endpoint-id"};
  EXPECT_TRUE(platf::audio::tests::sink_device_available("endpoint-id", &id_enumerator));
  EXPECT_EQ(id_enumerator.get_device_calls, 1);
  EXPECT_EQ(id_enumerator.last_requested_id, L"endpoint-id");

  fake_device_enumerator_t name_enumerator {L"endpoint-id", L"Friendly Endpoint"};
  EXPECT_TRUE(platf::audio::tests::sink_device_available("Friendly Endpoint", &name_enumerator));
  EXPECT_EQ(name_enumerator.get_device_calls, 1);
  EXPECT_EQ(name_enumerator.last_requested_id, L"endpoint-id");
}

TEST(WindowsAudioTest, RejectsUnknownOrUnenumerableSinks) {
  fake_device_enumerator_t unknown_enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::sink_device_available("unknown", &unknown_enumerator));
  EXPECT_EQ(unknown_enumerator.get_device_calls, 0);

  fake_device_enumerator_t failed_enumerator {L"endpoint-id"};
  failed_enumerator.enumeration_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("endpoint-id", &failed_enumerator));
  EXPECT_EQ(failed_enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, RejectsUnavailableResolvedEndpoints) {
  fake_device_enumerator_t missing_enumerator {L"endpoint-id"};
  missing_enumerator.get_device_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &missing_enumerator));

  fake_device_enumerator_t state_failure_enumerator {L"endpoint-id"};
  state_failure_enumerator.device.state_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &state_failure_enumerator));

  fake_device_enumerator_t inactive_enumerator {L"endpoint-id"};
  inactive_enumerator.device.state = DEVICE_STATE_DISABLED;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &inactive_enumerator));
}

TEST(WindowsAudioTest, DefaultFollowingCaptureReinitializesAfterRenderEndpointChange) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, true, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 1);
}

TEST(WindowsAudioTest, PinnedCaptureContinuesAfterRenderEndpointChange) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(false, true, true, callback_count),
    platf::capture_e::timeout
  );
  EXPECT_EQ(callback_count, 1);
}

TEST(WindowsAudioTest, CaptureEndpointChangeDoesNotTriggerRenderCallback) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(false, true, false, callback_count),
    platf::capture_e::timeout
  );
  EXPECT_EQ(callback_count, 0);
}

TEST(WindowsAudioTest, DefaultChangeWithoutCallbackStillReinitializes) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, false, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 0);
}

TEST(WindowsAudioTest, KeepDefaultModeCapturesWithoutSwapping) {
  EXPECT_TRUE(platf::audio::tests::keep_default_supported());
  EXPECT_EQ(platf::audio::tests::capture_assigned_sink("endpoint-id"), "endpoint-id");
}

TEST(WindowsAudioTest, KeepDefaultModeInstallsNoReassertionCallback) {
  constexpr const char *virtual_sink = "virtual-Stereoendpoint-id";
  EXPECT_TRUE(platf::audio::tests::installs_default_callback(virtual_sink, false, false));
  EXPECT_FALSE(platf::audio::tests::installs_default_callback(virtual_sink, true, false));
  EXPECT_FALSE(platf::audio::tests::installs_default_callback(virtual_sink, false, true));
  EXPECT_FALSE(platf::audio::tests::installs_default_callback(virtual_sink, true, true));
  EXPECT_FALSE(platf::audio::tests::installs_default_callback("endpoint-id", false, false));
}

TEST(WindowsAudioTest, HandsOffModesSkipDefaultResetAndDriverInstall) {
  EXPECT_TRUE(platf::audio::tests::resets_default_device(false, false));
  EXPECT_FALSE(platf::audio::tests::resets_default_device(true, false));
  EXPECT_FALSE(platf::audio::tests::resets_default_device(false, true));
  EXPECT_TRUE(platf::audio::tests::installs_steam_drivers(false, false, true));
  EXPECT_FALSE(platf::audio::tests::installs_steam_drivers(true, false, true));
  EXPECT_FALSE(platf::audio::tests::installs_steam_drivers(false, true, true));
  EXPECT_FALSE(platf::audio::tests::installs_steam_drivers(false, false, false));
  platf::audio::tests::reset_with_keep_default();
}
#endif
