/**
 * @file src/platform/linux/vaapi_compat.cpp
 * @brief Compatibility definitions for libva versions used by FFmpeg.
 */

// standard includes
#include <cstdint>

extern "C" {
  /**
   * @brief Native VA display handle compatible with the libva ABI.
   */
  using VADisplay = void *;

  /**
   * @brief VA buffer identifier compatible with the libva ABI.
   */
  using VABufferID = unsigned int;

  /**
   * @brief VA status code compatible with the libva ABI.
   */
  using VAStatus = int;

  /**
   * @brief Map a VA buffer using the legacy libva entry point.
   *
   * @param dpy VA display.
   * @param buf_id VA buffer ID.
   * @param pbuf Output mapped buffer pointer.
   * @return VA status code.
   */
  VAStatus
    vaMapBuffer(
      VADisplay dpy,
      VABufferID buf_id,
      void **pbuf
    );

  /**
   * @brief Provide vaMapBuffer2 for libva versions before 2.21.0.
   *
   * The older API cannot honor mapping flags, which matches Sunshine's
   * upstream compatibility behavior for distributions that ship libva 2.20.
   *
   * @param dpy VA display.
   * @param buf_id VA buffer ID.
   * @param pbuf Output mapped buffer pointer.
   * @param flags Mapping flags unsupported by the legacy API.
   * @return VA status code.
   */
  VAStatus
    vaMapBuffer2(
      VADisplay dpy,
      VABufferID buf_id,
      void **pbuf,
      std::uint32_t flags
    ) {
    return vaMapBuffer(dpy, buf_id, pbuf);
  }
}
