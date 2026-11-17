/**
 * @file tests/unit/platform/windows/test_audio.cpp
 * @brief Tests for Windows audio capture: sink selection, capture targets, process loopback, and endpoint-change handling.
 *
 * The microphone seams below drive the backend through the shared
 * parameter-bundle overload, so every microphone_* case also pins the params
 * shape end to end on this backend.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // test includes
  #include "src/platform/windows/audio_tests.h"

  // standard includes
  #include <array>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <functional>
  #include <memory>
  #include <string>
  #include <tuple>
  #include <type_traits>
  #include <vector>

  // platform includes
  #include <mmdeviceapi.h>
  #include <mmreg.h>
  #include <propsys.h>
  #if __has_include(<audioclientactivationparams.h>)
    #include <audioclientactivationparams.h>
    #define SUNSHINE_TEST_HAS_AUDIOCLIENT_ACTIVATION_PARAMS 1
  #endif

  // local includes
  #include "src/config.h"
  #include "src/platform/common.h"

namespace platf::audio::tests {
  std::vector<WAVEFORMATEXTENSIBLE> virtual_sink_waveformats(WORD channel_count);
  int initialize_audio_control(const std::function<std::remove_pointer_t<decltype(&CoCreateInstance)>> &create_instance);
  int set_keep_default_sink(const std::string &sink);
  bool keep_default_supported();
  std::string capture_assigned_sink(const std::string &sink);
  bool installs_default_callback(const std::string &sink, bool keep_default);
  bool resets_default_device(bool keep_default);
  bool installs_steam_drivers(bool keep_default, bool install_requested);
  void reset_with_keep_default();
  bool sink_device_available(const std::string &sink, IMMDeviceEnumerator *device_enum);
  std::optional<sink_t> reported_sinks(IMMDeviceEnumerator *device_enum);
  bool microphone_available(const std::string &assigned_sink, const std::string &configured_sink, IMMDeviceEnumerator *device_enum);
  bool microphone_follows_default_device(const std::string &configured_sink, bool keep_default);
  bool capture_follows_default_device(IMMDeviceEnumerator *device_enum, IMMDevice *capture_device);
  capture_e simulate_default_device_change(bool follows_default_device, bool install_callback, bool render_device_changed, int &callback_count);
  bool use_process_loopback(std::uint32_t process_id, platf::audio_capture_intent_e intent);
  bool process_loopback_fails_with_null_activator();
  bool process_loopback_wrapper_succeeds_with_fake_activation();
  bool process_loopback_wrapper_fails_with_null_activation();
  bool process_loopback_wrapper_fails_with_empty_activation();
  bool process_loopback_wrapper_fails_when_initialization_fails();
  std::uint32_t recorded_capture_process_id(std::uint32_t process_id, std::uint64_t creation_time);
  std::uint64_t recorded_capture_creation_time(std::uint32_t process_id, std::uint64_t creation_time);
  bool process_identity_matches(std::uint32_t recorded_process_id, std::uint64_t recorded_creation_time, std::uint32_t current_process_id, std::uint64_t current_creation_time);
  std::uint32_t effective_capture_process_id(std::uint32_t recorded_process_id, std::uint64_t recorded_creation_time, bool current_available, std::uint32_t current_process_id, std::uint64_t current_creation_time);
  std::uint32_t validated_capture_target_for_pid(std::uint32_t process_id, std::uint64_t creation_time);
  platf::audio_capture_intent_e intent_after_conditional_clear(platf::audio_capture_intent_e set_intent, std::uint32_t set_pid, platf::audio_capture_intent_e clear_intent, std::uint32_t clear_pid);
  std::uint32_t pid_after_conditional_clear(platf::audio_capture_intent_e set_intent, std::uint32_t set_pid, platf::audio_capture_intent_e clear_intent, std::uint32_t clear_pid);
  std::uint64_t identity_creation_time_after_conditional_clear(bool matching_clear);
  bool assigned_sink_snapshot_is_stable();
  bool concurrent_control_access_is_consistent();
  bool find_device_matches(const std::wstring &match_name, IMMDeviceEnumerator *device_enum);
  bool activated_process_target_is_current(std::uint32_t activated_process_id, std::uint32_t revalidated_process_id);
  #ifdef SUNSHINE_TEST_HAS_AUDIOCLIENT_ACTIVATION_PARAMS
  AUDIOCLIENT_ACTIVATION_PARAMS make_process_loopback_activation_params(std::uint32_t process_id);
  bool activation_blob_holds_heap_copy(std::uint32_t process_id);
  #endif
  #if defined(SUNSHINE_TEST_HAS_AUDIOCLIENT_ACTIVATION_PARAMS) && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
  int process_loopback_completion_after_timeout_is_discarded();
  int collect_reports_failure_combinations();
  int real_activation_paths_cover_success_failure_and_timeout();
  #endif
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
      if (FAILED(open_store_status)) {
        return open_store_status;
      }

      properties.AddRef();
      *property_store = &properties;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *device_id) override {
      if (FAILED(get_id_status)) {
        return get_id_status;
      }

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
    HRESULT get_id_status = S_OK;
    HRESULT open_store_status = S_OK;
    DWORD state = DEVICE_STATE_ACTIVE;
  };

  class fake_device_collection_t final: public IMMDeviceCollection {
  public:
    explicit fake_device_collection_t(fake_device_t &device):
        devices {&device} {
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
      if (FAILED(count_status)) {
        return count_status;
      }

      *device_count = static_cast<UINT>(devices.size());
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice **item) override {
      if (FAILED(item_status)) {
        return item_status;
      }

      if (index >= devices.size()) {
        return E_INVALIDARG;
      }

      devices[index]->AddRef();
      *item = devices[index];
      return S_OK;
    }

    std::vector<fake_device_t *> devices;
    HRESULT count_status = S_OK;
    HRESULT item_status = S_OK;
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
      if (FAILED(get_default_status)) {
        return get_default_status;
      }
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

    void add_device(std::unique_ptr<fake_device_t> extra_device) {
      collection.devices.push_back(extra_device.get());
      extra_devices.push_back(std::move(extra_device));
    }

    fake_device_t device;
    fake_device_collection_t collection;
    std::vector<std::unique_ptr<fake_device_t>> extra_devices;
    HRESULT enumeration_status = S_OK;
    HRESULT get_device_status = S_OK;
    HRESULT get_default_status = S_OK;
    int get_device_calls = 0;
    int get_default_device_calls = 0;
    std::wstring last_requested_id;
  };

  /**
   * @brief Read the test process creation marker from the owned process handle.
   *
   * Mirrors the production handoff: the marker is queried from an owned handle
   * rather than by PID, so it always names this process. Tests pass it as the
   * recorded baseline for the deterministic valid target, or perturb it to
   * model a PID reused by a different process.
   *
   * @return Combined creation marker, or 0 when the query fails.
   */
  std::uint64_t current_process_creation_time() {
    FILETIME creation_time {};
    FILETIME exit_time {};
    FILETIME kernel_time {};
    FILETIME user_time {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
      return 0;
    }
    return (static_cast<std::uint64_t>(creation_time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(creation_time.dwLowDateTime);
  }

  /**
   * @brief Build a loopback probe targeting the current process.
   *
   * Fills the shared baseline (endpoint-id sinks, host_and_client intent, live
   * PID and creation marker); tests override individual fields for their case.
   *
   * @param enumerator Device enumerator the probe resolves endpoints through.
   * @return Probe ready for the microphone() test seams.
   */
  platf::audio::tests::microphone_loopback_probe_t make_current_process_probe(fake_device_enumerator_t &enumerator) {
    return {
      .assigned_sink = "endpoint-id",
      .configured_sink = {},
      .intent = platf::audio_capture_intent_e::host_and_client,
      .process_id = static_cast<std::uint32_t>(::GetCurrentProcessId()),
      .creation_time = current_process_creation_time(),
      .device_enum = &enumerator,
      .keep_default = false,
    };
  }
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
class WindowsKeepDefaultTest: public testing::Test {
protected:
  void SetUp() override {
    previous = config::audio;
    config::audio.keep_default_sink = true;
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

TEST_F(WindowsKeepDefaultTest, InitializesWithoutPolicyInterface) {
  EXPECT_EQ(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 0);
  EXPECT_EQ(enumerator_request_count(), 1);
}

TEST_F(WindowsKeepDefaultTest, InitializationRequiresEndpointEnumerator) {
  fail_enumerator_initialization();
  EXPECT_NE(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 0);
  EXPECT_EQ(enumerator_request_count(), 1);
}

TEST_F(WindowsKeepDefaultTest, DisabledModeStillRequiresPolicyInterface) {
  config::audio.keep_default_sink = false;
  EXPECT_NE(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 1);
  EXPECT_EQ(enumerator_request_count(), 0);
}

TEST_F(WindowsKeepDefaultTest, SinkChangesDoNotAccessPolicyOrFormatInterfaces) {
  EXPECT_EXIT(std::exit(platf::audio::tests::set_keep_default_sink("virtual-Stereoendpoint-id")), testing::ExitedWithCode(0), "");
  EXPECT_EXIT(std::exit(platf::audio::tests::set_keep_default_sink("other-endpoint")), testing::ExitedWithCode(0), "");
}

TEST_F(WindowsKeepDefaultTest, AssignedSinkWinsOverConfiguredSink) {
  fake_device_enumerator_t enumerator {L"assigned-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("assigned-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.last_requested_id, L"assigned-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(WindowsKeepDefaultTest, ExplicitSinkPinStaysPinned) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("configured-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"configured-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(WindowsKeepDefaultTest, BlankConfigCaptureFollowsDefaultDespiteAssignedHostSink) {
  // The assigned host sink is only a startup snapshot: with keep-default and a
  // blank sink the endpoint decision follows the Windows default instead of
  // resolving that snapshot. microphone() builds its own enumerator on that
  // path, so call counts on an injected enumerator cannot pin this behavior;
  // the follow decision below does.
  EXPECT_TRUE(platf::audio::tests::microphone_follows_default_device({}, true));
  EXPECT_FALSE(platf::audio::tests::microphone_follows_default_device("configured-id", true));
  EXPECT_FALSE(platf::audio::tests::microphone_follows_default_device({}, false));

  // A render-default change must rearm a following capture.
  int callback_count = 0;
  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, true, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 1);
}

TEST_F(WindowsKeepDefaultTest, VirtualAssignedCaptureDoesNotFollowDefault) {
  config::audio.keep_default_sink = false;
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("virtual-Stereoendpoint-id", {}, &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
  EXPECT_TRUE(platf::audio::tests::installs_default_callback("virtual-Stereoendpoint-id", false));
}

TEST_F(WindowsKeepDefaultTest, MissingSinkDoesNotFallBackToDefault) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "missing-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(WindowsKeepDefaultTest, DisconnectedSinkDoesNotFallBackToDefault) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.device.state = DEVICE_STATE_UNPLUGGED;
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(WindowsKeepDefaultTest, DisabledModePreservesDefaultFallback) {
  config::audio.keep_default_sink = false;
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_TRUE(platf::audio::tests::capture_follows_default_device(&enumerator, nullptr));
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST_F(WindowsKeepDefaultTest, ReactivatedSinkCanBeResolvedAgain) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.device.state = DEVICE_STATE_UNPLUGGED;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("configured-id", &enumerator));
  enumerator.device.state = DEVICE_STATE_ACTIVE;
  EXPECT_TRUE(platf::audio::tests::sink_device_available("configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(WindowsKeepDefaultTest, KeepsConfiguredSinkWhenDefaultUnavailable) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.get_default_status = E_FAIL;

  const auto sinks = platf::audio::tests::reported_sinks(&enumerator);

  ASSERT_TRUE(sinks.has_value());
  EXPECT_EQ(sinks->host, "configured-id");
  EXPECT_FALSE(sinks->null.has_value());
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
}

TEST_F(WindowsKeepDefaultTest, EmptyConfigStillUsesDefaultEndpointAsHost) {
  config::audio.sink.clear();
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  const auto sinks = platf::audio::tests::reported_sinks(&enumerator);

  ASSERT_TRUE(sinks.has_value());
  EXPECT_EQ(sinks->host, "endpoint-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
}

TEST_F(WindowsKeepDefaultTest, EmptyConfigWithoutDefaultReportsNoSink) {
  config::audio.sink.clear();
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  enumerator.get_default_status = E_FAIL;

  EXPECT_FALSE(platf::audio::tests::reported_sinks(&enumerator).has_value());
}

TEST_F(WindowsKeepDefaultTest, DisabledModeWithoutDefaultReportsNoSink) {
  config::audio.keep_default_sink = false;
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.get_default_status = E_FAIL;

  EXPECT_FALSE(platf::audio::tests::reported_sinks(&enumerator).has_value());
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

TEST(WindowsAudioTest, EndpointLookupSkipsUnreadableEntries) {
  fake_device_enumerator_t enumerator {L"broken-id"};
  enumerator.device.get_id_status = E_FAIL;
  enumerator.add_device(std::make_unique<fake_device_t>(L"endpoint-id", L"Friendly Endpoint"));

  EXPECT_TRUE(platf::audio::tests::find_device_matches(L"endpoint-id", &enumerator));
  EXPECT_TRUE(platf::audio::tests::find_device_matches(L"Friendly Endpoint", &enumerator));
}

TEST(WindowsAudioTest, EndpointLookupFailsClosedWhenEntriesAreUnreadable) {
  fake_device_enumerator_t id_failure {L"endpoint-id"};
  id_failure.device.get_id_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::find_device_matches(L"endpoint-id", &id_failure));

  fake_device_enumerator_t store_failure {L"endpoint-id"};
  store_failure.device.open_store_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::find_device_matches(L"endpoint-id", &store_failure));

  fake_device_enumerator_t count_failure {L"endpoint-id"};
  count_failure.collection.count_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::find_device_matches(L"endpoint-id", &count_failure));

  fake_device_enumerator_t item_failure {L"endpoint-id"};
  item_failure.collection.item_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::find_device_matches(L"endpoint-id", &item_failure));
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
  EXPECT_TRUE(platf::audio::tests::installs_default_callback(virtual_sink, false));
  EXPECT_FALSE(platf::audio::tests::installs_default_callback(virtual_sink, true));
  EXPECT_FALSE(platf::audio::tests::installs_default_callback("endpoint-id", false));
}

TEST(WindowsAudioTest, ProcessLoopbackRequiresHostAndClientIntentAndPid) {
  using intent = platf::audio_capture_intent_e;
  // No PID means endpoint capture regardless of intent.
  EXPECT_FALSE(platf::audio::tests::use_process_loopback(0, intent::client_only));
  EXPECT_FALSE(platf::audio::tests::use_process_loopback(0, intent::host_and_client));
  EXPECT_FALSE(platf::audio::tests::use_process_loopback(0, intent::pinned_sink));
  // Quiet streaming with a PID must not bypass the silent virtual endpoint.
  EXPECT_FALSE(platf::audio::tests::use_process_loopback(1234, intent::client_only));
  // A pinned sink never selects process capture; the shared guard maps a
  // configured sink to this intent and records PID 0, so the backend needs no
  // separate sink check (see AudioCaptureIntentTest and
  // AudioCaptureProcessTreeTest in tests/unit/test_audio.cpp).
  EXPECT_FALSE(platf::audio::tests::use_process_loopback(1234, intent::pinned_sink));
  // The only true row: Play on this PC too with a PID.
  EXPECT_TRUE(platf::audio::tests::use_process_loopback(1234, intent::host_and_client));
}

TEST(WindowsAudioTest, CaptureProcessSetterStoresPid) {
  EXPECT_EQ(platf::audio::tests::recorded_capture_process_id(1234, 100), 1234u);
  EXPECT_EQ(platf::audio::tests::recorded_capture_process_id(0, 0), 0u);
}

TEST(WindowsAudioTest, CaptureProcessSetterStoresIdentityVerbatim) {
  // The baseline is stored without a PID query, so even a fabricated identity
  // for a nonexistent PID round-trips unchanged; only validation resolves it
  // to endpoint capture. A setter that re-queried by PID would record a zero
  // marker for 42424242 instead.
  EXPECT_EQ(platf::audio::tests::recorded_capture_creation_time(42424242, 0xBEEFCAFEu), 0xBEEFCAFEu);
  EXPECT_EQ(platf::audio::tests::recorded_capture_process_id(42424242, 0xBEEFCAFEu), 42424242u);
  EXPECT_EQ(platf::audio::tests::recorded_capture_creation_time(0, 0xBEEFCAFEu), 0u);
}

TEST(WindowsAudioTest, MicrophoneAttemptsLoopbackForCurrentProcessTarget) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  EXPECT_TRUE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, probe.process_id);
}

TEST(WindowsAudioTest, MicrophoneReturnsLoopbackForValidTarget) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  // The endpoint fallback creates no fake clients, so a returned microphone
  // proves the loopback branch — including post-activation revalidation —
  // succeeded rather than falling back.
  EXPECT_TRUE(platf::audio::tests::microphone_returns_loopback_for_valid_target(probe));
  EXPECT_EQ(probe.attempted_process_id, probe.process_id);
  EXPECT_TRUE(probe.microphone_returned);
}

TEST(WindowsAudioTest, MicrophoneFallsBackToEndpointWhenActivationFails) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  EXPECT_TRUE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, probe.process_id);
  // The failing activator falls back to endpoint capture: the assigned sink is
  // resolved through the injected enumerator, and endpoint initialization
  // fails without audio hardware.
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
  EXPECT_FALSE(probe.microphone_returned);
}

TEST(WindowsAudioTest, MicrophoneSkipsLoopbackWithoutCaptureTarget) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  platf::audio::tests::microphone_loopback_probe_t probe {
    .assigned_sink = "endpoint-id",
    .configured_sink = {},
    .intent = platf::audio_capture_intent_e::host_and_client,
    .process_id = 0,
    .device_enum = &enumerator,
    .keep_default = false,
  };
  EXPECT_FALSE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, 0u);
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_FALSE(probe.microphone_returned);
}

TEST(WindowsAudioTest, MicrophoneSkipsLoopbackForSilentIntent) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  probe.intent = platf::audio_capture_intent_e::client_only;
  EXPECT_FALSE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, 0u);
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_FALSE(probe.microphone_returned);
}

TEST(WindowsAudioTest, MicrophoneAttemptsLoopbackForBlankKeepDefaultTarget) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  probe.keep_default = true;
  EXPECT_TRUE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, probe.process_id);
  // The follow-default fallback builds its own enumerator, so the injected one stays untouched.
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, MicrophoneSkipsLoopbackForPinnedIntent) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  // Even with a live PID, a pinned sink stays pinned: the backend never
  // attempts loopback for pinned intent, independent of the guard recording
  // PID 0 for such sessions upstream.
  probe.configured_sink = "endpoint-id";
  probe.intent = platf::audio_capture_intent_e::pinned_sink;
  probe.keep_default = true;
  EXPECT_FALSE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, 0u);
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
  EXPECT_FALSE(probe.microphone_returned);
}

TEST(WindowsAudioTest, MicrophoneLoopbackDecisionIgnoresConfiguredSink) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  // The backend decides on intent and revalidated PID only: a configured sink
  // no longer blocks the attempt here because the shared guard already maps a
  // configured sink to pinned intent (and PID 0) before the backend runs.
  probe.configured_sink = "other-sink";
  EXPECT_TRUE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, probe.process_id);
  // The failing activator falls back to endpoint capture against the assigned sink.
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
  EXPECT_FALSE(probe.microphone_returned);
}

TEST(WindowsAudioTest, MicrophoneRejectsStaleIdentityAfterPidReuse) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  auto probe = make_current_process_probe(enumerator);
  ASSERT_NE(probe.process_id, 0u);
  ASSERT_NE(probe.creation_time, 0u);

  // The recorded baseline names the original occupant; the live PID now names
  // a different process (same PID, different marker). Validation must reject
  // the stale baseline so no loopback activation targets the replacement, and
  // endpoint fallback must resolve through the assigned sink instead.
  probe.creation_time += 1;
  EXPECT_FALSE(platf::audio::tests::microphone_attempts_loopback_for_target(probe));
  EXPECT_EQ(probe.attempted_process_id, 0u);
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
  EXPECT_FALSE(probe.microphone_returned);
}

TEST(WindowsAudioTest, ConditionalClearResetsMatchingTarget) {
  using intent = platf::audio_capture_intent_e;
  EXPECT_EQ(platf::audio::tests::pid_after_conditional_clear(intent::host_and_client, 1234, intent::host_and_client, 1234), 0u);
  EXPECT_EQ(platf::audio::tests::intent_after_conditional_clear(intent::host_and_client, 1234, intent::host_and_client, 1234), intent::client_only);
}

TEST(WindowsAudioTest, ConditionalClearPreservesMismatchedTarget) {
  using intent = platf::audio_capture_intent_e;
  // A survivor that recorded a different PID keeps its target.
  EXPECT_EQ(platf::audio::tests::pid_after_conditional_clear(intent::host_and_client, 1234, intent::host_and_client, 5678), 1234u);
  EXPECT_EQ(platf::audio::tests::intent_after_conditional_clear(intent::host_and_client, 1234, intent::host_and_client, 5678), intent::host_and_client);
  // A survivor that recorded a different intent keeps its target.
  EXPECT_EQ(platf::audio::tests::pid_after_conditional_clear(intent::host_and_client, 1234, intent::client_only, 1234), 1234u);
  EXPECT_EQ(platf::audio::tests::intent_after_conditional_clear(intent::host_and_client, 1234, intent::client_only, 1234), intent::host_and_client);
  // An endpoint clear cannot wipe a process target and vice versa.
  EXPECT_EQ(platf::audio::tests::pid_after_conditional_clear(intent::host_and_client, 1234, intent::host_and_client, 0), 1234u);
  EXPECT_EQ(platf::audio::tests::pid_after_conditional_clear(intent::client_only, 0, intent::host_and_client, 1234), 0u);
  EXPECT_EQ(platf::audio::tests::intent_after_conditional_clear(intent::client_only, 0, intent::host_and_client, 1234), intent::client_only);
}

TEST(WindowsAudioTest, ConditionalClearResetsRecordedIdentity) {
  // A matching clear runs on_capture_target_cleared(), resetting the paired
  // creation-time identity; a mismatched clear must preserve it.
  EXPECT_EQ(platf::audio::tests::identity_creation_time_after_conditional_clear(true), 0u);
  EXPECT_NE(platf::audio::tests::identity_creation_time_after_conditional_clear(false), 0u);
}

TEST(WindowsAudioTest, AssignedSinkSnapshotStaysStableAcrossRewrite) {
  EXPECT_TRUE(platf::audio::tests::assigned_sink_snapshot_is_stable());
}

TEST(WindowsAudioTest, ConcurrentControlAccessStaysConsistent) {
  EXPECT_TRUE(platf::audio::tests::concurrent_control_access_is_consistent());
}

TEST(WindowsAudioTest, ProcessIdentityAcceptsSameIdentity) {
  EXPECT_TRUE(platf::audio::tests::process_identity_matches(1234, 100, 1234, 100));
  EXPECT_EQ(platf::audio::tests::effective_capture_process_id(1234, 100, true, 1234, 100), 1234u);
}

TEST(WindowsAudioTest, ProcessIdentityRejectsDifferentCreationTime) {
  // Same PID with a different creation time is a reused PID, not the recorded process.
  EXPECT_FALSE(platf::audio::tests::process_identity_matches(1234, 100, 1234, 200));
  EXPECT_EQ(platf::audio::tests::effective_capture_process_id(1234, 100, true, 1234, 200), 0u);
}

TEST(WindowsAudioTest, ProcessIdentityRejectsZeroPid) {
  EXPECT_FALSE(platf::audio::tests::process_identity_matches(0, 0, 0, 0));
  EXPECT_FALSE(platf::audio::tests::process_identity_matches(0, 100, 0, 100));
  EXPECT_EQ(platf::audio::tests::effective_capture_process_id(0, 0, true, 0, 0), 0u);
  EXPECT_EQ(platf::audio::tests::validated_capture_target_for_pid(0, 0), 0u);
}

TEST(WindowsAudioTest, ProcessIdentityRejectsDifferentPidAndMissingCreationTime) {
  EXPECT_FALSE(platf::audio::tests::process_identity_matches(1234, 100, 5678, 100));
  EXPECT_FALSE(platf::audio::tests::process_identity_matches(1234, 0, 1234, 0));
  EXPECT_EQ(platf::audio::tests::effective_capture_process_id(1234, 0, true, 1234, 0), 0u);
}

TEST(WindowsAudioTest, ProcessIdentityRejectsExitedOrUnavailableProcess) {
  // No current identity models an exited process or a failed OpenProcess/GetProcessTimes query.
  EXPECT_EQ(platf::audio::tests::effective_capture_process_id(1234, 100, false, 1234, 100), 0u);
  EXPECT_EQ(platf::audio::tests::effective_capture_process_id(1234, 100, false, 0, 0), 0u);
}

TEST(WindowsAudioTest, ValidatedCaptureTargetAcceptsCurrentProcessAndRejectsUnavailablePid) {
  const auto current_process_id = static_cast<std::uint32_t>(::GetCurrentProcessId());
  ASSERT_NE(current_process_id, 0u);
  const auto creation_time = current_process_creation_time();
  ASSERT_NE(creation_time, 0u);
  EXPECT_EQ(platf::audio::tests::validated_capture_target_for_pid(current_process_id, creation_time), current_process_id);
  EXPECT_EQ(platf::audio::tests::validated_capture_target_for_pid(0xFFFFFFFFu, 100), 0u);
  // A live PID with a stale marker models reuse: the original process exited
  // and its PID now names a different process, so validation must reject it.
  EXPECT_EQ(platf::audio::tests::validated_capture_target_for_pid(current_process_id, creation_time + 1), 0u);
}

TEST(WindowsAudioTest, ActivatedProcessTargetSurvivesOnlyMatchingRevalidation) {
  EXPECT_TRUE(platf::audio::tests::activated_process_target_is_current(1234, 1234));
  EXPECT_FALSE(platf::audio::tests::activated_process_target_is_current(1234, 0));
  EXPECT_FALSE(platf::audio::tests::activated_process_target_is_current(1234, 5678));
  EXPECT_FALSE(platf::audio::tests::activated_process_target_is_current(0, 0));
  EXPECT_FALSE(platf::audio::tests::activated_process_target_is_current(0, 1234));
}

TEST(WindowsAudioTest, ProcessLoopbackFailsWithNullActivator) {
  EXPECT_TRUE(platf::audio::tests::process_loopback_fails_with_null_activator());
}

TEST(WindowsAudioTest, ProcessLoopbackWrapperSucceedsWithFakeActivation) {
  EXPECT_TRUE(platf::audio::tests::process_loopback_wrapper_succeeds_with_fake_activation());
}

TEST(WindowsAudioTest, ProcessLoopbackWrapperFailsWithNullActivation) {
  EXPECT_TRUE(platf::audio::tests::process_loopback_wrapper_fails_with_null_activation());
}

TEST(WindowsAudioTest, ProcessLoopbackWrapperFailsWithEmptyActivation) {
  EXPECT_TRUE(platf::audio::tests::process_loopback_wrapper_fails_with_empty_activation());
}

TEST(WindowsAudioTest, ProcessLoopbackWrapperFailsWhenInitializationFails) {
  EXPECT_TRUE(platf::audio::tests::process_loopback_wrapper_fails_when_initialization_fails());
}

  #ifdef SUNSHINE_TEST_HAS_AUDIOCLIENT_ACTIVATION_PARAMS
TEST(WindowsAudioTest, ProcessLoopbackActivationParamsIncludeProcessTree) {
  const auto params = platf::audio::tests::make_process_loopback_activation_params(1234);
  EXPECT_EQ(params.ActivationType, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK);
  EXPECT_EQ(params.ProcessLoopbackParams.TargetProcessId, 1234u);
  EXPECT_EQ(params.ProcessLoopbackParams.ProcessLoopbackMode, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE);
}

TEST(WindowsAudioTest, ProcessLoopbackActivationBlobIsHeapOwnedAndClearable) {
  EXPECT_TRUE(platf::audio::tests::activation_blob_holds_heap_copy(1234));
}
  #else
TEST(WindowsAudioTest, ProcessLoopbackActivationParamsUnavailable) {
  // Production loopback is compiled out under the same SDK detection, so a
  // visible skip here is consistent coverage reporting, not a silent gap.
  GTEST_SKIP() << "Requires Windows SDK with audioclientactivationparams.h";
}
  #endif

  #if defined(SUNSHINE_TEST_HAS_AUDIOCLIENT_ACTIVATION_PARAMS) && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
TEST(WindowsAudioTest, ProcessLoopbackCompletionAfterTimeoutIsDiscarded) {
  EXPECT_EQ(platf::audio::tests::process_loopback_completion_after_timeout_is_discarded(), 0);
}

TEST(WindowsAudioTest, CollectReportsFailureCombinations) {
  EXPECT_EQ(platf::audio::tests::collect_reports_failure_combinations(), 0);
}

TEST(WindowsAudioTest, RealActivationPathsCoverSuccessFailureAndTimeout) {
  EXPECT_EQ(platf::audio::tests::real_activation_paths_cover_success_failure_and_timeout(), 0);
}
  #else
TEST(WindowsAudioTest, ProcessLoopbackActivationUnavailable) {
  // Production loopback is compiled out under the same SDK detection, so a
  // visible skip here is consistent coverage reporting, not a silent gap.
  GTEST_SKIP() << "Requires Windows SDK with process-loopback activation support";
}
  #endif

TEST(WindowsAudioTest, HandsOffModeSkipsDefaultResetAndDriverInstall) {
  EXPECT_TRUE(platf::audio::tests::resets_default_device(false));
  EXPECT_FALSE(platf::audio::tests::resets_default_device(true));
  EXPECT_TRUE(platf::audio::tests::installs_steam_drivers(false, true));
  EXPECT_FALSE(platf::audio::tests::installs_steam_drivers(true, true));
  EXPECT_FALSE(platf::audio::tests::installs_steam_drivers(false, false));
  platf::audio::tests::reset_with_keep_default();
}
#endif
