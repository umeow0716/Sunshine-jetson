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
   * @brief Packed BGRx frame accepted by the Jetson VIC conversion path.
   */
  struct bgrx_frame_view_t {
    const std::uint8_t *data;  ///< First byte of the packed BGRx image.
    int width;  ///< Source image width in pixels.
    int height;  ///< Source image height in pixels.
    int row_stride;  ///< Bytes between adjacent packed image rows.
  };

  /**
   * @brief Packed RGB pixel formats accepted from a DMA-BUF capture source.
   */
  enum class dmabuf_pixel_format_e {
    bgrx,  ///< BGR with an unused fourth byte.
    bgra,  ///< BGR with alpha in the fourth byte.
    rgbx,  ///< RGB with an unused fourth byte.
    rgba,  ///< RGB with alpha in the fourth byte.
  };

  /**
   * @brief Single-plane packed RGB DMA-BUF imported by the Jetson VIC path.
   */
  struct dmabuf_frame_view_t {
    int fd;  ///< DMA-BUF file descriptor retained by the capture frame.
    int width;  ///< Source image width in pixels.
    int height;  ///< Source image height in pixels.
    std::uint32_t pitch;  ///< Bytes between adjacent packed image rows.
    std::uint32_t offset;  ///< Byte offset of the image within the DMA-BUF.
    std::uint64_t modifier;  ///< DRM layout modifier reported by the producer.
    dmabuf_pixel_format_e pixel_format;  ///< Packed RGB component order.
    bool y_invert;  ///< Whether VIC must vertically invert the imported frame.
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
     * @return True when appsrc, the encoder, appsink, and a usable input path are available.
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
     * @brief Convert a packed BGRx frame into a prepared NVMM encoder surface using VIC.
     *
     * @param frame Source image and layout.
     * @return True when a converted frame is ready for `encode_prepared()`.
     */
    bool prepare_bgrx(const bgrx_frame_view_t &frame);

    /**
     * @brief Import a packed RGB DMA-BUF and convert it into encoder NVMM using VIC.
     *
     * @param frame DMA-BUF descriptor and packed pixel layout.
     * @return True when an imported frame is ready for `encode_prepared()`.
     */
    bool prepare_dmabuf(const dmabuf_frame_view_t &frame);

    /**
     * @brief Submit the most recently prepared VIC frame to the hardware encoder.
     *
     * @param frame_index Monotonic Sunshine frame index.
     * @param force_idr Whether the hardware encoder must emit an IDR picture.
     * @return Encoded access unit, or no value when no frame is prepared or encoding fails.
     */
    std::optional<encoded_frame_t> encode_prepared(std::uint64_t frame_index, bool force_idr);

    /**
     * @brief Report whether VIC conversion can feed the active direct-NVMM pipeline.
     *
     * @return True when NvBufSurfTransform support and direct NVMM input are active.
     */
    bool supports_vic() const;

    /**
     * @brief Report whether the active pipeline accepts direct NVMM surfaces.
     *
     * @return True when frames bypass `nvvidconv` and enter the encoder as `NvBufSurface` objects.
     */
    bool uses_nvmm() const;

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
