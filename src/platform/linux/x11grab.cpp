/**
 * @file src/platform/linux/x11grab.cpp
 * @brief Definitions for x11 capture.
 */
// standard includes
#include <fstream>
#include <ranges>
#include <thread>

// plaform includes
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "misc.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/task_pool.h"
#include "src/video.h"
#include "vaapi.h"
#include "x11grab.h"

#ifdef SUNSHINE_BUILD_JETSON_NVMM
  #include <nvbufsurface.h>
#endif

using namespace std::literals;

#ifndef EGL_NATIVE_PIXMAP_KHR
  #define EGL_NATIVE_PIXMAP_KHR 0x30B0
#endif

namespace platf {
  /**
   * @brief Load XCB entry points used by the X11 capture backend.
   *
   * @return 0 when required XCB symbols are loaded; nonzero otherwise.
   */
  int load_xcb();
  /**
   * @brief Load X11 entry points used by the X11 capture backend.
   *
   * @return 0 when required X11 symbols are loaded; nonzero otherwise.
   */
  int load_x11();

  namespace x11 {
/**
 * @def _FN(x, ret, args)
 * @brief Macro for FN.
 */
#define _FN(x, ret, args) \
  /** \
   * @brief Function pointer type for the dynamically loaded X11 entry point. \
   */ \
  typedef ret(*x##_fn) args; \
  /** \
   * @brief Loaded X11 entry point pointer. \
   */ \
  static x##_fn x

    _FN(GetImage, XImage *, (Display * display, Drawable d, int x, int y, unsigned int width, unsigned int height, unsigned long plane_mask, int format));

    _FN(OpenDisplay, Display *, (_Xconst char *display_name));
    _FN(GetWindowAttributes, Status, (Display * display, Window w, XWindowAttributes *window_attributes_return));

    _FN(CloseDisplay, int, (Display * display));
    _FN(Free, int, (void *data));
    _FN(InitThreads, Status, (void) );

    namespace rr {
      _FN(GetScreenResources, XRRScreenResources *, (Display * dpy, Window window));
      _FN(GetOutputInfo, XRROutputInfo *, (Display * dpy, XRRScreenResources *resources, RROutput output));
      _FN(GetCrtcInfo, XRRCrtcInfo *, (Display * dpy, XRRScreenResources *resources, RRCrtc crtc));
      _FN(FreeScreenResources, void, (XRRScreenResources * resources));
      _FN(FreeOutputInfo, void, (XRROutputInfo * outputInfo));
      _FN(FreeCrtcInfo, void, (XRRCrtcInfo * crtcInfo));

      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;

        if (funcs_loaded) {
          return 0;
        }

        if (!handle) {
          handle = dyn::handle({"libXrandr.so.2", "libXrandr.so"});
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &GetScreenResources, "XRRGetScreenResources"},
          {(dyn::apiproc *) &GetOutputInfo, "XRRGetOutputInfo"},
          {(dyn::apiproc *) &GetCrtcInfo, "XRRGetCrtcInfo"},
          {(dyn::apiproc *) &FreeScreenResources, "XRRFreeScreenResources"},
          {(dyn::apiproc *) &FreeOutputInfo, "XRRFreeOutputInfo"},
          {(dyn::apiproc *) &FreeCrtcInfo, "XRRFreeCrtcInfo"},
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }

    }  // namespace rr

    namespace fix {
      _FN(GetCursorImage, XFixesCursorImage *, (Display * dpy));

      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;

        if (funcs_loaded) {
          return 0;
        }

        if (!handle) {
          handle = dyn::handle({"libXfixes.so.3", "libXfixes.so"});
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &GetCursorImage, "XFixesGetCursorImage"},
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }
    }  // namespace fix

    namespace composite {
      using name_window_pixmap_fn = Pixmap (*)(Display *, Window);
      using redirect_subwindows_fn = void (*)(Display *, Window, int);
      using unredirect_subwindows_fn = void (*)(Display *, Window, int);

      static name_window_pixmap_fn NameWindowPixmap {nullptr};
      static redirect_subwindows_fn RedirectSubwindows {nullptr};
      static unredirect_subwindows_fn UnredirectSubwindows {nullptr};

      /**
       * @brief Load the XComposite entry points required by GPU X11 capture.
       *
       * @return 0 when XComposite can be used; nonzero otherwise.
       */
      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;
        if (funcs_loaded) {
          return 0;
        }
        if (!handle) {
          handle = dyn::handle({"libXcomposite.so.1", "libXcomposite.so"});
          if (!handle) {
            return -1;
          }
        }
        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &NameWindowPixmap, "XCompositeNameWindowPixmap"},
          {(dyn::apiproc *) &RedirectSubwindows, "XCompositeRedirectSubwindows"},
          {(dyn::apiproc *) &UnredirectSubwindows, "XCompositeUnredirectSubwindows"},
        };
        if (dyn::load(handle, funcs)) {
          return -1;
        }
        funcs_loaded = true;
        return 0;
      }
    }  // namespace composite

    static int init() {
      static void *handle {nullptr};
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libX11.so.6", "libX11.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &GetImage, "XGetImage"},
        {(dyn::apiproc *) &OpenDisplay, "XOpenDisplay"},
        {(dyn::apiproc *) &GetWindowAttributes, "XGetWindowAttributes"},
        {(dyn::apiproc *) &Free, "XFree"},
        {(dyn::apiproc *) &CloseDisplay, "XCloseDisplay"},
        {(dyn::apiproc *) &InitThreads, "XInitThreads"},
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }
  }  // namespace x11

  namespace xcb {
    static xcb_extension_t *shm_id;

    _FN(shm_get_image_reply, xcb_shm_get_image_reply_t *, (xcb_connection_t * c, xcb_shm_get_image_cookie_t cookie, xcb_generic_error_t **e));

    _FN(shm_get_image_unchecked, xcb_shm_get_image_cookie_t, (xcb_connection_t * c, xcb_drawable_t drawable, int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t plane_mask, uint8_t format, xcb_shm_seg_t shmseg, uint32_t offset));

    _FN(shm_attach, xcb_void_cookie_t, (xcb_connection_t * c, xcb_shm_seg_t shmseg, uint32_t shmid, uint8_t read_only));

    _FN(shm_detach, xcb_void_cookie_t, (xcb_connection_t * c, xcb_shm_seg_t shmseg));

    _FN(get_extension_data, xcb_query_extension_reply_t *, (xcb_connection_t * c, xcb_extension_t *ext));

    _FN(get_setup, xcb_setup_t *, (xcb_connection_t * c));
    _FN(disconnect, void, (xcb_connection_t * c));
    _FN(connection_has_error, int, (xcb_connection_t * c));
    _FN(connect, xcb_connection_t *, (const char *displayname, int *screenp));
    _FN(setup_roots_iterator, xcb_screen_iterator_t, (const xcb_setup_t *R));
    _FN(generate_id, std::uint32_t, (xcb_connection_t * c));
    _FN(flush, int, (xcb_connection_t * c));

    /**
     * @brief Initialize shared-memory support for X11 capture.
     *
     * @return 0 when XCB shared-memory functions are loaded; nonzero otherwise.
     */
    int init_shm() {
      static void *handle {nullptr};
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libxcb-shm.so.0", "libxcb-shm.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &shm_id, "xcb_shm_id"},
        {(dyn::apiproc *) &shm_get_image_reply, "xcb_shm_get_image_reply"},
        {(dyn::apiproc *) &shm_get_image_unchecked, "xcb_shm_get_image_unchecked"},
        {(dyn::apiproc *) &shm_attach, "xcb_shm_attach"},
        {(dyn::apiproc *) &shm_detach, "xcb_shm_detach"},
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    /**
     * @brief Initialize XFixes cursor tracking for an X11 display.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      static void *handle {nullptr};
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libxcb.so.1", "libxcb.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &get_extension_data, "xcb_get_extension_data"},
        {(dyn::apiproc *) &get_setup, "xcb_get_setup"},
        {(dyn::apiproc *) &disconnect, "xcb_disconnect"},
        {(dyn::apiproc *) &connection_has_error, "xcb_connection_has_error"},
        {(dyn::apiproc *) &connect, "xcb_connect"},
        {(dyn::apiproc *) &setup_roots_iterator, "xcb_setup_roots_iterator"},
        {(dyn::apiproc *) &generate_id, "xcb_generate_id"},
        {(dyn::apiproc *) &flush, "xcb_flush"},
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

#undef _FN
  }  // namespace xcb

  /**
   * @brief Release image resources.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  void freeImage(XImage *);
  /**
   * @brief Release x resources.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  void freeX(XFixesCursorImage *);

  /**
   * @brief Shared XCB connection kept alive by per-frame XShm buffers.
   */
  using xcb_shared_connect_t = std::shared_ptr<xcb_connection_t>;
  /**
   * @brief XCB image pointer released with `xcb_image_destroy`.
   */
  using xcb_img_t = util::c_ptr<xcb_shm_get_image_reply_t>;

  /**
   * @brief XImage pointer released with `XDestroyImage`.
   */
  using ximg_t = util::safe_ptr<XImage, freeImage>;
  /**
   * @brief XFixes cursor image pointer released with `XFree`.
   */
  using xcursor_t = util::safe_ptr<XFixesCursorImage, freeX>;

  /**
   * @brief XRandR CRTC info pointer released with `XRRFreeCrtcInfo`.
   */
  using crtc_info_t = util::dyn_safe_ptr<_XRRCrtcInfo, &x11::rr::FreeCrtcInfo>;
  /**
   * @brief XRandR output info pointer released with `XRRFreeOutputInfo`.
   */
  using output_info_t = util::dyn_safe_ptr<_XRROutputInfo, &x11::rr::FreeOutputInfo>;
  /**
   * @brief XRandR screen resources pointer released with `XRRFreeScreenResources`.
   */
  using screen_res_t = util::dyn_safe_ptr<_XRRScreenResources, &x11::rr::FreeScreenResources>;

  /**
   * @brief RAII wrapper that removes a SysV shared-memory segment.
   */
  class shm_id_t {
  public:
    shm_id_t():
        id {-1} {
    }

    /**
     * @brief Take ownership of a SysV shared-memory segment ID.
     *
     * @param id SysV shared-memory segment ID.
     */
    shm_id_t(int id):
        id {id} {
    }

    /**
     * @brief Move ownership of a SysV shared-memory segment ID.
     *
     * @param other Shared-memory ID wrapper whose segment ownership is moved.
     */
    shm_id_t(shm_id_t &&other) noexcept:
        id(other.id) {
      other.id = -1;
    }

    ~shm_id_t() {
      if (id != -1) {
        shmctl(id, IPC_RMID, nullptr);
        id = -1;
      }
    }

    int id;  ///< SysV shared-memory segment identifier returned by shmget.
  };

  /**
   * @brief RAII wrapper that detaches mapped SysV shared memory.
   */
  class shm_data_t {
  public:
    shm_data_t():
        data {(void *) -1} {
    }

    /**
     * @brief Take ownership of an attached shared-memory mapping.
     *
     * @param data Pointer returned by shmat.
     */
    shm_data_t(void *data):
        data {data} {
    }

    /**
     * @brief Move ownership of an attached shared-memory mapping.
     *
     * @param other Shared-memory mapping wrapper whose attachment is moved.
     */
    shm_data_t(shm_data_t &&other) noexcept:
        data(other.data) {
      other.data = (void *) -1;
    }

    ~shm_data_t() {
      if ((std::uintptr_t) data != -1) {
        shmdt(data);
      }
    }

    void *data;  ///< Address returned by shmat for the shared-memory segment.
  };

  /**
   * @brief X11 image wrapper used by the software capture path.
   */
  struct x11_img_t: public img_t {
    ximg_t img;  ///< XImage backing the current software-captured frame.
  };

  /**
   * @brief X11 shared-memory image and segment ownership.
   */
  struct shm_img_t: public img_t {
    xcb_shared_connect_t xcb;  ///< XCB connection that owns the server-side SHM attachment.
    std::uint32_t seg {};  ///< XCB shared-memory segment ID used for this image.
    shm_id_t shm_id;  ///< SysV shared-memory segment backing this image.
    shm_data_t shm_data;  ///< Process mapping of the SysV shared-memory segment.

    /**
     * @brief Detach the X server before releasing the mapped SysV segment.
     */
    ~shm_img_t() override {
      if (xcb && shm_id.id != -1) {
        xcb::shm_detach(xcb.get(), seg);
        xcb::flush(xcb.get());
      }
      data = nullptr;
    }
  };

  static void blend_cursor(Display *display, img_t &img, int offsetX, int offsetY) {
    xcursor_t overlay {x11::fix::GetCursorImage(display)};

    if (!overlay) {
      BOOST_LOG(error) << "Couldn't get cursor from XFixesGetCursorImage"sv;
      return;
    }

    overlay->x -= overlay->xhot;
    overlay->y -= overlay->yhot;

    overlay->x -= offsetX;
    overlay->y -= offsetY;

    overlay->x = std::max((short) 0, overlay->x);
    overlay->y = std::max((short) 0, overlay->y);

    auto pixels = (int *) img.data;

    auto screen_height = img.height;
    auto screen_width = img.width;

    auto delta_height = std::min<uint16_t>(overlay->height, std::max(0, screen_height - overlay->y));
    auto delta_width = std::min<uint16_t>(overlay->width, std::max(0, screen_width - overlay->x));
    for (auto y = 0; y < delta_height; ++y) {
      auto overlay_begin = &overlay->pixels[y * overlay->width];
      auto overlay_end = &overlay->pixels[y * overlay->width + delta_width];

      auto pixels_begin = &pixels[(y + overlay->y) * (img.row_pitch / img.pixel_pitch) + overlay->x];

      std::for_each(overlay_begin, overlay_end, [&](long pixel) {
        int *pixel_p = (int *) &pixel;

        auto colors_in = (uint8_t *) pixels_begin;

        auto alpha = (*(uint *) pixel_p) >> 24u;
        if (alpha == 255) {
          *pixels_begin = *pixel_p;
        } else {
          auto colors_out = (uint8_t *) pixel_p;
          colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
          colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
          colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
        }
        ++pixels_begin;
      });
    }
  }

  /**
   * @brief X11 display, window, and attribute handles for capture.
   */
  struct x11_attr_t: public display_t {
    std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

    x11::xdisplay_t xdisplay;  ///< X11 display connection used for capture.
    Window xwindow;  ///< Root window being captured.
    XWindowAttributes xattr;  ///< Cached X11 window attributes used to detect size changes.

    mem_type_e mem_type;  ///< Mem type.

    /**
     * Last X (NOT the streamed monitor!) size.
     * This way we can trigger reinitialization if the dimensions changed while streaming
     */
    // int env_width, env_height;

    /**
     * @brief Open the X11 display and initialize capture attributes.
     *
     * @param mem_type Requested memory path for the capture backend.
     */
    x11_attr_t(mem_type_e mem_type):
        xdisplay {x11::OpenDisplay(nullptr)},
        xwindow {},
        xattr {},
        mem_type {mem_type} {
      x11::InitThreads();
    }

    /**
     * @brief Open the X11 display and cache capture window attributes.
     *
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(const std::string &display_name, const ::video::config_t &config) {
      if (!xdisplay) {
        BOOST_LOG(error) << "Could not open X11 display"sv;
        return -1;
      }

      delay = ::video::capture_frame_interval(config);

      xwindow = DefaultRootWindow(xdisplay.get());

      refresh();

      int streamedMonitor = -1;
      if (!display_name.empty() && std::ranges::all_of(display_name, ::isdigit)) {
        // Resolve (legacy) monitor index from display_name
        streamedMonitor = (int) util::from_view(display_name);
      }

      screen_res_t screenr {x11::rr::GetScreenResources(xdisplay.get(), xwindow)};
      int output = screenr->noutput;

      output_info_t result;
      bool result_found = false;
      int monitor = 0;
      for (int x = 0; x < output; ++x) {
        output_info_t out_info {x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x])};
        if (out_info && x11::output_is_usable(out_info->connection, out_info->crtc)) {
          // Match monitor by index if a valid one is present, otherwise try to resolve by matching output name to display_name
          if ((streamedMonitor >= 0 && monitor == streamedMonitor) || (streamedMonitor < 0 && out_info->name == display_name)) {
            result = std::move(out_info);
            result_found = true;
            break;
          }
          monitor++;
        }
      }

      if (result_found && result->crtc) {
        crtc_info_t crt_info {x11::rr::GetCrtcInfo(xdisplay.get(), screenr.get(), result->crtc)};
        BOOST_LOG(info)
          << "Streaming display: "sv << result->name << " with res "sv << crt_info->width << 'x' << crt_info->height << " offset by "sv << crt_info->x << 'x' << crt_info->y;

        width = crt_info->width;
        height = crt_info->height;
        offset_x = crt_info->x;
        offset_y = crt_info->y;
      } else {
        BOOST_LOG(warning) << "Couldn't get info for requested display ["sv << display_name << "], defaulting to recording entire virtual desktop"sv;
        width = xattr.width;
        height = xattr.height;
      }

      env_width = xattr.width;
      env_height = xattr.height;

      return 0;
    }

    /**
     * Called when the display attributes should change.
     */
    void refresh() {
      x11::GetWindowAttributes(xdisplay.get(), xwindow, &xattr);  // Update xattr's
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return capture_e::ok;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out XImage-backed captured frame returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      refresh();

      // The whole X server changed, so we must reinit everything
      if (xattr.width != env_width || xattr.height != env_height) {
        BOOST_LOG(warning) << "X dimensions changed in non-SHM mode, request reinit"sv;
        return capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (x11_img_t *) img_out.get();

      XImage *x_img {x11::GetImage(xdisplay.get(), xwindow, offset_x, offset_y, width, height, AllPlanes, ZPixmap)};
      img->frame_timestamp = std::chrono::steady_clock::now();

      img->width = x_img->width;
      img->height = x_img->height;
      img->data = (uint8_t *) x_img->data;
      img->row_pitch = x_img->bytes_per_line;
      img->pixel_pitch = x_img->bits_per_pixel / 8;
      img->img.reset(x_img);

      if (cursor) {
        blend_cursor(xdisplay.get(), *img, offset_x, offset_y);
      }

      return capture_e::ok;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<x11_img_t>();
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, false);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == mem_type_e::cuda) {
        return cuda::make_avcodec_encode_device(width, height, false);
      }
#endif

      return std::make_unique<avcodec_encode_device_t>();
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(img_t *img) override {
      // TODO: stop cheating and give black image
      if (!img) {
        return -1;
      };
      auto pull_dummy_img_callback = [&img](std::shared_ptr<platf::img_t> &img_out) -> bool {
        img_out = img->shared_from_this();
        return true;
      };
      std::shared_ptr<platf::img_t> img_out;
      snapshot(pull_dummy_img_callback, img_out, 0s, true);
      return 0;
    }
  };

#ifdef SUNSHINE_BUILD_JETSON_NVMM
  /**
   * @brief GPU-backed X11 image retained for Jetson DMA-BUF capture.
   */
  struct x11_gpu_img_t: public egl::img_descriptor_t {
    NvBufSurface *surface {nullptr};  ///< Pitch-linear BGRX surface rendered by OpenGL.
    bool egl_mapped {false};  ///< Whether the NvBufSurface EGL image is mapped.
    EGLImage target_image {EGL_NO_IMAGE};  ///< EGL image wrapping the output surface.
    gl::tex_t target_texture;  ///< OpenGL texture bound to the output EGL image.
    gl::frame_buf_t target_framebuffer;  ///< Framebuffer used as the compositor target.

    /**
     * @brief Release the output surface and EGL resources.
     */
    ~x11_gpu_img_t() override {
      if (target_image != EGL_NO_IMAGE && target_texture.size() > 0) {
        // NvBufSurface owns the EGL image; only release our GL reference here.
        target_image = EGL_NO_IMAGE;
      }
      if (surface) {
        if (egl_mapped) {
          NvBufSurfaceUnMapEglImage(surface, 0);
        }
        NvBufSurfaceDestroy(surface);
      }
    }
  };

  /**
   * @brief GPU-only X11 compositor using EGL native pixmap imports.
   *
   * The compositor renders the X11 root pixmap and top-level child windows
   * directly into a Jetson NvBufSurface. No full-frame CPU readback or memcpy
   * is performed. The resulting DMA-BUF is consumed by the existing VIC path.
   */
  struct x11_gpu_attr_t: public x11_attr_t {
    egl::display_t egl_display;  ///< EGL display connected to the X11 server.
    std::optional<egl::ctx_t> egl_context;  ///< Current OpenGL context.
    gl::program_t cursor_program;  ///< Shader used to alpha-composite the XFixes cursor.
    gl::tex_t cursor_texture;  ///< Cursor image texture.
    GLuint cursor_vao {0};  ///< Empty vertex array used by the cursor triangle.
    int cursor_width {0};  ///< Width of the uploaded cursor texture.
    int cursor_height {0};  ///< Height of the uploaded cursor texture.

    /**
     * @brief Construct a GPU X11 display.
     */
    x11_gpu_attr_t():
        x11_attr_t(mem_type_e::nvmm) {
    }

    /**
     * @brief Release the OpenGL vertex array used for cursor compositing.
     */
    ~x11_gpu_attr_t() override {
      if (cursor_vao != 0 && egl_context) {
        gl::ctx.DeleteVertexArrays(1, &cursor_vao);
      }
    }

    /**
     * @brief Create an encoder conversion device for the GPU-backed image type.
     *
     * @param pix_fmt Requested encoder pixel format.
     * @return Empty software device; frames are supplied as DMA-BUF descriptors.
     */
    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e /*pix_fmt*/) override {
      return std::make_unique<avcodec_encode_device_t>();
    }

    /**
     * @brief Allocate a Jetson surface used as the GPU compositor target.
     *
     * @return GPU-backed image, or null on allocation failure.
     */
    std::shared_ptr<img_t> alloc_img() override {
      auto img = std::make_shared<x11_gpu_img_t>();
      NvBufSurfaceCreateParams params {};
      params.gpuId = 0;
      params.width = static_cast<std::uint32_t>(width);
      params.height = static_cast<std::uint32_t>(height);
      params.colorFormat = NVBUF_COLOR_FORMAT_BGRx;
      params.layout = NVBUF_LAYOUT_PITCH;
      params.memType = NVBUF_MEM_SURFACE_ARRAY;
      if (NvBufSurfaceCreate(&img->surface, 1, &params) != 0 || !img->surface) {
        BOOST_LOG(error) << "Could not allocate X11 GPU compositor surface"sv;
        return nullptr;
      }
      img->surface->numFilled = 1;
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->surface->surfaceList[0].pitch;
      std::fill_n(img->sd.fds, 4, -1);
      img->sd.width = width;
      img->sd.height = height;
      img->sd.fourcc = 'X' | ('R' << 8) | ('2' << 16) | ('4' << 24);
      img->sd.modifier = 0;
      NvBufSurfaceMapParams map_params {};
      if (NvBufSurfaceGetMapParams(img->surface, 0, &map_params) != 0) {
        BOOST_LOG(error) << "Could not export X11 GPU compositor surface"sv;
        NvBufSurfaceDestroy(img->surface);
        img->surface = nullptr;
        return nullptr;
      }
      img->sd.fds[0] = dup(static_cast<int>(map_params.fd));
      img->sd.pitches[0] = map_params.planes[0].pitch;
      img->sd.offsets[0] = map_params.planes[0].offset;
      if (img->sd.fds[0] < 0) {
        BOOST_LOG(error) << "Could not duplicate X11 GPU compositor DMA-BUF"sv;
        NvBufSurfaceDestroy(img->surface);
        img->surface = nullptr;
        return nullptr;
      }
      return img;
    }

    /**
     * @brief Build a GL texture from an X11 native pixmap.
     *
     * @param pixmap X11 pixmap identifier.
     * @param texture Output texture bound to the EGL image.
     * @param image Output EGL image handle.
     * @param framebuffer Output framebuffer attached to the texture.
     * @param width Pixmap width.
     * @param height Pixmap height.
     * @return True when the pixmap was imported successfully.
     */
    bool import_pixmap(Pixmap pixmap, gl::tex_t &texture, EGLImage &image, gl::frame_buf_t &framebuffer, int width, int height) {
      const EGLAttrib image_attributes[] {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      image = eglCreateImage(
        egl_display.get(),
        EGL_NO_CONTEXT,
        EGL_NATIVE_PIXMAP_KHR,
        reinterpret_cast<EGLClientBuffer>(pixmap),
        image_attributes
      );
      if (image == EGL_NO_IMAGE) {
        return false;
      }

      texture = gl::tex_t::make(1);
      if (texture.size() != 1 || !gl::egl_image_target_texture_2d()) {
        eglDestroyImage(egl_display.get(), image);
        image = EGL_NO_IMAGE;
        return false;
      }
      gl::ctx.BindTexture(GL_TEXTURE_2D, texture[0]);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, image);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

      framebuffer = gl::frame_buf_t::make(1);
      gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer[0]);
      gl::ctx.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture[0], 0);
      const auto status = gl::ctx.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
      gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      return status == GL_FRAMEBUFFER_COMPLETE && width > 0 && height > 0;
    }

    /**
     * @brief Read the pixmap ID used by the X11 desktop background.
     *
     * @return Root background pixmap, or None when no property is available.
     */
    Pixmap root_pixmap() const {
      auto *display = const_cast<Display *>(xdisplay.get());
      const auto atom = XInternAtom(display, "_XROOTPMAP_ID", True);
      if (atom == None) {
        return None;
      }
      Atom type = None;
      int format = 0;
      unsigned long count = 0;
      unsigned long remaining = 0;
      unsigned char *data = nullptr;
      const auto status = XGetWindowProperty(display, xwindow, atom, 0, 1, False, XA_PIXMAP, &type, &format, &count, &remaining, &data);
      Pixmap result = None;
      if (status == Success && data && type == XA_PIXMAP && format == 32 && count == 1) {
        result = *reinterpret_cast<Pixmap *>(data);
      }
      if (data) {
        XFree(data);
      }
      return result;
    }

    /**
     * @brief Composite the current X11 desktop into a DMA-BUF surface.
     *
     * @param img Destination GPU image.
     * @param cursor Whether the client requested cursor compositing.
     * @return Capture status.
     */
    capture_e snapshot(x11_gpu_img_t &img, bool /*cursor*/) {
      if (!egl_context || eglMakeCurrent(egl_display.get(), EGL_NO_SURFACE, EGL_NO_SURFACE, std::get<1>(*egl_context->operator->())) == EGL_FALSE) {
        return capture_e::error;
      }
      if (!img.egl_mapped) {
        if (NvBufSurfaceMapEglImage(img.surface, 0) != 0 || !img.surface->surfaceList[0].mappedAddr.eglImage) {
          return capture_e::error;
        }
        img.egl_mapped = true;
        img.target_image = static_cast<EGLImage>(img.surface->surfaceList[0].mappedAddr.eglImage);
        img.target_texture = gl::tex_t::make(1);
        gl::ctx.BindTexture(GL_TEXTURE_2D, img.target_texture[0]);
        gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, img.target_image);
        gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
        img.target_framebuffer = gl::frame_buf_t::make(1);
        gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, img.target_framebuffer[0]);
        gl::ctx.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, img.target_texture[0], 0);
        gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      }

      gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, img.target_framebuffer[0]);
      gl::ctx.Viewport(0, 0, width, height);
      gl::ctx.Disable(GL_SCISSOR_TEST);
      gl::ctx.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      gl::ctx.Clear(GL_COLOR_BUFFER_BIT);

      auto blit_pixmap = [&](Pixmap pixmap, int source_width, int source_height, int dst_x, int dst_y, int source_x, int source_y, int copy_width, int copy_height) {
        if (pixmap == None || copy_width <= 0 || copy_height <= 0) {
          return;
        }
        gl::tex_t source_texture;
        EGLImage source_image = EGL_NO_IMAGE;
        gl::frame_buf_t source_fb;
        if (!import_pixmap(pixmap, source_texture, source_image, source_fb, source_width, source_height)) {
          return;
        }
        gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, source_fb[0]);
        gl::ctx.BlitFramebuffer(source_x, source_y, source_x + copy_width, source_y + copy_height,
                                dst_x, dst_y, dst_x + copy_width, dst_y + copy_height,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl::ctx.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        eglDestroyImage(egl_display.get(), source_image);
      };

      const auto background = root_pixmap();
      blit_pixmap(background, env_width, env_height, 0, 0, offset_x, offset_y, width, height);

      Window root_return = None;
      Window parent_return = None;
      Window *children = nullptr;
      unsigned int child_count = 0;
      if (XQueryTree(xdisplay.get(), xwindow, &root_return, &parent_return, &children, &child_count)) {
        for (unsigned int index = 0; index < child_count; ++index) {
          XWindowAttributes attributes {};
          if (!XGetWindowAttributes(xdisplay.get(), children[index], &attributes) || attributes.map_state != IsViewable || attributes.width <= 0 || attributes.height <= 0) {
            continue;
          }
          const auto pixmap = x11::composite::NameWindowPixmap(xdisplay.get(), children[index]);
          const auto left = std::max(attributes.x - offset_x, 0);
          const auto top = std::max(attributes.y - offset_y, 0);
          const auto source_left = std::max(offset_x - attributes.x, 0);
          const auto source_top = std::max(offset_y - attributes.y, 0);
          const auto copy_width = std::min(attributes.width - source_left, width - left);
          const auto copy_height = std::min(attributes.height - source_top, height - top);
          blit_pixmap(pixmap, attributes.width, attributes.height, left, height - top - copy_height, source_left, attributes.height - source_top - copy_height, copy_width, copy_height);
          if (pixmap != None) {
            XFreePixmap(xdisplay.get(), pixmap);
          }
        }
        if (children) {
          XFree(children);
        }
      }

      auto *cursor = x11::fix::GetCursorImage(xdisplay.get());
      if (cursor && cursor_program.handle() != std::numeric_limits<GLuint>::max()) {
        const auto cursor_x = static_cast<int>(cursor->x) - static_cast<int>(cursor->xhot) - offset_x;
        const auto cursor_y = static_cast<int>(cursor->y) - static_cast<int>(cursor->yhot) - offset_y;
        if (cursor_x < width && cursor_y < height && cursor_x + cursor->width > 0 && cursor_y + cursor->height > 0) {
          if (cursor_width != cursor->width || cursor_height != cursor->height) {
            cursor_width = cursor->width;
            cursor_height = cursor->height;
            cursor_texture = gl::tex_t::make(1);
            gl::ctx.BindTexture(GL_TEXTURE_2D, cursor_texture[0]);
            gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl::ctx.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cursor_width, cursor_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, cursor->pixels);
            gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
          } else {
            gl::ctx.BindTexture(GL_TEXTURE_2D, cursor_texture[0]);
            gl::ctx.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cursor_width, cursor_height, GL_BGRA, GL_UNSIGNED_BYTE, cursor->pixels);
            gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
          }
          const auto left = std::max(cursor_x, 0);
          const auto top = std::max(cursor_y, 0);
          const auto right = std::min(cursor_x + cursor->width, width);
          const auto bottom = std::min(cursor_y + cursor->height, height);
          gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, img.target_framebuffer[0]);
          gl::ctx.Viewport(left, height - bottom, right - left, bottom - top);
          gl::ctx.UseProgram(cursor_program.handle());
          gl::ctx.ActiveTexture(GL_TEXTURE0);
          gl::ctx.BindTexture(GL_TEXTURE_2D, cursor_texture[0]);
          gl::ctx.Enable(GL_BLEND);
          gl::ctx.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          gl::ctx.BindVertexArray(cursor_vao);
          gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);
          gl::ctx.BindVertexArray(0);
          gl::ctx.Disable(GL_BLEND);
          gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
        }
        x11::Free(cursor);
      }

      gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      // The VIC consumer runs in a separate driver queue.  A flush alone can
      // leave the exported DMA-BUF backed by an unfinished GL render, which
      // appears as an all-black frame to the encoder.
      gl::ctx.Finish();
      img.frame_timestamp = std::chrono::steady_clock::now();
      img.sd.fds[0] = -1;
      NvBufSurfaceMapParams map_params {};
      if (NvBufSurfaceGetMapParams(img.surface, 0, &map_params) != 0) {
        return capture_e::error;
      }
      img.sd.fds[0] = dup(static_cast<int>(map_params.fd));
      img.sd.pitches[0] = map_params.planes[0].pitch;
      img.sd.offsets[0] = map_params.planes[0].offset;
      img.data = nullptr;
      return img.sd.fds[0] >= 0 ? capture_e::ok : capture_e::error;
    }

    /**
     * @brief Capture frames from the X11 compositor into GPU surfaces.
     */
    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();
      while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
        }
        next_frame += delay;
        if (next_frame < now) {
          next_frame = now + delay;
        }
        std::shared_ptr<img_t> output;
        if (!pull_free_image_cb(output)) {
          return capture_e::interrupted;
        }
        auto *img = dynamic_cast<x11_gpu_img_t *>(output.get());
        if (!img || snapshot(*img, cursor && *cursor) != capture_e::ok || !push_captured_image_cb(std::move(output), true)) {
          return capture_e::error;
        }
      }
    }

    /**
     * @brief Initialize X11, XComposite, EGL, and the OpenGL context.
     */
    int init(const std::string &display_name, const ::video::config_t &config) {
      if (x11_attr_t::init(display_name, config) || x11::composite::init()) {
        return -1;
      }
      egl_display = egl::make_display(xdisplay.get());
      if (!egl_display) {
        return -1;
      }
      egl_context = egl::make_ctx(egl_display.get());
      if (!egl_context) {
        return -1;
      }
      if (!gl::egl_image_target_texture_2d()) {
        return -1;
      }
      const auto vertex = gl::shader_t::compile(
        "#version 330\n"
        "out vec2 texcoord;\n"
        "void main() {\n"
        "  const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
        "  texcoord = (positions[gl_VertexID] + 1.0) * 0.5;\n"
        "  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);\n"
        "}\n",
        GL_VERTEX_SHADER
      );
      const auto fragment = gl::shader_t::compile(
        "#version 330\n"
        "in vec2 texcoord;\n"
        "out vec4 color;\n"
        "uniform sampler2D cursor_texture;\n"
        "void main() { color = texture(cursor_texture, texcoord); }\n",
        GL_FRAGMENT_SHADER
      );
      if (vertex.has_right() || fragment.has_right()) {
        return -1;
      }
      auto program = gl::program_t::link(vertex.left(), fragment.left());
      if (program.has_right()) {
        return -1;
      }
      cursor_program = std::move(program.left());
      gl::ctx.GenVertexArrays(1, &cursor_vao);
      BOOST_LOG(info) << "X11 GPU compositor enabled through EGL native pixmaps"sv;
      return 0;
    }

    /**
     * @brief Report that dummy frames are valid for this backend.
     */
    int dummy_img(img_t * /*img*/) override {
      return 0;
    }
  };
#endif

  /**
   * @brief X11 shared-memory image dimensions and identifiers.
   */
  struct shm_attr_t: public x11_attr_t {
    x11::xdisplay_t shm_xdisplay;  ///< X11 display held separately to prevent races with x11_attr_t::xdisplay.
    xcb_shared_connect_t xcb;  ///< XCB connection shared with every per-frame SHM buffer.
    xcb_screen_t *display;  ///< XCB screen containing the captured root window.

    task_pool_util::TaskPool::task_id_t refresh_task_id;  ///< Refresh task ID.

    /**
     * @brief Refresh X11 shared-memory capture after a scheduled delay.
     */
    void delayed_refresh() {
      refresh();

      refresh_task_id = task_pool.pushDelayed(&shm_attr_t::delayed_refresh, 2s, this).task_id;
    }

    /**
     * @brief Open an X11 shared-memory capture backend.
     *
     * @param mem_type Requested memory path for the capture backend.
     */
    shm_attr_t(mem_type_e mem_type):
        x11_attr_t(mem_type),
        shm_xdisplay {x11::OpenDisplay(nullptr)} {
      refresh_task_id = task_pool.pushDelayed(&shm_attr_t::delayed_refresh, 2s, this).task_id;
    }

    ~shm_attr_t() override {
      while (!task_pool.cancel(refresh_task_id));
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return capture_e::ok;
    }

    /**
     * @brief Capture a display frame into the provided image object.
     *
     * @param pull_free_image_cb Callback that provides an available image buffer.
     * @param img_out Shared-memory captured frame returned to the streaming pipeline.
     * @param timeout Maximum time to wait for the operation.
     * @param cursor Cursor image or visibility state to composite.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      // The whole X server changed, so we must reinit everything
      if (xattr.width != env_width || xattr.height != env_height) {
        BOOST_LOG(warning) << "X dimensions changed in SHM mode, request reinit"sv;
        return capture_e::reinit;
      } else {
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        const auto img = std::static_pointer_cast<shm_img_t>(img_out);

        auto img_cookie = xcb::shm_get_image_unchecked(xcb.get(), display->root, offset_x, offset_y, width, height, ~0, XCB_IMAGE_FORMAT_Z_PIXMAP, img->seg, 0);
        auto frame_timestamp = std::chrono::steady_clock::now();

        xcb_img_t img_reply {xcb::shm_get_image_reply(xcb.get(), img_cookie, nullptr)};
        if (!img_reply) {
          BOOST_LOG(error) << "Could not get image reply"sv;
          return capture_e::reinit;
        }

        img_out->frame_timestamp = frame_timestamp;

        if (cursor) {
          blend_cursor(shm_xdisplay.get(), *img_out, offset_x, offset_y);
        }

        return capture_e::ok;
      }
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<img_t> alloc_img() override {
      auto img = std::make_shared<shm_img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->xcb = xcb;

      img->shm_id.id = shmget(IPC_PRIVATE, frame_size(), IPC_CREAT | 0600);
      if (img->shm_id.id == -1) {
        BOOST_LOG(error) << "Could not allocate per-frame X11 shared memory"sv;
        return nullptr;
      }

      img->shm_data.data = shmat(img->shm_id.id, nullptr, 0);
      if (reinterpret_cast<std::uintptr_t>(img->shm_data.data) == static_cast<std::uintptr_t>(-1)) {
        BOOST_LOG(error) << "Could not map per-frame X11 shared memory"sv;
        return nullptr;
      }
      img->data = static_cast<std::uint8_t *>(img->shm_data.data);
      img->seg = xcb::generate_id(xcb.get());
      xcb::shm_attach(xcb.get(), img->seg, img->shm_id.id, false);
      if (xcb::flush(xcb.get()) <= 0) {
        BOOST_LOG(error) << "Could not attach per-frame X11 shared memory"sv;
        return nullptr;
      }

      return img;
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    /**
     * @brief Initialize X11 shared-memory capture for the selected display.
     *
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(const std::string &display_name, const ::video::config_t &config) {
      if (x11_attr_t::init(display_name, config)) {
        return 1;
      }

      shm_xdisplay.reset(x11::OpenDisplay(nullptr));
      auto *connection = xcb::connect(nullptr, nullptr);
      if (!connection || xcb::connection_has_error(connection)) {
        if (connection) {
          xcb::disconnect(connection);
        }
        return -1;
      }
      xcb = xcb_shared_connect_t {connection, xcb::disconnect};

      if (!xcb::get_extension_data(xcb.get(), xcb::shm_id)->present) {
        BOOST_LOG(error) << "Missing SHM extension"sv;

        return -1;
      }

      auto iter = xcb::setup_roots_iterator(xcb::get_setup(xcb.get()));
      display = iter.data;

      return 0;
    }

    /**
     * @brief Calculate the XCB shared-memory frame size.
     *
     * @return Frame size in bytes for BGRA pixels.
     */
    std::uint32_t frame_size() const {
      return width * height * 4;
    }
  };

  /**
   * @brief Create an X11 display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return X11 display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> x11_display(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi &&
        hwdevice_type != platf::mem_type_e::cuda && hwdevice_type != platf::mem_type_e::nvmm) {
      BOOST_LOG(error) << "Could not initialize x11 display with the given hw device type"sv;
      return nullptr;
    }

    if (xcb::init_shm() || xcb::init() || x11::init() || x11::rr::init() || x11::fix::init()) {
      BOOST_LOG(error) << "Couldn't init x11 libraries"sv;

      return nullptr;
    }

#ifdef SUNSHINE_BUILD_JETSON_NVMM
    if (hwdevice_type == platf::mem_type_e::nvmm) {
      auto gpu_disp = std::make_shared<x11_gpu_attr_t>();
      if (gpu_disp->init(display_name, config) == 0) {
        return gpu_disp;
      }
      BOOST_LOG(warning) << "X11 GPU compositor unavailable; falling back to XShm capture"sv;
    }
#endif

    // Attempt to use shared memory X11 to avoid copying the frame
    auto shm_disp = std::make_shared<shm_attr_t>(hwdevice_type);

    auto status = shm_disp->init(display_name, config);
    if (status > 0) {
      // x11_attr_t::init() failed, don't bother trying again.
      return nullptr;
    }

    if (status == 0) {
      return shm_disp;
    }

    // Fallback
    auto x11_disp = std::make_shared<x11_attr_t>(hwdevice_type);
    if (x11_disp->init(display_name, config)) {
      return nullptr;
    }

    return x11_disp;
  }

  /**
   * @brief Enumerate display names accepted by the X11 backend.
   *
   * @return X11 display names, or an empty list when X11 probing fails.
   */
  std::vector<std::string> x11_display_names() {
    if (load_x11() || load_xcb()) {
      BOOST_LOG(error) << "Couldn't init x11 libraries"sv;

      return {};
    }

    BOOST_LOG(info) << "Detecting displays"sv;

    x11::xdisplay_t xdisplay {x11::OpenDisplay(nullptr)};
    if (!xdisplay) {
      return {};
    }

    auto xwindow = DefaultRootWindow(xdisplay.get());
    screen_res_t screenr {x11::rr::GetScreenResources(xdisplay.get(), xwindow)};
    int output = screenr->noutput;

    std::vector<std::string> names;
    int monitor = 0;
    for (int x = 0; x < output; ++x) {
      output_info_t out_info {x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x])};
      if (out_info) {
        const auto connected = x11::output_is_usable(out_info->connection, out_info->crtc);
        BOOST_LOG(info) << "Detected display: "sv << out_info->name << " (id: "sv << monitor << ") connected: "sv << connected;
        if (connected) {
          names.emplace_back(out_info->name);
          ++monitor;
        }
      }
    }

    return names;
  }

  /**
   * @brief Release image resources.
   */
  void freeImage(XImage *p) {
    XDestroyImage(p);
  }

  /**
   * @brief Release x resources.
   */
  void freeX(XFixesCursorImage *p) {
    x11::Free(p);
  }

  /**
   * @brief Load XCB entry points used by the X11 capture backend.
   */
  int load_xcb() {
    // This will be called once only
    static int xcb_status = xcb::init_shm() || xcb::init();

    return xcb_status;
  }

  /**
   * @brief Load X11 entry points used by the X11 capture backend.
   */
  int load_x11() {
    // This will be called once only
    static int x11_status =
      window_system == window_system_e::NONE ||
      x11::init() || x11::rr::init() || x11::fix::init();

    return x11_status;
  }

  namespace x11 {
    bool output_is_usable(int connection, unsigned long crtc) {
      return connection == RR_Connected && crtc != None;
    }

    std::optional<cursor_t> cursor_t::make() {
      if (load_x11()) {
        return std::nullopt;
      }

      cursor_t cursor;

      cursor.ctx.reset((cursor_ctx_t::pointer) x11::OpenDisplay(nullptr));

      return cursor;
    }

    void cursor_t::capture(egl::cursor_t &img) {
      auto display = (xdisplay_t::pointer) ctx.get();

      xcursor_t xcursor = fix::GetCursorImage(display);

      if (img.serial != xcursor->cursor_serial) {
        auto buf_size = xcursor->width * xcursor->height * sizeof(int);

        if (img.buffer.size() < buf_size) {
          img.buffer.resize(buf_size);
        }

        std::transform(xcursor->pixels, xcursor->pixels + buf_size / 4, (int *) img.buffer.data(), [](long pixel) -> int {
          return pixel;
        });
      }

      img.data = img.buffer.data();
      img.width = img.src_w = xcursor->width;
      img.height = img.src_h = xcursor->height;
      img.x = xcursor->x - xcursor->xhot;
      img.y = xcursor->y - xcursor->yhot;
      img.pixel_pitch = 4;
      img.row_pitch = img.pixel_pitch * img.width;
      img.serial = xcursor->cursor_serial;
    }

    void cursor_t::blend(img_t &img, int offsetX, int offsetY) {
      blend_cursor((xdisplay_t::pointer) ctx.get(), img, offsetX, offsetY);
    }

    /**
     * @brief Open and initialize the display connection used for capture.
     */
    xdisplay_t make_display() {
      if (platf::load_x11()) {
        return nullptr;
      }
      return OpenDisplay(nullptr);
    }

    /**
     * @brief Release display resources.
     */
    void freeDisplay(_XDisplay *xdisplay) {
      CloseDisplay(xdisplay);
    }

    /**
     * @brief Release cursor context resources.
     *
     * @param ctx Native context object used by the operation or callback.
     */
    void freeCursorCtx(cursor_ctx_t::pointer ctx) {
      CloseDisplay((xdisplay_t::pointer) ctx);
    }
  }  // namespace x11
}  // namespace platf
