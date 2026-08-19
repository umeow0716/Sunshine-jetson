/**
 * @file tests/unit/test_jetson_encoder.cpp
 * @brief Tests for NVIDIA Jetson GStreamer encoder helpers.
 */

#ifdef SUNSHINE_BUILD_JETSON

  // standard includes
  #include <algorithm>
  #include <array>
  #include <cstdint>
  #include <utility>
  #include <vector>

  // lib includes
  #include <gtest/gtest.h>

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
  }
}

#endif  // SUNSHINE_BUILD_JETSON
