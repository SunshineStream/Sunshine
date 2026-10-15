/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <algorithm>
#include <array>
#include <bitset>
#include <sstream>
#include <thread>

// lib includes
#include <boost/regex.hpp>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/thread_safe.h"

namespace platf {
  using namespace std::literals;

  /**
   * @brief PulseAudio position for each `speaker::speaker_e`, in `speaker_e` order.
   */
  constexpr std::array<pa_channel_position_t, speaker::MAX_SPEAKERS> position_mapping {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  /**
   * @brief Convert a PulseAudio operation result to a log string.
   *
   * @param name Human-readable name to assign.
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @return Value converted to string.
   */
  std::string to_string(const char *name, const std::uint8_t *mapping, int channels) {
    std::stringstream ss;

    ss << "rate=48000 sink_name="sv << name << " format=float channels="sv << channels << " channel_map="sv;
    std::for_each_n(mapping, channels - 1, [&ss](std::uint8_t pos) {
      ss << pa_channel_position_to_string(position_mapping[pos]) << ',';
    });

    ss << pa_channel_position_to_string(position_mapping[mapping[channels - 1]]);

    ss << " sink_properties=device.description="sv << name;
    auto result = ss.str();

    BOOST_LOG(debug) << "null-sink args: "sv << result;
    return result;
  }

  /**
   * @brief PulseAudio recording stream and channel metadata.
   */
  struct mic_attr_t: public mic_t {
    util::safe_ptr<pa_simple, &pa_simple_free> mic;  ///< PulseAudio simple recording stream for microphone capture.

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_buf Sample buf.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_buf) override {
      auto sample_size = sample_buf.size();

      auto buf = sample_buf.data();
      int status;
      if (pa_simple_read(mic.get(), buf, sample_size * sizeof(float), &status)) {
        BOOST_LOG(error) << "pa_simple_read() failed: "sv << pa_strerror(status);

        return capture_e::capture_error;
      }

      return capture_e::ok;
    }
  };

  /**
   * @brief Fill a PulseAudio channel map with the canonical layout for a channel count.
   *
   * Capture always uses the canonical layout, never the Opus channel mapping: the Opus mapping
   * is applied by the multistream encoder and undone by the client, so applying it here too
   * would leave the samples permuted. `speaker::map_stereo`, `map_surround51` and
   * `map_surround71` are the identity over `speaker::speaker_e`, so the canonical position of
   * channel `i` is `position_mapping[i]`.
   *
   * @param channels Number of audio channels in the stream.
   * @param pa_map Channel map to fill; left untouched when `channels` is out of range.
   * @return `true` when the channel count maps onto a known speaker layout.
   */
  bool fill_canonical_channel_map(int channels, pa_channel_map &pa_map) {
    if (channels < 1 || channels > speaker::MAX_SPEAKERS) {
      return false;
    }

    pa_channel_map_init(&pa_map);
    pa_map.channels = static_cast<std::uint8_t>(channels);
    for (int i = 0; i < channels; ++i) {
      pa_map.map[i] = position_mapping[i];
    }

    return true;
  }

  /**
   * @brief Create a microphone capture stream for the requested layout.
   * @details Capture always uses the canonical channel layout; Opus mapping is applied by the encoder.
   *
   * @param channels Number of audio channels in the stream.
   * @param sample_rate Audio sample rate in hertz.
   * @param frame_size Number of samples captured per audio frame.
   * @param source_name Source name.
   * @return Microphone capture object for the requested audio layout, or nullptr on failure.
   */
  std::unique_ptr<mic_t> microphone(int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    // Capture in the canonical channel layout, not the one described by the Opus mapping.
    //
    // The Opus mapping is applied by the multistream encoder in audio.cpp and is undone by
    // the client when it decodes, so the two cancel out. Passing it to PulseAudio as the
    // capture channel map makes PulseAudio reorder the samples as well, and that reordering
    // is never undone: the client ends up rendering permuted channels.
    //
    // This is invisible with the built-in mappings, which are all identities, but it
    // scrambles the audio whenever a client requests custom surround-params (the webOS
    // client asks for 0,1,4,5,2,3). The WASAPI backend ignores the mapping for this reason.
    //
    pa_channel_map pa_map;
    if (!fill_canonical_channel_map(channels, pa_map)) {
      BOOST_LOG(error) << "Unsupported channel count for audio capture: "sv << channels;
      return nullptr;
    }

    auto mic = std::make_unique<mic_attr_t>();

    pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};

    pa_buffer_attr pa_attr = {
      .maxlength = uint32_t(-1),
      .tlength = uint32_t(-1),
      .prebuf = uint32_t(-1),
      .minreq = uint32_t(-1),
      .fragsize = uint32_t(frame_size * channels * sizeof(float))
    };

    int status;

    mic->mic.reset(
      pa_simple_new(nullptr, "sunshine", pa_stream_direction_t::PA_STREAM_RECORD, source_name.c_str(), "sunshine-record", &ss, &pa_map, &pa_attr, &status)
    );

    if (!mic->mic) {
      auto err_str = pa_strerror(status);
      BOOST_LOG(error) << "pa_simple_new() failed: "sv << err_str;
      return nullptr;
    }

    return mic;
  }

  namespace pa {
    template<bool B, class T>
    struct add_const_helper;

    /**
     * @brief Template helper that preserves constness for const inputs.
     */
    template<class T>
    struct add_const_helper<true, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const std::remove_pointer_t<T> *;
    };

    /**
     * @brief Template helper that leaves non-const inputs mutable.
     */
    template<class T>
    struct add_const_helper<false, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const T *;
    };

    /**
     * @brief PulseAudio callback info type with pointer constness normalized.
     */
    template<class T>
    using add_const_t = typename add_const_helper<std::is_pointer_v<T>, T>::type;

    /**
     * @brief Release memory allocated by PulseAudio.
     *
     * @param p Pointer allocated by PulseAudio and released with `pa_xfree`.
     */
    template<class T>
    void pa_free(T *p) {
      pa_xfree(p);
    }

    /**
     * @brief Owning pointer for a PulseAudio context.
     */
    using ctx_t = util::safe_ptr<pa_context, &pa_context_unref>;
    /**
     * @brief Owning pointer for a PulseAudio mainloop.
     */
    using loop_t = util::safe_ptr<pa_mainloop, &pa_mainloop_free>;
    /**
     * @brief Owning pointer for a PulseAudio asynchronous operation.
     */
    using op_t = util::safe_ptr<pa_operation, &pa_operation_unref>;
    /**
     * @brief Owning pointer for PulseAudio strings allocated with `pa_xmalloc`.
     */
    using string_t = util::safe_ptr<char, &pa_free<char>>;

    /**
     * @brief Callback wrapper for PulseAudio introspection results without an end marker.
     */
    template<class T>
    using cb_simple_t = std::function<void(ctx_t::pointer, add_const_t<T> i)>;

    /**
     * @brief Handle PulseAudio sink-input introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, void *userdata) {  // NOSONAR(cpp:S5008): PulseAudio introspection callbacks require a void pointer for user data.
      auto &f = *(cb_simple_t<T> *) userdata;

      // Cannot similarly filter on eol here. Unless reported otherwise assume
      // we have no need for special filtering like cb?
      f(ctx, i);
    }

    /**
     * @brief Callback wrapper for PulseAudio introspection results with an end marker.
     */
    template<class T>
    using cb_t = std::function<void(ctx_t::pointer, add_const_t<T> i, int eol)>;

    /**
     * @brief Handle PulseAudio source introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param eol PulseAudio end-of-list marker.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, int eol, void *userdata) {  // NOSONAR(cpp:S5008): PulseAudio introspection callbacks require a void pointer for user data.
      auto &f = *(cb_t<T> *) userdata;

      // For some reason, pulseaudio calls this callback after disconnecting
      if (i && eol) {
        return;
      }

      f(ctx, i, eol);
    }

    /**
     * @brief Forward a PulseAudio integer callback value into a Sunshine alarm.
     * @details The PulseAudio callback signature supplies a context that is not needed to ring the alarm.
     *
     * @param i Integer value returned by the PulseAudio operation.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void cb_i(ctx_t::pointer, std::uint32_t i, void *userdata) {
      auto alarm = (safe::alarm_raw_t<int> *) userdata;

      alarm->ring(i);
    }

    /**
     * @brief Translate PulseAudio context state changes into server events.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void ctx_state_cb(ctx_t::pointer ctx, void *userdata) {
      const auto &f = *(std::function<void(ctx_t::pointer)> *) userdata;

      f(ctx);
    }

    /**
     * @brief Record completion of a PulseAudio asynchronous operation.
     * @details The PulseAudio callback signature supplies a context that is not needed to report completion.
     *
     * @param status Native status code returned by the platform API.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void success_cb(ctx_t::pointer, int status, void *userdata) {
      assert(userdata != nullptr);

      auto alarm = (safe::alarm_raw_t<int> *) userdata;
      alarm->ring(status ? 0 : 1);
    }

    /**
     * @brief PulseAudio server controller that creates and removes Sunshine sinks.
     */
    class server_t: public audio_control_t {
      enum ctx_event_e : int {
        ready,
        terminated,
        failed
      };

    public:
      loop_t loop;  ///< PulseAudio threaded mainloop instance.
      ctx_t ctx;  ///< PulseAudio threaded mainloop context.
      std::string requested_sink;  ///< Requested sink.

      static constexpr const char *stereo_name = "sink-sunshine-stereo";  ///< Persistent stereo null sink name.
      static constexpr const char *surround51_name = "sink-sunshine-surround51";  ///< Persistent 5.1 null sink name.
      static constexpr const char *surround71_name = "sink-sunshine-surround71";  ///< Persistent 7.1 null sink name.

      /**
       * @brief Lifecycle state for one Sunshine null sink.
       */
      struct null_sink_t {
        std::uint32_t module_index = PA_INVALID_INDEX;  ///< Owning PulseAudio module index, when module-backed.
        bool present = false;  ///< Sink exists in the graph (pre-existing or loaded by this session).
        bool owned = false;  ///< Loaded by this session; safe to unload on teardown.
      };

      struct {
        null_sink_t stereo;  ///< Stereo null sink lifecycle.
        null_sink_t surround51;  ///< 5.1 null sink lifecycle.
        null_sink_t surround71;  ///< 7.1 null sink lifecycle.
      } index;  ///< Per-sink lifecycle bookkeeping.

      std::unique_ptr<safe::event_t<ctx_event_e>> events;  ///< Event queue receiving PulseAudio context state changes.
      std::unique_ptr<std::function<void(ctx_t::pointer)>> events_cb;  ///< Callback that translates PulseAudio context updates into events.

      std::jthread worker;  ///< Thread running the PulseAudio mainloop.

      /**
       * @brief Initialize PulseAudio mainloop, context, and Sunshine null sinks.
       *
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init() {
        events = std::make_unique<safe::event_t<ctx_event_e>>();
        loop.reset(pa_mainloop_new());
        ctx.reset(pa_context_new(pa_mainloop_get_api(loop.get()), "sunshine"));

        events_cb = std::make_unique<std::function<void(ctx_t::pointer)>>([this](ctx_t::pointer callback_ctx) {
          switch (pa_context_get_state(callback_ctx)) {
            case PA_CONTEXT_READY:
              events->raise(ready);
              break;
            case PA_CONTEXT_TERMINATED:
              BOOST_LOG(debug) << "PulseAudio context terminated"sv;
              events->raise(terminated);
              break;
            case PA_CONTEXT_FAILED:
              BOOST_LOG(debug) << "PulseAudio context failed"sv;
              events->raise(failed);
              break;
            case PA_CONTEXT_CONNECTING:
              BOOST_LOG(debug) << "Connecting to pulseaudio"sv;
            case PA_CONTEXT_UNCONNECTED:
            case PA_CONTEXT_AUTHORIZING:
            case PA_CONTEXT_SETTING_NAME:
              break;
          }
        });

        pa_context_set_state_callback(ctx.get(), ctx_state_cb, events_cb.get());

        auto status = pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr);
        if (status) {
          BOOST_LOG(error) << "Couldn't connect to pulseaudio: "sv << pa_strerror(status);
          return -1;
        }

        worker = std::jthread {
          [](loop_t::pointer loop) {
            int retval;
            platf::set_thread_name("audio::pulseaudio");
            auto status = pa_mainloop_run(loop, &retval);

            if (status < 0) {
              BOOST_LOG(error) << "Couldn't run pulseaudio main loop"sv;
              return;
            }
          },
          loop.get()
        };

        auto event = events->pop();
        if (event == failed) {
          return -1;
        }

        return 0;
      }

      /**
       * @brief Create a PulseAudio null sink for one channel layout.
       *
       * @param name Human-readable name to assign.
       * @param channel_mapping Channel mapping.
       * @param channels Number of audio channels in the stream.
       * @return PulseAudio module index for the new sink, or PA_INVALID_INDEX on failure.
       */
      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_load_module(
            ctx.get(),
            "module-null-sink",
            to_string(name, channel_mapping, channels).c_str(),
            cb_i,
            alarm.get()
          ),
        };

        alarm->wait();
        return *alarm->status();
      }

      /**
       * @brief Unload a Sunshine-created PulseAudio null sink.
       *
       * @param i PulseAudio introspection info supplied to the callback.
       * @return 0 when the sink is absent or unloaded; nonzero on PulseAudio failure.
       */
      int unload_null(std::uint32_t i) {
        if (i == PA_INVALID_INDEX) {
          return 0;
        }

        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_unload_module(ctx.get(), i, success_cb, alarm.get())
        };

        alarm->wait();

        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't unload null-sink with index ["sv << i << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        return 0;
      }

      /**
       * @brief Adopt a pre-existing Sunshine null sink found during enumeration.
       * @details Reuses sinks from persistent configuration or a live session without taking
       *          ownership, so teardown never unloads what this session didn't create. The present
       *          guard also keeps repeat sink_info() calls from demoting owned sinks to adopted.
       *
       * @param sink Lifecycle state for the sink.
       * @param name Sunshine null sink name to match.
       * @param found PulseAudio introspection info for the enumerated sink.
       * @param nullcount Running count of Sunshine virtual sinks found.
       */
      void adopt_sink(null_sink_t &sink, const char *name, const pa_sink_info *found, int &nullcount) {
        if (std::strcmp(found->name, name)) {
          return;
        }

        if (!sink.present) {
          sink.module_index = found->owner_module;
          sink.present = true;
        }

        ++nullcount;
      }

      /**
       * @brief Create the Sunshine null sink for one layout when it is missing.
       *
       * @param sink Lifecycle state for the sink.
       * @param name Sunshine null sink name to create.
       * @param channel_mapping Channel mapping.
       * @param channels Number of audio channels in the stream.
       * @param layout Human-readable layout name for logging.
       * @param nullcount Running count of Sunshine virtual sinks found.
       */
      void ensure_sink(null_sink_t &sink, const char *name, const std::uint8_t *channel_mapping, int channels, const char *layout, int &nullcount) {
        if (sink.present) {
          return;
        }

        sink.module_index = load_null(name, channel_mapping, channels);
        if (sink.module_index == PA_INVALID_INDEX) {
          BOOST_LOG(warning) << "Couldn't create virtual sink for "sv << layout << ": "sv << pa_strerror(pa_context_errno(ctx.get()));
        } else {
          sink.present = true;
          sink.owned = true;
          ++nullcount;
        }
      }

      /**
       * @brief Query host and virtual sink names available to Sunshine.
       *
       * @return Host and virtual sink names when the backend can report them.
       */
      std::optional<sink_t> sink_info() override {
        auto alarm = safe::make_alarm<int>();

        sink_t sink;

        // Count of all virtual sinks that are created by us
        int nullcount = 0;

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer callback_ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(pa_context_errno(callback_ctx));

              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          adopt_sink(index.stereo, stereo_name, sink_info, nullcount);
          adopt_sink(index.surround51, surround51_name, sink_info, nullcount);
          adopt_sink(index.surround71, surround71_name, sink_info, nullcount);
        };

        op_t op {pa_context_get_sink_info_list(ctx.get(), cb<pa_sink_info *>, &f)};

        if (!op) {
          BOOST_LOG(error) << "Couldn't create card info operation: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return std::nullopt;
        }

        alarm->wait();

        if (*alarm->status()) {
          return std::nullopt;
        }

        auto sink_name = get_default_sink_name();
        sink.host = sink_name;

        ensure_sink(index.stereo, stereo_name, speaker::map_stereo.data(), static_cast<int>(speaker::map_stereo.size()), "stereo", nullcount);
        ensure_sink(index.surround51, surround51_name, speaker::map_surround51.data(), static_cast<int>(speaker::map_surround51.size()), "surround-51", nullcount);
        ensure_sink(index.surround71, surround71_name, speaker::map_surround71.data(), static_cast<int>(speaker::map_surround71.size()), "surround-71", nullcount);

        if (sink_name.empty()) {
          BOOST_LOG(warning) << "Couldn't find an active default sink. Continuing with virtual audio only."sv;
        }

        if (nullcount == 3) {
          sink.null = std::make_optional(sink_t::null_t {stereo_name, surround51_name, surround71_name});
        }

        return std::make_optional(std::move(sink));
      }

      /**
       * @brief Get default sink name.
       *
       * @return PulseAudio name of the current default sink, or an empty string.
       */
      std::string get_default_sink_name() {
        std::string sink_name;
        auto alarm = safe::make_alarm<int>();

        cb_simple_t<pa_server_info *> server_f = [&](ctx_t::pointer callback_ctx, const pa_server_info *server_info) {
          if (!server_info) {
            BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(pa_context_errno(callback_ctx));
            alarm->ring(-1);
          }

          if (server_info->default_sink_name) {
            sink_name = server_info->default_sink_name;
          }
          alarm->ring(0);
        };

        op_t server_op {pa_context_get_server_info(ctx.get(), cb<pa_server_info *>, &server_f)};
        alarm->wait();
        // No need to check status. If it failed just return default name.
        return sink_name;
      }

      /**
       * @brief Get monitor name.
       *
       * @param sink_name Sink name.
       * @return PulseAudio monitor source name for the supplied sink, or an empty string.
       */
      std::string get_monitor_name(const std::string &sink_name) {
        std::string monitor_name;
        auto alarm = safe::make_alarm<int>();

        if (sink_name.empty()) {
          return monitor_name;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer callback_ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(callback_ctx));
              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          monitor_name = sink_info->monitor_source_name;
        };

        op_t sink_op {pa_context_get_sink_info_by_name(ctx.get(), sink_name.c_str(), cb<pa_sink_info *>, &sink_f)};

        alarm->wait();
        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      /**
       * @brief Create a microphone capture stream for the requested layout.
       * @details The common audio interface supplies an Opus mapping that canonical capture does not need.
       *
       * @param channels Number of audio channels in the stream.
       * @param sample_rate Audio sample rate in hertz.
       * @param frame_size Number of samples captured per audio frame.
       * @param continuous_audio Continuous audio.
       * @param host_audio_enabled Whether host playback should remain enabled during capture.
       * @return Microphone capture object for the requested audio layout.
       */
      std::unique_ptr<mic_t> microphone(const std::uint8_t *, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
        // Sink choice priority:
        // 1. Config sink
        // 2. Last sink swapped to (Usually virtual in this case)
        // 3. Default Sink
        // An attempt was made to always use default to match the switching mechanic,
        // but this happens right after the swap so the default returned by PA was not
        // the new one just set!
        auto sink_name = config::audio.sink;
        if (sink_name.empty()) {
          sink_name = requested_sink;
        }
        if (sink_name.empty()) {
          sink_name = get_default_sink_name();
        }

        return ::platf::microphone(channels, sample_rate, frame_size, get_monitor_name(sink_name));
      }

      bool is_sink_available(const std::string &sink) override {
        BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
        return true;
      }

      /**
       * @brief Whether the backend can capture a sink without changing the host default.
       *
       * @return Always true: capture_sink() records the target for the monitor lookup.
       */
      bool supports_keep_default_sink() const override {
        return true;
      }

      /**
       * @brief Capture the given sink without changing the host default.
       *
       * @param sink Audio sink name to capture.
       * @return Always 0: selecting a capture target cannot fail here.
       */
      int capture_sink(const std::string &sink) override {
        BOOST_LOG(info) << "Keeping host default sink; capturing ["sv << sink << "] without swapping"sv;
        requested_sink = sink;

        return 0;
      }

      /**
       * @brief Update the sink value on the backend.
       *
       * @param sink Audio sink name to route or capture.
       * @return Status from updating sink.
       */
      int set_sink(const std::string &sink) override {
        auto alarm = safe::make_alarm<int>();

        BOOST_LOG(info) << "Setting default sink to: ["sv << sink << "]"sv;
        op_t op {
          pa_context_set_default_sink(
            ctx.get(),
            sink.c_str(),
            success_cb,
            alarm.get()
          ),
        };

        if (!op) {
          BOOST_LOG(error) << "Couldn't create set default-sink operation: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        alarm->wait();
        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't set default-sink ["sv << sink << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return -1;
        }

        requested_sink = sink;

        return 0;
      }

      /**
       * @brief Unload a null sink loaded by this session; leave adopted sinks alone.
       *
       * Adopted sinks (persistent PipeWire config or another session's) stay
       * loaded: unloading them would yank routed streams back to the host
       * default mid-game.
       *
       * @param sink Lifecycle state for the sink.
       * @param name Sink name for logging.
       */
      void unload_owned(null_sink_t &sink, const char *name) {
        if (!sink.owned) {
          BOOST_LOG(debug) << "Leaving adopted null-sink ["sv << name << "] loaded"sv;
          return;
        }

        unload_null(sink.module_index);
      }

      ~server_t() override {
        try {
          unload_owned(index.stereo, stereo_name);
          unload_owned(index.surround51, surround51_name);
          unload_owned(index.surround71, surround71_name);
        } catch (...) {
          // Teardown must not throw: unload failures were already logged above.
        }

        if (worker.joinable()) {
          pa_context_disconnect(ctx.get());

          KITTY_WHILE_LOOP(auto event = events->pop(), event != terminated && event != failed, {
            event = events->pop();
          })

          pa_mainloop_quit(loop.get(), 0);
          worker.join();
        }
      }
    };
  }  // namespace pa

  /**
   * @brief Create the platform audio controller.
   */
  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
