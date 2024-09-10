/**
 * @file src/nvenc/common_impl/nvenc_base.cpp
 * @brief Definitions for abstract platform-agnostic base of standalone NVENC encoder.
 */
#include "nvenc_base.h"

#include "nvenc_utils.h"

#include "src/utility.h"

#define NVENC_INT_VERSION (NVENCAPI_MAJOR_VERSION * 100 + NVENCAPI_MINOR_VERSION)

namespace {

#ifdef NVENC_NAMESPACE
  using namespace NVENC_NAMESPACE;
#endif

  GUID
  quality_preset_guid_from_number(unsigned number) {
    if (number > 7) number = 7;

    switch (number) {
      case 1:
      default:
        return NV_ENC_PRESET_P1_GUID;

      case 2:
        return NV_ENC_PRESET_P2_GUID;

      case 3:
        return NV_ENC_PRESET_P3_GUID;

      case 4:
        return NV_ENC_PRESET_P4_GUID;

      case 5:
        return NV_ENC_PRESET_P5_GUID;

      case 6:
        return NV_ENC_PRESET_P6_GUID;

      case 7:
        return NV_ENC_PRESET_P7_GUID;
    }
  };

  bool
  equal_guids(const GUID &guid1, const GUID &guid2) {
    return std::memcmp(&guid1, &guid2, sizeof(GUID)) == 0;
  }

  auto
  quality_preset_string_from_guid(const GUID &guid) {
    if (equal_guids(guid, NV_ENC_PRESET_P1_GUID)) {
      return "P1";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P2_GUID)) {
      return "P2";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P3_GUID)) {
      return "P3";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P4_GUID)) {
      return "P4";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P5_GUID)) {
      return "P5";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P6_GUID)) {
      return "P6";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P7_GUID)) {
      return "P7";
    }
    return "Unknown";
  }

}  // namespace

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
namespace nvenc {
#endif

  nvenc_base::nvenc_base(NV_ENC_DEVICE_TYPE device_type):
      device_type(device_type) {
  }

  nvenc_base::~nvenc_base() {
    // Use destroy_encoder() instead
  }

  bool
  nvenc_base::create_encoder(
    const nvenc_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &sunshine_colorspace,
    platf::pix_fmt_e sunshine_buffer_format) {
    if (!nvenc && !init_library()) return false;

    if (encoder) destroy_encoder();
    auto fail_guard = util::fail_guard([this] { destroy_encoder(); });

    auto colorspace = nvenc_colorspace_from_sunshine_colorspace(sunshine_colorspace);
    auto buffer_format = nvenc_format_from_sunshine_format(sunshine_buffer_format);

    encoder_params.width = client_config.width;
    encoder_params.height = client_config.height;
    encoder_params.buffer_format = buffer_format;
    encoder_params.rfi = true;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    session_params.device = device;
    session_params.deviceType = device_type;
    session_params.apiVersion = NVENCAPI_VERSION;
    if (nvenc_failed(nvenc->nvEncOpenEncodeSessionEx(&session_params, &encoder))) {
      BOOST_LOG(error) << "NvEnc: NvEncOpenEncodeSessionEx() failed: " << last_nvenc_error_string;
      return false;
    }

    uint32_t encode_guid_count = 0;
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDCount(encoder, &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDCount() failed: " << last_nvenc_error_string;
      return false;
    };

    std::vector<GUID> encode_guids(encode_guid_count);
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDs(encoder, encode_guids.data(), encode_guids.size(), &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDs() failed: " << last_nvenc_error_string;
      return false;
    }

    NV_ENC_INITIALIZE_PARAMS init_params = { NV_ENC_INITIALIZE_PARAMS_VER };

    switch (client_config.videoFormat) {
      case 0:
        // H.264
        init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
        break;

      case 1:
        // HEVC
        init_params.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
        break;

#if NVENC_INT_VERSION >= 1200
      case 2:
        // AV1
        init_params.encodeGUID = NV_ENC_CODEC_AV1_GUID;
        break;
#endif

      default:
        BOOST_LOG(error) << "NvEnc: unknown video format " << client_config.videoFormat;
        return false;
    }

    {
      auto search_predicate = [&](const GUID &guid) {
        return equal_guids(init_params.encodeGUID, guid);
      };
      if (std::find_if(encode_guids.begin(), encode_guids.end(), search_predicate) == encode_guids.end()) {
        BOOST_LOG(error) << "NvEnc: encoding format is not supported by the gpu";
        return false;
      }
    }

    auto get_encoder_cap = [&](NV_ENC_CAPS cap) {
      NV_ENC_CAPS_PARAM param = { NV_ENC_CAPS_PARAM_VER };
      param.capsToQuery = cap;
      int value = 0;
      nvenc->nvEncGetEncodeCaps(encoder, init_params.encodeGUID, &param, &value);
      return value;
    };

    auto buffer_is_10bit = [&]() {
      return buffer_format == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    };

    auto buffer_is_yuv444 = [&]() {
      return buffer_format == NV_ENC_BUFFER_FORMAT_AYUV || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    };

    {
      auto supported_width = get_encoder_cap(NV_ENC_CAPS_WIDTH_MAX);
      auto supported_height = get_encoder_cap(NV_ENC_CAPS_HEIGHT_MAX);
      if (encoder_params.width > supported_width || encoder_params.height > supported_height) {
        BOOST_LOG(error) << "NvEnc: gpu max encode resolution " << supported_width << "x" << supported_height
                         << ", requested " << encoder_params.width << "x" << encoder_params.height;
        return false;
      }
    }

    if (buffer_is_10bit() && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
      BOOST_LOG(error) << "NvEnc: gpu doesn't support 10-bit encode";
      return false;
    }

    if (buffer_is_yuv444() && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE)) {
      BOOST_LOG(error) << "NvEnc: gpu doesn't support YUV444 encode";
      return false;
    }

    if (async_event_handle && !get_encoder_cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT)) {
      BOOST_LOG(warning) << "NvEnc: gpu doesn't support async encode";
      async_event_handle = nullptr;
    }

    encoder_params.rfi = get_encoder_cap(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION);

    init_params.presetGUID = quality_preset_guid_from_number(config.quality_preset);
    init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_params.enablePTD = 1;
    init_params.enableEncodeAsync = async_event_handle ? 1 : 0;
    init_params.enableWeightedPrediction = config.weighted_prediction && get_encoder_cap(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION);

    init_params.encodeWidth = encoder_params.width;
    init_params.darWidth = encoder_params.width;
    init_params.encodeHeight = encoder_params.height;
    init_params.darHeight = encoder_params.height;
    init_params.frameRateNum = client_config.framerate;
    init_params.frameRateDen = 1;

#if NVENC_INT_VERSION >= 1202
    {
      using enum nvenc_split_frame_encoding;
      init_params.splitEncodeMode = config.split_frame_encoding == disabled      ? NV_ENC_SPLIT_DISABLE_MODE :
                                    config.split_frame_encoding == force_enabled ? NV_ENC_SPLIT_AUTO_FORCED_MODE :
                                                                                   NV_ENC_SPLIT_AUTO_MODE;
    }
#endif

    NV_ENC_PRESET_CONFIG preset_config = {
      .version = NV_ENC_PRESET_CONFIG_VER,
      .presetCfg = { .version = NV_ENC_CONFIG_VER },
    };
    if (nvenc_failed(nvenc->nvEncGetEncodePresetConfigEx(encoder, init_params.encodeGUID, init_params.presetGUID, init_params.tuningInfo, &preset_config))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodePresetConfigEx() failed: " << last_nvenc_error_string;
      return false;
    }

    NV_ENC_CONFIG enc_config = preset_config.presetCfg;
    enc_config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    enc_config.gopLength = NVENC_INFINITE_GOPLENGTH;
    enc_config.frameIntervalP = 1;
    enc_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    enc_config.rcParams.zeroReorderDelay = 1;
    enc_config.rcParams.enableLookahead = 0;
    enc_config.rcParams.lowDelayKeyFrameScale = 1;
    enc_config.rcParams.multiPass = config.two_pass == nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                    config.two_pass == nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                            NV_ENC_MULTI_PASS_DISABLED;

    enc_config.rcParams.enableAQ = config.adaptive_quantization;
    enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

    if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
      enc_config.rcParams.vbvBufferSize = client_config.bitrate * 1000 / client_config.framerate;
      if (config.vbv_percentage_increase > 0) {
        enc_config.rcParams.vbvBufferSize += enc_config.rcParams.vbvBufferSize * config.vbv_percentage_increase / 100;
      }
    }

    auto set_h264_hevc_common_format_config = [&](auto &format_config) {
      format_config.repeatSPSPPS = 1;
      format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
      if (client_config.slicesPerFrame > 1 ||
          NVENC_INT_VERSION < 1202 ||
          config.split_frame_encoding == nvenc_split_frame_encoding::disabled) {
        format_config.sliceMode = 3;
        format_config.sliceModeData = client_config.slicesPerFrame;
      }
      if (buffer_is_yuv444()) {
        format_config.chromaFormatIDC = 3;
      }
      format_config.enableFillerDataInsertion = config.insert_filler_data;
    };

    auto set_ref_frames = [&](uint32_t &ref_frames_option, NV_ENC_NUM_REF_FRAMES &L0_option, uint32_t ref_frames_default) {
      if (client_config.numRefFrames > 0) {
        ref_frames_option = client_config.numRefFrames;
      }
      else {
        ref_frames_option = ref_frames_default;
      }
      if (ref_frames_option > 0 && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES)) {
        ref_frames_option = 1;
        encoder_params.rfi = false;
      }
      encoder_params.ref_frames_in_dpb = ref_frames_option;
      // This limits ref frames any frame can use to 1, but allows larger buffer size for fallback if some frames are invalidated through rfi
      L0_option = NV_ENC_NUM_REF_FRAMES_1;
    };

    auto set_minqp_if_enabled = [&](int value) {
      if (config.enable_min_qp) {
        enc_config.rcParams.enableMinQP = 1;
        enc_config.rcParams.minQP.qpInterP = value;
        enc_config.rcParams.minQP.qpIntra = value;
      }
    };

    auto fill_h264_hevc_vui = [&](auto &vui_config) {
      vui_config.videoSignalTypePresentFlag = 1;
      vui_config.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
      vui_config.videoFullRangeFlag = colorspace.full_range;
      vui_config.colourDescriptionPresentFlag = 1;
      vui_config.colourPrimaries = colorspace.primaries;
      vui_config.transferCharacteristics = colorspace.tranfer_function;
      vui_config.colourMatrix = colorspace.matrix;
      vui_config.chromaSampleLocationFlag = buffer_is_yuv444() ? 0 : 1;
      vui_config.chromaSampleLocationTop = 0;
      vui_config.chromaSampleLocationBot = 0;
    };

    switch (client_config.videoFormat) {
      case 0: {
        // H.264
        enc_config.profileGUID = buffer_is_yuv444() ? NV_ENC_H264_PROFILE_HIGH_444_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
        auto &format_config = enc_config.encodeCodecConfig.h264Config;
        set_h264_hevc_common_format_config(format_config);
        if (config.h264_cavlc || !get_encoder_cap(NV_ENC_CAPS_SUPPORT_CABAC)) {
          format_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
        }
        else {
          format_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
        }
        set_ref_frames(format_config.maxNumRefFrames, format_config.numRefL0, 5);
        set_minqp_if_enabled(config.min_qp_h264);
        fill_h264_hevc_vui(format_config.h264VUIParameters);
        break;
      }

      case 1: {
        // HEVC
        auto &format_config = enc_config.encodeCodecConfig.hevcConfig;
        set_h264_hevc_common_format_config(format_config);
        if (buffer_is_10bit()) {
#if NVENC_INT_VERSION >= 1202
          format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
          format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
#else
          format_config.pixelBitDepthMinus8 = 2;
#endif
        }
        set_ref_frames(format_config.maxNumRefFramesInDPB, format_config.numRefL0, 5);
        set_minqp_if_enabled(config.min_qp_hevc);
        fill_h264_hevc_vui(format_config.hevcVUIParameters);
        break;
      }

#if NVENC_INT_VERSION >= 1200
      case 2: {
        // AV1
        auto &format_config = enc_config.encodeCodecConfig.av1Config;
        format_config.repeatSeqHdr = 1;
        format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        if (buffer_is_yuv444()) {
          format_config.chromaFormatIDC = 3;
        }
        format_config.enableBitstreamPadding = config.insert_filler_data;
        if (buffer_is_10bit()) {
  #if NVENC_INT_VERSION >= 1202
          format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
          format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
  #else
          format_config.inputPixelBitDepthMinus8 = 2;
          format_config.pixelBitDepthMinus8 = 2;
  #endif
        }
        format_config.colorPrimaries = colorspace.primaries;
        format_config.transferCharacteristics = colorspace.tranfer_function;
        format_config.matrixCoefficients = colorspace.matrix;
        format_config.colorRange = colorspace.full_range;
        format_config.chromaSamplePosition = buffer_is_yuv444() ? 0 : 1;
        set_ref_frames(format_config.maxNumRefFramesInDPB, format_config.numFwdRefs, 8);
        set_minqp_if_enabled(config.min_qp_av1);

        if (client_config.slicesPerFrame > 1) {
          // NVENC only supports slice counts that are powers of two, so we'll pick powers of two
          // with bias to rows due to hopefully more similar macroblocks with a row vs a column.
          format_config.numTileRows = std::pow(2, std::ceil(std::log2(client_config.slicesPerFrame) / 2));
          format_config.numTileColumns = std::pow(2, std::floor(std::log2(client_config.slicesPerFrame) / 2));
        }
        break;
      }
#endif
    }

    init_params.encodeConfig = &enc_config;

    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params))) {
      BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << last_nvenc_error_string;
      return false;
    }

    if (async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = { NV_ENC_EVENT_PARAMS_VER };
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncRegisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterAsyncEvent() failed: " << last_nvenc_error_string;
        return false;
      }
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER create_bitstream_buffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    if (nvenc_failed(nvenc->nvEncCreateBitstreamBuffer(encoder, &create_bitstream_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncCreateBitstreamBuffer() failed: " << last_nvenc_error_string;
      return false;
    }
    output_bitstream = create_bitstream_buffer.bitstreamBuffer;

    if (!create_and_register_input_buffer()) {
      return false;
    }

    {
      auto f = stat_trackers::two_digits_after_decimal();
      BOOST_LOG(debug) << "NvEnc: requested encoded frame size " << f % (client_config.bitrate / 8. / client_config.framerate) << " kB";
    }

    {
      auto video_format_string = client_config.videoFormat == 0 ? "H.264 " :
                                 client_config.videoFormat == 1 ? "HEVC " :
                                 client_config.videoFormat == 2 ? "AV1 " :
                                                                  " ";
      std::string extra;
      if (init_params.enableEncodeAsync) extra += " async";
      if (buffer_is_yuv444()) extra += " yuv444";
      if (buffer_is_10bit()) extra += " 10-bit";
      if (enc_config.rcParams.multiPass != NV_ENC_MULTI_PASS_DISABLED) extra += " two-pass";
      if (config.vbv_percentage_increase > 0 && get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
        extra += " vbv+" + std::to_string(config.vbv_percentage_increase);
      }
      if (encoder_params.rfi) extra += " rfi";
      if (init_params.enableWeightedPrediction) extra += " weighted-prediction";
      if (enc_config.rcParams.enableAQ) extra += " spatial-aq";
      if (enc_config.rcParams.enableMinQP) extra += " qpmin=" + std::to_string(enc_config.rcParams.minQP.qpInterP);
      if (config.insert_filler_data) extra += " filler-data";

      BOOST_LOG(info) << "NvEnc: created encoder v" << NVENC_INT_VERSION << " "
                      << video_format_string << quality_preset_string_from_guid(init_params.presetGUID) << extra;
    }

    encoder_state = {};
    fail_guard.disable();
    return true;
  }

  void
  nvenc_base::destroy_encoder() {
    if (output_bitstream) {
      if (nvenc_failed(nvenc->nvEncDestroyBitstreamBuffer(encoder, output_bitstream))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyBitstreamBuffer() failed: " << last_nvenc_error_string;
      }
      output_bitstream = nullptr;
    }
    if (encoder && async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = { NV_ENC_EVENT_PARAMS_VER };
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncUnregisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnregisterAsyncEvent() failed: " << last_nvenc_error_string;
      }
    }
    if (registered_input_buffer) {
      if (nvenc_failed(nvenc->nvEncUnregisterResource(encoder, registered_input_buffer))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnregisterResource() failed: " << last_nvenc_error_string;
      }
      registered_input_buffer = nullptr;
    }
    if (encoder) {
      if (nvenc_failed(nvenc->nvEncDestroyEncoder(encoder))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyEncoder() failed: " << last_nvenc_error_string;
      }
      encoder = nullptr;
    }

    encoder_state = {};
    encoder_params = {};
  }

  nvenc_encoded_frame
  nvenc_base::encode_frame(uint64_t frame_index, bool force_idr) {
    if (!encoder) {
      return {};
    }

    assert(registered_input_buffer);
    assert(output_bitstream);

    if (!synchronize_input_buffer()) {
      BOOST_LOG(error) << "NvEnc: failed to synchronize input buffer";
      return {};
    }

    NV_ENC_MAP_INPUT_RESOURCE mapped_input_buffer = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    mapped_input_buffer.registeredResource = registered_input_buffer;

    if (nvenc_failed(nvenc->nvEncMapInputResource(encoder, &mapped_input_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncMapInputResource() failed: " << last_nvenc_error_string;
      return {};
    }
    auto unmap_guard = util::fail_guard([&] {
      if (nvenc_failed(nvenc->nvEncUnmapInputResource(encoder, mapped_input_buffer.mappedResource))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnmapInputResource() failed: " << last_nvenc_error_string;
      }
    });

    NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
    pic_params.inputWidth = encoder_params.width;
    pic_params.inputHeight = encoder_params.height;
    pic_params.encodePicFlags = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
    pic_params.inputTimeStamp = frame_index;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic_params.inputBuffer = mapped_input_buffer.mappedResource;
    pic_params.bufferFmt = mapped_input_buffer.mappedBufferFmt;
    pic_params.outputBitstream = output_bitstream;
    pic_params.completionEvent = async_event_handle;

    if (nvenc_failed(nvenc->nvEncEncodePicture(encoder, &pic_params))) {
      BOOST_LOG(error) << "NvEnc: NvEncEncodePicture() failed: " << last_nvenc_error_string;
      return {};
    }

    NV_ENC_LOCK_BITSTREAM lock_bitstream = { NV_ENC_LOCK_BITSTREAM_VER };
    lock_bitstream.outputBitstream = output_bitstream;
    lock_bitstream.doNotWait = 0;

    if (async_event_handle && !wait_for_async_event(100)) {
      BOOST_LOG(error) << "NvEnc: frame " << frame_index << " encode wait timeout";
      return {};
    }

    if (nvenc_failed(nvenc->nvEncLockBitstream(encoder, &lock_bitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncLockBitstream() failed: " << last_nvenc_error_string;
      return {};
    }

    auto data_pointer = (uint8_t *) lock_bitstream.bitstreamBufferPtr;
    nvenc_encoded_frame encoded_frame {
      { data_pointer, data_pointer + lock_bitstream.bitstreamSizeInBytes },
      lock_bitstream.outputTimeStamp,
      lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR,
      encoder_state.rfi_needs_confirmation,
    };

    if (encoder_state.rfi_needs_confirmation) {
      // Invalidation request has been fulfilled, and video network packet will be marked as such
      encoder_state.rfi_needs_confirmation = false;
    }

    encoder_state.last_encoded_frame_index = frame_index;

    if (encoded_frame.idr) {
      BOOST_LOG(debug) << "NvEnc: idr frame " << encoded_frame.frame_index;
    }

    if (nvenc_failed(nvenc->nvEncUnlockBitstream(encoder, lock_bitstream.outputBitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncUnlockBitstream() failed: " << last_nvenc_error_string;
    }

    encoder_state.frame_size_logger.collect_and_log(encoded_frame.data.size() / 1000.);

    return encoded_frame;
  }

  bool
  nvenc_base::invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) {
    if (!encoder || !encoder_params.rfi) return false;

    if (first_frame >= encoder_state.last_rfi_range.first &&
        last_frame <= encoder_state.last_rfi_range.second) {
      BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " already done";
      return true;
    }

    encoder_state.rfi_needs_confirmation = true;

    if (last_frame < first_frame) {
      BOOST_LOG(error) << "NvEnc: invaid rfi request " << first_frame << "-" << last_frame << ", generating IDR";
      return false;
    }

    BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame
                     << " expanding to last encoded frame " << encoder_state.last_encoded_frame_index;
    last_frame = encoder_state.last_encoded_frame_index;

    encoder_state.last_rfi_range = { first_frame, last_frame };

    if (last_frame - first_frame + 1 >= encoder_params.ref_frames_in_dpb) {
      BOOST_LOG(debug) << "NvEnc: rfi request too large, generating IDR";
      return false;
    }

    for (auto i = first_frame; i <= last_frame; i++) {
      if (nvenc_failed(nvenc->nvEncInvalidateRefFrames(encoder, i))) {
        BOOST_LOG(error) << "NvEnc: NvEncInvalidateRefFrames() " << i << " failed: " << last_nvenc_error_string;
        return false;
      }
    }

    return true;
  }

  bool
  nvenc_base::nvenc_failed(NVENCSTATUS status) {
    auto status_string = [](NVENCSTATUS status) -> std::string {
      switch (status) {
#define nvenc_status_case(x) \
  case x:                    \
    return #x;
        nvenc_status_case(NV_ENC_SUCCESS);
        nvenc_status_case(NV_ENC_ERR_NO_ENCODE_DEVICE);
        nvenc_status_case(NV_ENC_ERR_UNSUPPORTED_DEVICE);
        nvenc_status_case(NV_ENC_ERR_INVALID_ENCODERDEVICE);
        nvenc_status_case(NV_ENC_ERR_INVALID_DEVICE);
        nvenc_status_case(NV_ENC_ERR_DEVICE_NOT_EXIST);
        nvenc_status_case(NV_ENC_ERR_INVALID_PTR);
        nvenc_status_case(NV_ENC_ERR_INVALID_EVENT);
        nvenc_status_case(NV_ENC_ERR_INVALID_PARAM);
        nvenc_status_case(NV_ENC_ERR_INVALID_CALL);
        nvenc_status_case(NV_ENC_ERR_OUT_OF_MEMORY);
        nvenc_status_case(NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
        nvenc_status_case(NV_ENC_ERR_UNSUPPORTED_PARAM);
        nvenc_status_case(NV_ENC_ERR_LOCK_BUSY);
        nvenc_status_case(NV_ENC_ERR_NOT_ENOUGH_BUFFER);
        nvenc_status_case(NV_ENC_ERR_INVALID_VERSION);
        nvenc_status_case(NV_ENC_ERR_MAP_FAILED);
        nvenc_status_case(NV_ENC_ERR_NEED_MORE_INPUT);
        nvenc_status_case(NV_ENC_ERR_ENCODER_BUSY);
        nvenc_status_case(NV_ENC_ERR_EVENT_NOT_REGISTERD);
        nvenc_status_case(NV_ENC_ERR_GENERIC);
        nvenc_status_case(NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY);
        nvenc_status_case(NV_ENC_ERR_UNIMPLEMENTED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_REGISTER_FAILED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_NOT_REGISTERED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_NOT_MAPPED);
        // Newer versions of sdk may add more constants, look for them at the end of NVENCSTATUS enum
#undef nvenc_status_case
        default:
          return std::to_string(status);
      }
    };

    last_nvenc_error_string.clear();
    if (status != NV_ENC_SUCCESS) {
      /* This API function gives broken strings more often than not
      if (nvenc && encoder) {
        last_nvenc_error_string = nvenc->nvEncGetLastErrorString(encoder);
        if (!last_nvenc_error_string.empty()) last_nvenc_error_string += " ";
      }
      */
      last_nvenc_error_string += status_string(status);
      return true;
    }

    return false;
  }
}
