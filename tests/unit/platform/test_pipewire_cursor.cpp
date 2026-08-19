/**
 * @file tests/unit/platform/test_pipewire_cursor.cpp
 * @brief Tests for PipeWire cursor metadata parsing and composition.
 */

#ifdef SUNSHINE_BUILD_PORTAL

  // standard includes
  #include <cstring>
  #include <vector>

  // lib includes
  #include <gtest/gtest.h>
  #include <spa/param/video/raw.h>

  // local includes
  #include "src/platform/linux/pipewire_cursor.h"

namespace {
  /**
   * @brief Owned PipeWire cursor metadata used by parser tests.
   */
  class cursor_metadata_fixture_t {
  public:
    /**
     * @brief Create metadata containing a tightly packed cursor bitmap.
     *
     * @param width Bitmap width in pixels.
     * @param height Bitmap height in pixels.
     * @param format SPA video format of the bitmap bytes.
     */
    cursor_metadata_fixture_t(std::uint32_t width, std::uint32_t height, std::uint32_t format):
        bytes_(pipewire::cursor::metadata_size(width, height)),
        metadata_ {SPA_META_Cursor, static_cast<std::uint32_t>(bytes_.size()), bytes_.data()} {
      auto *cursor = this->cursor();
      cursor->id = 1;
      cursor->bitmap_offset = sizeof(spa_meta_cursor);

      auto *bitmap = this->bitmap();
      bitmap->format = format;
      bitmap->size = SPA_RECTANGLE(width, height);
      bitmap->stride = static_cast<std::int32_t>(width * 4);
      bitmap->offset = sizeof(spa_meta_bitmap);
    }

    /**
     * @brief Return the mutable cursor structure.
     *
     * @return Cursor metadata stored by the fixture.
     */
    spa_meta_cursor *cursor() {
      return reinterpret_cast<spa_meta_cursor *>(bytes_.data());
    }

    /**
     * @brief Return the mutable bitmap structure.
     *
     * @return Bitmap metadata stored by the fixture.
     */
    spa_meta_bitmap *bitmap() {
      return reinterpret_cast<spa_meta_bitmap *>(bytes_.data() + sizeof(spa_meta_cursor));
    }

    /**
     * @brief Return the mutable bitmap pixels.
     *
     * @return Pointer to tightly packed bitmap bytes.
     */
    std::uint8_t *pixels() {
      return reinterpret_cast<std::uint8_t *>(bitmap()) + sizeof(spa_meta_bitmap);
    }

    /**
     * @brief Return the SPA metadata wrapper.
     *
     * @return Metadata element referencing the owned bytes.
     */
    const spa_meta *metadata() const {
      return &metadata_;
    }

  private:
    std::vector<std::uint8_t> bytes_;  ///< Cursor, bitmap, and inline pixel allocation.
    spa_meta metadata_;  ///< SPA wrapper referencing the owned allocation.
  };
}  // namespace

TEST(PipeWireCursorTest, ParsesBitmapAndRetainsItForPositionUpdates) {
  cursor_metadata_fixture_t fixture {2, 1, SPA_VIDEO_FORMAT_BGRA};
  fixture.cursor()->position = SPA_POINT(20, 30);
  fixture.cursor()->hotspot = SPA_POINT(2, 3);
  const std::uint8_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
  std::memcpy(fixture.pixels(), expected, sizeof(expected));

  pipewire::cursor::state_t state;
  EXPECT_EQ(pipewire::cursor::update(state, fixture.metadata()), pipewire::cursor::update_e::bitmap);
  EXPECT_TRUE(state.visible);
  EXPECT_EQ(state.x, 18);
  EXPECT_EQ(state.y, 27);
  EXPECT_EQ(state.width, 2U);
  EXPECT_EQ(state.height, 1U);
  EXPECT_EQ(state.pixels, std::vector<std::uint8_t>(std::begin(expected), std::end(expected)));
  const auto serial = state.serial;

  spa_meta_cursor position_cursor {};
  position_cursor.id = 1;
  position_cursor.position = SPA_POINT(50, 60);
  position_cursor.hotspot = SPA_POINT(2, 3);
  spa_meta position_metadata {SPA_META_Cursor, sizeof(position_cursor), &position_cursor};
  EXPECT_EQ(pipewire::cursor::update(state, &position_metadata), pipewire::cursor::update_e::position);
  EXPECT_EQ(state.x, 48);
  EXPECT_EQ(state.y, 57);
  EXPECT_EQ(state.serial, serial);
  EXPECT_EQ(state.pixels, std::vector<std::uint8_t>(std::begin(expected), std::end(expected)));
}

TEST(PipeWireCursorTest, NormalizesRgbaBitmapToBgra) {
  cursor_metadata_fixture_t fixture {1, 1, SPA_VIDEO_FORMAT_RGBA};
  const std::uint8_t rgba[] = {10, 20, 30, 40};
  std::memcpy(fixture.pixels(), rgba, sizeof(rgba));

  pipewire::cursor::state_t state;
  ASSERT_EQ(pipewire::cursor::update(state, fixture.metadata()), pipewire::cursor::update_e::bitmap);
  EXPECT_EQ(state.pixels, (std::vector<std::uint8_t> {30, 20, 10, 40}));
}

TEST(PipeWireCursorTest, NormalizesOpaqueXFormats) {
  cursor_metadata_fixture_t bgrx_fixture {1, 1, SPA_VIDEO_FORMAT_BGRx};
  const std::uint8_t bgrx[] = {10, 20, 30, 0};
  std::memcpy(bgrx_fixture.pixels(), bgrx, sizeof(bgrx));

  pipewire::cursor::state_t state;
  ASSERT_EQ(pipewire::cursor::update(state, bgrx_fixture.metadata()), pipewire::cursor::update_e::bitmap);
  EXPECT_EQ(state.pixels, (std::vector<std::uint8_t> {10, 20, 30, 255}));

  cursor_metadata_fixture_t rgbx_fixture {1, 1, SPA_VIDEO_FORMAT_RGBx};
  const std::uint8_t rgbx[] = {10, 20, 30, 0};
  std::memcpy(rgbx_fixture.pixels(), rgbx, sizeof(rgbx));
  ASSERT_EQ(pipewire::cursor::update(state, rgbx_fixture.metadata()), pipewire::cursor::update_e::bitmap);
  EXPECT_EQ(state.pixels, (std::vector<std::uint8_t> {30, 20, 10, 255}));
}

TEST(PipeWireCursorTest, HandlesHiddenAndAbsentCursorMetadata) {
  cursor_metadata_fixture_t fixture {1, 1, SPA_VIDEO_FORMAT_BGRA};
  fixture.pixels()[3] = 255;
  pipewire::cursor::state_t state;
  ASSERT_EQ(pipewire::cursor::update(state, fixture.metadata()), pipewire::cursor::update_e::bitmap);

  fixture.bitmap()->offset = 0;
  EXPECT_EQ(pipewire::cursor::update(state, fixture.metadata()), pipewire::cursor::update_e::hidden);
  EXPECT_FALSE(state.visible);
  EXPECT_TRUE(state.pixels.empty());

  spa_meta_cursor no_update_cursor {};
  spa_meta no_update_metadata {SPA_META_Cursor, sizeof(no_update_cursor), &no_update_cursor};
  EXPECT_EQ(pipewire::cursor::update(state, &no_update_metadata), pipewire::cursor::update_e::none);
  EXPECT_EQ(pipewire::cursor::update(state, nullptr), pipewire::cursor::update_e::none);
}

TEST(PipeWireCursorTest, RejectsMalformedMetadataWithoutChangingState) {
  pipewire::cursor::state_t state;
  state.visible = true;
  state.x = 7;
  state.pixels = {1, 2, 3, 4};

  spa_meta_cursor cursor {};
  cursor.id = 1;
  cursor.bitmap_offset = sizeof(cursor) + 64;
  spa_meta metadata {SPA_META_Cursor, sizeof(cursor), &cursor};
  EXPECT_EQ(pipewire::cursor::update(state, &metadata), pipewire::cursor::update_e::invalid);
  EXPECT_TRUE(state.visible);
  EXPECT_EQ(state.x, 7);
  EXPECT_EQ(state.pixels, (std::vector<std::uint8_t> {1, 2, 3, 4}));
}

TEST(PipeWireCursorTest, RejectsInvalidBitmapLayouts) {
  pipewire::cursor::state_t state;
  spa_meta_cursor cursor {};
  cursor.id = 1;
  spa_meta short_metadata {SPA_META_Cursor, sizeof(cursor) - 1, &cursor};
  EXPECT_EQ(pipewire::cursor::update(state, &short_metadata), pipewire::cursor::update_e::invalid);

  cursor_metadata_fixture_t unsupported {1, 1, SPA_VIDEO_FORMAT_NV12};
  EXPECT_EQ(pipewire::cursor::update(state, unsupported.metadata()), pipewire::cursor::update_e::invalid);

  cursor_metadata_fixture_t oversized {pipewire::cursor::max_dimension + 1, 1, SPA_VIDEO_FORMAT_BGRA};
  EXPECT_EQ(pipewire::cursor::update(state, oversized.metadata()), pipewire::cursor::update_e::invalid);

  cursor_metadata_fixture_t invalid_stride {1, 1, SPA_VIDEO_FORMAT_BGRA};
  invalid_stride.bitmap()->stride = -1;
  EXPECT_EQ(pipewire::cursor::update(state, invalid_stride.metadata()), pipewire::cursor::update_e::invalid);

  cursor_metadata_fixture_t truncated {1, 1, SPA_VIDEO_FORMAT_BGRA};
  auto truncated_metadata = *truncated.metadata();
  --truncated_metadata.size;
  EXPECT_EQ(pipewire::cursor::update(state, &truncated_metadata), pipewire::cursor::update_e::invalid);
}

TEST(PipeWireCursorTest, BlendsPremultipliedBgraAndClipsAtFrameEdges) {
  pipewire::cursor::state_t state;
  state.visible = true;
  state.x = -1;
  state.y = 0;
  state.width = 2;
  state.height = 1;
  state.pixels = {
    0,
    0,
    0,
    0,
    10,
    20,
    30,
    128,
  };
  std::vector<std::uint8_t> frame {
    100,
    100,
    100,
    0,
    50,
    50,
    50,
    0,
  };

  ASSERT_TRUE(pipewire::cursor::blend(state, frame, 2, 1, 8, SPA_VIDEO_FORMAT_BGRA));
  EXPECT_EQ(frame, (std::vector<std::uint8_t> {
                     60,
                     70,
                     80,
                     255,
                     50,
                     50,
                     50,
                     0,
                   }));
}

TEST(PipeWireCursorTest, BlendsIntoRgbaAndRejectsUnsupportedLayouts) {
  pipewire::cursor::state_t state;
  state.visible = true;
  state.width = 1;
  state.height = 1;
  state.pixels = {1, 2, 3, 255};
  std::vector<std::uint8_t> frame {10, 20, 30, 0};

  ASSERT_TRUE(pipewire::cursor::blend(state, frame, 1, 1, 4, SPA_VIDEO_FORMAT_RGBA));
  EXPECT_EQ(frame, (std::vector<std::uint8_t> {3, 2, 1, 255}));
  EXPECT_FALSE(pipewire::cursor::blend(state, frame, 1, 1, 4, SPA_VIDEO_FORMAT_NV12));
  EXPECT_FALSE(pipewire::cursor::blend(state, std::span<std::uint8_t> {frame.data(), 3}, 1, 1, 4, SPA_VIDEO_FORMAT_BGRA));
}

TEST(PipeWireCursorTest, HandlesInvisibleOffscreenAndUntrustedCursorPixels) {
  std::vector<std::uint8_t> frame {100, 100, 100, 0};
  pipewire::cursor::state_t state;
  EXPECT_TRUE(pipewire::cursor::blend(state, frame, 1, 1, 4, SPA_VIDEO_FORMAT_BGRA));
  EXPECT_EQ(frame, (std::vector<std::uint8_t> {100, 100, 100, 0}));

  state.visible = true;
  state.x = 2;
  state.width = 1;
  state.height = 1;
  state.pixels = {1, 2, 3, 255};
  EXPECT_TRUE(pipewire::cursor::blend(state, frame, 1, 1, 4, SPA_VIDEO_FORMAT_BGRx));
  EXPECT_EQ(frame, (std::vector<std::uint8_t> {100, 100, 100, 0}));

  state.x = 0;
  state.pixels = {250, 250, 250, 128};
  EXPECT_TRUE(pipewire::cursor::blend(state, frame, 1, 1, 4, SPA_VIDEO_FORMAT_BGRA));
  EXPECT_EQ(frame, (std::vector<std::uint8_t> {255, 255, 255, 255}));
}

#endif  // SUNSHINE_BUILD_PORTAL
