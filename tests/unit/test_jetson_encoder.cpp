/**
 * @file tests/unit/test_jetson_encoder.cpp
 * @brief Tests for NVIDIA Jetson GStreamer encoder helpers.
 */

#ifdef SUNSHINE_BUILD_JETSON

  // standard includes
  #include <algorithm>
  #include <array>
  #include <cstdint>
  #include <memory>
  #include <utility>
  #include <vector>

  // lib includes
  #include <gtest/gtest.h>
  #ifdef SUNSHINE_BUILD_JETSON_VIC
    #include <nvbufsurface.h>
  #endif

  // local includes
  #include "src/jetson/jetson_encoder.h"

TEST(JetsonEncoderTest, ReturnsStableElementNames) {
  EXPECT_EQ(jetson::element_name(jetson::codec_e::h264), "nvv4l2h264enc");
  EXPECT_EQ(jetson::element_name(jetson::codec_e::hevc), "nvv4l2h265enc");
}

TEST(JetsonEncoderTest, DetectsH264IdrWithThreeAndFourByteStartCodes) {
  constexpr std::array<std::uint8_t, 15> access_unit {
    0x00,
    0x00,
    0x00,
    0x01,
    0x67,
    0x01,
    0x00,
    0x00,
    0x01,
    0x68,
    0x02,
    0x00,
    0x00,
    0x01,
    0x65,
  };
  EXPECT_TRUE(jetson::contains_idr(jetson::codec_e::h264, access_unit));
}

TEST(JetsonEncoderTest, RejectsH264AccessUnitWithoutIdr) {
  constexpr std::array<std::uint8_t, 10> access_unit {
    0x00,
    0x00,
    0x00,
    0x01,
    0x67,
    0x00,
    0x00,
    0x01,
    0x41,
    0x00,
  };
  EXPECT_FALSE(jetson::contains_idr(jetson::codec_e::h264, access_unit));
}

TEST(JetsonEncoderTest, DetectsBothHevcIdrNalTypes) {
  constexpr std::array<std::uint8_t, 5> idr_with_radl {0x00, 0x00, 0x01, 19 << 1, 0x01};
  constexpr std::array<std::uint8_t, 6> idr_without_radl {0x00, 0x00, 0x00, 0x01, 20 << 1, 0x01};
  EXPECT_TRUE(jetson::contains_idr(jetson::codec_e::hevc, idr_with_radl));
  EXPECT_TRUE(jetson::contains_idr(jetson::codec_e::hevc, idr_without_radl));
}

TEST(JetsonEncoderTest, RejectsTruncatedAndNonIdrHevcData) {
  constexpr std::array<std::uint8_t, 4> truncated_start_code {0x00, 0x00, 0x00, 0x01};
  constexpr std::array<std::uint8_t, 5> trail_picture {0x00, 0x00, 0x01, 0x02, 0x01};
  EXPECT_FALSE(jetson::contains_idr(jetson::codec_e::hevc, truncated_start_code));
  EXPECT_FALSE(jetson::contains_idr(jetson::codec_e::hevc, trail_picture));
}

TEST(JetsonEncoderTest, RejectsInvalidPipelineConfiguration) {
  jetson::encoder_t encoder;
  jetson::pipeline_config_t config {
    jetson::codec_e::h264,
    jetson::pixel_format_e::nv12,
    jetson::colorspace_e::rec709,
    0,
    240,
    30,
    1,
    1'000'000,
    1,
    false,
  };
  EXPECT_FALSE(encoder.start(config));
  EXPECT_EQ(encoder.last_error(), "Invalid Jetson encoder dimensions, frame rate, or bitrate");

  config.width = 320;
  config.pixel_format = jetson::pixel_format_e::p010;
  EXPECT_FALSE(encoder.start(config));
  EXPECT_EQ(encoder.last_error(), "Jetson H.264 streaming supports only 8-bit NV12 input");
}

TEST(JetsonEncoderTest, RejectsFrameBeforePipelineStarts) {
  jetson::encoder_t encoder;
  const jetson::frame_view_t frame {nullptr, nullptr, 0, 0};
  const jetson::bgrx_frame_view_t bgrx {nullptr, 0, 0, 0};
  const jetson::dmabuf_frame_view_t dmabuf {-1, 0, 0, 0, 0, 0, jetson::dmabuf_pixel_format_e::bgrx, false};
  EXPECT_FALSE(encoder.uses_nvmm());
  EXPECT_FALSE(encoder.supports_vic());
  EXPECT_FALSE(encoder.prepare_bgrx(bgrx));
  EXPECT_EQ(encoder.last_error(), "Jetson GStreamer pipeline is not running");
  EXPECT_FALSE(encoder.prepare_dmabuf(dmabuf));
  EXPECT_EQ(encoder.last_error(), "Jetson GStreamer pipeline is not running");
  EXPECT_FALSE(encoder.encode_prepared(0, false).has_value());
  EXPECT_EQ(encoder.last_error(), "Jetson GStreamer pipeline is not running");
  EXPECT_FALSE(encoder.encode(frame, 0, false).has_value());
  EXPECT_EQ(encoder.last_error(), "Jetson GStreamer pipeline is not running");
}

TEST(JetsonEncoderTest, EncodesSupportedFormatsOnAvailableJetsonHardware) {
  constexpr int width = 320;
  constexpr int height = 240;
  constexpr std::array formats {
    std::pair {jetson::codec_e::h264, jetson::pixel_format_e::nv12},
    std::pair {jetson::codec_e::hevc, jetson::pixel_format_e::nv12},
    std::pair {jetson::codec_e::hevc, jetson::pixel_format_e::p010},
  };

  for (const auto &[codec, pixel_format] : formats) {
    SCOPED_TRACE(::testing::Message() << jetson::element_name(codec) << " format=" << static_cast<int>(pixel_format));
    if (!jetson::encoder_t::is_available(codec)) {
      GTEST_SKIP() << jetson::element_name(codec) << " is not installed";
    }

    const auto bytes_per_sample = pixel_format == jetson::pixel_format_e::nv12 ? 1U : 2U;
    const auto row_bytes = static_cast<std::size_t>(width) * bytes_per_sample;
    const auto luma_size = row_bytes * height;
    std::vector<std::uint8_t> raw_frame(luma_size * 3 / 2, 0x80);
    std::fill_n(raw_frame.begin(), luma_size, 0x10);

    jetson::encoder_t encoder;
    const jetson::pipeline_config_t config {
      codec,
      pixel_format,
      jetson::colorspace_e::rec709,
      width,
      height,
      30,
      1,
      1'000'000,
      1,
      false,
    };
    ASSERT_TRUE(encoder.start(config)) << encoder.last_error();
  #ifdef SUNSHINE_BUILD_JETSON_NVMM
    EXPECT_TRUE(encoder.uses_nvmm());
  #else
    EXPECT_FALSE(encoder.uses_nvmm());
  #endif

    const jetson::frame_view_t frame {
      raw_frame.data(),
      raw_frame.data() + luma_size,
      static_cast<int>(row_bytes),
      static_cast<int>(row_bytes),
    };
    const auto encoded = encoder.encode(frame, 0, true);
    ASSERT_TRUE(encoded.has_value()) << encoder.last_error();
    EXPECT_FALSE(encoded->data.empty());
    EXPECT_EQ(encoded->frame_index, 0U);
    EXPECT_TRUE(encoded->idr);

  #ifdef SUNSHINE_BUILD_JETSON_VIC
    EXPECT_TRUE(encoder.supports_vic());
    constexpr int source_width = 640;
    constexpr int source_height = 360;
    std::vector<std::uint8_t> bgrx_frame(static_cast<std::size_t>(source_width) * source_height * 4, 0x40);
    const jetson::bgrx_frame_view_t bgrx {
      bgrx_frame.data(),
      source_width,
      source_height,
      source_width * 4,
    };
    ASSERT_TRUE(encoder.prepare_bgrx(bgrx)) << encoder.last_error();
    for (std::uint64_t frame_index = 1; frame_index <= 6; ++frame_index) {
      const auto vic_encoded = encoder.encode_prepared(frame_index, frame_index == 4);
      ASSERT_TRUE(vic_encoded.has_value()) << encoder.last_error();
      EXPECT_FALSE(vic_encoded->data.empty());
      EXPECT_EQ(vic_encoded->frame_index, frame_index);
      if (frame_index == 4) {
        EXPECT_TRUE(vic_encoded->idr);
      }
    }

    NvBufSurfaceCreateParams source_params {};
    source_params.gpuId = 0;
    source_params.width = source_width;
    source_params.height = source_height;
    source_params.colorFormat = NVBUF_COLOR_FORMAT_BGRx;
    source_params.layout = NVBUF_LAYOUT_BLOCK_LINEAR;
    source_params.memType = NVBUF_MEM_SURFACE_ARRAY;
    NvBufSurface *source = nullptr;
    ASSERT_EQ(NvBufSurfaceCreate(&source, 1, &source_params), 0);
    ASSERT_NE(source, nullptr);
    std::unique_ptr<NvBufSurface, decltype(&NvBufSurfaceDestroy)> source_owner {source, &NvBufSurfaceDestroy};
    source->numFilled = 1;
    ASSERT_EQ(NvBufSurfaceMemSet(source, 0, 0, 0x40), 0);

    NvBufSurfaceMapParams map_params {};
    ASSERT_EQ(NvBufSurfaceGetMapParams(source, 0, &map_params), 0);
    const auto modifier = source->surfaceList[0].layout == NVBUF_LAYOUT_PITCH ? 0U :
                                                                                0x10U | map_params.planes[0].blockheightlog2;
    const jetson::dmabuf_frame_view_t dmabuf {
      static_cast<int>(source->surfaceList[0].bufferDesc),
      source_width,
      source_height,
      map_params.planes[0].pitch,
      map_params.planes[0].offset,
      modifier,
      jetson::dmabuf_pixel_format_e::bgrx,
      true,
    };
    ASSERT_TRUE(encoder.prepare_dmabuf(dmabuf)) << encoder.last_error();
    const auto dmabuf_encoded = encoder.encode_prepared(7, false);
    EXPECT_EQ(NvBufSurfaceDestroy(source_owner.release()), 0);
    ASSERT_TRUE(dmabuf_encoded.has_value()) << encoder.last_error();
    EXPECT_FALSE(dmabuf_encoded->data.empty());
    EXPECT_EQ(dmabuf_encoded->frame_index, 7U);
  #else
    EXPECT_FALSE(encoder.supports_vic());
  #endif
  }
}

#endif  // SUNSHINE_BUILD_JETSON
