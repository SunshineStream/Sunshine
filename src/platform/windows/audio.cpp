/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// standard includes
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// platform includes
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <objbase.h>
#include <roapi.h>
#include <synchapi.h>
#if __has_include(<audioclientactivationparams.h>)
  #include <audioclientactivationparams.h>
  #define SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS 1
#endif

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "utf_utils.h"
#ifdef SUNSHINE_TESTS
  #include "audio_tests.h"
#endif

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

#ifdef DOXYGEN
/**
 * @brief Property key for a device description.
 */
extern const PROPERTYKEY PKEY_Device_DeviceDesc;
/**
 * @brief Property key for a device friendly name.
 */
extern const PROPERTYKEY PKEY_Device_FriendlyName;
/**
 * @brief Property key for a device interface friendly name.
 */
extern const PROPERTYKEY PKEY_DeviceInterface_FriendlyName;
#else
DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);
#endif

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#endif

namespace {

  constexpr auto SAMPLE_RATE = 48000;
#ifdef STEAM_DRIVER_SUBDIR
  constexpr auto STEAM_AUDIO_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";
#endif

  constexpr auto waveformat_mask_stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  constexpr auto waveformat_mask_surround51_with_backspeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

  constexpr auto waveformat_mask_surround51_with_sidespeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  constexpr auto waveformat_mask_surround71 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                              SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                                              SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  enum class sample_format_e {
    f32,
    s32,
    s24in32,
    s24,
    s16,
    _size,
  };

  constexpr WAVEFORMATEXTENSIBLE create_waveformat(sample_format_e sample_format, WORD channel_count, DWORD channel_mask) {
    WAVEFORMATEXTENSIBLE waveformat = {};

    switch (sample_format) {
      default:
      case sample_format_e::f32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s24in32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s24:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 24;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s16:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 16;
        waveformat.Samples.wValidBitsPerSample = 16;
        break;
    }

    static_assert(std::to_underlying(sample_format_e::_size) == 5, "Unrecognized sample_format_e");

    waveformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveformat.Format.nChannels = channel_count;
    waveformat.Format.nSamplesPerSec = SAMPLE_RATE;

    waveformat.Format.nBlockAlign = waveformat.Format.nChannels * waveformat.Format.wBitsPerSample / 8;
    waveformat.Format.nAvgBytesPerSec = waveformat.Format.nSamplesPerSec * waveformat.Format.nBlockAlign;
    waveformat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    waveformat.dwChannelMask = channel_mask;

    return waveformat;
  }

  using virtual_sink_waveformats_t = std::vector<WAVEFORMATEXTENSIBLE>;

  /**
   * @brief Build supported waveformats in preference order for an N-channel virtual audio device.
   * @tparam channel_count Number of virtual audio channels
   * @returns std::vector<WAVEFORMATEXTENSIBLE>
   * @note The list of virtual formats returned are sorted in preference order and the first valid
   *       format will be used. All bits-per-sample options are listed because we try to match
   *       this to the default audio device. See also: set_format() below.
   */
  template<WORD channel_count>
  virtual_sink_waveformats_t create_virtual_sink_waveformats() {
    using enum sample_format_e;
    if constexpr (channel_count == 2) {
      auto channel_mask = waveformat_mask_stereo;
      // The 32-bit formats are a lower priority for stereo because using one will disable Dolby/DTS
      // spatial audio mode if the user enabled it on the Steam speaker.
      return {
        create_waveformat(s24in32, channel_count, channel_mask),
        create_waveformat(s24, channel_count, channel_mask),
        create_waveformat(s16, channel_count, channel_mask),
        create_waveformat(f32, channel_count, channel_mask),
        create_waveformat(s32, channel_count, channel_mask),
      };
    } else if (channel_count == 6) {
      auto channel_mask1 = waveformat_mask_surround51_with_backspeakers;
      auto channel_mask2 = waveformat_mask_surround51_with_sidespeakers;
      return {
        create_waveformat(f32, channel_count, channel_mask1),
        create_waveformat(f32, channel_count, channel_mask2),
        create_waveformat(s32, channel_count, channel_mask1),
        create_waveformat(s32, channel_count, channel_mask2),
        create_waveformat(s24in32, channel_count, channel_mask1),
        create_waveformat(s24in32, channel_count, channel_mask2),
        create_waveformat(s24, channel_count, channel_mask1),
        create_waveformat(s24, channel_count, channel_mask2),
        create_waveformat(s16, channel_count, channel_mask1),
        create_waveformat(s16, channel_count, channel_mask2),
      };
    } else if (channel_count == 8) {
      auto channel_mask = waveformat_mask_surround71;
      return {
        create_waveformat(f32, channel_count, channel_mask),
        create_waveformat(s32, channel_count, channel_mask),
        create_waveformat(s24in32, channel_count, channel_mask),
        create_waveformat(s24, channel_count, channel_mask),
        create_waveformat(s16, channel_count, channel_mask),
      };
    }
  }

  std::string waveformat_to_pretty_string(const WAVEFORMATEXTENSIBLE &waveformat) {
    std::string result = waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? "F" :
                         waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_PCM        ? "S" :
                                                                                   "UNKNOWN";

    result += std::format("{} {} ", static_cast<int>(waveformat.Samples.wValidBitsPerSample), static_cast<int>(waveformat.Format.nSamplesPerSec));

    switch (waveformat.dwChannelMask) {
      case waveformat_mask_stereo:
        result += "2.0";
        break;

      case waveformat_mask_surround51_with_backspeakers:
        result += "5.1";
        break;

      case waveformat_mask_surround51_with_sidespeakers:
        result += "5.1 (sidespeakers)";
        break;

      case waveformat_mask_surround71:
        result += "7.1";
        break;

      default:
        result += std::format("{} channels (unrecognized)", static_cast<int>(waveformat.Format.nChannels));
        break;
    }

    return result;
  }

}  // namespace

using namespace std::literals;

namespace platf::audio {
  /**
   * @brief Release the COM or platform reference owned by the pointer.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  template<class T>
  void Release(T *p) {
    p->Release();
  }

  /**
   * @brief Free memory allocated by COM task APIs.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  template<class T>
  void co_task_free(T *p) {
    CoTaskMemFree((LPVOID) p);
  }

  /**
   * @brief COM device enumerator pointer for WASAPI endpoint discovery.
   */
  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, &Release<IMMDeviceEnumerator>>;
  /**
   * @brief COM pointer to a Windows audio endpoint device.
   */
  using device_t = util::safe_ptr<IMMDevice, &Release<IMMDevice>>;
  /**
   * @brief COM pointer to a collection of Windows audio endpoint devices.
   */
  using collection_t = util::safe_ptr<IMMDeviceCollection, &Release<IMMDeviceCollection>>;
  /**
   * @brief COM pointer to the WASAPI audio client interface.
   */
  using audio_client_t = util::safe_ptr<IAudioClient, &Release<IAudioClient>>;
  /**
   * @brief COM pointer to the WASAPI capture client interface.
   */
  using audio_capture_t = util::safe_ptr<IAudioCaptureClient, &Release<IAudioCaptureClient>>;
  /**
   * @brief CoTaskMem-allocated WAVEFORMATEX pointer.
   */
  using wave_format_t = util::safe_ptr<WAVEFORMATEX, &co_task_free<WAVEFORMATEX>>;
  /**
   * @brief CoTaskMem-allocated wide string pointer.
   */
  using wstring_t = util::safe_ptr<WCHAR, &co_task_free<WCHAR>>;
  /**
   * @brief Windows HANDLE wrapper closed with `CloseHandle`.
   */
  using handle_t = util::safe_ptr_v2<void, BOOL, &CloseHandle>;
  /**
   * @brief COM pointer to the Windows policy configuration interface.
   */
  using policy_t = util::safe_ptr<IPolicyConfig, &Release<IPolicyConfig>>;
  /**
   * @brief COM pointer to a Windows property store.
   */
  using prop_t = util::safe_ptr<IPropertyStore, &Release<IPropertyStore>>;

  /**
   * @brief Initializes COM for the current thread and uninitializes it on exit.
   */
  class co_init_t: public deinit_t {
  public:
    co_init_t() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    }

    ~co_init_t() override {
      CoUninitialize();
    }
  };

  /**
   * @brief RAII wrapper that initializes and clears a Windows PROPVARIANT.
   */
  class prop_var_t {
  public:
    prop_var_t() {
      PropVariantInit(&prop);
    }

    ~prop_var_t() {
      PropVariantClear(&prop);
    }

    PROPVARIANT prop;  ///< Variant value returned by Windows property-store queries.
  };

  /**
   * @brief Windows audio format details selected for capture.
   */
  struct format_t {
    WORD channel_count;  ///< Channel count.
    std::string name;  ///< Human-readable name for this item.
    int capture_waveformat_channel_mask;  ///< Capture waveformat channel mask.
    virtual_sink_waveformats_t virtual_sink_waveformats;  ///< Virtual sink waveformats.
  };

  /**
   * @brief Formats.
   */
  const std::array<const format_t, 3> formats = {
    format_t {
      2,
      "Stereo",
      waveformat_mask_stereo,
      create_virtual_sink_waveformats<2>(),
    },
    format_t {
      6,
      "Surround 5.1",
      waveformat_mask_surround51_with_backspeakers,
      create_virtual_sink_waveformats<6>(),
    },
    format_t {
      8,
      "Surround 7.1",
      waveformat_mask_surround71,
      create_virtual_sink_waveformats<8>(),
    },
  };

  /**
   * @brief Whether per-process loopback capture should be attempted.
   *
   * The configured-sink pin needs no separate check here: the shared guard
   * maps a non-empty sink to pinned_sink intent and records PID 0 for every
   * non-host-and-client case, so a nonzero PID with host-and-client intent
   * already implies no pin. See audio::should_capture_process_tree().
   *
   * @param process_id Revalidated application PID, or 0 for endpoint capture.
   * @param intent Capture intent recorded for the next microphone() call.
   * @return True only for host-and-client intent with a known PID.
   */
  [[nodiscard]] constexpr bool use_process_loopback(std::uint32_t process_id, audio_capture_intent_e intent) noexcept {
    using enum audio_capture_intent_e;
    return intent == host_and_client && process_id != 0;
  }

  // Process identities use the shared platf::process_identity_t: the PID and
  // creation marker arrive as one handed-off bundle from the owned child
  // handle, so this backend stores the baseline verbatim instead of querying
  // it by PID (see platf::process_identity_t).

  /**
   * @brief Whether a current process identity is the recorded capture target.
   *
   * This is the pure comparison seam for PID-reuse hardening: it performs no
   * OS calls, so tests can exercise it with synthetic identities.
   *
   * @param recorded Identity recorded when the capture target was set.
   * @param current Identity queried immediately before activation.
   * @return True only when both identities name the same nonzero PID with the same nonzero creation time.
   */
  [[nodiscard]] constexpr bool process_identity_matches(const process_identity_t &recorded, const process_identity_t &current) noexcept {
    return recorded.process_id != 0 && current.process_id == recorded.process_id && recorded.creation_time != 0 && current.creation_time == recorded.creation_time;
  }

  /**
   * @brief Whether the recorded capture target still refers to a live process.
   *
   * @param recorded Identity recorded when the capture target was set.
   * @param current Identity queried immediately before activation, or empty when the process exited or could not be opened.
   * @return True only when a current identity is available and matches the recorded identity.
   */
  [[nodiscard]] constexpr bool capture_target_is_live(const process_identity_t &recorded, const std::optional<process_identity_t> &current) noexcept {
    return current.has_value() && process_identity_matches(recorded, *current);
  }

  /**
   * @brief Resolve the PID that may be used for process-loopback activation.
   *
   * @param recorded Identity recorded when the capture target was set.
   * @param current Identity queried immediately before activation, or empty when the process exited or could not be opened.
   * @return The recorded PID when the target is still the same live process, or 0 to select endpoint capture.
   */
  [[nodiscard]] constexpr std::uint32_t effective_capture_process_id(const process_identity_t &recorded, const std::optional<process_identity_t> &current) noexcept {
    return capture_target_is_live(recorded, current) ? recorded.process_id : 0;
  }

  /**
   * @brief Whether an activated process-loopback client still captures the validated target.
   *
   * The PID is revalidated immediately after activation because it may have
   * exited or been reused while ActivateAudioInterfaceAsync was resolving it.
   * This is the pure discard-decision seam: it performs no OS calls, so tests
   * can exercise it with synthetic PID pairs.
   *
   * @param activated_process_id PID the completed activation was started for.
   * @param revalidated_process_id PID revalidation still accepts, or 0 when the target exited or was reused.
   * @return True only when revalidation still accepts the activated PID.
   */
  [[nodiscard]] constexpr bool activated_process_target_is_current(std::uint32_t activated_process_id, std::uint32_t revalidated_process_id) noexcept {
    return activated_process_id != 0 && revalidated_process_id == activated_process_id;
  }

  /**
   * @brief Query the current identity of a process.
   *
   * The process handle is held only for the duration of this call. Failure to
   * open the process (including access denied), failure to read its times, or
   * a process that has already exited all fail closed by returning no identity.
   *
   * @param process_id Operating-system PID to query, or 0 for no process.
   * @return Current process identity, or empty when it cannot be verified as live.
   */
  std::optional<process_identity_t> query_process_identity(std::uint32_t process_id) {
    if (process_id == 0) {
      return std::nullopt;
    }
    handle_t process;
    process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process) {
      const auto last_error = GetLastError();
      BOOST_LOG(debug) << "Cannot open process ["sv << process_id << "] to verify its capture identity: [0x"sv << util::hex(last_error).to_string_view() << ']';
      return std::nullopt;
    }
    DWORD exit_code {};
    if (!GetExitCodeProcess(process.get(), &exit_code) || exit_code != STILL_ACTIVE) {
      BOOST_LOG(debug) << "Process ["sv << process_id << "] is no longer running; it will not be used for process loopback capture"sv;
      return std::nullopt;
    }
    FILETIME creation_time {};
    FILETIME exit_time {};
    FILETIME kernel_time {};
    FILETIME user_time {};
    if (!GetProcessTimes(process.get(), &creation_time, &exit_time, &kernel_time, &user_time)) {
      const auto last_error = GetLastError();
      BOOST_LOG(debug) << "Cannot read creation time for process ["sv << process_id << "]: [0x"sv << util::hex(last_error).to_string_view() << ']';
      return std::nullopt;
    }
    const auto combined_creation_time = (static_cast<std::uint64_t>(creation_time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(creation_time.dwLowDateTime);
    if (combined_creation_time == 0) {
      return std::nullopt;
    }
    return process_identity_t {process_id, combined_creation_time};
  }

  /**
   * @brief Activator that creates and initializes a process-loopback audio client.
   */
  using process_loopback_activator_t = std::function<audio_client_t(std::uint32_t, const format_t &)>;

  /**
   * @brief Activation step that returns an activated, not yet initialized, process-loopback audio client for a PID.
   *
   * This is the injectable seam around `activate_process_loopback_audio_client()`: production passes the real
   * WASAPI activation, while tests pass a fake so the wrapper in `make_process_loopback_audio_client()` can be
   * exercised without audio hardware.
   */
  using process_loopback_activation_t = std::function<audio_client_t(std::uint32_t)>;

  constexpr DWORD process_loopback_activation_timeout_ms {10000};  ///< Deadline for asynchronous process-loopback activation.

#ifdef SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS
  /**
   * @brief Build activation parameters for process-loopback capture.
   *
   * @param process_id Launched application PID to capture.
   * @return Activation parameters including the target process tree.
   */
  [[nodiscard]] AUDIOCLIENT_ACTIVATION_PARAMS make_process_loopback_activation_params(std::uint32_t process_id) noexcept {
    AUDIOCLIENT_ACTIVATION_PARAMS params {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = process_id;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    return params;
  }

  /**
   * @brief Fill a PROPVARIANT with a heap-owned copy of loopback activation parameters.
   *
   * ActivateAudioInterfaceAsync reads the VT_BLOB payload asynchronously, so
   * the bytes must outlive the call. The copy is heap-allocated with
   * CoTaskMemAlloc, which makes PropVariantClear() always safe: it frees the
   * heap copy and can never free a stack address.
   *
   * @param prop PROPVARIANT to fill; it must be PropVariantInit'd and empty.
   * @param params Activation parameters to copy into the heap blob.
   * @return S_OK when the blob was installed, E_OUTOFMEMORY when the copy cannot be allocated.
   */
  HRESULT init_process_loopback_activation_blob(PROPVARIANT &prop, const AUDIOCLIENT_ACTIVATION_PARAMS &params) {
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = static_cast<BYTE *>(CoTaskMemAlloc(sizeof(params)));
    if (!prop.blob.pBlobData) {
      prop.vt = VT_EMPTY;
      prop.blob.cbSize = 0;
      return E_OUTOFMEMORY;
    }
    std::memcpy(prop.blob.pBlobData, &params, sizeof(params));
    return S_OK;
  }
#endif

  /**
   * @brief How initialize_audio_client() reacts when GetMixFormat fails.
   */
  enum class mix_format_failure_e {
    fail,  ///< Return no client.
    tolerate,  ///< Keep the created format and continue.
  };

  /**
   * @brief Initialize an activated audio client for shared loopback capture.
   *
   * @param audio_client Activated WASAPI audio client.
   * @param format Requested capture format.
   * @param mix_format_failure Whether GetMixFormat failure keeps the created format.
   * @return Initialized audio client, or nullptr on failure.
   */
  audio_client_t initialize_audio_client(audio_client_t audio_client, const format_t &format, mix_format_failure_e mix_format_failure) {
    using enum mix_format_failure_e;
    if (!audio_client) {
      return nullptr;
    }
    WAVEFORMATEXTENSIBLE capture_waveformat =
      create_waveformat(sample_format_e::f32, format.channel_count, format.capture_waveformat_channel_mask);
    {
      wave_format_t mixer_waveformat;
      auto status = audio_client->GetMixFormat(&mixer_waveformat);
      if (FAILED(status)) {
        if (mix_format_failure == fail) {
          BOOST_LOG(error) << "Couldn't get mix format for audio device: [0x"sv << util::hex(status).to_string_view() << ']';
          return nullptr;
        }
        BOOST_LOG(warning) << "Couldn't get mix format for process loopback client: [0x"sv << util::hex(status).to_string_view() << ']';
      } else {
        if (mixer_waveformat->nChannels == format.channel_count && mixer_waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixer_waveformat->cbSize >= 22) {
          auto waveformatext_pointer = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mixer_waveformat.get());
          capture_waveformat.dwChannelMask = waveformatext_pointer->dwChannelMask;
        }
        BOOST_LOG(info) << "Audio mixer format is "sv << mixer_waveformat->wBitsPerSample << "-bit, "sv
                        << mixer_waveformat->nSamplesPerSec << " Hz, "sv
                        << ((mixer_waveformat->nSamplesPerSec != 48000) ? "will be resampled to 48000 by Windows"sv : "no resampling needed"sv);
      }
    }
    auto status = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
      0,
      0,
      (LPWAVEFORMATEX) &capture_waveformat,
      nullptr
    );
    if (status) {
      BOOST_LOG(error) << "Couldn't initialize audio client for ["sv << format.name << "]: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }
    BOOST_LOG(info) << "Audio capture format is "sv << logging::bracket(waveformat_to_pretty_string(capture_waveformat));
    return audio_client;
  }

#if defined(SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS) && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
  /**
   * @brief Completion handler for asynchronous WASAPI process-loopback activation.
   *
   * The callback fills in the result fields and then signals the completion event.
   * The waiter reads the fields only after a signaled wait; on timeout it must not
   * touch them at all, because the callback may still be running or arrive later.
   * A signaled wait also makes the callback's writes visible to the waiter: the
   * wait/signal functions act as memory barriers.
   * https://learn.microsoft.com/en-us/windows/win32/sync/synchronization-and-multiprocessor-issues
   */
  class process_loopback_completion_handler_t final: public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
  public:
    /**
     * @brief Construct the handler and its completion event.
     */
    process_loopback_completion_handler_t() {
      event_handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }

    /**
     * @brief Destroy the handler and close its completion event.
     */
    ~process_loopback_completion_handler_t() {
      if (event_handle) {
        CloseHandle(event_handle);
      }
    }

    /**
     * @brief Query a supported COM interface.
     * @param riid Requested interface identifier.
     * @param ppvInterface Receives the interface pointer.
     * @return S_OK when supported, E_NOINTERFACE otherwise.
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) override {  // NOSONAR(cpp:S5008): IUnknown::QueryInterface requires a void double pointer in the COM ABI.
      if (!ppvInterface) {
        return E_POINTER;
      }
      if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
        *ppvInterface = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
      } else if (riid == __uuidof(IAgileObject)) {
        *ppvInterface = static_cast<IAgileObject *>(this);
      } else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
      }
      AddRef();
      return S_OK;
    }

    /**
     * @brief Increment the reference count.
     * @return New reference count.
     */
    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++ref_count;
    }

    /**
     * @brief Decrement the reference count, deleting when it reaches zero.
     * @return New reference count.
     */
    ULONG STDMETHODCALLTYPE Release() override {
      auto count = --ref_count;
      if (count == 0) {
        delete this;
      }
      return count;
    }

    /**
     * @brief Record asynchronous activation completion and signal the waiter.
     * @param activate_operation Completed activation operation.
     * @return S_OK after recording the result.
     */
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *activate_operation) override {
      if (!activate_operation) {
        activate_result = E_POINTER;
        activation_hresult = E_POINTER;
        query_interface_result = E_POINTER;
        if (event_handle) {
          SetEvent(event_handle);
        }
        return S_OK;
      }
      IUnknown *unknown = nullptr;
      activate_result = activate_operation->GetActivateResult(&activation_hresult, &unknown);
      if (SUCCEEDED(activate_result) && SUCCEEDED(activation_hresult) && unknown) {
        IAudioClient *raw_client = nullptr;
        query_interface_result = unknown->QueryInterface(IID_IAudioClient, (void **) &raw_client);  // NOSONAR(cpp:S5008): IUnknown::QueryInterface requires a void double pointer in the COM ABI.
        if (SUCCEEDED(query_interface_result) && raw_client) {
          activated_client.reset(raw_client);
        } else {
          if (raw_client) {
            raw_client->Release();
          }
          BOOST_LOG(warning) << "Process loopback QueryInterface(IAudioClient) failed: [0x"sv << util::hex(query_interface_result).to_string_view() << ']';
        }
      }
      if (unknown) {
        unknown->Release();
      }
      if (event_handle) {
        SetEvent(event_handle);
      }
      return S_OK;
    }

    HANDLE event_handle = nullptr;  ///< Event signaled when activation completes.
    HRESULT activate_result = E_PENDING;  ///< Result of GetActivateResult.
    HRESULT activation_hresult = E_PENDING;  ///< Activation HRESULT reported by the operation.
    HRESULT query_interface_result = E_PENDING;  ///< Result of QueryInterface for IAudioClient, or E_PENDING when not attempted.
    audio_client_t activated_client;  ///< Activated audio client, when activation succeeds.

  private:
    std::atomic<ULONG> ref_count {1};  ///< COM reference count.
  };

  /**
   * @brief Wait for loopback activation completion and collect its result.
   *
   * Consumes the caller's handler reference on every path. When the wait reports
   * anything but WAIT_OBJECT_0 the completion fields are never touched, because the
   * callback may still be running or arrive later; a signaled wait means the callback
   * has finished writing them. A late callback stays alive through the reference
   * Windows itself holds until the operation completes; the handler must not be
   * freed before the callback runs.
   * https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync
   *
   * The handler must be non-null with a live caller reference; the caller must not
   * touch it after return, even on timeout, because the late callback is covered by
   * the operation's own reference.
   *
   * When `wait_timed_out` is provided and the wait expires, the caller
   * reference is NOT consumed: `*wait_timed_out` is set and ownership of
   * that reference returns to the caller, which must keep the handler (and
   * any activation blob the pending operation may still read) alive until
   * the completion event fires, then release it.
   *
   * @param handler Completion handler passed to ActivateAudioInterfaceAsync; its caller reference is consumed unless the wait times out with `wait_timed_out` provided.
   * @param process_id Launched application PID being captured, used for diagnostics.
   * @param timeout_ms Wait deadline in milliseconds.
   * @param wait_timed_out Optional out-flag set when the wait expires without consuming the caller reference.
   * @return Activated (not yet initialized) audio client, or nullptr on failure or timeout.
   */
  audio_client_t collect_process_loopback_activation(process_loopback_completion_handler_t *handler, std::uint32_t process_id, DWORD timeout_ms = process_loopback_activation_timeout_ms, bool *wait_timed_out = nullptr) {
    auto wait_result = WaitForSingleObject(handler->event_handle, timeout_ms);
    if (wait_result != WAIT_OBJECT_0) {
      BOOST_LOG(warning) << "Process loopback activation timed out for process ["sv << process_id
                         << "]: wait returned [0x"sv << util::hex(wait_result).to_string_view() << "] after timeout ["sv << timeout_ms
                         << " ms]; completion results unavailable"sv;
      if (wait_timed_out) {
        *wait_timed_out = true;
        return nullptr;
      }
      handler->Release();
      return nullptr;
    }
    audio_client_t client;
    if (SUCCEEDED(handler->activate_result) && SUCCEEDED(handler->activation_hresult) && SUCCEEDED(handler->query_interface_result) && handler->activated_client) {
      client = std::move(handler->activated_client);
    } else {
      BOOST_LOG(warning) << "Process loopback activation did not complete successfully for process ["sv << process_id
                         << "]: wait returned [0x"sv << util::hex(wait_result).to_string_view() << "] after timeout ["sv << timeout_ms
                         << " ms], GetActivateResult returned [0x"sv << util::hex(handler->activate_result).to_string_view()
                         << "], activation returned [0x"sv << util::hex(handler->activation_hresult).to_string_view()
                         << "], QueryInterface(IAudioClient) returned [0x"sv << util::hex(handler->query_interface_result).to_string_view() << ']';
    }
    handler->Release();
    return client;
  }

  /**
   * @brief Activate a WASAPI audio client for a process tree via ActivateAudioInterfaceAsync.
   *
   * @param process_id Launched application PID to capture.
   * @return Activated (not yet initialized) audio client, or nullptr on failure.
   */
  audio_client_t activate_process_loopback_audio_client(std::uint32_t process_id) {
    auto *handler = new process_loopback_completion_handler_t();
    if (!handler->event_handle) {
      BOOST_LOG(warning) << "Process loopback activation for process ["sv << process_id << "] failed to create its completion event: [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
      handler->Release();
      return nullptr;
    }
    // Unlike Microsoft's ApplicationLoopback sample, which borrows the address of
    // the stack-local params for the activation blob, the payload here is a
    // heap copy: init_process_loopback_activation_blob() allocates it with
    // CoTaskMemAlloc, so the prop_var_t destructor's PropVariantClear() always
    // frees a heap address and can never free the stack. The prop_var_t itself
    // is heap-allocated too, because on a wait timeout the pending operation
    // may still read the blob and call the handler: both must outlive this
    // stack frame until the completion event fires (handled below).
    const auto params = make_process_loopback_activation_params(process_id);
    auto *activation_prop = new prop_var_t();
    if (FAILED(init_process_loopback_activation_blob(activation_prop->prop, params))) {
      BOOST_LOG(warning) << "Process loopback activation for process ["sv << process_id << "] could not allocate its activation blob"sv;
      delete activation_prop;
      handler->Release();
      return nullptr;
    }
    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    auto status = ActivateAudioInterfaceAsync(
      VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
      IID_IAudioClient,
      &activation_prop->prop,
      handler,
      &operation
    );
    if (operation) {
      operation->Release();
    }
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Process loopback activation failed to start for process ["sv << process_id << "]: ActivateAudioInterfaceAsync returned [0x"sv << util::hex(status).to_string_view() << ']';
      delete activation_prop;
      handler->Release();
      return nullptr;
    }
    // collect_process_loopback_activation() consumes the caller's handler
    // reference unless the wait times out; on timeout keep the handler and
    // activation blob alive on a detached waiter until the pending
    // operation completes, instead of freeing state it may still read.
    bool wait_timed_out = false;
    auto client = collect_process_loopback_activation(handler, process_id, process_loopback_activation_timeout_ms, &wait_timed_out);
    if (wait_timed_out) {
      try {
        std::thread {[handler, activation_prop]() {
          WaitForSingleObject(handler->event_handle, INFINITE);
          delete activation_prop;
          handler->Release();
        }}.detach();
      } catch (...) {
        // No waiter could be started; leak the handler and blob on purpose
        // rather than free state the pending operation may still read.
        BOOST_LOG(warning) << "Process loopback activation for process ["sv << process_id << "] timed out and its cleanup waiter could not start; leaking its activation state until process exit"sv;
      }
      return nullptr;
    }
    delete activation_prop;
    return client;
  }
#endif

  /**
   * @brief Create and initialize a process-loopback audio client using a supplied activation step.
   *
   * @param process_id Launched application PID to capture.
   * @param format Requested capture format.
   * @param activation Activation step returning an activated, not yet initialized, client for the PID.
   * @return Initialized audio client, or nullptr when activation is missing or initialization fails.
   */
  audio_client_t make_process_loopback_audio_client(std::uint32_t process_id, const format_t &format, const process_loopback_activation_t &activation) {
    if (!activation) {
      BOOST_LOG(warning) << "Process loopback activation is unavailable for process ["sv << process_id << ']';
      return nullptr;
    }
    return initialize_audio_client(activation(process_id), format, mix_format_failure_e::tolerate);
  }

  /**
   * @brief Create and initialize a process-loopback audio client for a PID.
   *
   * @param process_id Launched application PID to capture.
   * @param format Requested capture format.
   * @return Initialized audio client, or nullptr when process loopback is unavailable or fails.
   */
  audio_client_t make_process_loopback_audio_client(std::uint32_t process_id, const format_t &format) {
#if defined(SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS) && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
    return make_process_loopback_audio_client(process_id, format, process_loopback_activation_t {&activate_process_loopback_audio_client});
#else
    (void) process_id;
    (void) format;
    return nullptr;
#endif
  }

  /**
   * @brief Whether WASAPI process loopback can work on this build and OS.
   *
   * Process loopback needs the activation-parameter headers at compile time
   * and Windows build 20348 or later (Windows 11 / Server 2022) at runtime.
   * The version is read through `RtlGetVersion` so application-manifest
   * version lies cannot mask the real build, and the result is cached.
   *
   * @return True when process-loopback activation is worth attempting.
   */
  [[nodiscard]] bool process_loopback_supported() {
#if defined(SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS) && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
    static const bool supported = []() {
      using rtl_get_version_t = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
      const auto ntdll = GetModuleHandleW(L"ntdll.dll");
      if (!ntdll) {
        return false;
      }
      const auto rtl_get_version = reinterpret_cast<rtl_get_version_t>(GetProcAddress(ntdll, "RtlGetVersion"));
      if (!rtl_get_version) {
        return false;
      }
      RTL_OSVERSIONINFOW version {};
      version.dwOSVersionInfoSize = sizeof(version);
      if (rtl_get_version(&version) != 0) {
        return false;
      }
      return version.dwMajorVersion > 10 || (version.dwMajorVersion == 10 && version.dwBuildNumber >= 20348);
    }();
    return supported;
#else
    return false;
#endif
  }

  /**
   * @brief Create audio client.
   *
   * @param device D3D, audio, or platform device used by the operation.
   * @param format Pixel, audio, or protocol format being converted.
   * @return Constructed audio client object.
   */
  audio_client_t make_audio_client(device_t &device, const format_t &format) {
    audio_client_t audio_client;
    auto status = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      (void **) &audio_client
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't activate Device: [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return initialize_audio_client(std::move(audio_client), format, mix_format_failure_e::fail);
  }

  /**
   * @brief Query the default Windows render endpoint.
   *
   * @param device_enum Windows multimedia device enumerator.
   * @return Default render endpoint, or an empty handle if lookup fails.
   */
  device_t default_device(device_enum_t &device_enum) {
    device_t device;
    HRESULT status;
    status = device_enum->GetDefaultAudioEndpoint(
      eRender,
      eConsole,
      &device
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get default audio endpoint [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return device;
  }

  /**
   * @brief Windows audio endpoint notification callback registered with MMDevice.
   */
  class audio_notification_t: public ::IMMNotificationClient {
  public:
    audio_notification_t() = default;

    // IUnknown implementation (unused by IMMDeviceEnumerator)
    /**
     * @brief Satisfy IUnknown reference counting for the notification callback.
     *
     * @return Static reference count because the callback lifetime is externally owned.
     */
    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    /**
     * @brief Release the COM or platform reference owned by the pointer.
     *
     * @return Reference count or status returned after releasing the object.
     */
    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    /**
     * @brief Implement IUnknown's interface query for the endpoint notification callback.
     *
     * @param riid COM interface identifier requested by QueryInterface.
     * @param ppvInterface Output pointer receiving the requested interface.
     * @return S_OK when the interface is supported; E_NOINTERFACE otherwise.
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) override {  // NOSONAR(cpp:S5008): IUnknown::QueryInterface requires a void double pointer in the COM ABI.
      if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *) this;
        return S_OK;
      } else if (__uuidof(IMMNotificationClient) == riid) {
        AddRef();
        *ppvInterface = (IMMNotificationClient *) this;
        return S_OK;
      } else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
      }
    }

    // IMMNotificationClient
    /**
     * @brief Handle a Windows default-audio-device change notification.
     *
     * @param flow Audio endpoint data-flow direction.
     * @param role Audio endpoint role used for default-device lookup.
     * @param pwstrDeviceId Windows endpoint ID for the new default device.
     * @return S_OK after recording the render-device change notification.
     */
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) override {
      if (flow == eRender) {
        default_render_device_changed_flag.store(true);
      }
      return S_OK;
    }

    /**
     * @brief Ignore endpoint-add notifications.
     *
     * @param pwstrDeviceId Windows endpoint ID for the added device.
     * @return S_OK because Sunshine does not act on this notification.
     */
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override {
      return S_OK;
    }

    /**
     * @brief Ignore endpoint-removal notifications.
     *
     * @param pwstrDeviceId Windows endpoint ID for the removed device.
     * @return S_OK because Sunshine does not act on this notification.
     */
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override {
      return S_OK;
    }

    /**
     * @brief Handle Windows audio endpoint state changes.
     *
     * @param pwstrDeviceId Audio device ID.
     * @param dwNewState New device state.
     * @return COM status code.
     */
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
      LPCWSTR pwstrDeviceId,
      DWORD dwNewState
    ) override {
      return S_OK;
    }

    /**
     * @brief Handle Windows audio endpoint property changes.
     *
     * @param pwstrDeviceId Audio device ID.
     * @param key Changed property key.
     * @return COM status code.
     */
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
      LPCWSTR pwstrDeviceId,
      const PROPERTYKEY key
    ) override {
      return S_OK;
    }

    /**
     * @brief Checks if the default rendering device changed and resets the change flag
     * @return `true` if the device changed since last call
     */
    bool check_default_render_device_changed() {
      return default_render_device_changed_flag.exchange(false);
    }

  private:
    std::atomic_bool default_render_device_changed_flag;
  };

  /**
   * @brief WASAPI microphone capture stream and endpoint notification state.
   */
  class mic_wasapi_t: public mic_t {
  public:
    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_out Sample out.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_out) override {
      auto sample_size = sample_out.size();

      // Refill the sample buffer if needed
      while (sample_buf_pos - std::begin(sample_buf) < sample_size) {
        auto capture_result = _fill_buffer();
        if (capture_result == capture_e::timeout && continuous_audio) {
          // Write silence to sample_buf
          std::fill_n(sample_buf_pos, sample_size, 0.0f);
          sample_buf_pos += sample_size;
        } else if (capture_result != capture_e::ok) {
          return capture_result;
        }
      }

      // Fill the output buffer with samples
      std::copy_n(std::begin(sample_buf), sample_size, std::begin(sample_out));

      // Move any excess samples to the front of the buffer
      std::move(&sample_buf[sample_size], sample_buf_pos, std::begin(sample_buf));
      sample_buf_pos -= sample_size;

      return capture_e::ok;
    }

    /**
     * @brief Initialize WASAPI capture for the selected audio endpoint.
     *
     * Only the channels, frame size, and continuous flag are used: WASAPI
     * capture is fixed at 48 kHz float, the Opus mapping is encoder-side, and
     * host-audio routing is resolved by the shared capture layer.
     *
     * @param params Microphone capture parameters for the requested layout.
     * @param capture_device Endpoint device to capture from; the default render device is used when empty.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(const microphone_params_t &params, device_t capture_device) {
      audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      if (!audio_event) {
        BOOST_LOG(error) << "Couldn't create Event handle"sv;

        return -1;
      }

      HRESULT status;

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = device_enum->RegisterEndpointNotificationCallback(&endpt_notification);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't register endpoint notification [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      select_capture_device(std::move(capture_device));

      if (!device) {
        return -1;
      }

      for (const auto &format : formats) {
        if (format.channel_count != params.channels) {
          BOOST_LOG(debug) << "Skipping audio format ["sv << format.name << "] with channel count ["sv
                           << format.channel_count << " != "sv << params.channels << ']';
          continue;
        }

        BOOST_LOG(debug) << "Trying audio format ["sv << format.name << ']';
        audio_client = make_audio_client(device, format);

        if (audio_client) {
          BOOST_LOG(debug) << "Found audio format ["sv << format.name << ']';
          channels = params.channels;
          break;
        }
      }

      if (!audio_client) {
        BOOST_LOG(error) << "Couldn't find supported format for audio"sv;
        return -1;
      }

      return finish_capture_init(params);
    }

    /**
     * @brief Initialize WASAPI capture for a specific process tree.
     *
     * @param params Microphone capture parameters for the requested layout.
     * @param process_id Launched application PID to capture.
     * @param activator Activator creating an initialized process-loopback client.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init_process_loopback(const microphone_params_t &params, std::uint32_t process_id, const process_loopback_activator_t &activator) {
      audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      if (!audio_event) {
        BOOST_LOG(error) << "Couldn't create Event handle"sv;
        return -1;
      }
      follows_default_device = false;
      for (const auto &format : formats) {
        if (format.channel_count != params.channels) {
          continue;
        }
        audio_client = activator(process_id, format);
        if (audio_client) {
          channels = params.channels;
          break;
        }
      }
      if (!audio_client) {
        BOOST_LOG(warning) << "Couldn't create process loopback audio client"sv;
        return -1;
      }
      return finish_capture_init(params);
    }

    /**
     * @brief Finish capture initialization after an audio client is initialized.
     *
     * @param params Microphone capture parameters for the requested layout.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int finish_capture_init(const microphone_params_t &params) {
      HRESULT status;
      REFERENCE_TIME default_latency = 100000;
      audio_client->GetDevicePeriod(&default_latency, nullptr);
      default_latency_ms = default_latency / 1000;
      continuous_audio = params.continuous_audio;

      std::uint32_t frames;
      status = audio_client->GetBufferSize(&frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't acquire the number of audio frames [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      // *2 --> needs to fit double
      sample_buf = util::buffer_t<float> {std::max(frames, params.frame_size) * 2 * static_cast<std::uint32_t>(params.channels)};
      sample_buf_pos = std::begin(sample_buf);

      status = audio_client->GetService(IID_IAudioCaptureClient, (void **) &audio_capture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't initialize audio capture client [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = audio_client->SetEventHandle(audio_event.get());
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't set event handle [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      {
        DWORD task_index = 0;
        mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
        if (!mmcss_task_handle) {
          BOOST_LOG(error) << "Couldn't associate audio capture thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
        }
      }

      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't start recording [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      return 0;
    }

    /**
     * @brief Select the endpoint used by this capture stream.
     *
     * @param capture_device Explicit endpoint to capture, or an empty pointer to follow the default endpoint.
     */
    void select_capture_device(device_t capture_device) {
      follows_default_device = !capture_device;
      if (follows_default_device) {
        device = default_device(device_enum);
      } else {
        device = std::move(capture_device);
      }
    }

    ~mic_wasapi_t() override {
      if (device_enum) {
        device_enum->UnregisterEndpointNotificationCallback(&endpt_notification);
      }

      if (audio_client) {
        audio_client->Stop();
      }

      if (mmcss_task_handle) {
        AvRevertMmThreadCharacteristics(mmcss_task_handle);
      }
    }

  private:
    capture_e _fill_buffer() {
      HRESULT status;

      // Total number of samples
      struct sample_aligned_t {
        std::uint32_t uninitialized;
        float *samples;
      };

      sample_aligned_t sample_aligned;

      // number of samples / number of channels
      struct block_aligned_t {
        std::uint32_t audio_sample_size;
      };

      block_aligned_t block_aligned;

      // Check if the default audio device has changed
      if (endpt_notification.check_default_render_device_changed()) {
        // Invoke the audio_control_t's callback if it wants one
        if (default_endpt_changed_cb) {
          (*default_endpt_changed_cb)();
        }

        // Reinitialize to pick up the new default device, unless capture is
        // pinned to an explicitly requested sink
        if (follows_default_device) {
          return capture_e::reinit;
        }
      }

      status = WaitForSingleObjectEx(audio_event.get(), default_latency_ms, FALSE);
      switch (status) {
        case WAIT_OBJECT_0:
          break;
        case WAIT_TIMEOUT:
          return capture_e::timeout;
        default:
          BOOST_LOG(error) << "Couldn't wait for audio event: [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::capture_error;
      }

      std::uint32_t packet_size {};
      for (
        status = audio_capture->GetNextPacketSize(&packet_size);
        SUCCEEDED(status) && packet_size > 0;
        status = audio_capture->GetNextPacketSize(&packet_size)) {
        DWORD buffer_flags;
        status = audio_capture->GetBuffer(
          (BYTE **) &sample_aligned.samples,
          &block_aligned.audio_sample_size,
          &buffer_flags,
          nullptr,
          nullptr
        );

        switch (status) {
          case S_OK:
            break;
          case AUDCLNT_E_DEVICE_INVALIDATED:
            return capture_e::reinit;
          default:
            BOOST_LOG(error) << "Couldn't capture audio [0x"sv << util::hex(status).to_string_view() << ']';
            return capture_e::capture_error;
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          BOOST_LOG(debug) << "Audio capture signaled buffer discontinuity";
        }

        sample_aligned.uninitialized = std::end(sample_buf) - sample_buf_pos;
        auto n = std::min(sample_aligned.uninitialized, block_aligned.audio_sample_size * channels);

        if (n < block_aligned.audio_sample_size * channels) {
          BOOST_LOG(warning) << "Audio capture buffer overflow";
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill_n(sample_buf_pos, n, 0);
        } else {
          std::copy_n(sample_aligned.samples, n, sample_buf_pos);
        }

        sample_buf_pos += n;

        audio_capture->ReleaseBuffer(block_aligned.audio_sample_size);
      }

      if (status == AUDCLNT_E_DEVICE_INVALIDATED) {
        return capture_e::reinit;
      }

      if (FAILED(status)) {
        return capture_e::capture_error;
      }

      return capture_e::ok;
    }

  public:
    handle_t audio_event;  ///< Event signaled by WASAPI when captured audio is available.

    device_enum_t device_enum;  ///< Device enum.
    device_t device;  ///< WASAPI endpoint device selected for capture.
    audio_client_t audio_client;  ///< WASAPI audio client configured for shared-mode capture.
    audio_capture_t audio_capture;  ///< WASAPI capture client used to read sample packets.

    audio_notification_t endpt_notification;  ///< Endpoint notification callback registered with Windows.
    std::optional<std::function<void()>> default_endpt_changed_cb;  ///< Callback invoked when the default endpoint changes.

    REFERENCE_TIME default_latency_ms;  ///< WASAPI default device period used as capture latency.

    util::buffer_t<float> sample_buf;  ///< Floating-point sample buffer filled from WASAPI packets.
    float *sample_buf_pos;  ///< Current write position in `sample_buf`.
    int channels;  ///< Number of channels in the capture format.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool follows_default_device;  ///< Whether capture follows the default render device rather than an explicit sink.

    HANDLE mmcss_task_handle = nullptr;  ///< MMCSS task handle for the audio capture thread.
  };

  /**
   * @brief Platform audio controller that manages sinks and microphone capture.
   */
  class audio_control_t: public ::platf::audio_control_t {
  public:
    // The positional microphone() override below hides the parameter-bundle
    // overload; unhide it so new code (and the test seams) can call the bundle
    // form on this type directly instead of via a base-class reference.
    using ::platf::audio_control_t::microphone;

    /**
     * @brief Query host and virtual sink names available to Sunshine.
     *
     * @return Host and virtual sink names when the backend can report them.
     */
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      // Fill host sink name with the device_id of the current default audio device.
      {
        auto device = default_device(device_enum);
        if (!device) {
          // Keep-default with a configured sink does not need a default: capture that
          // endpoint directly, but only when it resolves to an active device right now.
          if (config::audio.keep_default_sink && !config::audio.sink.empty() && get_sink_device(config::audio.sink)) {
            BOOST_LOG(warning) << "No default audio device available; capturing configured sink ["sv << config::audio.sink << ']';
            sink.host = config::audio.sink;
            return sink;
          }
          return std::nullopt;
        }

        audio::wstring_t id;
        device->GetId(&id);

        sink.host = utf_utils::to_utf8(id.get());
      }

      // Keep-default routing never selects the virtual sink, so skip its discovery.
      if (config::audio.keep_default_sink) {
        return sink;
      }

      // Prepare to search for the device_id of the virtual audio sink device,
      // this device can be either user-configured or
      // the Steam Streaming Speakers we use by default.
      match_fields_list_t match_list;
      if (config::audio.virtual_sink.empty()) {
        match_list = match_steam_speakers();
      } else {
        match_list = match_all_fields(utf_utils::from_utf8(config::audio.virtual_sink));
      }

      // Search for the virtual audio sink device currently present in the system.
      auto matched = find_device_id(match_list);
      if (matched) {
        // Prepare to fill virtual audio sink names with device_id.
        auto device_id = utf_utils::to_utf8(matched->second);
        // Also prepend format name (basically channel layout at the moment)
        // because we don't want to extend the platform interface.
        sink.null = std::make_optional(sink_t::null_t {
          "virtual-"s + formats[0].name + device_id,
          "virtual-"s + formats[1].name + device_id,
          "virtual-"s + formats[2].name + device_id,
        });
      } else if (!config::audio.virtual_sink.empty()) {
        BOOST_LOG(warning) << "Couldn't find the specified virtual audio sink " << config::audio.virtual_sink;
      }

      return sink;
    }

    bool is_sink_available(const std::string &sink) override {
      const auto match_list = match_all_fields(utf_utils::from_utf8(sink));
      const auto matched = find_device_id(match_list);
      return static_cast<bool>(matched);
    }

    /**
     * @brief Extract virtual audio sink information possibly encoded in the sink name.
     * @param sink The sink name
     * @return A pair of device_id and format reference if the sink name matches
     *         our naming scheme for virtual audio sinks, `std::nullopt` otherwise.
     */
    std::optional<std::pair<std::wstring, std::reference_wrapper<const format_t>>> extract_virtual_sink_info(std::string_view sink) const {
      // Encoding format:
      // [virtual-(format name)]device_id
      std::string_view current = sink;
      if (auto prefix = "virtual-"sv; current.find(prefix) == 0) {
        current = current.substr(prefix.size(), current.size() - prefix.size());

        for (const auto &format : formats) {
          auto &name = format.name;
          if (current.find(name) == 0) {
            auto device_id = utf_utils::from_utf8(std::string {current.substr(name.size(), current.size() - name.size())});
            return std::make_pair(device_id, std::reference_wrapper(format));
          }
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Resolve a sink name to the audio endpoint device it refers to.
     *
     * @param sink Sink name, virtual sink descriptor, or device identifier.
     * @return Endpoint device to capture from, or an empty pointer if the sink couldn't be resolved.
     */
    device_t get_sink_device(const std::string &sink) {
      std::wstring device_id;
      if (auto virtual_sink_info = extract_virtual_sink_info(sink)) {
        device_id = virtual_sink_info->first;
      } else if (auto matched = find_device_id(match_all_fields(utf_utils::from_utf8(sink)))) {
        device_id = matched->second;
      } else {
        return nullptr;
      }

      device_t device;
      if (FAILED(device_enum->GetDevice(device_id.c_str(), &device))) {
        return nullptr;
      }

      if (DWORD device_state {}; FAILED(device->GetState(&device_state)) || device_state != DEVICE_STATE_ACTIVE) {
        return nullptr;
      }

      return device;
    }

    /**
     * @brief Consistent copy of the recorded capture target.
     */
    struct capture_target_snapshot_t {
      audio_capture_intent_e intent;  ///< Intent for the next microphone() call.
      std::uint32_t process_id;  ///< PID for process-loopback capture, or 0 for endpoint capture.
      process_identity_t identity;  ///< Creation-time identity recorded with `process_id`, empty when unverifiable.
    };

    /**
     * @brief Copy the recorded capture target under the base capture mutex.
     *
     * Holds `capture_state_mutex_` once so the intent, PID, and identity form
     * one consistent snapshot with the shared `recorded_target_` record.
     *
     * @return Intent, PID, and identity as one consistent snapshot.
     */
    capture_target_snapshot_t snapshot_capture_target() const {
      std::lock_guard lock {capture_state_mutex_};
      return {recorded_target_.intent, recorded_target_.process_id, capture_target_identity};
    }

    /**
     * @brief Copy the assigned sink name under the base capture mutex.
     *
     * @return Sink assigned while Sunshine captures host audio, possibly empty.
     */
    std::string snapshot_assigned_sink() const {
      return snapshot_recorded_sink();
    }

    /**
     * @brief Record the capture target for the next microphone() call.
     *
     * Thread-safe: the intent, PID, and identity are stored as one snapshot
     * under `capture_state_mutex_`, so microphone() never sees a mismatched
     * triple. The identity arrives pre-queried from the owned child handle
     * under the process lifecycle lock and is stored verbatim as the
     * revalidation baseline: querying it here by PID instead would reopen the
     * reuse window the handoff closes (an exited process whose PID was already
     * reused would record the replacement's creation time and defeat both the
     * pre-activation and post-activation checks). An unverifiable baseline
     * fails closed at validation time, where the PID resolves to endpoint capture.
     *
     * @param intent Capture intent resolved from the audio configuration.
     * @param identity Operating-system identity to capture, or {0, 0} for endpoint capture.
     */
    void set_capture_target(audio_capture_intent_e intent, process_identity_t identity) override {
      if (identity.process_id == 0) {
        identity = {};
      }
      std::lock_guard lock {capture_state_mutex_};
      recorded_target_.intent = intent;
      recorded_target_.process_id = identity.process_id;
      capture_target_identity = identity;
    }

    /**
     * @brief Release the recorded capture target when it still belongs to the caller.
     *
     * Thread-safe: the compare-and-reset runs under `capture_state_mutex_`.
     * The target resets to endpoint capture only when the stored intent and
     * PID still equal the given ones, so one exiting session cannot wipe
     * another session's recorded target.
     *
     * @param intent Capture intent recorded by the session being torn down.
     * @param process_id PID recorded by the session being torn down, or 0 for endpoint capture.
     */
    void clear_capture_target(audio_capture_intent_e intent, std::uint32_t process_id) override {
      clear_recorded_capture_target_if_matches(intent, process_id);
    }

#ifdef SUNSHINE_TESTS
    /**
     * @brief Install a test override for the process-loopback activator used by microphone().
     *
     * Tests install a counting activator that returns no client so the
     * loopback-attempt and activation-failure fallback can be exercised
     * without audio hardware. Production never calls this; an empty override
     * selects the real WASAPI activator.
     *
     * @param activator Activator microphone() uses instead of the production one.
     */
    void set_test_loopback_activator(process_loopback_activator_t activator) {
      test_loopback_activator = std::move(activator);
    }
#endif

    /**
     * @brief Revalidate the recorded capture target immediately before activation.
     *
     * Snapshots the target under `capture_state_mutex_`, then revalidates without
     * holding the lock so process queries never block other sessions.
     *
     * @return Recorded PID when it still refers to the same live process, or 0 when the process exited, the PID was reused, or its identity cannot be verified.
     */
    std::uint32_t validated_capture_process_id() {
      return validated_snapshot_process_id(snapshot_capture_target());
    }

    /**
     * @brief Whether the Windows backend can capture one launched process tree.
     *
     * @return True only when this build and OS support WASAPI process loopback.
     */
    bool supports_process_capture() const override {
      return process_loopback_supported();
    }

    /**
     * @brief Create a microphone capture stream for the requested layout.
     *
     * Positional override per the shared design; new callers use the
     * parameter-bundle overload unhidden above. Operates on mutex-guarded
     * snapshots of the capture target and assigned sink, so concurrent
     * sessions cannot change either mid-creation. The default-endpoint
     * callback reasserts the snapshot taken here, never the shared string.
     * Test builds may override the loopback activator via
     * set_test_loopback_activator(); production always uses the WASAPI activator.
     *
     * @param mapping Opus channel mapping table for the requested layout.
     * @param channels Number of audio channels in the stream.
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param continuous_audio Continuous audio.
     * @param host_audio_enabled Whether host playback should remain enabled during capture.
     * @return Microphone capture object for the requested audio layout.
     */
    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, bool host_audio_enabled) override {
      const microphone_params_t params {mapping, channels, sample_rate, frame_size, continuous_audio, host_audio_enabled};
      const auto capture_snapshot = snapshot_capture_target();
      const auto target_process_id = validated_snapshot_process_id(capture_snapshot);
      if (use_process_loopback(target_process_id, capture_snapshot.intent)) {
        auto process_mic = std::make_unique<mic_wasapi_t>();
        auto loopback_activator = process_loopback_activator_t {static_cast<audio_client_t (*)(std::uint32_t, const format_t &)>(&make_process_loopback_audio_client)};
#ifdef SUNSHINE_TESTS
        // Tests install a failing activator to exercise the loopback-attempt
        // and activation-failure fallback without audio hardware.
        if (test_loopback_activator) {
          loopback_activator = test_loopback_activator;
        }
#endif
        if (!process_mic->init_process_loopback(params, target_process_id, loopback_activator)) {
          // The PID may have exited or been reused while activation was
          // resolving it, so revalidate after activation and discard on
          // mismatch instead of capturing the wrong process.
          if (activated_process_target_is_current(target_process_id, validated_snapshot_process_id(capture_snapshot))) {
            BOOST_LOG(info) << "Capturing audio from process ["sv << target_process_id << "] via process loopback"sv;
            return process_mic;
          }
          BOOST_LOG(warning) << "Process ["sv << target_process_id << "] exited or its PID was reused during activation; falling back to endpoint capture"sv;
        } else {
          BOOST_LOG(warning) << "Process loopback capture failed for process ["sv << target_process_id << "]; falling back to endpoint capture"sv;
        }
      }

      auto mic = std::make_unique<mic_wasapi_t>();

      // An explicit sink pin stays pinned. With keep-default and no configured
      // sink, the assigned sink is only the host sink recorded at startup, so
      // capture must follow the Windows default instead of resolving that
      // snapshot after the startup endpoint disappears.
      const bool follow_default_device = should_follow_default_device();

      // Snapshot the assigned sink once: the default-endpoint callback installed
      // below runs on the capture thread and must reassert this copy, never a
      // shared string that another session may rewrite concurrently.
      const auto assigned_sink_snapshot = snapshot_assigned_sink();

      // Prefer the session's assigned sink; otherwise use the configured sink.
      const auto requested_sink = assigned_sink_snapshot.empty() ? config::audio.sink : assigned_sink_snapshot;

      // Capture the requested sink directly instead of relying on it being the default
      // render device, so that capture keeps working when the default device differs
      // from the sink or changes during the session.
      device_t capture_device;
      if (follow_default_device) {
        BOOST_LOG(info) << "Capturing audio from the Windows default sink"sv;
      } else if (!requested_sink.empty()) {
        capture_device = get_sink_device(requested_sink);
        if (!capture_device) {
          BOOST_LOG(error) << "Couldn't resolve audio sink ["sv << requested_sink << "] to a capture device"sv;
          return nullptr;
        }

        BOOST_LOG(info) << "Capturing audio from sink ["sv << requested_sink << ']';
      }

      if (mic->init(params, std::move(capture_device))) {
        return nullptr;
      }

      // If this is a virtual sink, set a callback that will change the sink back if it's changed.
      // Hands-off modes never reinstall: capture follows the recorded endpoint directly.
      if (should_install_default_callback(assigned_sink_snapshot)) {
        mic->default_endpt_changed_cb = [this, sink = assigned_sink_snapshot] {
          BOOST_LOG(info) << "Resetting sink to ["sv << sink << "] after default changed";
          set_sink(sink);
        };
      }

      return mic;
    }

    /**
     * If the requested sink is a virtual sink, meaning no speakers attached to
     * the host, then we can seamlessly set the format to stereo and surround sound.
     *
     * Any virtual sink detected will be prefixed by:
     *    virtual-(format name)
     * If it doesn't contain that prefix, then the format will not be changed
     * @param sink Audio sink name to route or capture.
     * @return Status from updating format.
     */
    std::optional<std::wstring> set_format(const std::string &sink) {
      if (sink.empty()) {
        return std::nullopt;
      }

      auto virtual_sink_info = extract_virtual_sink_info(sink);

      if (!virtual_sink_info) {
        // Sink name does not begin with virtual-(format name), hence it's not a virtual sink
        // and we don't want to change playback format of the corresponding device.
        // Also need to perform matching, sink name is not necessarily device_id in this case.
        auto matched = find_device_id(match_all_fields(utf_utils::from_utf8(sink)));
        if (matched) {
          return matched->second;
        } else {
          BOOST_LOG(error) << "Couldn't find audio sink " << sink;
          return std::nullopt;
        }
      }

      // When switching to a Steam virtual speaker device, try to retain the bit depth of the
      // default audio device. Switching from a 16-bit device to a 24-bit one has been known to
      // cause glitches for some users.
      int wanted_bits_per_sample = 32;
      auto current_default_dev = default_device(device_enum);
      if (current_default_dev) {
        audio::prop_t prop;
        prop_var_t current_device_format;

        if (SUCCEEDED(current_default_dev->OpenPropertyStore(STGM_READ, &prop)) && SUCCEEDED(prop->GetValue(PKEY_AudioEngine_DeviceFormat, &current_device_format.prop))) {
          auto *format = (WAVEFORMATEXTENSIBLE *) current_device_format.prop.blob.pBlobData;
          wanted_bits_per_sample = format->Samples.wValidBitsPerSample;
          BOOST_LOG(info) << "Virtual audio device will use "sv << wanted_bits_per_sample << "-bit to match default device"sv;
        }
      }

      auto &device_id = virtual_sink_info->first;
      auto &waveformats = virtual_sink_info->second.get().virtual_sink_waveformats;
      for (const auto &waveformat : waveformats) {
        // We're using completely undocumented and unlisted API,
        // better not pass objects without copying them first.
        auto device_id_copy = device_id;
        auto waveformat_copy = waveformat;
        auto waveformat_copy_pointer = reinterpret_cast<WAVEFORMATEX *>(&waveformat_copy);

        if (wanted_bits_per_sample != waveformat.Samples.wValidBitsPerSample) {
          continue;
        }

        WAVEFORMATEXTENSIBLE p {};
        if (SUCCEEDED(policy->SetDeviceFormat(device_id_copy.c_str(), waveformat_copy_pointer, (WAVEFORMATEX *) &p))) {
          BOOST_LOG(info) << "Changed virtual audio sink format to " << logging::bracket(waveformat_to_pretty_string(waveformat));
          return device_id;
        }
      }

      BOOST_LOG(error) << "Couldn't set virtual audio sink waveformat";
      return std::nullopt;
    }

    /**
     * @brief Update the sink value on the backend.
     *
     * Thread-safe: only the assigned-sink store is guarded; the endpoint
     * switch itself runs outside `capture_state_mutex_`. Safe to call from the
     * default-endpoint callback on the capture thread.
     *
     * @param sink Audio sink name to route or capture.
     * @return Status from updating sink.
     */
    int set_sink(const std::string &sink) override {
      if (config::audio.keep_default_sink) {
        // Keep-default never swaps the host default; capture follows the recorded endpoint.
        record_capture_sink(sink);
        return 0;
      }

      auto device_id = set_format(sink);
      if (!device_id) {
        return -1;
      }

      int failure {};
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        auto status = policy->SetDefaultEndpoint(device_id->c_str(), (ERole) x);
        if (status) {
          // Depending on the format of the string, we could get either of these errors
          if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == E_INVALIDARG) {
            BOOST_LOG(warning) << "Audio sink not found: "sv << sink;
          } else {
            BOOST_LOG(warning) << "Couldn't set ["sv << sink << "] to role ["sv << x << "]: 0x"sv << util::hex(status).to_string_view();
          }

          ++failure;
        }
      }

      // Remember the assigned sink name, so we have it for later if we need to set it
      // back after another application changes it
      if (!failure) {
        record_capture_sink(sink);
      }

      return failure;
    }

    /**
     * @brief Whether the backend can capture a sink without changing the host default.
     *
     * @return Always true: capture_sink() records the target for direct WASAPI capture.
     */
    bool supports_keep_default_sink() const override {
      return true;
    }

    /**
     * @brief Capture the given sink without changing the host default.
     *
     * Records the target for microphone() under the base capture mutex; unlike
     * set_sink(), this never calls SetDefaultEndpoint and never installs the
     * Steam drivers.
     *
     * @param sink Audio sink name to capture.
     * @return Always 0: recording a capture target cannot fail here.
     */
    int capture_sink(const std::string &sink) override {
      return record_keep_default_capture_sink(sink);
    }

    /**
     * @brief Whether endpoint capture should follow the Windows default device.
     *
     * Only the automatically captured host sink follows the default: an
     * explicit `audio_sink` pin stays pinned, and the swap path keeps
     * capturing its assigned (virtual) endpoint.
     *
     * @return True when keep-default is enabled with no configured sink.
     */
    [[nodiscard]] bool should_follow_default_device() const noexcept {
      return config::audio.keep_default_sink && config::audio.sink.empty();
    }

    /**
     * @brief Whether a default-endpoint change may reinstall the assigned sink.
     *
     * Keep-default sessions must never reassert the default: capture follows the
     * recorded endpoint directly, so default changes are simply observed.
     *
     * @param sink Assigned sink name that would be reinstalled.
     * @return True when the reassertion callback may be installed.
     */
    [[nodiscard]] bool should_install_default_callback(std::string_view sink) const {
      return static_cast<bool>(extract_virtual_sink_info(sink)) && !config::audio.keep_default_sink;
    }

    /**
     * @brief Whether startup may move the default away from Steam Streaming Speakers.
     *
     * @return False in hands-off modes that never touch the host default.
     */
    [[nodiscard]] bool should_reset_default_device() const noexcept {
      return !config::audio.keep_default_sink;
    }

    /**
     * @brief Whether the Steam Streaming Speakers driver may be installed.
     *
     * Installing a driver reconfigures the audio subsystem (device arrival makes
     * Windows re-evaluate defaults), which keep-default sessions must not trigger.
     *
     * @return False in hands-off modes that never touch the host audio graph.
     */
    [[nodiscard]] bool should_install_steam_drivers() const noexcept {
      return !config::audio.keep_default_sink && config::audio.install_steam_drivers;
    }

    /**
     * @brief Enumerates supported match field options.
     */
    enum class match_field_e {
      device_id,  ///< Match device_id
      device_friendly_name,  ///< Match endpoint friendly name
      adapter_friendly_name,  ///< Match adapter friendly name
      device_description,  ///< Match endpoint description
    };

    /**
     * @brief List of format fields used to compare audio formats.
     */
    using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
    /**
     * @brief One matched audio-format field and its expected value.
     */
    using matched_field_t = std::pair<match_field_e, std::wstring>;

    /**
     * @brief Build matching fields for Steam Streaming Speakers.
     *
     * @return Field list used to identify Steam's virtual speaker endpoint.
     */
    audio_control_t::match_fields_list_t match_steam_speakers() {
      return {
        {match_field_e::adapter_friendly_name, L"Steam Streaming Speakers"}
      };
    }

    /**
     * @brief Build matching fields that all contain the same endpoint name.
     *
     * @param name Endpoint name or identifier to match across all fields.
     * @return Field list requiring every supported endpoint field to match the name.
     */
    audio_control_t::match_fields_list_t match_all_fields(std::wstring_view name) {
      using enum match_field_e;
      return {
        {device_id, std::wstring {name}},  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        {device_friendly_name, std::wstring {name}},  // Digital Audio (S/PDIF) (High Definition Audio Device)
        {device_description, std::wstring {name}},  // Digital Audio (S/PDIF)
        {adapter_friendly_name, std::wstring {name}},  // High Definition Audio Device
      };
    }

    /**
     * @brief Identity and property store read from one enumerated endpoint.
     */
    struct endpoint_probe_t {
      std::wstring device_id;  ///< Device identifier string.
      prop_t prop;  ///< Opened property store for endpoint metadata.
    };

    /**
     * @brief Read one enumerated endpoint's identity and property store.
     *
     * Every Item(), GetId(), and OpenPropertyStore() HRESULT is checked; an
     * entry that cannot be read reports std::nullopt so the caller skips it
     * instead of risking a null device id or a null property store dereference.
     *
     * @param collection Enumerated active render endpoints.
     * @param index Endpoint index within the collection, used for diagnostics.
     * @return Probe data, or std::nullopt when the endpoint cannot be read.
     */
    static std::optional<endpoint_probe_t> probe_audio_endpoint(collection_t &collection, int index) {
      device_t device;
      auto status = collection->Item(index, &device);
      if (FAILED(status) || !device) {
        BOOST_LOG(warning) << "Skipping endpoint ["sv << index << "] that could not be read: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      wstring_t wstring_id;
      status = device->GetId(&wstring_id);
      if (FAILED(status) || !wstring_id.get()) {
        BOOST_LOG(warning) << "Skipping endpoint ["sv << index << "] without a readable device id: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      prop_t prop;
      status = device->OpenPropertyStore(STGM_READ, &prop);
      if (FAILED(status) || !prop) {
        BOOST_LOG(warning) << "Skipping endpoint ["sv << index << "] without a readable property store: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      return endpoint_probe_t {std::wstring {wstring_id.get()}, std::move(prop)};
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
     *
     * Entries that cannot be read are skipped: every Item(), GetId(), and
     * OpenPropertyStore() HRESULT is checked, and a failed entry continues to
     * the next device instead of risking a null device id or a null property
     * store dereference.
     *
     * @param match_list Pairs of match fields and values
     * @return Optional pair of matched field and device_id
     */
    std::optional<matched_field_t> find_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      UINT count = 0;
      status = collection->GetCount(&count);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't count endpoints: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        auto probe = probe_audio_endpoint(collection, x);
        if (!probe) {
          continue;
        }
        std::wstring device_id = std::move(probe->device_id);
        audio::prop_t prop = std::move(probe->prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
            // Note: no using-enum here; the enumerator names would shadow the
            // device_id/device_friendly_name/adapter_friendly_name locals below.
            switch (match_list[i].first) {
              case match_field_e::device_id:
                match_value = device_id.c_str();
                break;

              case match_field_e::device_friendly_name:
                match_value = device_friendly_name.prop.pwszVal;
                break;

              case match_field_e::adapter_friendly_name:
                match_value = adapter_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_description:
                match_value = device_desc.prop.pwszVal;
                break;
            }
            if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
              matched[i] = device_id;
            }
          }
        }
      }

      for (size_t i = 0; i < match_list.size(); i++) {
        if (!matched[i].empty()) {
          return matched_field_t(match_list[i].first, matched[i]);
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Resets the default audio device from Steam Streaming Speakers.
     */
    void reset_default_device() {
      if (!should_reset_default_device()) {
        return;
      }

      auto matched_steam = find_device_id(match_steam_speakers());
      if (!matched_steam) {
        return;
      }
      auto steam_device_id = matched_steam->second;

      {
        // Get the current default audio device (if present)
        auto current_default_dev = default_device(device_enum);
        if (!current_default_dev) {
          return;
        }

        audio::wstring_t current_default_id;
        current_default_dev->GetId(&current_default_id);

        // If Steam Streaming Speakers are already not default, we're done.
        if (steam_device_id != current_default_id.get()) {
          return;
        }
      }

      // Disable the Steam Streaming Speakers temporarily to allow the OS to pick a new default.
      auto hr = policy->SetEndpointVisibility(steam_device_id.c_str(), FALSE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to disable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // Get the newly selected default audio device
      auto new_default_dev = default_device(device_enum);

      // Enable the Steam Streaming Speakers again
      hr = policy->SetEndpointVisibility(steam_device_id.c_str(), TRUE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to enable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // If there's now no audio device, the Steam Streaming Speakers were the only device available.
      // There's no other device to set as the default, so just return.
      if (!new_default_dev) {
        return;
      }

      audio::wstring_t new_default_id;
      new_default_dev->GetId(&new_default_id);

      // Set the new default audio device
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        policy->SetDefaultEndpoint(new_default_id.get(), (ERole) x);
      }

      BOOST_LOG(info) << "Successfully reset default audio device"sv;
    }

    /**
     * @brief Installs the Steam Streaming Speakers driver, if present.
     * @return `true` if installation was successful.
     */
    bool install_steam_audio_drivers() {
#ifdef STEAM_DRIVER_SUBDIR
      // MinGW's libnewdev.a is missing DiInstallDriverW() even though the headers have it,
      // so we have to load it at runtime. It's Vista or later, so it will always be available.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(error) << "newdev.dll failed to load"sv;
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        BOOST_LOG(error) << "DiInstallDriverW() is missing"sv;
        return false;
      }

      // Get the current default audio device (if present)
      auto old_default_dev = default_device(device_enum);

      // Install the Steam Streaming Speakers driver
      std::array<WCHAR, MAX_PATH> driver_path = {};
      ExpandEnvironmentStringsW(STEAM_AUDIO_DRIVER_PATH, driver_path.data(), driver_path.size());
      if (fn_DiInstallDriverW(nullptr, driver_path.data(), 0, nullptr)) {
        BOOST_LOG(info) << "Successfully installed Steam Streaming Speakers"sv;

        // Wait for 5 seconds to allow the audio subsystem to reconfigure things before
        // modifying the default audio device or enumerating devices again.
        Sleep(5000);

        // If there was a previous default device, restore that original device as the
        // default output device just in case installing the new one changed it.
        if (old_default_dev) {
          audio::wstring_t old_default_id;
          old_default_dev->GetId(&old_default_id);

          for (int x = 0; x < (int) ERole_enum_count; ++x) {
            policy->SetDefaultEndpoint(old_default_id.get(), (ERole) x);
          }
        }

        return true;
      } else {
        auto err = GetLastError();
        switch (err) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Administrator privileges are required to install Steam Streaming Speakers"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Steam audio drivers not found. This is expected if you don't have Steam installed."sv;
            break;
          default:
            BOOST_LOG(warning) << "Failed to install Steam audio drivers: "sv << err;
            break;
        }

        return false;
      }
#else
      BOOST_LOG(warning) << "Unable to install Steam Streaming Speakers on unknown architecture"sv;
      return false;
#endif
    }

    /**
     * @brief Initialize endpoint enumeration and policy control when routing is managed by Sunshine.
     *
     * @tparam CreateInstance Callable type used for COM activation.
     * @param create_instance COM factory used to initialize the required interfaces.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    template<typename CreateInstance = decltype(&CoCreateInstance)>
    int init(const CreateInstance &create_instance = &CoCreateInstance) {
      if (!config::audio.keep_default_sink) {
        auto status = create_instance(
          CLSID_CPolicyConfigClient,
          nullptr,
          CLSCTX_ALL,
          IID_IPolicyConfig,
          (void **) &policy
        );

        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't create audio policy config: [0x"sv << util::hex(status).to_string_view() << ']';

          return -1;
        }
      }

      auto status = create_instance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      return 0;
    }

    /**
     * @brief Destroy the Windows audio control.
     */
    ~audio_control_t() override = default;

    policy_t policy;  ///< Windows policy configuration interface used to switch default audio devices.
    audio::device_enum_t device_enum;  ///< Device enumerator used to query and watch audio endpoints.

  private:
    /**
     * @brief Revalidate a capture-target snapshot immediately before or after activation.
     *
     * Operates only on the given snapshot plus a fresh process query, so
     * callers must not hold `capture_state_mutex_` while revalidation runs.
     *
     * @param snapshot Capture target snapshot to revalidate.
     * @return Snapshot PID when it still refers to the same live process, or 0 for endpoint capture.
     */
    static std::uint32_t validated_snapshot_process_id(const capture_target_snapshot_t &snapshot) {
      if (snapshot.process_id == 0) {
        return 0;
      }
      const auto current = query_process_identity(snapshot.process_id);
      const auto effective = effective_capture_process_id(snapshot.identity, current);
      if (effective == 0) {
        BOOST_LOG(warning) << "Capture target process ["sv << snapshot.process_id << "] exited or its PID was reused; falling back to endpoint capture"sv;
      }
      return effective;
    }

#ifdef SUNSHINE_TESTS
    process_loopback_activator_t test_loopback_activator;  ///< Test override for the loopback activator used by microphone(), empty in production.
#endif
    /**
     * @brief Reset the WASAPI creation-time identity paired with the recorded target.
     *
     * Invoked by the base `clear_recorded_capture_target_if_matches()` while holding
     * `capture_state_mutex_`, so the intent/PID/identity reset stays atomic under one
     * lock hold. Non-reentrant by design: plain struct reset only, no OS queries,
     * no base-helper calls.
     */
    void on_capture_target_cleared() override {
      capture_target_identity = {};
    }

    process_identity_t capture_target_identity {};  ///< Creation-time identity recorded with the PID, guarded by the base `capture_state_mutex_`, empty when unverifiable.
  };

#ifdef SUNSHINE_TESTS
  namespace tests {
    /**
     * @brief Build production virtual speaker formats without initializing audio hardware.
     *
     * @param channel_count Supported virtual speaker channel count.
     * @return Preferred waveformats, or an empty list for unsupported channel counts.
     */
    std::vector<WAVEFORMATEXTENSIBLE> virtual_sink_waveformats(WORD channel_count) {
      switch (channel_count) {
        case 2:
          return create_virtual_sink_waveformats<2>();
        case 6:
          return create_virtual_sink_waveformats<6>();
        case 8:
          return create_virtual_sink_waveformats<8>();
        default:
          return {};
      }
    }

    /**
     * @brief Exercise controller initialization with a supplied COM factory.
     * @param create_instance Factory providing or rejecting the requested interfaces.
     * @return Status from controller initialization.
     */
    int initialize_audio_control(const std::function<std::remove_pointer_t<decltype(&CoCreateInstance)>> &create_instance) {
      audio_control_t control;
      return control.init(create_instance);
    }

    /**
     * @brief Exercise the keep-default sink-change guard without policy interfaces.
     * @param sink Sink a caller attempts to select.
     * @return Status from the production sink setter.
     */
    int set_keep_default_sink(const std::string &sink) {
      audio_control_t control;
      control.reset_default_device();
      return control.set_sink(sink);
    }

    /**
     * @brief Exercise keep-default support detection without initializing Windows policy interfaces.
     * @return True when the backend captures without changing the host default.
     */
    bool keep_default_supported() {
      audio_control_t control;
      return control.supports_keep_default_sink();
    }

    /**
     * @brief Exercise capture-only sink recording without policy interfaces.
     * @param sink Sink a caller attempts to capture.
     * @return Sink recorded for direct capture, or empty when recording failed.
     */
    std::string capture_assigned_sink(const std::string &sink) {
      audio_control_t control;
      if (control.capture_sink(sink)) {
        return {};
      }

      return control.snapshot_assigned_sink();
    }

    /**
     * @brief Exercise the process-loopback predicate without audio hardware.
     * @param process_id Revalidated application PID, or 0 for endpoint capture.
     * @param intent Capture intent recorded for the next microphone() call.
     * @return True when process loopback should be attempted.
     */
    bool use_process_loopback(std::uint32_t process_id, audio_capture_intent_e intent) {
      return platf::audio::use_process_loopback(process_id, intent);
    }

  #ifdef SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS
    /**
     * @brief Build process-loopback activation parameters for a PID.
     * @param process_id Launched application PID to capture.
     * @return Activation parameters including the target process tree.
     */
    AUDIOCLIENT_ACTIVATION_PARAMS make_process_loopback_activation_params(std::uint32_t process_id) {
      return platf::audio::make_process_loopback_activation_params(process_id);
    }

    /**
     * @brief Build a heap-owned activation blob and verify it clears safely.
     *
     * Pins the regression-proof property of the production activation blob:
     * the payload is a heap copy (never the address of the stack params), and
     * PropVariantClear() releases it cleanly.
     *
     * @param process_id Launched application PID to capture.
     * @return True when the blob holds a heap copy of the params and clears cleanly.
     */
    bool activation_blob_holds_heap_copy(std::uint32_t process_id) {
      const auto params = platf::audio::make_process_loopback_activation_params(process_id);
      PROPVARIANT prop {};
      PropVariantInit(&prop);
      if (FAILED(platf::audio::init_process_loopback_activation_blob(prop, params))) {
        return false;
      }
      const bool holds_copy = prop.vt == VT_BLOB &&
                              prop.blob.cbSize == sizeof(params) &&
                              prop.blob.pBlobData != nullptr &&
                              prop.blob.pBlobData != reinterpret_cast<const BYTE *>(&params) &&
                              std::memcmp(prop.blob.pBlobData, &params, sizeof(params)) == 0;
      const auto clear_status = PropVariantClear(&prop);
      return holds_copy && SUCCEEDED(clear_status) && prop.vt == VT_EMPTY;
    }
  #endif

    /**
     * @brief Exercise process-loopback initialization with an activator that always fails.
     * @return True when initialization fails as expected (the fallback trigger).
     */
    bool process_loopback_fails_with_null_activator() {
      mic_wasapi_t mic;
      process_loopback_activator_t activator = [](std::uint32_t, const format_t &) -> audio_client_t {
        return nullptr;
      };
      const microphone_params_t params {nullptr, 2, 48000, 240, false, false};
      return mic.init_process_loopback(params, 1234, activator) != 0;
    }

    /**
     * @brief Fake IAudioClient with a controllable Initialize result for process-loopback wrapper tests.
     */
    class fake_process_loopback_audio_client_t final: public IAudioClient {
    public:
      /**
       * @brief Construct a fake client returning the given result from Initialize().
       * @param initialize_status HRESULT returned by Initialize().
       */
      explicit fake_process_loopback_audio_client_t(HRESULT initialize_status):
          initialize_status {initialize_status} {
        ++live_count;
      }

      /**
       * @brief Destroy the fake client, releasing its lifetime count.
       */
      ~fake_process_loopback_audio_client_t() {
        --live_count;
      }

      /**
       * @brief Query a supported COM interface.
       * @param riid Requested interface identifier.
       * @param object Receives the interface pointer.
       * @return S_OK when supported, E_NOINTERFACE otherwise.
       */
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {  // NOSONAR(cpp:S5008): IUnknown::QueryInterface requires a void double pointer in the COM ABI.
        if (!object) {
          return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IAudioClient)) {
          *object = static_cast<IAudioClient *>(this);
          AddRef();
          return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
      }

      /**
       * @brief Increment the reference count.
       * @return New reference count.
       */
      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count;
      }

      /**
       * @brief Decrement the reference count, deleting when it reaches zero.
       * @return New reference count.
       */
      ULONG STDMETHODCALLTYPE Release() override {
        auto count = --ref_count;
        if (count == 0) {
          delete this;
        }
        return count;
      }

      /**
       * @brief Simulate client initialization with the configured result.
       * @return Configured Initialize() result.
       */
      HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX *, LPCGUID) override {
        return initialize_status;
      }

      /**
       * @brief Report an empty buffer.
       * @param buffer_frames Receives the buffer size in frames.
       * @return S_OK after reporting zero frames.
       */
      HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32 *buffer_frames) override {
        if (buffer_frames) {
          *buffer_frames = 0;
        }
        return S_OK;
      }

      /**
       * @brief Report zero stream latency.
       * @param latency Receives the stream latency.
       * @return S_OK after reporting zero latency.
       */
      HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME *latency) override {
        if (latency) {
          *latency = 0;
        }
        return S_OK;
      }

      /**
       * @brief Report zero padding frames.
       * @param padding_frames Receives the padding size in frames.
       * @return S_OK after reporting zero frames.
       */
      HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32 *padding_frames) override {
        if (padding_frames) {
          *padding_frames = 0;
        }
        return S_OK;
      }

      /**
       * @brief Reject format support queries, which wrapper tests do not use.
       * @return E_NOTIMPL.
       */
      HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX *, WAVEFORMATEX **closest_match) override {
        if (closest_match) {
          *closest_match = nullptr;
        }
        return E_NOTIMPL;
      }

      /**
       * @brief Fail mix-format lookup so the tolerated failure path is exercised.
       * @param device_format Receives the device format on success.
       * @return E_FAIL to exercise the tolerated mix-format failure.
       */
      HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX **device_format) override {
        if (device_format) {
          *device_format = nullptr;
        }
        return E_FAIL;
      }

      /**
       * @brief Report zero device periods.
       * @param default_period Receives the default device period.
       * @param minimum_period Receives the minimum device period.
       * @return S_OK after reporting zero periods.
       */
      HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME *default_period, REFERENCE_TIME *minimum_period) override {
        if (default_period) {
          *default_period = 0;
        }
        if (minimum_period) {
          *minimum_period = 0;
        }
        return S_OK;
      }

      /**
       * @brief Simulate starting capture.
       * @return S_OK.
       */
      HRESULT STDMETHODCALLTYPE Start() override {
        return S_OK;
      }

      /**
       * @brief Simulate stopping capture.
       * @return S_OK.
       */
      HRESULT STDMETHODCALLTYPE Stop() override {
        return S_OK;
      }

      /**
       * @brief Simulate resetting capture.
       * @return S_OK.
       */
      HRESULT STDMETHODCALLTYPE Reset() override {
        return S_OK;
      }

      /**
       * @brief Accept an event handle without storing it.
       * @return S_OK.
       */
      HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override {
        return S_OK;
      }

      /**
       * @brief Reject service queries, which wrapper tests do not use.
       * @param object Receives the requested service.
       * @return E_NOTIMPL.
       */
      HRESULT STDMETHODCALLTYPE GetService(REFIID, void **object) override {  // NOSONAR(cpp:S5008): IUnknown::QueryInterface requires a void double pointer in the COM ABI.
        if (object) {
          *object = nullptr;
        }
        return E_NOTIMPL;
      }

      static inline std::atomic live_count {0};  ///< Live fake-client count for leak checks.

    private:
      HRESULT initialize_status;  ///< HRESULT returned by Initialize().
      std::atomic<ULONG> ref_count {1};  ///< COM reference count.
    };

  #if defined(SUNSHINE_HAS_AUDIOCLIENT_ACTIVATION_PARAMS) && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
    /**
     * @brief Fake async operation reporting successful activation of a supplied interface.
     */
    class fake_process_loopback_async_operation_t final: public IActivateAudioInterfaceAsyncOperation {
    public:
      /**
       * @brief Construct a fake operation serving the given interface.
       * @param served_interface Interface returned from GetActivateResult; the operation holds its own reference.
       */
      explicit fake_process_loopback_async_operation_t(IUnknown *served_interface):
          served_interface {served_interface} {
        if (served_interface) {
          served_interface->AddRef();
        }
      }

      /**
       * @brief Destroy the operation, releasing the served interface.
       */
      ~fake_process_loopback_async_operation_t() {
        if (served_interface) {
          served_interface->Release();
        }
      }

      /**
       * @brief Query a supported COM interface.
       * @param riid Requested interface identifier.
       * @param object Receives the interface pointer.
       * @return S_OK when supported, E_NOINTERFACE otherwise.
       */
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {  // NOSONAR(cpp:S5008): IUnknown::QueryInterface requires a void double pointer in the COM ABI.
        if (!object) {
          return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceAsyncOperation)) {
          *object = static_cast<IActivateAudioInterfaceAsyncOperation *>(this);
          AddRef();
          return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
      }

      /**
       * @brief Increment the reference count.
       * @return New reference count.
       */
      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count;
      }

      /**
       * @brief Decrement the reference count, deleting when it reaches zero.
       * @return New reference count.
       */
      ULONG STDMETHODCALLTYPE Release() override {
        auto count = --ref_count;
        if (count == 0) {
          delete this;
        }
        return count;
      }

      /**
       * @brief Report successful activation of the served interface.
       * @param activation_hresult Receives the activation HRESULT.
       * @param activated_interface Receives the served interface.
       * @return S_OK.
       */
      HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT *activation_hresult, IUnknown **activated_interface) override {
        if (!activation_hresult || !activated_interface) {
          return E_POINTER;
        }
        *activation_hresult = S_OK;
        *activated_interface = served_interface;
        if (served_interface) {
          served_interface->AddRef();
        }
        return S_OK;
      }

    private:
      IUnknown *served_interface {};  ///< Served interface held with an owning reference, or nullptr.
      std::atomic<ULONG> ref_count {1};  ///< COM reference count.
    };

    /**
     * @brief Verify timeout, failure, and success activation paths discard and deliver safely.
     *
     * Drives the real completion handler through the production wait-and-collect path
     * in phases: a null completion arriving strictly after a short timeout must be
     * discarded without delivering a client, a late success completion must likewise be
     * discarded without leaking its client, an invalid wait handle must report no
     * client, a pre-signaled failure must report no client, a pre-signaled success
     * must deliver its client, and the same success through the default timeout
     * argument must deliver it the way production calls it. The timeout phases hold
     * the worker behind a gate event
     * so the completion provably starts only after the wait expired, and hold an extra
     * handler reference while waiting to model the reference Windows retains until the
     * callback runs, so the late callback always sees a live handler. A final overlap
     * loop hammers the race to pin crash-freedom and lifetime balance.
     *
     * The stress loop pins crash-freedom and lifetime balance only; it does not prove
     * overlap occurred. Data-race freedom is unverified: the msys2/ucrt64 MinGW
     * toolchain has no ThreadSanitizer support.
     *
     * @return 0 when every phase passes; otherwise the failing-phase id. Setup failures
     * (a missing completion event or gate handle) reuse the id of the phase they belong to.
     */
    int process_loopback_completion_after_timeout_is_discarded() {
      constexpr int kPhasePassed {0};  ///< All phases passed.
      constexpr int kPhaseTimeoutOrdering {1};  ///< Timeout setup failed, or the wait did not expire before the late null completion started.
      constexpr int kPhaseTimeoutDiscard {2};  ///< Expired wait delivered a client for the late null completion.
      constexpr int kPhaseLateSuccess {3};  ///< Late-success setup failed, the wait did not expire first, or the expired wait delivered a client.
      constexpr int kPhaseWaitFailed {4};  ///< Invalid-handle wait delivered a client.
      constexpr int kPhaseSignaledFailure {5};  ///< Pre-signaled failure setup failed or delivered a client.
      constexpr int kPhaseSignaledSuccess {6};  ///< Pre-signaled success setup failed or delivered no client.
      constexpr int kPhaseStress {7};  ///< Overlap stress iteration setup failed.
      constexpr int kPhaseLifetime {8};  ///< A fake client leaked across the late-success phase or the stress loop.
      constexpr int kPhaseDefaultTimeout {9};  ///< Pre-signaled success through the default timeout delivered no client.
      constexpr DWORD kShortTimeoutMs {10};  ///< Timeout that expires while the gated completion is still held.
      constexpr DWORD kSignaledWaitMs {1000};  ///< Wait allowing pre-signaled completions to report.
      constexpr DWORD kGateWaitMs {5000};  ///< Bound on the gated worker wait so a stuck gate fails instead of hanging.
      {
        auto *handler = new platf::audio::process_loopback_completion_handler_t();
        if (!handler->event_handle) {
          handler->Release();
          return kPhaseTimeoutOrdering;
        }
        handler->AddRef();  // Model the reference Windows holds until the callback runs.
        HANDLE gate = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!gate) {
          handler->Release();
          handler->Release();
          return kPhaseTimeoutOrdering;
        }
        std::atomic<bool> completion_started {false};
        std::thread late_completion([handler, gate, &completion_started] {
          WaitForSingleObject(gate, kGateWaitMs);
          completion_started.store(true, std::memory_order_release);
          handler->ActivateCompleted(nullptr);
          handler->Release();
        });
        bool wait_timed_out = false;
        auto client = platf::audio::collect_process_loopback_activation(handler, 1234, kShortTimeoutMs, &wait_timed_out);
        const bool expired_before_completion = !completion_started.load(std::memory_order_acquire);
        SetEvent(gate);
        late_completion.join();
        CloseHandle(gate);
        if (wait_timed_out) {
          // Model the production detached waiter releasing the retained
          // caller reference once the late completion has run.
          handler->Release();
        }
        if (!expired_before_completion || !wait_timed_out) {
          return kPhaseTimeoutOrdering;
        }
        if (client) {
          return kPhaseTimeoutDiscard;
        }
      }
      {
        const int live_baseline = fake_process_loopback_audio_client_t::live_count.load();
        {
          auto *served_client = new fake_process_loopback_audio_client_t(S_OK);
          fake_process_loopback_async_operation_t operation(served_client);
          served_client->Release();
          auto *handler = new platf::audio::process_loopback_completion_handler_t();
          if (!handler->event_handle) {
            handler->Release();
            return kPhaseLateSuccess;
          }
          handler->AddRef();  // Model the reference Windows holds until the callback runs.
          HANDLE gate = CreateEventA(nullptr, TRUE, FALSE, nullptr);
          if (!gate) {
            handler->Release();
            handler->Release();
            return kPhaseLateSuccess;
          }
          std::atomic<bool> completion_started {false};
          std::thread late_completion([handler, gate, &completion_started, &operation] {
            WaitForSingleObject(gate, kGateWaitMs);
            completion_started.store(true, std::memory_order_release);
            handler->ActivateCompleted(&operation);
            handler->Release();
          });
          bool wait_timed_out = false;
          auto client = platf::audio::collect_process_loopback_activation(handler, 1234, kShortTimeoutMs, &wait_timed_out);
          const bool expired_before_completion = !completion_started.load(std::memory_order_acquire);
          SetEvent(gate);
          late_completion.join();
          CloseHandle(gate);
          if (wait_timed_out) {
            handler->Release();
          }
          if (!expired_before_completion || !wait_timed_out || client) {
            return kPhaseLateSuccess;
          }
        }
        if (fake_process_loopback_audio_client_t::live_count.load() != live_baseline) {
          return kPhaseLifetime;
        }
      }
      {
        auto *handler = new platf::audio::process_loopback_completion_handler_t();
        if (!handler->event_handle) {
          handler->Release();
          return kPhaseWaitFailed;
        }
        // Production never passes an invalid handle (activate_process_loopback_audio_client checks
        // creation), so this pins the shared non-signaled wait branch rather than a production path.
        CloseHandle(handler->event_handle);
        handler->event_handle = nullptr;
        auto client = platf::audio::collect_process_loopback_activation(handler, 1234, kSignaledWaitMs);
        if (client) {
          return kPhaseWaitFailed;
        }
      }
      {
        auto *handler = new platf::audio::process_loopback_completion_handler_t();
        if (!handler->event_handle) {
          handler->Release();
          return kPhaseSignaledFailure;
        }
        handler->ActivateCompleted(nullptr);
        auto client = platf::audio::collect_process_loopback_activation(handler, 1234, kSignaledWaitMs);
        if (client) {
          return kPhaseSignaledFailure;
        }
      }
      {
        auto *served_client = new fake_process_loopback_audio_client_t(S_OK);
        fake_process_loopback_async_operation_t operation(served_client);
        served_client->Release();
        auto *handler = new platf::audio::process_loopback_completion_handler_t();
        if (!handler->event_handle) {
          handler->Release();
          return kPhaseSignaledSuccess;
        }
        handler->ActivateCompleted(&operation);
        auto client = platf::audio::collect_process_loopback_activation(handler, 1234, kSignaledWaitMs);
        if (!client) {
          return kPhaseSignaledSuccess;
        }
      }
      {
        // Same success shape production uses: default timeout argument with a pre-signaled event.
        auto *served_client = new fake_process_loopback_audio_client_t(S_OK);
        fake_process_loopback_async_operation_t operation(served_client);
        served_client->Release();
        auto *handler = new platf::audio::process_loopback_completion_handler_t();
        if (!handler->event_handle) {
          handler->Release();
          return kPhaseDefaultTimeout;
        }
        handler->ActivateCompleted(&operation);
        auto client = platf::audio::collect_process_loopback_activation(handler, 1234);
        if (!client) {
          return kPhaseDefaultTimeout;
        }
      }
      {
        const int live_baseline = fake_process_loopback_audio_client_t::live_count.load();
        for (int i = 0; i < 25; ++i) {
          auto *served_client = new fake_process_loopback_audio_client_t(S_OK);
          fake_process_loopback_async_operation_t operation(served_client);
          served_client->Release();
          auto *handler = new platf::audio::process_loopback_completion_handler_t();
          if (!handler->event_handle) {
            handler->Release();
            return kPhaseStress;
          }
          handler->AddRef();  // Model the reference Windows holds until the callback runs.
          std::thread racer([handler, &operation, i] {
            if (i % 2 != 0) {
              ::Sleep(1);
            }
            if (i % 2 == 0) {
              handler->ActivateCompleted(&operation);
            } else {
              handler->ActivateCompleted(nullptr);
            }
            handler->Release();
          });
          bool wait_timed_out = false;
          auto client = platf::audio::collect_process_loopback_activation(handler, 1234, (i % 3 == 0) ? 0 : 1, &wait_timed_out);
          (void) client;  // Either outcome is legal under the race; only crash-freedom and lifetime are pinned.
          racer.join();
          if (wait_timed_out) {
            handler->Release();
          }
        }
        if (fake_process_loopback_audio_client_t::live_count.load() != live_baseline) {
          return kPhaseLifetime;
        }
      }
      return kPhasePassed;
    }
  #endif

    /**
     * @brief Exercise the process-loopback wrapper with a fake activation that succeeds.
     * @return True when the wrapper returns an initialized client.
     */
    bool process_loopback_wrapper_succeeds_with_fake_activation() {
      process_loopback_activation_t activation = [](std::uint32_t) {
        audio_client_t client;
        client.reset(new fake_process_loopback_audio_client_t(S_OK));
        return client;
      };
      auto client = make_process_loopback_audio_client(1234, formats[0], activation);
      return static_cast<bool>(client);
    }

    /**
     * @brief Exercise the process-loopback wrapper with an activation that returns no client.
     * @return True when the wrapper returns no client.
     */
    bool process_loopback_wrapper_fails_with_null_activation() {
      process_loopback_activation_t activation = [](std::uint32_t) -> audio_client_t {
        return nullptr;
      };
      return !make_process_loopback_audio_client(1234, formats[0], activation);
    }

    /**
     * @brief Exercise the process-loopback wrapper with no activation step installed.
     * @return True when the wrapper returns no client.
     */
    bool process_loopback_wrapper_fails_with_empty_activation() {
      process_loopback_activation_t activation;
      return !make_process_loopback_audio_client(1234, formats[0], activation);
    }

    /**
     * @brief Exercise the process-loopback wrapper when client initialization fails.
     * @return True when the wrapper returns no client.
     */
    bool process_loopback_wrapper_fails_when_initialization_fails() {
      process_loopback_activation_t activation = [](std::uint32_t) {
        audio_client_t client;
        client.reset(new fake_process_loopback_audio_client_t(E_FAIL));
        return client;
      };
      return !make_process_loopback_audio_client(1234, formats[0], activation);
    }

    /**
     * @brief Record a capture PID through the production setter.
     * @param process_id PID to record.
     * @param creation_time Creation marker to record with the PID.
     * @return PID stored on the control.
     */
    std::uint32_t recorded_capture_process_id(std::uint32_t process_id, std::uint64_t creation_time) {
      audio_control_t control;
      control.set_capture_target(audio_capture_intent_e::host_and_client, process_identity_t {process_id, creation_time});
      return control.snapshot_capture_target().process_id;
    }

    /**
     * @brief Record a capture identity through the production setter.
     *
     * The baseline is stored verbatim without a PID query, so even a
     * fabricated identity for a nonexistent PID round-trips unchanged; only
     * validation resolves it to endpoint capture.
     *
     * @param process_id PID to record.
     * @param creation_time Creation marker to record with the PID.
     * @return Creation marker stored on the control.
     */
    std::uint64_t recorded_capture_creation_time(std::uint32_t process_id, std::uint64_t creation_time) {
      audio_control_t control;
      control.set_capture_target(audio_capture_intent_e::host_and_client, process_identity_t {process_id, creation_time});
      return control.snapshot_capture_target().identity.creation_time;
    }

    /**
     * @brief Exercise the pure process-identity comparison with synthetic identities.
     * @param recorded_process_id PID recorded with the capture target.
     * @param recorded_creation_time Creation time recorded with the capture target.
     * @param current_process_id PID queried before activation.
     * @param current_creation_time Creation time queried before activation.
     * @return True when the current identity is the recorded process.
     */
    bool process_identity_matches(std::uint32_t recorded_process_id, std::uint64_t recorded_creation_time, std::uint32_t current_process_id, std::uint64_t current_creation_time) {
      return platf::audio::process_identity_matches(
        process_identity_t {recorded_process_id, recorded_creation_time},
        process_identity_t {current_process_id, current_creation_time}
      );
    }

    /**
     * @brief Exercise the pure effective-PID resolution with synthetic identities.
     * @param recorded_process_id PID recorded with the capture target.
     * @param recorded_creation_time Creation time recorded with the capture target.
     * @param current_available Whether a current identity could be queried at all.
     * @param current_process_id PID queried before activation.
     * @param current_creation_time Creation time queried before activation.
     * @return Recorded PID when still valid, or 0 for endpoint capture.
     */
    std::uint32_t effective_capture_process_id(std::uint32_t recorded_process_id, std::uint64_t recorded_creation_time, bool current_available, std::uint32_t current_process_id, std::uint64_t current_creation_time) {
      std::optional<process_identity_t> current;
      if (current_available) {
        current = process_identity_t {current_process_id, current_creation_time};
      }
      return platf::audio::effective_capture_process_id(
        process_identity_t {recorded_process_id, recorded_creation_time},
        current
      );
    }

    /**
     * @brief Exercise recorded-target validation through the production control.
     * @param process_id PID to record and immediately revalidate.
     * @param creation_time Creation marker recorded as the validation baseline.
     * @return PID accepted for process loopback, or 0 for endpoint capture.
     */
    std::uint32_t validated_capture_target_for_pid(std::uint32_t process_id, std::uint64_t creation_time) {
      audio_control_t control;
      control.set_capture_target(audio_capture_intent_e::host_and_client, process_identity_t {process_id, creation_time});
      return control.validated_capture_process_id();
    }

    /**
     * @brief Exercise the post-activation discard decision with synthetic PIDs.
     * @param activated_process_id PID the completed activation was started for.
     * @param revalidated_process_id PID revalidation still accepts, or 0 when the target exited or was reused.
     * @return True when revalidation still accepts the activated PID.
     */
    bool activated_process_target_is_current(std::uint32_t activated_process_id, std::uint32_t revalidated_process_id) {
      return platf::audio::activated_process_target_is_current(activated_process_id, revalidated_process_id);
    }

    /**
     * @brief Set a capture target then conditionally clear it, reporting the stored intent.
     *
     * @param set_intent Intent recorded by set_capture_target().
     * @param set_pid PID recorded by set_capture_target().
     * @param clear_intent Intent passed to clear_capture_target().
     * @param clear_pid PID passed to clear_capture_target().
     * @return Intent stored on the control after the conditional clear.
     */
    audio_capture_intent_e intent_after_conditional_clear(audio_capture_intent_e set_intent, std::uint32_t set_pid, audio_capture_intent_e clear_intent, std::uint32_t clear_pid) {
      audio_control_t control;
      // The clear contract matches on intent and PID only; the marker is unverifiable here by design.
      control.set_capture_target(set_intent, process_identity_t {set_pid, 0});
      control.clear_capture_target(clear_intent, clear_pid);
      return control.snapshot_capture_target().intent;
    }

    /**
     * @brief Set a capture target then conditionally clear it, reporting the stored PID.
     *
     * @param set_intent Intent recorded by set_capture_target().
     * @param set_pid PID recorded by set_capture_target().
     * @param clear_intent Intent passed to clear_capture_target().
     * @param clear_pid PID passed to clear_capture_target().
     * @return PID stored on the control after the conditional clear.
     */
    std::uint32_t pid_after_conditional_clear(audio_capture_intent_e set_intent, std::uint32_t set_pid, audio_capture_intent_e clear_intent, std::uint32_t clear_pid) {
      audio_control_t control;
      // The clear contract matches on intent and PID only; the marker is unverifiable here by design.
      control.set_capture_target(set_intent, process_identity_t {set_pid, 0});
      control.clear_capture_target(clear_intent, clear_pid);
      return control.snapshot_capture_target().process_id;
    }

    /**
     * @brief Record this process as the capture target, conditionally clear it, and report the stored identity.
     *
     * The current process is the deterministic verifiable target: its PID and
     * creation time are read directly from the owned process handle, so the
     * recorded identity is nonzero until `on_capture_target_cleared()` runs.
     *
     * @param matching_clear Whether the clear uses the recorded intent and PID.
     * @return Identity creation time stored on the control after the clear.
     */
    std::uint64_t identity_creation_time_after_conditional_clear(bool matching_clear) {
      audio_control_t control;
      const auto process_id = static_cast<std::uint32_t>(GetCurrentProcessId());
      FILETIME creation_time {};
      FILETIME exit_time {};
      FILETIME kernel_time {};
      FILETIME user_time {};
      std::uint64_t combined_creation_time = 0;
      if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        combined_creation_time = (static_cast<std::uint64_t>(creation_time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(creation_time.dwLowDateTime);
      }
      control.set_capture_target(audio_capture_intent_e::host_and_client, process_identity_t {process_id, combined_creation_time});
      control.clear_capture_target(audio_capture_intent_e::host_and_client, matching_clear ? process_id : process_id + 1);
      return control.snapshot_capture_target().identity.creation_time;
    }

    /**
     * @brief Verify an assigned-sink snapshot stays stable while the shared value changes.
     *
     * The default-endpoint callback reasserts the snapshot taken at install
     * time; this proves later writes cannot mutate a previously taken copy.
     *
     * @return True when the first snapshot keeps the old sink after a rewrite.
     */
    bool assigned_sink_snapshot_is_stable() {
      audio_control_t control;
      control.capture_sink("sink-a");
      const auto snapshot = control.snapshot_assigned_sink();
      control.capture_sink("sink-b");
      return snapshot == "sink-a" && control.snapshot_assigned_sink() == "sink-b";
    }

    /**
     * @brief Hammer the guarded control state from multiple threads.
     *
     * Exercises capture-target set/clear/snapshot together with assigned-sink
     * writes and snapshots. Every snapshot must read one of the written
     * values; completing without a data race is the regression pin.
     *
     * @return True when every snapshot read a consistent value.
     */
    bool concurrent_control_access_is_consistent() {
      using enum audio_capture_intent_e;
      audio_control_t control;
      std::atomic consistent {true};
      const auto worker = [&](std::uint32_t pid) {
        for (int i = 0; i < 50 && consistent.load(std::memory_order_relaxed); ++i) {
          control.capture_sink(std::format("sink-{}", pid));
          if (control.snapshot_assigned_sink().compare(0, 5, "sink-") != 0) {
            consistent.store(false, std::memory_order_relaxed);
          }
          control.set_capture_target(host_and_client, process_identity_t {pid, pid});
          const auto target = control.snapshot_capture_target();
          const bool valid_target = (target.intent == host_and_client && target.process_id >= 1 && target.process_id <= 4) ||
                                    (target.intent == client_only && target.process_id == 0);
          if (!valid_target) {
            consistent.store(false, std::memory_order_relaxed);
          }
          control.clear_capture_target(host_and_client, pid);
        }
      };
      {
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (std::uint32_t pid = 1; pid <= 4; ++pid) {
          threads.emplace_back(worker, pid);
        }
      }
      return consistent.load(std::memory_order_relaxed);
    }

    /**
     * @brief Exercise the default-callback guard with a controlled keep-default mode.
     * @param sink Assigned sink name that would be reinstalled.
     * @param keep_default keep_default_sink value to test with.
     * @return True when the production guard installs the reassertion callback.
     */
    bool installs_default_callback(const std::string &sink, bool keep_default) {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, keep_default);
      bool installed = control.should_install_default_callback(sink);
      config::audio.keep_default_sink = previous_keep;
      return installed;
    }

    /**
     * @brief Exercise the startup-reset guard with a controlled keep-default mode.
     * @param keep_default keep_default_sink value to test with.
     * @return True when the production guard permits resetting the default device.
     */
    bool resets_default_device(bool keep_default) {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, keep_default);
      bool resets = control.should_reset_default_device();
      config::audio.keep_default_sink = previous_keep;
      return resets;
    }

    /**
     * @brief Exercise the driver-install guard with a controlled keep-default mode.
     * @param keep_default keep_default_sink value to test with.
     * @param install_requested install_steam_drivers value to test with.
     * @return True when the production guard permits installing the Steam drivers.
     */
    bool installs_steam_drivers(bool keep_default, bool install_requested) {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, keep_default);
      auto previous_install = std::exchange(config::audio.install_steam_drivers, install_requested);
      bool installs = control.should_install_steam_drivers();
      config::audio.keep_default_sink = previous_keep;
      config::audio.install_steam_drivers = previous_install;
      return installs;
    }

    /**
     * @brief Exercise the startup reset with keep-default enabled and null policy interfaces.
     *
     * Surviving this call proves the guard returns before touching policy or enumerator.
     */
    void reset_with_keep_default() {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, true);
      control.reset_default_device();
      config::audio.keep_default_sink = previous_keep;
    }

    /**
     * @brief Resolve a sink through the production Windows endpoint lookup.
     *
     * @param sink Sink name, virtual sink descriptor, or device identifier.
     * @param device_enum Device enumerator supplied by the test.
     * @return `true` when the sink resolves to an active endpoint.
     */
    bool sink_device_available(const std::string &sink, IMMDeviceEnumerator *device_enum) {
      audio_control_t control;
      device_enum->AddRef();
      control.device_enum.reset(device_enum);
      return static_cast<bool>(control.get_sink_device(sink));
    }

    /**
     * @brief Run endpoint lookup against a test-supplied enumerator, reporting match success.
     *
     * Exercises the production find_device_id() path, including its
     * skip-on-failure handling for unreadable entries, without audio hardware.
     *
     * @param match_name Endpoint name to match across all supported fields.
     * @param device_enum Device enumerator supplied by the test.
     * @return True when the endpoint resolves to an active device.
     */
    bool find_device_matches(const std::wstring &match_name, IMMDeviceEnumerator *device_enum) {
      audio_control_t control;
      device_enum->AddRef();
      control.device_enum.reset(device_enum);
      return static_cast<bool>(control.find_device_id(control.match_all_fields(match_name)));
    }

    /**
     * @brief Report host and virtual sinks through the production discovery path.
     *
     * @param device_enum Device enumerator supplied by the test.
     * @return Sinks reported for the current configuration, when any.
     */
    std::optional<sink_t> reported_sinks(IMMDeviceEnumerator *device_enum) {
      audio_control_t control;
      device_enum->AddRef();
      control.device_enum.reset(device_enum);
      return control.sink_info();
    }

    /**
     * @brief Exercise microphone creation with controlled assigned and configured sinks.
     *
     * Calls through the parameter-bundle overload, so the shared params shape
     * is the exercised path on this backend.
     *
     * @param assigned_sink Sink selected by the shared audio context.
     * @param configured_sink Sink configured by the user.
     * @param device_enum Device enumerator supplied by the test.
     * @return `true` when microphone initialization succeeds.
     */
    bool microphone_available(const std::string &assigned_sink, const std::string &configured_sink, IMMDeviceEnumerator *device_enum) {
      audio_control_t control;
      device_enum->AddRef();
      control.device_enum.reset(device_enum);
      control.capture_sink(assigned_sink);

      const microphone_params_t params {nullptr, 2, 48000, 240, false, false};
      auto previous_configured_sink = std::exchange(config::audio.sink, configured_sink);
      auto microphone = control.microphone(params);
      config::audio.sink = std::move(previous_configured_sink);
      return static_cast<bool>(microphone);
    }

    /**
     * @brief Exercise microphone() end to end with a recorded capture target and a failing test activator.
     *
     * Records the given capture target through the production setter, installs
     * a counting activator that returns no client, and runs the production
     * microphone() via the parameter-bundle overload. A valid target proves
     * the loopback-attempt branch ran when the activator was invoked; the
     * failing activator then proves the activation-failure fallback by
     * resolving the endpoint path afterwards. The current process identity is
     * the deterministic valid target: its PID and creation time are read from
     * the owned handle, while PID 0, an unverifiable marker, a mismatched
     * marker, or a mismatched intent must never attempt loopback.
     *
     * @param probe Capture probe: sinks, intent, identity, enumerator, and keep-default flag in; attempt evidence out.
     * @return True when process-loopback activation was attempted.
     */
    bool microphone_attempts_loopback_for_target(microphone_loopback_probe_t &probe) {
      audio_control_t control;
      probe.device_enum->AddRef();
      control.device_enum.reset(probe.device_enum);
      control.capture_sink(probe.assigned_sink);
      control.set_capture_target(probe.intent, process_identity_t {probe.process_id, probe.creation_time});
      control.set_test_loopback_activator([&probe](std::uint32_t attempted_pid, const format_t &) -> audio_client_t {
        probe.attempted_process_id = attempted_pid;
        return nullptr;
      });

      probe.attempted_process_id = 0;
      const microphone_params_t params {nullptr, 2, 48000, 240, false, false};
      auto previous_configured_sink = std::exchange(config::audio.sink, probe.configured_sink);
      auto previous_keep_default = std::exchange(config::audio.keep_default_sink, probe.keep_default);
      auto microphone = control.microphone(params);
      config::audio.sink = std::move(previous_configured_sink);
      config::audio.keep_default_sink = previous_keep_default;
      probe.microphone_returned = static_cast<bool>(microphone);
      return probe.attempted_process_id != 0;
    }

    /**
     * @brief Report whether endpoint capture follows the Windows default device.
     *
     * Exercises the production should_follow_default_device() decision without
     * requiring microphone initialization (which builds its own enumerator and
     * needs audio hardware), so the blank keep-default follow behavior is
     * pinned directly instead of via untouched-enumerator call counts.
     *
     * @param configured_sink Configured audio sink tested with, possibly empty.
     * @param keep_default keep_default_sink value tested with.
     * @return True when endpoint capture follows the default endpoint.
     */
    bool microphone_follows_default_device(const std::string &configured_sink, bool keep_default) {
      audio_control_t control;
      auto previous_configured_sink = std::exchange(config::audio.sink, configured_sink);
      auto previous_keep_default = std::exchange(config::audio.keep_default_sink, keep_default);
      const bool follows = control.should_follow_default_device();
      config::audio.sink = std::move(previous_configured_sink);
      config::audio.keep_default_sink = previous_keep_default;
      return follows;
    }

    /**
     * @brief Select a default or explicit capture endpoint through the production selection path.
     *
     * @param device_enum Device enumerator supplied by the test.
     * @param capture_device Explicit endpoint, or `nullptr` to select the default endpoint.
     * @return `true` when capture follows the default endpoint.
     */
    bool capture_follows_default_device(IMMDeviceEnumerator *device_enum, IMMDevice *capture_device) {
      mic_wasapi_t microphone;
      device_enum->AddRef();
      microphone.device_enum.reset(device_enum);

      device_t selected_device;
      if (capture_device) {
        capture_device->AddRef();
        selected_device.reset(capture_device);
      }

      microphone.select_capture_device(std::move(selected_device));
      return microphone.follows_default_device;
    }

    /**
     * @brief Exercise the production default-device-change path without live audio hardware.
     *
     * @param follows_default_device Whether the capture follows the default render endpoint.
     * @param install_callback Whether to install a default-device-change callback.
     * @param render_device_changed Whether to signal a render rather than capture endpoint change.
     * @param callback_count Receives the number of callback invocations.
     * @return Capture result produced after processing the notification.
     */
    capture_e simulate_default_device_change(bool follows_default_device, bool install_callback, bool render_device_changed, int &callback_count) {
      mic_wasapi_t mic;
      mic.audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      mic.default_latency_ms = 0;
      mic.sample_buf = util::buffer_t<float> {1};
      mic.sample_buf_pos = std::begin(mic.sample_buf);
      mic.continuous_audio = false;
      mic.follows_default_device = follows_default_device;

      if (install_callback) {
        mic.default_endpt_changed_cb = [&callback_count] {
          ++callback_count;
        };
      }

      mic.endpt_notification.OnDefaultDeviceChanged(
        render_device_changed ? eRender : eCapture,
        eConsole,
        nullptr
      );

      std::vector<float> sample(1);
      return mic.sample(sample);
    }
  }  // namespace tests
#endif
}  // namespace platf::audio

namespace platf {

  // It's not big enough to justify it's own source file :/
  namespace dxgi {
    /**
     * @brief Initialize the Windows audio-control backend.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init();
  }  // namespace dxgi

  std::unique_ptr<audio_control_t> audio_control() {
    auto control = std::make_unique<audio::audio_control_t>();

    if (control->init()) {
      return nullptr;
    }

    // Install Steam Streaming Speakers if needed. We do this during audio_control() to ensure
    // the sink information returned includes the new Steam Streaming Speakers device.
    // Hands-off modes skip the install: driver installation reconfigures the audio subsystem.
    if (control->should_install_steam_drivers() && !control->find_device_id(control->match_steam_speakers())) {
      // This is best effort. Don't fail if it doesn't work.
      control->install_steam_audio_drivers();
    }

    return control;
  }

  std::unique_ptr<deinit_t> init() {
    if (dxgi::init()) {
      return nullptr;
    }

    // Initialize COM
    auto co_init = std::make_unique<platf::audio::co_init_t>();

    // If Steam Streaming Speakers are currently the default audio device,
    // change the default to something else (if another device is available).
    audio::audio_control_t audio_ctrl;
    if (audio_ctrl.init() == 0) {
      audio_ctrl.reset_default_device();
    }

    return co_init;
  }
}  // namespace platf
