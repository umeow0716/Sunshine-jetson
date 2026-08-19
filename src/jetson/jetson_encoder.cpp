/**
 * @file src/jetson/jetson_encoder.cpp
 * @brief NVIDIA Jetson GStreamer hardware encoder implementation.
 */

// standard includes
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

// lib includes
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video-color.h>

// local includes
#include "jetson_encoder.h"

namespace {
  constexpr auto sample_timeout = 2 * GST_SECOND;  ///< Maximum time to wait for one encoded access unit.

  std::once_flag gstreamer_init_flag;  ///< Protects process-wide GStreamer initialization.
  bool gstreamer_initialized = false;  ///< Whether process-wide GStreamer initialization succeeded.

  /**
   * @brief Initialize GStreamer once for all Jetson encoder instances.
   *
   * @return True when GStreamer is ready for element creation.
   */
  bool initialize_gstreamer() {
    std::call_once(gstreamer_init_flag, []() {
      GError *error = nullptr;
      gstreamer_initialized = gst_init_check(nullptr, nullptr, &error);
      if (error) {
        g_error_free(error);
      }
    });
    return gstreamer_initialized;
  }

  /**
   * @brief Check that a GStreamer element factory exists.
   *
   * @param name Factory name to locate.
   * @return True when the factory is installed.
   */
  bool factory_available(const char *name) {
    auto *factory = gst_element_factory_find(name);
    if (!factory) {
      return false;
    }
    gst_object_unref(factory);
    return true;
  }

  /**
   * @brief Convert Jetson color settings to GStreamer colorimetry.
   *
   * @param config Pipeline color settings.
   * @return GStreamer colorimetry structure for raw caps.
   */
  GstVideoColorimetry make_colorimetry(const jetson::pipeline_config_t &config) {
    GstVideoColorimetry result {};
    result.range = config.full_range ? GST_VIDEO_COLOR_RANGE_0_255 : GST_VIDEO_COLOR_RANGE_16_235;

    switch (config.colorspace) {
      case jetson::colorspace_e::rec601:
        result.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
        result.transfer = GST_VIDEO_TRANSFER_BT601;
        result.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
        break;
      case jetson::colorspace_e::rec709:
        result.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
        result.transfer = GST_VIDEO_TRANSFER_BT709;
        result.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
        break;
      case jetson::colorspace_e::bt2020_sdr:
        result.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
        result.transfer = GST_VIDEO_TRANSFER_BT2020_10;
        result.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
        break;
      case jetson::colorspace_e::bt2020_pq:
        result.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
        result.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
        result.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
        break;
    }

    return result;
  }

  /**
   * @brief Locate the next Annex-B NAL-unit start code.
   *
   * @param data Byte stream being scanned.
   * @param offset First byte eligible to begin a start code.
   * @return Offset of the first NAL header after a start code, or `data.size()`.
   */
  std::size_t find_nal_header(std::span<const std::uint8_t> data, std::size_t offset) {
    for (auto index = offset; index + 3 <= data.size(); ++index) {
      if (data[index] != 0 || data[index + 1] != 0) {
        continue;
      }
      if (data[index + 2] == 1) {
        return index + 3;
      }
      if (index + 4 <= data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
        return index + 4;
      }
    }
    return data.size();
  }
}  // namespace

namespace jetson {
  std::string_view element_name(codec_e codec) {
    switch (codec) {
      case codec_e::h264:
        return "nvv4l2h264enc";
      case codec_e::hevc:
        return "nvv4l2h265enc";
    }
    return {};
  }

  bool contains_idr(codec_e codec, std::span<const std::uint8_t> data) {
    auto nal_header = find_nal_header(data, 0);
    while (nal_header < data.size()) {
      if (codec == codec_e::h264) {
        if ((data[nal_header] & 0x1F) == 5) {
          return true;
        }
      } else {
        const auto nal_type = (data[nal_header] >> 1) & 0x3F;
        if (nal_type == 19 || nal_type == 20) {
          return true;
        }
      }
      nal_header = find_nal_header(data, nal_header + 1);
    }
    return false;
  }

  /**
   * @brief Private GStreamer state for one Jetson encoder.
   */
  struct encoder_t::impl_t {
    /**
     * @brief Stop and release any active pipeline.
     */
    ~impl_t() {
      reset();
    }

    /**
     * @brief Stop the active pipeline and clear borrowed element pointers.
     */
    void reset() {
      if (pipeline) {
        if (started && source) {
          gst_app_src_end_of_stream(GST_APP_SRC(source));
          auto *bus = gst_element_get_bus(pipeline);
          if (auto *message = gst_bus_timed_pop_filtered(bus, GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR))) {
            gst_message_unref(message);
          }
          gst_object_unref(bus);
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
        gst_object_unref(pipeline);
      }
      pipeline = nullptr;
      source = nullptr;
      hardware_encoder = nullptr;
      sink = nullptr;
      started = false;
      produced_frame = false;
    }

    /**
     * @brief Copy the latest GStreamer bus error into `error`.
     *
     * @param fallback Message used when the bus contains no detailed error.
     */
    void read_bus_error(std::string_view fallback) {
      error.assign(fallback);
      if (!pipeline) {
        return;
      }

      auto *bus = gst_element_get_bus(pipeline);
      auto *message = gst_bus_timed_pop_filtered(bus, 0, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
      gst_object_unref(bus);
      if (!message) {
        return;
      }

      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *gst_error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &gst_error, &debug);
        if (gst_error && gst_error->message) {
          error.assign(gst_error->message);
        }
        if (debug) {
          error.append(" (").append(debug).append(")");
        }
        if (gst_error) {
          g_error_free(gst_error);
        }
        g_free(debug);
      }
      gst_message_unref(message);
    }

    GstElement *pipeline = nullptr;  ///< Pipeline that owns all child elements.
    GstElement *source = nullptr;  ///< Borrowed appsrc child pointer.
    GstElement *hardware_encoder = nullptr;  ///< Borrowed NVIDIA encoder child pointer.
    GstElement *sink = nullptr;  ///< Borrowed appsink child pointer.
    pipeline_config_t config {};  ///< Settings used by the active pipeline.
    std::string error;  ///< Most recent failure message.
    bool started = false;  ///< Whether the pipeline reached the playing state.
    bool produced_frame = false;  ///< Whether negotiation and one encoded output have completed.
  };

  encoder_t::encoder_t():
      impl_ {std::make_unique<impl_t>()} {
  }

  encoder_t::~encoder_t() = default;

  bool encoder_t::is_available(codec_e codec) {
    if (!initialize_gstreamer()) {
      return false;
    }
    return factory_available("appsrc") &&
           factory_available("nvvidconv") &&
           factory_available(element_name(codec).data()) &&
           factory_available("appsink");
  }

  bool encoder_t::start(const pipeline_config_t &config) {
    impl_->reset();
    impl_->error.clear();

    if (config.width <= 0 || config.height <= 0 ||
        config.framerate_numerator <= 0 || config.framerate_denominator <= 0 ||
        config.bitrate == 0) {
      impl_->error = "Invalid Jetson encoder dimensions, frame rate, or bitrate";
      return false;
    }
    if (config.codec == codec_e::h264 && config.pixel_format != pixel_format_e::nv12) {
      impl_->error = "Jetson H.264 streaming supports only 8-bit NV12 input";
      return false;
    }
    if (!is_available(config.codec)) {
      impl_->error = "Required Jetson GStreamer elements are not installed";
      return false;
    }

    impl_->pipeline = gst_pipeline_new("sunshine-jetson-pipeline");
    impl_->source = gst_element_factory_make("appsrc", "sunshine-jetson-source");
    auto *converter = gst_element_factory_make("nvvidconv", "sunshine-jetson-converter");
    auto *caps_filter = gst_element_factory_make("capsfilter", "sunshine-jetson-nvmm-caps");
    impl_->hardware_encoder = gst_element_factory_make(element_name(config.codec).data(), "sunshine-jetson-encoder");
    impl_->sink = gst_element_factory_make("appsink", "sunshine-jetson-sink");

    if (!impl_->pipeline || !impl_->source || !converter || !caps_filter || !impl_->hardware_encoder || !impl_->sink) {
      impl_->error = "Could not create the Jetson GStreamer pipeline elements";
      if (impl_->pipeline) {
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
      }
      for (auto *element : {impl_->source, converter, caps_filter, impl_->hardware_encoder, impl_->sink}) {
        if (element) {
          gst_object_unref(element);
        }
      }
      impl_->source = nullptr;
      impl_->hardware_encoder = nullptr;
      impl_->sink = nullptr;
      return false;
    }

    gst_bin_add_many(GST_BIN(impl_->pipeline), impl_->source, converter, caps_filter, impl_->hardware_encoder, impl_->sink, nullptr);
    if (!gst_element_link_many(impl_->source, converter, caps_filter, impl_->hardware_encoder, impl_->sink, nullptr)) {
      impl_->error = "Could not link the Jetson GStreamer pipeline";
      impl_->reset();
      return false;
    }

    const auto *format_name = config.pixel_format == pixel_format_e::nv12 ? "NV12" : "P010_10LE";
    auto colorimetry = make_colorimetry(config);
    auto *colorimetry_string = gst_video_colorimetry_to_string(&colorimetry);
    auto *source_caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      format_name,
      "width",
      G_TYPE_INT,
      config.width,
      "height",
      G_TYPE_INT,
      config.height,
      "framerate",
      GST_TYPE_FRACTION,
      config.framerate_numerator,
      config.framerate_denominator,
      "colorimetry",
      G_TYPE_STRING,
      colorimetry_string,
      nullptr
    );
    g_free(colorimetry_string);
    gst_app_src_set_caps(GST_APP_SRC(impl_->source), source_caps);
    gst_caps_unref(source_caps);

    auto *nvmm_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, format_name, nullptr);
    gst_caps_set_features(nvmm_caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
    g_object_set(caps_filter, "caps", nvmm_caps, nullptr);
    gst_caps_unref(nvmm_caps);

    const auto bytes_per_sample = config.pixel_format == pixel_format_e::nv12 ? 1U : 2U;
    const auto frame_bytes = static_cast<guint64>(config.width) * config.height * bytes_per_sample * 3 / 2;
    g_object_set(
      impl_->source,
      "is-live",
      TRUE,
      "format",
      GST_FORMAT_TIME,
      "block",
      TRUE,
      "max-bytes",
      frame_bytes * 8,
      nullptr
    );

    const auto reference_frames = std::clamp(config.reference_frames, 1U, 8U);
    const auto frames_per_second = std::max(1U, static_cast<std::uint32_t>(config.framerate_numerator / config.framerate_denominator));
    const auto keyframe_interval = frames_per_second * 60U;
    g_object_set(
      impl_->hardware_encoder,
      "bitrate",
      static_cast<guint>(config.bitrate),
      "control-rate",
      1,
      "ratecontrol-enable",
      TRUE,
      "preset-level",
      1,
      "num-B-Frames",
      0U,
      "num-Ref-Frames",
      static_cast<guint>(reference_frames),
      "iframeinterval",
      static_cast<guint>(keyframe_interval),
      "idrinterval",
      static_cast<guint>(keyframe_interval),
      "insert-sps-pps",
      TRUE,
      "insert-vui",
      TRUE,
      "copy-timestamp",
      TRUE,
      "extended-colorformat",
      config.full_range,
      "maxperf-enable",
      TRUE,
      nullptr
    );
    g_object_set(impl_->hardware_encoder, "profile", config.codec == codec_e::h264 ? 4 : (config.pixel_format == pixel_format_e::p010 ? 1 : 0), nullptr);
    g_object_set(
      impl_->sink,
      "sync",
      FALSE,
      "async",
      FALSE,
      "max-buffers",
      1U,
      "drop",
      FALSE,
      "wait-on-eos",
      FALSE,
      nullptr
    );

    impl_->config = config;
    const auto state_change = gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING);
    if (state_change == GST_STATE_CHANGE_FAILURE) {
      impl_->read_bus_error("Jetson GStreamer pipeline could not enter the playing state");
      impl_->reset();
      return false;
    }
    if (state_change == GST_STATE_CHANGE_ASYNC &&
        gst_element_get_state(impl_->pipeline, nullptr, nullptr, 5 * GST_SECOND) == GST_STATE_CHANGE_FAILURE) {
      impl_->read_bus_error("Jetson GStreamer pipeline did not finish starting");
      impl_->reset();
      return false;
    }

    impl_->started = true;
    return true;
  }

  std::optional<encoded_frame_t> encoder_t::encode(const frame_view_t &frame, std::uint64_t frame_index, bool force_idr) {
    if (!impl_->started || !impl_->pipeline) {
      impl_->error = "Jetson GStreamer pipeline is not running";
      return std::nullopt;
    }
    if (!frame.luma || !frame.chroma || frame.luma_stride <= 0 || frame.chroma_stride <= 0) {
      impl_->error = "Jetson encoder received an invalid raw frame";
      return std::nullopt;
    }

    const auto bytes_per_sample = impl_->config.pixel_format == pixel_format_e::nv12 ? 1U : 2U;
    const auto row_bytes = static_cast<std::size_t>(impl_->config.width) * bytes_per_sample;
    if (static_cast<std::size_t>(frame.luma_stride) < row_bytes || static_cast<std::size_t>(frame.chroma_stride) < row_bytes) {
      impl_->error = "Jetson encoder received a raw frame with undersized strides";
      return std::nullopt;
    }

    const auto frame_bytes = row_bytes * impl_->config.height * 3 / 2;
    auto *buffer = gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
    if (!buffer) {
      impl_->error = "Could not allocate a GStreamer input buffer";
      return std::nullopt;
    }

    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      gst_buffer_unref(buffer);
      impl_->error = "Could not map a GStreamer input buffer";
      return std::nullopt;
    }

    auto *destination = map.data;
    for (int row = 0; row < impl_->config.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * row_bytes, frame.luma + static_cast<std::size_t>(row) * frame.luma_stride, row_bytes);
    }
    destination += row_bytes * impl_->config.height;
    for (int row = 0; row < impl_->config.height / 2; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * row_bytes, frame.chroma + static_cast<std::size_t>(row) * frame.chroma_stride, row_bytes);
    }
    gst_buffer_unmap(buffer, &map);

    const auto duration = gst_util_uint64_scale(GST_SECOND, impl_->config.framerate_denominator, impl_->config.framerate_numerator);
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame_index, GST_SECOND * static_cast<guint64>(impl_->config.framerate_denominator), impl_->config.framerate_numerator);
    GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
    GST_BUFFER_DURATION(buffer) = duration;
    GST_BUFFER_OFFSET(buffer) = frame_index;

    // The first frame is inherently an IDR. Emitting this action while the
    // NVIDIA element is still negotiating can race inside libtegrav4l2.
    if (force_idr && impl_->produced_frame) {
      g_signal_emit_by_name(impl_->hardware_encoder, "force-IDR");
    }

    const auto flow = gst_app_src_push_buffer(GST_APP_SRC(impl_->source), buffer);
    if (flow != GST_FLOW_OK) {
      impl_->read_bus_error("Jetson GStreamer appsrc rejected an input frame");
      return std::nullopt;
    }

    auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->sink), sample_timeout);
    if (!sample) {
      impl_->read_bus_error("Timed out waiting for the Jetson hardware encoder");
      return std::nullopt;
    }

    auto *encoded_buffer = gst_sample_get_buffer(sample);
    GstMapInfo encoded_map {};
    if (!encoded_buffer || !gst_buffer_map(encoded_buffer, &encoded_map, GST_MAP_READ)) {
      gst_sample_unref(sample);
      impl_->error = "Could not map the Jetson encoded access unit";
      return std::nullopt;
    }

    encoded_frame_t result {
      std::vector<std::uint8_t>(encoded_map.data, encoded_map.data + encoded_map.size),
      frame_index,
      false,
    };
    result.idr = contains_idr(impl_->config.codec, result.data) || !GST_BUFFER_FLAG_IS_SET(encoded_buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    gst_buffer_unmap(encoded_buffer, &encoded_map);
    gst_sample_unref(sample);

    if (result.data.empty()) {
      impl_->error = "Jetson hardware encoder returned an empty access unit";
      return std::nullopt;
    }
    impl_->produced_frame = true;
    return result;
  }

  std::string_view encoder_t::last_error() const {
    return impl_->error;
  }
}  // namespace jetson
