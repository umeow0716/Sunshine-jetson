/**
 * @file src/jetson/jetson_encoder.h
 * @brief NVIDIA Jetson GStreamer hardware encoder declarations.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace jetson {
  /**
   * @brief Video codec implemented by the Jetson hardware encoder.
   */
  enum class codec_e {
    h264,  ///< H.264/AVC using `nvv4l2h264enc`.
    hevc,  ///< H.265/HEVC using `nvv4l2h265enc`.
  };

  /**
   * @brief Raw pixel format accepted by the Jetson GStreamer pipeline.
   */
  enum class pixel_format_e {
    nv12,  ///< 8-bit NV12 4:2:0.
    p010,  ///< 10-bit P010 4:2:0.
  };

  /**
   * @brief Colorimetry attached to raw GStreamer input caps.
   */
  enum class colorspace_e {
    rec601,  ///< Rec. 601 SDR.
    rec709,  ///< Rec. 709 SDR.
    bt2020_sdr,  ///< Rec. 2020 SDR.
    bt2020_pq,  ///< Rec. 2020 with SMPTE ST 2084 PQ transfer.
  };

  /**
   * @brief Settings used to construct one Jetson encoding pipeline.
   */
  struct pipeline_config_t {
    codec_e codec;  ///< Codec produced by the hardware encoder.
    pixel_format_e pixel_format;  ///< Raw input pixel format.
    colorspace_e colorspace;  ///< Input and encoded-stream colorimetry.
    int width;  ///< Encoded width in pixels.
    int height;  ///< Encoded height in pixels.
    int framerate_numerator;  ///< Frame-rate numerator.
    int framerate_denominator;  ///< Frame-rate denominator.
    std::uint32_t bitrate;  ///< Target constant bitrate in bits per second.
    std::uint32_t reference_frames;  ///< Number of reference frames retained by the encoder.
    bool full_range;  ///< Whether luma and chroma use full-range values.
  };

  /**
   * @brief Plane pointers and strides for one NV12 or P010 input frame.
   */
  struct frame_view_t {
    const std::uint8_t *luma;  ///< First byte of the luma plane.
    const std::uint8_t *chroma;  ///< First byte of the interleaved chroma plane.
    int luma_stride;  ///< Bytes between adjacent luma rows.
    int chroma_stride;  ///< Bytes between adjacent chroma rows.
  };

  /**
   * @brief Encoded access unit returned by the Jetson pipeline.
   */
  struct encoded_frame_t {
    std::vector<std::uint8_t> data;  ///< Annex-B encoded access-unit bytes.
    std::uint64_t frame_index;  ///< Sunshine frame index associated with the access unit.
    bool idr;  ///< Whether the access unit contains an IDR picture.
  };

  /**
   * @brief Return the GStreamer element name used for a codec.
   *
   * @param codec Codec whose encoder element is requested.
   * @return Stable NVIDIA GStreamer element name.
   */
  std::string_view element_name(codec_e codec);

  /**
   * @brief Detect an IDR NAL unit in an Annex-B access unit.
   *
   * @param codec Codec used to interpret NAL-unit headers.
   * @param data Annex-B byte stream to inspect.
   * @return True when an H.264 or HEVC IDR NAL unit is present.
   */
  bool contains_idr(codec_e codec, std::span<const std::uint8_t> data);

  /**
   * @brief Synchronous low-latency wrapper around Jetson GStreamer encoding.
   */
  class encoder_t {
  public:
    /**
     * @brief Construct an uninitialized Jetson encoder.
     */
    encoder_t();

    /**
     * @brief Stop the GStreamer pipeline and release its elements.
     */
    ~encoder_t();

    encoder_t(const encoder_t &) = delete;
    encoder_t &operator=(const encoder_t &) = delete;
    encoder_t(encoder_t &&) = delete;
    encoder_t &operator=(encoder_t &&) = delete;

    /**
     * @brief Check whether the required Jetson GStreamer factories are installed.
     *
     * @param codec Codec whose factory should be checked.
     * @return True when appsrc, nvvidconv, the encoder, and appsink are available.
     */
    static bool is_available(codec_e codec);

    /**
     * @brief Construct and start the hardware encoding pipeline.
     *
     * @param config Pipeline dimensions, format, rate, and color settings.
     * @return True when the pipeline reaches the playing state.
     */
    bool start(const pipeline_config_t &config);

    /**
     * @brief Encode one raw frame and wait for its encoded access unit.
     *
     * @param frame Raw NV12 or P010 plane view.
     * @param frame_index Monotonic Sunshine frame index.
     * @param force_idr Whether the hardware encoder must emit an IDR picture.
     * @return Encoded access unit, or no value when the pipeline fails.
     */
    std::optional<encoded_frame_t> encode(const frame_view_t &frame, std::uint64_t frame_index, bool force_idr);

    /**
     * @brief Return the most recent initialization or encoding error.
     *
     * @return Human-readable error string owned by this encoder.
     */
    std::string_view last_error() const;

  private:
    /**
     * @brief GStreamer implementation hidden from consumers of this header.
     */
    struct impl_t;

    std::unique_ptr<impl_t> impl_;  ///< Owned GStreamer pipeline implementation.
  };
}  // namespace jetson
