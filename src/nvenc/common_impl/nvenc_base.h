/**
 * @file src/nvenc/common_impl/nvenc_base.h
 * @brief Declarations for abstract platform-agnostic base of standalone NVENC encoder.
 */
#pragma once

#include "../nvenc_config.h"
#include "../nvenc_encoded_frame.h"
#include "../nvenc_encoder.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/video.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
  #include <ffnvcodec/nvEncodeAPI.h>
namespace nvenc {
#endif

  /**
   * @brief Abstract platform-agnostic base of standalone NVENC encoder.
   *        Derived classes perform platform-specific operations.
   */
  class nvenc_base: virtual public nvenc_encoder {
  public:
    /**
     * @param device_type Underlying device type used by derived class.
     */
    explicit nvenc_base(NV_ENC_DEVICE_TYPE device_type);
    ~nvenc_base();

    bool
    create_encoder(const nvenc_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &sunshine_colorspace,
      platf::pix_fmt_e sunshine_buffer_format) override;

    void
    destroy_encoder() override;

    nvenc_encoded_frame
    encode_frame(uint64_t frame_index, bool force_idr) override;

    bool
    invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) override;

  protected:
    /**
     * @brief Required. Used for loading NvEnc library and setting `nvenc` variable with `NvEncodeAPICreateInstance()`.
     *        Called during `create_encoder()` if `nvenc` variable is not initialized.
     * @return `true` on success, `false` on error
     */
    virtual bool
    init_library() = 0;

    /**
     * @brief Required. Used for creating outside-facing input surface,
     *        registering this surface with `nvenc->nvEncRegisterResource()` and setting `registered_input_buffer` variable.
     *        Called during `create_encoder()`.
     * @return `true` on success, `false` on error
     */
    virtual bool
    create_and_register_input_buffer() = 0;

    /**
     * @brief Optional. Override if you must perform additional operations on the registered input surface in the beginning of `encode_frame()`.
     *        Typically used for interop copy.
     * @return `true` on success, `false` on error
     */
    virtual bool
    synchronize_input_buffer() { return true; }

    /**
     * @brief Optional. Override if you want to create encoder in async mode.
     *        In this case must also set `async_event_handle` variable.
     * @param timeout_ms Wait timeout in milliseconds
     * @return `true` on success, `false` on timeout or error
     */
    virtual bool
    wait_for_async_event(uint32_t timeout_ms) { return false; }

    bool
    nvenc_failed(NVENCSTATUS status);

    const NV_ENC_DEVICE_TYPE device_type;

    void *encoder = nullptr;

    struct {
      uint32_t width = 0;
      uint32_t height = 0;
      NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
      uint32_t ref_frames_in_dpb = 0;
      bool rfi = false;
    } encoder_params;

    std::string last_nvenc_error_string;

    // Derived classes set these variables
    void *device = nullptr;  ///< Platform-specific handle of encoding device.
                             ///< Should be set in constructor or `init_library()`.
    std::shared_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc;  ///< Function pointers list produced by `NvEncodeAPICreateInstance()`.
                                                         ///< Should be set in `init_library()`.
    NV_ENC_REGISTERED_PTR registered_input_buffer = nullptr;  ///< Platform-specific input surface registered with `NvEncRegisterResource()`.
                                                              ///< Should be set in `create_and_register_input_buffer()`.
    void *async_event_handle = nullptr;  ///< (optional) Platform-specific handle of event object event.
                                         ///< Can be set in constructor or `init_library()`, must override `wait_for_async_event()`.

  private:
    NV_ENC_OUTPUT_PTR output_bitstream = nullptr;

    struct {
      uint64_t last_encoded_frame_index = 0;
      bool rfi_needs_confirmation = false;
      std::pair<uint64_t, uint64_t> last_rfi_range;
      logging::min_max_avg_periodic_logger<double> frame_size_logger = { debug, "NvEnc: encoded frame sizes in kB", "" };
    } encoder_state;
  };
}
