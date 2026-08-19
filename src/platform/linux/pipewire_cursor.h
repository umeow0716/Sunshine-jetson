/**
 * @file src/platform/linux/pipewire_cursor.h
 * @brief PipeWire cursor metadata parsing and software composition helpers.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// lib includes
#include <spa/buffer/meta.h>

namespace pipewire::cursor {
  /**
   * @brief Maximum cursor edge length accepted from PipeWire metadata.
   */
  constexpr std::uint32_t max_dimension = 1024;

  /**
   * @brief Calculate the PipeWire allocation required for cursor metadata.
   *
   * @param width Maximum cursor bitmap width.
   * @param height Maximum cursor bitmap height.
   * @return Required metadata allocation in bytes.
   */
  constexpr std::size_t metadata_size(std::uint32_t width, std::uint32_t height) {
    return sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + static_cast<std::size_t>(width) * height * 4;
  }

  /**
   * @brief Result of applying one PipeWire cursor metadata block.
   */
  enum class update_e {
    none,  ///< Metadata contains no cursor update.
    position,  ///< Cursor position changed without a new bitmap.
    bitmap,  ///< Cursor bitmap and position changed.
    hidden,  ///< Cursor became hidden.
    invalid,  ///< Metadata was malformed or used an unsupported format.
  };

  /**
   * @brief Persistent normalized PipeWire cursor state.
   */
  struct state_t {
    bool visible = false;  ///< Whether a valid cursor bitmap is visible.
    std::int32_t x = 0;  ///< Cursor top-left X coordinate in stream pixels.
    std::int32_t y = 0;  ///< Cursor top-left Y coordinate in stream pixels.
    std::uint32_t width = 0;  ///< Cursor bitmap width in pixels.
    std::uint32_t height = 0;  ///< Cursor bitmap height in pixels.
    std::uint64_t serial = 0;  ///< Monotonic bitmap revision used by GPU upload paths.
    std::vector<std::uint8_t> pixels;  ///< Premultiplied BGRA cursor pixels.
  };

  /**
   * @brief Apply a PipeWire cursor metadata element to persistent state.
   *
   * Cursor bitmaps are normalized to tightly packed premultiplied BGRA. Position-only
   * updates retain the previous bitmap as required by the PipeWire protocol.
   *
   * @param state Persistent cursor state to update.
   * @param metadata PipeWire `SPA_META_Cursor` element, or `nullptr` when absent.
   * @return Classification of the applied update.
   */
  update_e update(state_t &state, const spa_meta *metadata);

  /**
   * @brief Alpha-blend a normalized cursor into a packed 8-bit RGB frame.
   *
   * @param state Cursor state to composite.
   * @param frame Writable frame bytes.
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @param stride Frame row stride in bytes.
   * @param format PipeWire SPA video format of the destination frame.
   * @return `true` when the format and frame layout are supported; otherwise `false`.
   */
  bool blend(
    const state_t &state,
    std::span<std::uint8_t> frame,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride,
    std::uint32_t format
  );
}  // namespace pipewire::cursor
