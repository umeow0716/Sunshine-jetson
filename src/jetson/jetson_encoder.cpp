/**
 * @file src/jetson/jetson_encoder.cpp
 * @brief NVIDIA Jetson GStreamer hardware encoder implementation.
 */

// standard includes
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unistd.h>

// lib includes
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video-color.h>

#ifdef SUNSHINE_BUILD_JETSON_NVMM
  #include <nvbufsurface.h>
#endif
#ifdef SUNSHINE_BUILD_JETSON_VIC
  #include <nvbufsurftransform.h>
#endif

// local includes
#include "jetson_encoder.h"

namespace {
  constexpr auto sample_timeout = 2 * GST_SECOND;  ///< Maximum time to wait for one encoded access unit.

#ifdef SUNSHINE_BUILD_JETSON_NVMM
  constexpr std::size_t nvmm_surface_count = 4;  ///< Number of reusable encoder input surfaces.
#endif
#ifdef SUNSHINE_BUILD_JETSON_VIC
  std::mutex vic_transform_mutex;  ///< Serializes default-session VIC configuration and transforms.
#endif

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

#ifdef SUNSHINE_BUILD_JETSON_NVMM
  /**
   * @brief Convert Sunshine color settings to an NvBufSurface color format.
   *
   * @param config Pipeline pixel format, matrix, and range settings.
   * @return Matching semiplanar NVIDIA surface format.
   */
  NvBufSurfaceColorFormat make_nvmm_color_format(const jetson::pipeline_config_t &config) {
    if (config.pixel_format == jetson::pixel_format_e::p010) {
      switch (config.colorspace) {
        case jetson::colorspace_e::rec601:
          return config.full_range ? NVBUF_COLOR_FORMAT_NV12_10LE_ER : NVBUF_COLOR_FORMAT_NV12_10LE;
        case jetson::colorspace_e::rec709:
          return config.full_range ? NVBUF_COLOR_FORMAT_NV12_10LE_709_ER : NVBUF_COLOR_FORMAT_NV12_10LE_709;
        case jetson::colorspace_e::bt2020_sdr:
        case jetson::colorspace_e::bt2020_pq:
          return NVBUF_COLOR_FORMAT_NV12_10LE_2020;
      }
    }

    switch (config.colorspace) {
      case jetson::colorspace_e::rec601:
        return config.full_range ? NVBUF_COLOR_FORMAT_NV12_ER : NVBUF_COLOR_FORMAT_NV12;
      case jetson::colorspace_e::rec709:
        return config.full_range ? NVBUF_COLOR_FORMAT_NV12_709_ER : NVBUF_COLOR_FORMAT_NV12_709;
      case jetson::colorspace_e::bt2020_sdr:
      case jetson::colorspace_e::bt2020_pq:
        return NVBUF_COLOR_FORMAT_NV12_2020;
    }
    return NVBUF_COLOR_FORMAT_INVALID;
  }

  /**
   * @brief Initialize an encoder surface to black for future letterboxing.
   *
   * @param surface Newly allocated NV12 or P010 surface.
   * @param config Pixel depth and range that determine black sample values.
   * @param error Destination for mapping or synchronization errors.
   * @return True when both planes contain device-visible black values.
   */
  bool clear_nvmm_surface(NvBufSurface *surface, const jetson::pipeline_config_t &config, std::string &error) {
    if (config.pixel_format == jetson::pixel_format_e::nv12) {
      const auto luma_black = config.full_range ? 0 : 16;
      if (NvBufSurfaceMemSet(surface, 0, 0, luma_black) != 0 || NvBufSurfaceMemSet(surface, 0, 1, 128) != 0) {
        error = "Could not clear a Jetson NVMM encoder surface";
        return false;
      }
      return true;
    }

    if (NvBufSurfaceMap(surface, 0, -1, NVBUF_MAP_WRITE) != 0) {
      error = "Could not map a Jetson P010 encoder surface for initialization";
      return false;
    }
    if (NvBufSurfaceSyncForCpu(surface, 0, -1) != 0) {
      NvBufSurfaceUnMap(surface, 0, -1);
      error = "Could not synchronize a Jetson P010 encoder surface for initialization";
      return false;
    }

    const auto &surface_params = surface->surfaceList[0];
    const auto &planes = surface_params.planeParams;
    if (planes.num_planes < 2 || !surface_params.mappedAddr.addr[0] || !surface_params.mappedAddr.addr[1]) {
      NvBufSurfaceUnMap(surface, 0, -1);
      error = "Jetson P010 encoder surface has an incompatible plane layout";
      return false;
    }
    const auto luma_black = static_cast<std::uint16_t>(config.full_range ? 0 : (64 << 6));
    constexpr auto chroma_neutral = static_cast<std::uint16_t>(512 << 6);
    for (std::uint32_t row = 0; row < planes.height[0]; ++row) {
      auto *destination = static_cast<std::uint16_t *>(surface_params.mappedAddr.addr[0]) + row * planes.pitch[0] / sizeof(std::uint16_t);
      std::fill_n(destination, planes.pitch[0] / sizeof(std::uint16_t), luma_black);
    }
    for (std::uint32_t row = 0; row < planes.height[1]; ++row) {
      auto *destination = static_cast<std::uint16_t *>(surface_params.mappedAddr.addr[1]) + row * planes.pitch[1] / sizeof(std::uint16_t);
      std::fill_n(destination, planes.pitch[1] / sizeof(std::uint16_t), chroma_neutral);
    }

    const auto synchronized = NvBufSurfaceSyncForDevice(surface, 0, -1) == 0;
    const auto unmapped = NvBufSurfaceUnMap(surface, 0, -1) == 0;
    if (!synchronized || !unmapped) {
      error = "Could not synchronize an initialized Jetson P010 encoder surface";
      return false;
    }
    return true;
  }

  /**
   * @brief Own and recycle block-linear NvBufSurface objects for appsrc.
   */
  class nvmm_surface_pool_t {
  public:
    /**
     * @brief Allocate a fixed set of Jetson encoder input surfaces.
     *
     * @param config Dimensions and color format for every surface.
     * @param count Number of independently reusable surfaces.
     * @param error Destination for an allocation error.
     * @return Shared pool, or null when NvBufSurface allocation fails.
     */
    static std::shared_ptr<nvmm_surface_pool_t> create(
      const jetson::pipeline_config_t &config,
      std::size_t count,
      std::string &error
    ) {
      auto pool = std::shared_ptr<nvmm_surface_pool_t>(new (std::nothrow) nvmm_surface_pool_t());
      if (!pool) {
        error = "Could not allocate the Jetson NVMM surface pool";
        return {};
      }

      NvBufSurfaceCreateParams params {};
      params.gpuId = 0;
      params.width = static_cast<std::uint32_t>(config.width);
      params.height = static_cast<std::uint32_t>(config.height);
      params.colorFormat = make_nvmm_color_format(config);
      params.layout = NVBUF_LAYOUT_BLOCK_LINEAR;
      params.memType = NVBUF_MEM_SURFACE_ARRAY;

      for (std::size_t index = 0; index < count; ++index) {
        NvBufSurface *surface = nullptr;
        if (NvBufSurfaceCreate(&surface, 1, &params) != 0 || !surface) {
          error = "Could not allocate a Jetson NVMM encoder surface";
          return {};
        }
        surface->numFilled = 1;
        if (!clear_nvmm_surface(surface, config, error)) {
          NvBufSurfaceDestroy(surface);
          return {};
        }
        pool->surfaces_.push_back(surface);
        pool->available_.push_back(surface);
      }
      return pool;
    }

    /**
     * @brief Destroy every surface after GStreamer releases outstanding buffers.
     */
    ~nvmm_surface_pool_t() {
      for (auto *surface : surfaces_) {
        NvBufSurfaceDestroy(surface);
      }
    }

    nvmm_surface_pool_t(const nvmm_surface_pool_t &) = delete;
    nvmm_surface_pool_t &operator=(const nvmm_surface_pool_t &) = delete;
    nvmm_surface_pool_t(nvmm_surface_pool_t &&) = delete;
    nvmm_surface_pool_t &operator=(nvmm_surface_pool_t &&) = delete;

    /**
     * @brief Borrow one surface without waiting for GStreamer.
     *
     * @return Available surface, or null when all surfaces remain in flight.
     */
    NvBufSurface *acquire() {
      std::lock_guard lock {mutex_};
      if (available_.empty()) {
        return nullptr;
      }
      auto *surface = available_.back();
      available_.pop_back();
      return surface;
    }

    /**
     * @brief Return a GStreamer-released surface to the available list.
     *
     * @param surface Surface whose input buffer was released.
     */
    void release(NvBufSurface *surface) {
      std::lock_guard lock {mutex_};
      available_.push_back(surface);
    }

  private:
    /**
     * @brief Construct an empty surface pool before allocation.
     */
    nvmm_surface_pool_t() = default;

    std::mutex mutex_;  ///< Protects the available surface list.
    std::vector<NvBufSurface *> surfaces_;  ///< Every surface owned by this pool.
    std::vector<NvBufSurface *> available_;  ///< Surfaces not referenced by GStreamer.
  };

  /**
   * @brief Lifetime state attached to one GStreamer-wrapped NVMM surface.
   */
  struct nvmm_buffer_context_t {
    std::shared_ptr<nvmm_surface_pool_t> pool;  ///< Pool kept alive until the buffer is released.
    NvBufSurface *surface;  ///< Surface returned to the pool by the destroy callback.
  };

  /**
   * @brief Recycle an NVMM surface after GStreamer releases its wrapper.
   *
   * @param data Owned `nvmm_buffer_context_t` pointer.
   */
  void release_nvmm_buffer(gpointer data) {
    std::unique_ptr<nvmm_buffer_context_t> context {static_cast<nvmm_buffer_context_t *>(data)};
    context->pool->release(context->surface);
  }

  /**
   * @brief Wrap an acquired NvBufSurface in a GStreamer buffer.
   *
   * @param pool Pool that owns and later recycles the surface.
   * @param surface Filled surface borrowed from the pool.
   * @param error Destination for allocation errors.
   * @return Read-only GStreamer wrapper, or null on failure.
   */
  GstBuffer *wrap_nvmm_surface(
    const std::shared_ptr<nvmm_surface_pool_t> &pool,
    NvBufSurface *surface,
    std::string &error
  ) {
    auto *context = new (std::nothrow) nvmm_buffer_context_t {pool, surface};
    if (!context) {
      pool->release(surface);
      error = "Could not allocate Jetson NVMM buffer lifetime state";
      return nullptr;
    }
    auto *buffer = gst_buffer_new_wrapped_full(
      GST_MEMORY_FLAG_READONLY,
      surface,
      sizeof(*surface),
      0,
      sizeof(*surface),
      context,
      release_nvmm_buffer
    );
    if (!buffer) {
      release_nvmm_buffer(context);
      error = "Could not wrap a Jetson NVMM encoder surface";
    }
    return buffer;
  }

  /**
   * @brief Copy a raw semiplanar frame into a reusable block-linear surface.
   *
   * @param pool Surface pool that supplies and owns the destination.
   * @param config Frame dimensions and pixel layout.
   * @param frame Raw CPU plane pointers and strides.
   * @param error Destination for mapping or synchronization errors.
   * @return GStreamer buffer wrapping the filled surface, or null on failure.
   */
  GstBuffer *make_nvmm_buffer(
    const std::shared_ptr<nvmm_surface_pool_t> &pool,
    const jetson::pipeline_config_t &config,
    const jetson::frame_view_t &frame,
    std::string &error
  ) {
    auto *surface = pool->acquire();
    if (!surface) {
      error = "Jetson NVMM encoder surface pool is exhausted";
      return nullptr;
    }

    if (NvBufSurfaceMap(surface, 0, -1, NVBUF_MAP_WRITE) != 0) {
      pool->release(surface);
      error = "Could not map a Jetson NVMM encoder surface";
      return nullptr;
    }
    if (NvBufSurfaceSyncForCpu(surface, 0, -1) != 0) {
      NvBufSurfaceUnMap(surface, 0, -1);
      pool->release(surface);
      error = "Could not synchronize a Jetson NVMM surface for CPU access";
      return nullptr;
    }

    const auto bytes_per_sample = config.pixel_format == jetson::pixel_format_e::nv12 ? 1U : 2U;
    const auto row_bytes = static_cast<std::size_t>(config.width) * bytes_per_sample;
    const auto &surface_params = surface->surfaceList[0];
    const auto &planes = surface_params.planeParams;
    if (planes.num_planes < 2 ||
        !surface_params.mappedAddr.addr[0] || !surface_params.mappedAddr.addr[1] ||
        planes.pitch[0] < row_bytes || planes.pitch[1] < row_bytes ||
        planes.height[0] < static_cast<std::uint32_t>(config.height) ||
        planes.height[1] < static_cast<std::uint32_t>(config.height / 2)) {
      NvBufSurfaceUnMap(surface, 0, -1);
      pool->release(surface);
      error = "Jetson NVMM surface has an incompatible plane layout";
      return nullptr;
    }

    auto *luma = static_cast<std::uint8_t *>(surface_params.mappedAddr.addr[0]);
    auto *chroma = static_cast<std::uint8_t *>(surface_params.mappedAddr.addr[1]);
    for (int row = 0; row < config.height; ++row) {
      std::memcpy(luma + static_cast<std::size_t>(row) * planes.pitch[0], frame.luma + static_cast<std::size_t>(row) * frame.luma_stride, row_bytes);
    }
    for (int row = 0; row < config.height / 2; ++row) {
      std::memcpy(chroma + static_cast<std::size_t>(row) * planes.pitch[1], frame.chroma + static_cast<std::size_t>(row) * frame.chroma_stride, row_bytes);
    }

    const auto synchronized = NvBufSurfaceSyncForDevice(surface, 0, -1) == 0;
    const auto unmapped = NvBufSurfaceUnMap(surface, 0, -1) == 0;
    if (!synchronized || !unmapped) {
      pool->release(surface);
      error = "Could not synchronize a Jetson NVMM surface for hardware encoding";
      return nullptr;
    }

    return wrap_nvmm_surface(pool, surface, error);
  }

  #ifdef SUNSHINE_BUILD_JETSON_VIC
  /**
   * @brief Convert a public DMA-BUF pixel layout to the matching VIC input format.
   *
   * @param format Packed component order exported by the capture backend.
   * @return Matching NvBufSurface color format.
   */
  NvBufSurfaceColorFormat make_vic_input_format(jetson::dmabuf_pixel_format_e format) {
    switch (format) {
      case jetson::dmabuf_pixel_format_e::bgrx:
        return NVBUF_COLOR_FORMAT_BGRx;
      case jetson::dmabuf_pixel_format_e::bgra:
        return NVBUF_COLOR_FORMAT_BGRA;
      case jetson::dmabuf_pixel_format_e::rgbx:
        return NVBUF_COLOR_FORMAT_RGBx;
      case jetson::dmabuf_pixel_format_e::rgba:
        return NVBUF_COLOR_FORMAT_RGBA;
    }
    return NVBUF_COLOR_FORMAT_INVALID;
  }

  /**
   * @brief Reuse a pitch-linear BGRx surface and convert it to encoder NVMM with VIC.
   */
  class vic_converter_t {
  public:
    /**
     * @brief Release the retained CPU or imported DMA-BUF source surface.
     */
    ~vic_converter_t() {
      release_source();
    }

    vic_converter_t() = default;
    vic_converter_t(const vic_converter_t &) = delete;
    vic_converter_t &operator=(const vic_converter_t &) = delete;
    vic_converter_t(vic_converter_t &&) = delete;
    vic_converter_t &operator=(vic_converter_t &&) = delete;

    /**
     * @brief Convert one packed frame into a pooled encoder surface.
     *
     * @param pool Destination surface pool.
     * @param config Encoded dimensions and color settings.
     * @param frame Packed source frame.
     * @param error Destination for allocation, mapping, or VIC errors.
     * @return GStreamer wrapper for the converted destination, or null on failure.
     */
    GstBuffer *convert(
      const std::shared_ptr<nvmm_surface_pool_t> &pool,
      const jetson::pipeline_config_t &config,
      const jetson::bgrx_frame_view_t &frame,
      std::string &error
    ) {
      if (!ensure_source(frame.width, frame.height, error)) {
        return nullptr;
      }
      if (NvBufSurfaceMap(source_, 0, 0, NVBUF_MAP_WRITE) != 0) {
        error = "Could not map the Jetson VIC BGRx source surface";
        return nullptr;
      }
      if (NvBufSurfaceSyncForCpu(source_, 0, 0) != 0) {
        NvBufSurfaceUnMap(source_, 0, 0);
        error = "Could not synchronize the Jetson VIC source for CPU access";
        return nullptr;
      }

      const auto &source_params = source_->surfaceList[0];
      const auto source_pitch = source_params.planeParams.pitch[0];
      const auto row_bytes = static_cast<std::size_t>(frame.width) * 4;
      if (!source_params.mappedAddr.addr[0] || source_pitch < row_bytes) {
        NvBufSurfaceUnMap(source_, 0, 0);
        error = "Jetson VIC source has an incompatible plane layout";
        return nullptr;
      }
      auto *destination = static_cast<std::uint8_t *>(source_params.mappedAddr.addr[0]);
      for (int row = 0; row < frame.height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * source_pitch, frame.data + static_cast<std::size_t>(row) * frame.row_stride, row_bytes);
      }

      const auto synchronized = NvBufSurfaceSyncForDevice(source_, 0, 0) == 0;
      const auto unmapped = NvBufSurfaceUnMap(source_, 0, 0) == 0;
      if (!synchronized || !unmapped) {
        error = "Could not synchronize the Jetson VIC source for hardware access";
        return nullptr;
      }

      has_frame_ = true;
      return make_output(pool, config, error);
    }

    /**
     * @brief Import a packed RGB DMA-BUF and convert it into pooled encoder NVMM.
     *
     * @param pool Destination surface pool.
     * @param config Encoded dimensions and color settings.
     * @param frame DMA-BUF descriptor and packed source layout.
     * @param error Destination for import or VIC errors.
     * @return GStreamer wrapper for the converted destination, or null on failure.
     */
    GstBuffer *convert(
      const std::shared_ptr<nvmm_surface_pool_t> &pool,
      const jetson::pipeline_config_t &config,
      const jetson::dmabuf_frame_view_t &frame,
      std::string &error
    ) {
      release_source();

      const auto imported_fd = dup(frame.fd);
      if (imported_fd < 0) {
        error = "Could not retain the Jetson capture DMA-BUF";
        return nullptr;
      }

      const auto original_offset = lseek(imported_fd, 0, SEEK_CUR);
      const auto dma_buf_size = lseek(imported_fd, 0, SEEK_END);
      if (original_offset >= 0) {
        lseek(imported_fd, original_offset, SEEK_SET);
      }
      if (dma_buf_size <= 0 || static_cast<std::uint64_t>(dma_buf_size) > std::numeric_limits<std::uint32_t>::max()) {
        close(imported_fd);
        error = "Could not determine the Jetson DMA-BUF allocation size";
        return nullptr;
      }
      if (static_cast<std::uint64_t>(dma_buf_size) <= frame.offset) {
        close(imported_fd);
        error = "Jetson DMA-BUF has an invalid plane offset";
        return nullptr;
      }

      constexpr auto invalid_modifier = std::numeric_limits<std::uint64_t>::max();
      const auto pitch_linear = frame.modifier == 0 || frame.modifier == invalid_modifier;
      NvBufSurfaceMapParams map_params {};
      map_params.num_planes = 1;
      map_params.gpuId = 0;
      map_params.fd = static_cast<std::uint64_t>(imported_fd);
      map_params.totalSize = static_cast<std::uint32_t>(dma_buf_size);
      map_params.memType = NVBUF_MEM_SURFACE_ARRAY;
      map_params.layout = pitch_linear ? NVBUF_LAYOUT_PITCH : NVBUF_LAYOUT_BLOCK_LINEAR;
      map_params.scanformat = NVBUF_DISPLAYSCANFORMAT_PROGRESSIVE;
      map_params.colorFormat = make_vic_input_format(frame.pixel_format);
      map_params.planes[0].width = static_cast<std::uint32_t>(frame.width);
      map_params.planes[0].height = static_cast<std::uint32_t>(frame.height);
      map_params.planes[0].pitch = frame.pitch;
      map_params.planes[0].offset = frame.offset;
      map_params.planes[0].psize = map_params.totalSize - frame.offset;
      map_params.planes[0].blockheightlog2 = pitch_linear ? 0 : frame.modifier & 0xf;

      if (NvBufSurfaceImport(&source_, &map_params) != 0 || !source_) {
        close(imported_fd);
        error = "Jetson could not import the capture DMA-BUF as an NvBufSurface";
        source_ = nullptr;
        return nullptr;
      }
      width_ = frame.width;
      height_ = frame.height;
      flip_y_ = frame.y_invert;
      source_cpu_writable_ = false;
      has_frame_ = true;
      return make_output(pool, config, error);
    }

    /**
     * @brief Convert the retained BGRx source again for a repeated video frame.
     *
     * @param pool Destination surface pool.
     * @param config Encoded dimensions and color settings.
     * @param error Destination for VIC errors.
     * @return GStreamer wrapper for the converted destination, or null on failure.
     */
    GstBuffer *repeat(
      const std::shared_ptr<nvmm_surface_pool_t> &pool,
      const jetson::pipeline_config_t &config,
      std::string &error
    ) {
      if (!has_frame_) {
        error = "Jetson VIC has no retained BGRx frame";
        return nullptr;
      }
      return make_output(pool, config, error);
    }

  private:
    /**
     * @brief Transform the retained source into a new pooled output surface.
     *
     * @param pool Destination surface pool.
     * @param config Encoded dimensions and color settings.
     * @param error Destination for pool or transform errors.
     * @return GStreamer wrapper for the converted destination, or null on failure.
     */
    GstBuffer *make_output(
      const std::shared_ptr<nvmm_surface_pool_t> &pool,
      const jetson::pipeline_config_t &config,
      std::string &error
    ) {
      auto *output = pool->acquire();
      if (!output) {
        error = "Jetson NVMM encoder surface pool is exhausted";
        return nullptr;
      }

      const auto scale = std::min(
        static_cast<double>(config.width) / width_,
        static_cast<double>(config.height) / height_
      );
      const auto scaled_width = std::max(2, static_cast<int>(width_ * scale) & ~1);
      const auto scaled_height = std::max(2, static_cast<int>(height_ * scale) & ~1);
      NvBufSurfTransformRect source_rect {0, 0, static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_), {}};
      NvBufSurfTransformRect destination_rect {
        static_cast<std::uint32_t>((config.height - scaled_height) / 2) & ~1U,
        static_cast<std::uint32_t>((config.width - scaled_width) / 2) & ~1U,
        static_cast<std::uint32_t>(scaled_width),
        static_cast<std::uint32_t>(scaled_height),
        {}
      };
      NvBufSurfTransformConfigParams session {};
      session.compute_mode = NvBufSurfTransformCompute_VIC;
      NvBufSurfTransformParams transform {};
      transform.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST | NVBUFSURF_TRANSFORM_FILTER;
      if (flip_y_) {
        transform.transform_flag |= NVBUFSURF_TRANSFORM_FLIP;
        transform.transform_flip = NvBufSurfTransform_FlipY;
      }
      transform.transform_filter = NvBufSurfTransformInter_Algo3;
      transform.src_rect = &source_rect;
      transform.dst_rect = &destination_rect;

      std::lock_guard transform_lock {vic_transform_mutex};
      if (NvBufSurfTransformSetSessionParams(&session) != NvBufSurfTransformError_Success ||
          NvBufSurfTransform(source_, output, &transform) != NvBufSurfTransformError_Success) {
        pool->release(output);
        error = "Jetson VIC could not convert BGRx into the encoder surface";
        return nullptr;
      }
      return wrap_nvmm_surface(pool, output, error);
    }

    /**
     * @brief Allocate or resize the reusable BGRx source surface.
     *
     * @param width Required source width.
     * @param height Required source height.
     * @param error Destination for an allocation error.
     * @return True when `source_` matches the requested dimensions.
     */
    bool ensure_source(int width, int height, std::string &error) {
      if (source_ && source_cpu_writable_ && width == width_ && height == height_) {
        return true;
      }
      release_source();

      NvBufSurfaceCreateParams params {};
      params.gpuId = 0;
      params.width = static_cast<std::uint32_t>(width);
      params.height = static_cast<std::uint32_t>(height);
      params.colorFormat = NVBUF_COLOR_FORMAT_BGRx;
      params.layout = NVBUF_LAYOUT_PITCH;
      params.memType = NVBUF_MEM_SURFACE_ARRAY;
      if (NvBufSurfaceCreate(&source_, 1, &params) != 0 || !source_) {
        error = "Could not allocate the Jetson VIC BGRx source surface";
        return false;
      }
      source_->numFilled = 1;
      width_ = width;
      height_ = height;
      source_cpu_writable_ = true;
      return true;
    }

    /**
     * @brief Destroy the retained source and clear its layout state.
     */
    void release_source() {
      if (source_) {
        NvBufSurfaceDestroy(source_);
        source_ = nullptr;
      }
      width_ = 0;
      height_ = 0;
      flip_y_ = false;
      source_cpu_writable_ = false;
      has_frame_ = false;
    }

    NvBufSurface *source_ = nullptr;  ///< Reusable pitch-linear BGRx source.
    int width_ = 0;  ///< Current source width.
    int height_ = 0;  ///< Current source height.
    bool flip_y_ = false;  ///< Whether the retained source must be vertically inverted.
    bool source_cpu_writable_ = false;  ///< Whether the retained source is the reusable CPU-upload surface.
    bool has_frame_ = false;  ///< Whether the source contains a synchronized frame.
  };
  #endif
#endif

  /**
   * @brief Copy a raw semiplanar frame into ordinary GStreamer system memory.
   *
   * @param config Frame dimensions and pixel layout.
   * @param frame Raw CPU plane pointers and strides.
   * @param error Destination for allocation or mapping errors.
   * @return Filled GStreamer buffer, or null on failure.
   */
  GstBuffer *make_system_buffer(
    const jetson::pipeline_config_t &config,
    const jetson::frame_view_t &frame,
    std::string &error
  ) {
    const auto bytes_per_sample = config.pixel_format == jetson::pixel_format_e::nv12 ? 1U : 2U;
    const auto row_bytes = static_cast<std::size_t>(config.width) * bytes_per_sample;
    const auto frame_bytes = row_bytes * config.height * 3 / 2;
    auto *buffer = gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
    if (!buffer) {
      error = "Could not allocate a GStreamer input buffer";
      return nullptr;
    }

    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      gst_buffer_unref(buffer);
      error = "Could not map a GStreamer input buffer";
      return nullptr;
    }

    auto *destination = map.data;
    for (int row = 0; row < config.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * row_bytes, frame.luma + static_cast<std::size_t>(row) * frame.luma_stride, row_bytes);
    }
    destination += row_bytes * config.height;
    for (int row = 0; row < config.height / 2; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * row_bytes, frame.chroma + static_cast<std::size_t>(row) * frame.chroma_stride, row_bytes);
    }
    gst_buffer_unmap(buffer, &map);
    return buffer;
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
      if (prepared_buffer) {
        gst_buffer_unref(prepared_buffer);
        prepared_buffer = nullptr;
      }
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
      nvmm_input = false;
#ifdef SUNSHINE_BUILD_JETSON_NVMM
      nvmm_pool.reset();
#endif
#ifdef SUNSHINE_BUILD_JETSON_VIC
      vic_converter.reset();
#endif
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

    /**
     * @brief Push a prepared input buffer and collect one encoded access unit.
     *
     * @param buffer Owned input buffer transferred to appsrc.
     * @param frame_index Monotonic Sunshine frame index.
     * @param force_idr Whether the encoder should emit an IDR picture.
     * @return Encoded access unit, or no value when submission fails.
     */
    std::optional<encoded_frame_t> submit(GstBuffer *buffer, std::uint64_t frame_index, bool force_idr) {
      const auto duration = gst_util_uint64_scale(GST_SECOND, config.framerate_denominator, config.framerate_numerator);
      GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame_index, GST_SECOND * static_cast<guint64>(config.framerate_denominator), config.framerate_numerator);
      GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
      GST_BUFFER_DURATION(buffer) = duration;
      GST_BUFFER_OFFSET(buffer) = frame_index;

      // The first frame is inherently an IDR. Emitting this action while the
      // NVIDIA element is still negotiating can race inside libtegrav4l2.
      if (force_idr && produced_frame) {
        g_signal_emit_by_name(hardware_encoder, "force-IDR");
      }

      const auto flow = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
      if (flow != GST_FLOW_OK) {
        read_bus_error("Jetson GStreamer appsrc rejected an input frame");
        return std::nullopt;
      }

      auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), sample_timeout);
      if (!sample) {
        read_bus_error("Timed out waiting for the Jetson hardware encoder");
        return std::nullopt;
      }

      auto *encoded_buffer = gst_sample_get_buffer(sample);
      GstMapInfo encoded_map {};
      if (!encoded_buffer || !gst_buffer_map(encoded_buffer, &encoded_map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        error = "Could not map the Jetson encoded access unit";
        return std::nullopt;
      }

      encoded_frame_t result {
        std::vector<std::uint8_t>(encoded_map.data, encoded_map.data + encoded_map.size),
        frame_index,
        false,
      };
      result.idr = contains_idr(config.codec, result.data) || !GST_BUFFER_FLAG_IS_SET(encoded_buffer, GST_BUFFER_FLAG_DELTA_UNIT);
      gst_buffer_unmap(encoded_buffer, &encoded_map);
      gst_sample_unref(sample);

      if (result.data.empty()) {
        error = "Jetson hardware encoder returned an empty access unit";
        return std::nullopt;
      }
      produced_frame = true;
      return result;
    }

    GstElement *pipeline = nullptr;  ///< Pipeline that owns all child elements.
    GstElement *source = nullptr;  ///< Borrowed appsrc child pointer.
    GstElement *hardware_encoder = nullptr;  ///< Borrowed NVIDIA encoder child pointer.
    GstElement *sink = nullptr;  ///< Borrowed appsink child pointer.
    GstBuffer *prepared_buffer = nullptr;  ///< VIC-converted input awaiting encoder submission.
    pipeline_config_t config {};  ///< Settings used by the active pipeline.
    std::string error;  ///< Most recent failure message.
    bool started = false;  ///< Whether the pipeline reached the playing state.
    bool produced_frame = false;  ///< Whether negotiation and one encoded output have completed.
    bool nvmm_input = false;  ///< Whether appsrc accepts direct NvBufSurface wrappers.
#ifdef SUNSHINE_BUILD_JETSON_NVMM
    std::shared_ptr<nvmm_surface_pool_t> nvmm_pool;  ///< Reusable direct-input NVMM surfaces.
#endif
#ifdef SUNSHINE_BUILD_JETSON_VIC
    std::unique_ptr<vic_converter_t> vic_converter;  ///< Reusable BGRx-to-NVMM VIC converter.
#endif
  };

  encoder_t::encoder_t():
      impl_ {std::make_unique<impl_t>()} {
  }

  encoder_t::~encoder_t() = default;

  bool encoder_t::is_available(codec_e codec) {
    if (!initialize_gstreamer()) {
      return false;
    }
    const auto common_elements = factory_available("appsrc") &&
                                 factory_available(element_name(codec).data()) &&
                                 factory_available("appsink");
#ifdef SUNSHINE_BUILD_JETSON_NVMM
    return common_elements;
#else
    return common_elements && factory_available("nvvidconv");
#endif
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

    const auto start_pipeline = [&](bool direct_nvmm) {
      impl_->pipeline = gst_pipeline_new("sunshine-jetson-pipeline");
      impl_->source = gst_element_factory_make("appsrc", "sunshine-jetson-source");
      auto *converter = direct_nvmm ? nullptr : gst_element_factory_make("nvvidconv", "sunshine-jetson-converter");
      auto *caps_filter = direct_nvmm ? nullptr : gst_element_factory_make("capsfilter", "sunshine-jetson-nvmm-caps");
      impl_->hardware_encoder = gst_element_factory_make(element_name(config.codec).data(), "sunshine-jetson-encoder");
      impl_->sink = gst_element_factory_make("appsink", "sunshine-jetson-sink");

      if (!impl_->pipeline || !impl_->source || (!direct_nvmm && (!converter || !caps_filter)) || !impl_->hardware_encoder || !impl_->sink) {
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

      if (direct_nvmm) {
        gst_bin_add_many(GST_BIN(impl_->pipeline), impl_->source, impl_->hardware_encoder, impl_->sink, nullptr);
        if (!gst_element_link_many(impl_->source, impl_->hardware_encoder, impl_->sink, nullptr)) {
          impl_->error = "Could not link the direct Jetson NVMM pipeline";
          impl_->reset();
          return false;
        }
      } else {
        gst_bin_add_many(GST_BIN(impl_->pipeline), impl_->source, converter, caps_filter, impl_->hardware_encoder, impl_->sink, nullptr);
        if (!gst_element_link_many(impl_->source, converter, caps_filter, impl_->hardware_encoder, impl_->sink, nullptr)) {
          impl_->error = "Could not link the Jetson GStreamer pipeline";
          impl_->reset();
          return false;
        }
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
      if (direct_nvmm) {
        gst_caps_set_features(source_caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
      }
      gst_app_src_set_caps(GST_APP_SRC(impl_->source), source_caps);
      gst_caps_unref(source_caps);

      if (!direct_nvmm) {
        auto *nvmm_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, format_name, nullptr);
        gst_caps_set_features(nvmm_caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
        g_object_set(caps_filter, "caps", nvmm_caps, nullptr);
        gst_caps_unref(nvmm_caps);
      }

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
      impl_->nvmm_input = direct_nvmm;
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
    };

    std::string direct_nvmm_error;
#ifdef SUNSHINE_BUILD_JETSON_NVMM
    impl_->nvmm_pool = nvmm_surface_pool_t::create(config, nvmm_surface_count, direct_nvmm_error);
    if (impl_->nvmm_pool && start_pipeline(true)) {
      impl_->error.clear();
      return true;
    }
    if (direct_nvmm_error.empty()) {
      direct_nvmm_error = impl_->error;
    }
    impl_->reset();
#endif

    if (start_pipeline(false)) {
      impl_->error.clear();
      return true;
    }

    if (!direct_nvmm_error.empty()) {
      impl_->error = "Direct Jetson NVMM input failed: " + direct_nvmm_error + "; system-memory fallback failed: " + impl_->error;
    }
    return false;
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

    GstBuffer *buffer = nullptr;
#ifdef SUNSHINE_BUILD_JETSON_NVMM
    if (impl_->nvmm_input) {
      buffer = make_nvmm_buffer(impl_->nvmm_pool, impl_->config, frame, impl_->error);
    } else {
#endif
      buffer = make_system_buffer(impl_->config, frame, impl_->error);
#ifdef SUNSHINE_BUILD_JETSON_NVMM
    }
#endif
    if (!buffer) {
      return std::nullopt;
    }
    return impl_->submit(buffer, frame_index, force_idr);
  }

  bool encoder_t::prepare_bgrx(const bgrx_frame_view_t &frame) {
    if (!impl_->started || !impl_->pipeline) {
      impl_->error = "Jetson GStreamer pipeline is not running";
      return false;
    }
    if (!frame.data || frame.width <= 0 || frame.height <= 0 || frame.row_stride < frame.width * 4) {
      impl_->error = "Jetson VIC received an invalid BGRx frame";
      return false;
    }
    if (impl_->prepared_buffer) {
      gst_buffer_unref(impl_->prepared_buffer);
      impl_->prepared_buffer = nullptr;
    }

#ifdef SUNSHINE_BUILD_JETSON_VIC
    if (!supports_vic()) {
      impl_->error = "Jetson VIC conversion requires direct NVMM input";
      return false;
    }
    if (!impl_->vic_converter) {
      impl_->vic_converter = std::make_unique<vic_converter_t>();
    }
    impl_->prepared_buffer = impl_->vic_converter->convert(impl_->nvmm_pool, impl_->config, frame, impl_->error);
    return impl_->prepared_buffer != nullptr;
#else
    impl_->error = "Jetson VIC conversion was not compiled in";
    return false;
#endif
  }

  bool encoder_t::prepare_dmabuf(const dmabuf_frame_view_t &frame) {
    if (!impl_->started || !impl_->pipeline) {
      impl_->error = "Jetson GStreamer pipeline is not running";
      return false;
    }
    if (frame.fd < 0 || frame.width <= 0 || frame.height <= 0 ||
        frame.pitch < static_cast<std::uint32_t>(frame.width * 4)) {
      impl_->error = "Jetson VIC received an invalid DMA-BUF frame";
      return false;
    }
    if (impl_->prepared_buffer) {
      gst_buffer_unref(impl_->prepared_buffer);
      impl_->prepared_buffer = nullptr;
    }

#ifdef SUNSHINE_BUILD_JETSON_VIC
    if (!supports_vic()) {
      impl_->error = "Jetson DMA-BUF conversion requires VIC and direct NVMM input";
      return false;
    }
    if (!impl_->vic_converter) {
      impl_->vic_converter = std::make_unique<vic_converter_t>();
    }
    impl_->prepared_buffer = impl_->vic_converter->convert(impl_->nvmm_pool, impl_->config, frame, impl_->error);
    return impl_->prepared_buffer != nullptr;
#else
    impl_->error = "Jetson DMA-BUF conversion was not compiled in";
    return false;
#endif
  }

  std::optional<encoded_frame_t> encoder_t::encode_prepared(std::uint64_t frame_index, bool force_idr) {
    if (!impl_->started || !impl_->pipeline) {
      impl_->error = "Jetson GStreamer pipeline is not running";
      return std::nullopt;
    }
#ifdef SUNSHINE_BUILD_JETSON_VIC
    if (!impl_->prepared_buffer && impl_->vic_converter && supports_vic()) {
      impl_->prepared_buffer = impl_->vic_converter->repeat(impl_->nvmm_pool, impl_->config, impl_->error);
    }
#endif
    if (!impl_->prepared_buffer) {
      impl_->error = "Jetson encoder has no prepared VIC frame";
      return std::nullopt;
    }
    auto *buffer = impl_->prepared_buffer;
    impl_->prepared_buffer = nullptr;
    return impl_->submit(buffer, frame_index, force_idr);
  }

  bool encoder_t::supports_vic() const {
#ifdef SUNSHINE_BUILD_JETSON_VIC
    return uses_nvmm();
#else
    return false;
#endif
  }

  bool encoder_t::uses_nvmm() const {
    return impl_->started && impl_->nvmm_input;
  }

  std::string_view encoder_t::last_error() const {
    return impl_->error;
  }
}  // namespace jetson
