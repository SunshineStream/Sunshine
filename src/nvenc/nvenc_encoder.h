/**
 * @file src/nvenc/nvenc_encoder.h
 * @brief Declarations for NVENC encoder interface.
 */
#pragma once

#include "nvenc_config.h"
#include "nvenc_encoded_frame.h"

#include "src/platform/common.h"
#include "src/video.h"
#include "src/video_colorspace.h"

/**
 * @brief Standalone NVENC encoder
 */
namespace nvenc {

  /**
   * @brief Standalone NVENC encoder interface.
   */
  class nvenc_encoder {
  public:
    virtual ~nvenc_encoder() = default;

    /**
     * @brief Create the encoder.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error
     */
    virtual bool
    create_encoder(const nvenc_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format) = 0;

    /**
     * @brief Destroy the encoder.
     *        Also called in the destructor.
     */
    virtual void
    destroy_encoder() = 0;

    /**
     * @brief Encode the next frame using platform-specific input surface.
     * @param frame_index Frame index that uniquely identifies the frame.
     *        Afterwards serves as parameter for `invalidate_ref_frames()`.
     *        No restrictions on the first frame index, but later frame indexes must be subsequent.
     * @param force_idr Whether to encode frame as forced IDR.
     * @return Encoded frame.
     */
    virtual nvenc_encoded_frame
    encode_frame(uint64_t frame_index, bool force_idr) = 0;

    /**
     * @brief Perform reference frame invalidation (RFI) procedure.
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error.
     *         After error next frame must be encoded with `force_idr = true`.
     */
    virtual bool
    invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) = 0;
  };

}  // namespace nvenc
