/**
 * @file src/platform/linux/pipewire_cursor.cpp
 * @brief PipeWire cursor metadata parsing and software composition helpers.
 */

// standard includes
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

// lib includes
#include <spa/param/video/raw.h>

// local includes
#include "pipewire_cursor.h"

namespace pipewire::cursor {
  namespace {
    /**
     * @brief Return whether a byte range is contained by a metadata allocation.
     *
     * @param offset Byte offset from the metadata base.
     * @param size Requested byte count.
     * @param allocation_size Total metadata allocation size.
     * @return `true` when the complete range is contained by the allocation.
     */
    bool contains(std::size_t offset, std::size_t size, std::size_t allocation_size) {
      return offset <= allocation_size && size <= allocation_size - offset;
    }

    /**
     * @brief Convert one supported SPA pixel into normalized BGRA.
     *
     * @param input Source four-byte pixel.
     * @param output Destination BGRA pixel.
     * @param format SPA video format describing the source bytes.
     */
    void normalize_pixel(const std::uint8_t *input, std::uint8_t *output, std::uint32_t format) {
      switch (format) {
        case SPA_VIDEO_FORMAT_BGRA:
          std::copy_n(input, 4, output);
          break;
        case SPA_VIDEO_FORMAT_BGRx:
          std::copy_n(input, 3, output);
          output[3] = 255;
          break;
        case SPA_VIDEO_FORMAT_RGBA:
          output[0] = input[2];
          output[1] = input[1];
          output[2] = input[0];
          output[3] = input[3];
          break;
        case SPA_VIDEO_FORMAT_RGBx:
          output[0] = input[2];
          output[1] = input[1];
          output[2] = input[0];
          output[3] = 255;
          break;
      }
    }

    /**
     * @brief Return whether a packed SPA format is supported by the cursor helpers.
     *
     * @param format SPA video format identifier.
     * @return `true` for supported packed 8-bit formats.
     */
    bool supported_format(std::uint32_t format) {
      return format == SPA_VIDEO_FORMAT_BGRA || format == SPA_VIDEO_FORMAT_BGRx ||
             format == SPA_VIDEO_FORMAT_RGBA || format == SPA_VIDEO_FORMAT_RGBx;
    }

    /**
     * @brief Subtract cursor coordinates without signed overflow.
     *
     * @param position Cursor position reported by PipeWire.
     * @param hotspot Cursor hotspot offset reported by PipeWire.
     * @return Saturated top-left coordinate.
     */
    std::int32_t cursor_origin(std::int32_t position, std::int32_t hotspot) {
      const auto origin = static_cast<std::int64_t>(position) - hotspot;
      return static_cast<std::int32_t>(std::clamp<std::int64_t>(origin, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    }

    /**
     * @brief Blend one premultiplied cursor channel over a destination channel.
     *
     * @param destination Existing destination channel.
     * @param source Premultiplied cursor channel.
     * @param alpha Cursor alpha channel.
     * @return Blended channel value.
     */
    std::uint8_t blend_channel(std::uint8_t destination, std::uint8_t source, std::uint8_t alpha) {
      const auto blended = static_cast<unsigned>(source) + (static_cast<unsigned>(destination) * (255U - alpha) + 127U) / 255U;
      return static_cast<std::uint8_t>(std::min(blended, 255U));
    }
  }  // namespace

  update_e update(state_t &state, const spa_meta *metadata) {
    if (!metadata || metadata->type != SPA_META_Cursor || !metadata->data) {
      return update_e::none;
    }
    if (metadata->size < sizeof(spa_meta_cursor)) {
      return update_e::invalid;
    }

    const auto *cursor = static_cast<const spa_meta_cursor *>(metadata->data);
    if (!spa_meta_cursor_is_valid(cursor)) {
      return update_e::none;
    }

    if (cursor->bitmap_offset == 0) {
      state.x = cursor_origin(cursor->position.x, cursor->hotspot.x);
      state.y = cursor_origin(cursor->position.y, cursor->hotspot.y);
      return update_e::position;
    }

    const auto bitmap_offset = static_cast<std::size_t>(cursor->bitmap_offset);
    if (!contains(bitmap_offset, sizeof(spa_meta_bitmap), metadata->size)) {
      return update_e::invalid;
    }
    const auto *base = static_cast<const std::uint8_t *>(metadata->data);
    const auto *bitmap = reinterpret_cast<const spa_meta_bitmap *>(base + bitmap_offset);
    if (bitmap->offset == 0 || bitmap->size.width == 0 || bitmap->size.height == 0) {
      state.visible = false;
      state.x = cursor_origin(cursor->position.x, cursor->hotspot.x);
      state.y = cursor_origin(cursor->position.y, cursor->hotspot.y);
      state.width = 0;
      state.height = 0;
      state.pixels.clear();
      ++state.serial;
      return update_e::hidden;
    }
    if (!supported_format(bitmap->format) || bitmap->size.width > max_dimension || bitmap->size.height > max_dimension) {
      return update_e::invalid;
    }

    constexpr std::size_t bytes_per_pixel = 4;
    const auto row_bytes = static_cast<std::size_t>(bitmap->size.width) * bytes_per_pixel;
    if (bitmap->stride < 0 || static_cast<std::size_t>(bitmap->stride) < row_bytes) {
      return update_e::invalid;
    }
    const auto bitmap_data_offset = bitmap_offset + static_cast<std::size_t>(bitmap->offset);
    const auto bitmap_bytes = static_cast<std::size_t>(bitmap->stride) * (bitmap->size.height - 1U) + row_bytes;
    if (!contains(bitmap_data_offset, bitmap_bytes, metadata->size)) {
      return update_e::invalid;
    }

    std::vector<std::uint8_t> normalized(row_bytes * bitmap->size.height);
    const auto *source = base + bitmap_data_offset;
    for (std::uint32_t row = 0; row < bitmap->size.height; ++row) {
      const auto *source_row = source + static_cast<std::size_t>(row) * bitmap->stride;
      auto *destination_row = normalized.data() + static_cast<std::size_t>(row) * row_bytes;
      for (std::uint32_t column = 0; column < bitmap->size.width; ++column) {
        normalize_pixel(source_row + static_cast<std::size_t>(column) * bytes_per_pixel, destination_row + static_cast<std::size_t>(column) * bytes_per_pixel, bitmap->format);
      }
    }

    state.visible = true;
    state.x = cursor_origin(cursor->position.x, cursor->hotspot.x);
    state.y = cursor_origin(cursor->position.y, cursor->hotspot.y);
    state.width = bitmap->size.width;
    state.height = bitmap->size.height;
    state.pixels = std::move(normalized);
    ++state.serial;
    return update_e::bitmap;
  }

  bool blend(
    const state_t &state,
    std::span<std::uint8_t> frame,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride,
    std::uint32_t format
  ) {
    if (!supported_format(format) || width <= 0 || height <= 0 || static_cast<std::int64_t>(stride) < static_cast<std::int64_t>(width) * 4) {
      return false;
    }
    const auto required_size = static_cast<std::uint64_t>(stride) * (height - 1) + static_cast<std::uint64_t>(width) * 4;
    if (frame.size() < required_size) {
      return false;
    }
    if (!state.visible || state.width == 0 || state.height == 0 || state.pixels.size() != static_cast<std::size_t>(state.width) * state.height * 4) {
      return true;
    }

    const auto left = std::max<std::int64_t>(0, state.x);
    const auto top = std::max<std::int64_t>(0, state.y);
    const auto right = std::min<std::int64_t>(width, static_cast<std::int64_t>(state.x) + state.width);
    const auto bottom = std::min<std::int64_t>(height, static_cast<std::int64_t>(state.y) + state.height);
    if (left >= right || top >= bottom) {
      return true;
    }

    const bool destination_is_bgra = format == SPA_VIDEO_FORMAT_BGRA || format == SPA_VIDEO_FORMAT_BGRx;
    for (std::int64_t y = top; y < bottom; ++y) {
      auto *destination = frame.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(left) * 4;
      const auto source_y = static_cast<std::uint32_t>(y - state.y);
      const auto source_x = static_cast<std::uint32_t>(left - state.x);
      const auto *source = state.pixels.data() + (static_cast<std::size_t>(source_y) * state.width + source_x) * 4;
      for (std::int64_t x = left; x < right; ++x) {
        const auto alpha = source[3];
        const std::array<std::uint8_t, 3> source_channels = destination_is_bgra ?
                                                              std::array<std::uint8_t, 3> {source[0], source[1], source[2]} :
                                                              std::array<std::uint8_t, 3> {source[2], source[1], source[0]};
        destination[0] = blend_channel(destination[0], source_channels[0], alpha);
        destination[1] = blend_channel(destination[1], source_channels[1], alpha);
        destination[2] = blend_channel(destination[2], source_channels[2], alpha);
        destination[3] = 255;
        destination += 4;
        source += 4;
      }
    }
    return true;
  }
}  // namespace pipewire::cursor
