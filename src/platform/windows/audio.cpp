/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// standard includes
#include <array>
#include <format>
#include <functional>
#include <type_traits>
#include <utility>

// platform includes
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <roapi.h>
#include <synchapi.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "utf_utils.h"

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

    WAVEFORMATEXTENSIBLE capture_waveformat =
      create_waveformat(sample_format_e::f32, format.channel_count, format.capture_waveformat_channel_mask);

    {
      wave_format_t mixer_waveformat;
      status = audio_client->GetMixFormat(&mixer_waveformat);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't get mix format for audio device: [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      // Prefer the native channel layout of captured audio device when channel counts match
      if (mixer_waveformat->nChannels == format.channel_count && mixer_waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixer_waveformat->cbSize >= 22) {
        auto waveformatext_pointer = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mixer_waveformat.get());
        capture_waveformat.dwChannelMask = waveformatext_pointer->dwChannelMask;
      }

      BOOST_LOG(info) << "Audio mixer format is "sv << mixer_waveformat->wBitsPerSample << "-bit, "sv
                      << mixer_waveformat->nSamplesPerSec << " Hz, "sv
                      << ((mixer_waveformat->nSamplesPerSec != 48000) ? "will be resampled to 48000 by Windows"sv : "no resampling needed"sv);
    }

    status = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,  // Enable automatic resampling to 48 KHz
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
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param channels_out Channels out.
     * @param continuous Whether silent audio should continue to be emitted.
     * @param capture_device Endpoint device to capture from; the default render device is used when empty.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(std::uint32_t sample_rate, std::uint32_t frame_size, std::uint32_t channels_out, bool continuous, device_t capture_device) {
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
        if (format.channel_count != channels_out) {
          BOOST_LOG(debug) << "Skipping audio format ["sv << format.name << "] with channel count ["sv
                           << format.channel_count << " != "sv << channels_out << ']';
          continue;
        }

        BOOST_LOG(debug) << "Trying audio format ["sv << format.name << ']';
        audio_client = make_audio_client(device, format);

        if (audio_client) {
          BOOST_LOG(debug) << "Found audio format ["sv << format.name << ']';
          channels = channels_out;
          break;
        }
      }

      if (!audio_client) {
        BOOST_LOG(error) << "Couldn't find supported format for audio"sv;
        return -1;
      }

      REFERENCE_TIME default_latency;
      audio_client->GetDevicePeriod(&default_latency, nullptr);
      default_latency_ms = default_latency / 1000;
      continuous_audio = continuous;

      std::uint32_t frames;
      status = audio_client->GetBufferSize(&frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't acquire the number of audio frames [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      // *2 --> needs to fit double
      sample_buf = util::buffer_t<float> {std::max(frames, frame_size) * 2 * channels_out};
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
    /**
     * @brief Query host and virtual sink names available to Sunshine.
     *
     * @return Host and virtual sink names when the backend can report them.
     */
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      if (config::audio.external_audio) {
        if (config::audio.sink.empty()) {
          BOOST_LOG(error) << "external_audio requires an explicit audio_sink";
          return std::nullopt;
        }
        sink.host = config::audio.sink;
        return sink;
      }

      // Fill host sink name with the device_id of the current default audio device.
      {
        auto device = default_device(device_enum);
        if (!device) {
          return std::nullopt;
        }

        audio::wstring_t id;
        device->GetId(&id);

        sink.host = utf_utils::to_utf8(id.get());
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
    std::optional<std::pair<std::wstring, std::reference_wrapper<const format_t>>> extract_virtual_sink_info(const std::string &sink) {
      // Encoding format:
      // [virtual-(format name)]device_id
      std::string current = sink;
      if (auto prefix = "virtual-"sv; current.find(prefix) == 0) {
        current = current.substr(prefix.size(), current.size() - prefix.size());

        for (const auto &format : formats) {
          auto &name = format.name;
          if (current.find(name) == 0) {
            auto device_id = utf_utils::from_utf8(current.substr(name.size(), current.size() - name.size()));
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
     * @brief Create a microphone capture stream for the requested layout.
     *
     * @param mapping Opus channel mapping table for the requested layout.
     * @param channels Number of audio channels in the stream.
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param continuous_audio Continuous audio.
     * @param host_audio_enabled Whether host playback should remain enabled during capture.
     * @return Microphone capture object for the requested audio layout.
     */
    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
      auto mic = std::make_unique<mic_wasapi_t>();

      // External routing pins capture to the configured sink; otherwise prefer the session's assigned sink.
      const auto &requested_sink = config::audio.external_audio || assigned_sink.empty() ? config::audio.sink : assigned_sink;

      if (config::audio.external_audio && requested_sink.empty()) {
        BOOST_LOG(error) << "external_audio requires an explicit audio_sink";
        return nullptr;
      }

      // Capture the requested sink directly instead of relying on it being the default
      // render device, so that capture keeps working when the default device differs
      // from the sink or changes during the session.
      device_t capture_device;
      if (!requested_sink.empty()) {
        capture_device = get_sink_device(requested_sink);
        if (!capture_device) {
          BOOST_LOG(error) << "Couldn't resolve audio sink ["sv << requested_sink << "] to a capture device"sv;
          return nullptr;
        }

        BOOST_LOG(info) << "Capturing audio from sink ["sv << requested_sink << ']';
      }

      if (mic->init(sample_rate, frame_size, channels, continuous_audio, std::move(capture_device))) {
        return nullptr;
      }

      // If this is a virtual sink, set a callback that will change the sink back if it's changed.
      // Hands-off modes never reinstall: capture follows the recorded endpoint directly.
      if (should_install_default_callback(assigned_sink)) {
        mic->default_endpt_changed_cb = [this] {
          BOOST_LOG(info) << "Resetting sink to ["sv << assigned_sink << "] after default changed";
          set_sink(assigned_sink);
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
     * @param sink Audio sink name to route or capture.
     * @return Status from updating sink.
     */
    int set_sink(const std::string &sink) override {
      if (config::audio.external_audio) {
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
        assigned_sink = sink;
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
     * Records the target for microphone(); unlike set_sink(), this never calls
     * SetDefaultEndpoint and never installs the Steam drivers.
     *
     * @param sink Audio sink name to capture.
     * @return Always 0: recording a capture target cannot fail here.
     */
    int capture_sink(const std::string &sink) override {
      BOOST_LOG(info) << "Keeping host default sink; capturing ["sv << sink << "] without swapping"sv;
      assigned_sink = sink;

      return 0;
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
    bool should_install_default_callback(const std::string &sink) {
      return static_cast<bool>(extract_virtual_sink_info(sink)) && !config::audio.external_audio &&
             !config::audio.keep_default_sink;
    }

    /**
     * @brief Whether startup may move the default away from Steam Streaming Speakers.
     *
     * @return False in hands-off modes that never touch the host default.
     */
    bool should_reset_default_device() {
      return !config::audio.external_audio && !config::audio.keep_default_sink;
    }

    /**
     * @brief Whether the Steam Streaming Speakers driver may be installed.
     *
     * Installing a driver reconfigures the audio subsystem (device arrival makes
     * Windows re-evaluate defaults), which keep-default sessions must not trigger.
     *
     * @return False in hands-off modes that never touch the host audio graph.
     */
    bool should_install_steam_drivers() {
      return !config::audio.external_audio && !config::audio.keep_default_sink && config::audio.install_steam_drivers;
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
    audio_control_t::match_fields_list_t match_all_fields(const std::wstring &name) {
      using enum match_field_e;
      return {
        {device_id, name},  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        {device_friendly_name, name},  // Digital Audio (S/PDIF) (High Definition Audio Device)
        {device_description, name},  // Digital Audio (S/PDIF)
        {adapter_friendly_name, name},  // High Definition Audio Device
      };
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
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
      collection->GetCount(&count);

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        audio::device_t device;
        collection->Item(x, &device);

        audio::wstring_t wstring_id;
        device->GetId(&wstring_id);
        std::wstring device_id = wstring_id.get();

        audio::prop_t prop;
        device->OpenPropertyStore(STGM_READ, &prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
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
      if (!config::audio.external_audio) {
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
    std::string assigned_sink;  ///< Sink assigned while Sunshine captures host audio, captured directly by the microphone.
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
     * @brief Exercise sink discovery without initializing Windows policy interfaces.
     * @return Sinks reported by the production controller for the current configuration.
     */
    std::optional<sink_t> configured_sink_info() {
      audio_control_t control;
      return control.sink_info();
    }

    /**
     * @brief Exercise the external-routing sink-change guard without policy interfaces.
     * @param sink Sink a caller attempts to select.
     * @return Status from the production sink setter.
     */
    int set_external_sink(const std::string &sink) {
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

      return control.assigned_sink;
    }

    /**
     * @brief Exercise the default-callback guard with controlled hands-off modes.
     * @param sink Assigned sink name that would be reinstalled.
     * @param keep_default keep_default_sink value to test with.
     * @param external_audio external_audio value to test with.
     * @return True when the production guard installs the reassertion callback.
     */
    bool installs_default_callback(const std::string &sink, bool keep_default, bool external_audio) {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, keep_default);
      auto previous_external = std::exchange(config::audio.external_audio, external_audio);
      bool installed = control.should_install_default_callback(sink);
      config::audio.keep_default_sink = previous_keep;
      config::audio.external_audio = previous_external;
      return installed;
    }

    /**
     * @brief Exercise the startup-reset guard with controlled hands-off modes.
     * @param keep_default keep_default_sink value to test with.
     * @param external_audio external_audio value to test with.
     * @return True when the production guard permits resetting the default device.
     */
    bool resets_default_device(bool keep_default, bool external_audio) {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, keep_default);
      auto previous_external = std::exchange(config::audio.external_audio, external_audio);
      bool resets = control.should_reset_default_device();
      config::audio.keep_default_sink = previous_keep;
      config::audio.external_audio = previous_external;
      return resets;
    }

    /**
     * @brief Exercise the driver-install guard with controlled hands-off modes.
     * @param keep_default keep_default_sink value to test with.
     * @param external_audio external_audio value to test with.
     * @param install_requested install_steam_drivers value to test with.
     * @return True when the production guard permits installing the Steam drivers.
     */
    bool installs_steam_drivers(bool keep_default, bool external_audio, bool install_requested) {
      audio_control_t control;
      auto previous_keep = std::exchange(config::audio.keep_default_sink, keep_default);
      auto previous_external = std::exchange(config::audio.external_audio, external_audio);
      auto previous_install = std::exchange(config::audio.install_steam_drivers, install_requested);
      bool installs = control.should_install_steam_drivers();
      config::audio.keep_default_sink = previous_keep;
      config::audio.external_audio = previous_external;
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
     * @brief Exercise microphone creation with controlled assigned and configured sinks.
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
      control.assigned_sink = assigned_sink;

      auto previous_configured_sink = std::exchange(config::audio.sink, configured_sink);
      auto microphone = control.microphone(nullptr, 2, 48000, 240, false, false);
      config::audio.sink = std::move(previous_configured_sink);
      return static_cast<bool>(microphone);
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
