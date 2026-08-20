/**
 * @file src/platform/linux/x11grab.cpp
 * @brief Definitions for x11 capture.
 */
// standard includes
#include <fstream>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>

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
  std::mutex composite_error_mutex;  ///< Serializes the process-global Xlib error handler around XComposite requests.
  thread_local bool composite_request_failed {false};  ///< Whether the current trapped XComposite request failed.

  /**
   * @brief Record an XComposite request failure without terminating capture.
   *
   * @param display X11 display reporting the asynchronous error.
   * @param event X11 error details.
   * @return Zero after recording the failure.
   */
  int trap_composite_error(Display * /*display*/, XErrorEvent *event) {
    if (event && event->error_code == BadMatch) {
      composite_request_failed = true;
    }
    return 0;
  }

  /**
   * @brief GPU-backed X11 image retained for Jetson DMA-BUF capture.
   */
  struct x11_gpu_img_t: public egl::img_descriptor_t {
    gbm::bo_t bo;  ///< NVIDIA GBM buffer exported to EGL and the Jetson VIC.
    std::optional<egl::rgb_t> target_rgb;  ///< GBM DMA-BUF imported into the X11 EGL display.
    gl::frame_buf_t target_framebuffer;  ///< Framebuffer used as the compositor target.

    /**
     * @brief Release output GL resources before the backing GBM buffer.
     */
    ~x11_gpu_img_t() override {
      target_framebuffer = gl::frame_buf_t {};
      target_rgb.reset();
    }
  };

  /**
   * @brief GPU-only X11 compositor using EGL native pixmap imports.
   *
   * The compositor renders the X11 root pixmap and top-level child windows
   * directly into a NVIDIA GBM DMA-BUF. No full-frame CPU readback or memcpy
   * is performed. The resulting DMA-BUF is consumed by the existing VIC path.
   */
  struct x11_gpu_attr_t: public x11_attr_t {
    /**
     * @brief XComposite pixmap imported once and reused across capture frames.
     */
    struct cached_pixmap_t {
      Pixmap pixmap {None};  ///< X11 pixmap name backing the redirected window.
      EGLImage image {EGL_NO_IMAGE};  ///< EGL image importing the X11 pixmap.
      gl::tex_t texture;  ///< GL texture sampling the EGL image.
      int width {0};  ///< Pixmap width when the cache entry was created.
      int height {0};  ///< Pixmap height when the cache entry was created.
      int depth {0};  ///< X11 drawable depth when the cache entry was created.
      int x {0};  ///< Window X position in root coordinates.
      int y {0};  ///< Window Y position in root coordinates.
      std::uint64_t seen_frame {0};  ///< Capture generation in which the window was last visible.
      bool visible {false};  ///< Whether the window is currently mapped and drawable.
      bool owns_pixmap {false};  ///< Whether this client must free the X11 pixmap name.
    };

    file_t gbm_fd;  ///< NVIDIA display render-node file descriptor retained by the GBM device.
    gbm::gbm_t gbm_device;  ///< NVIDIA GBM device used to allocate compositor targets.
    egl::display_t egl_display;  ///< EGL display connected to the X11 server.
    std::optional<egl::ctx_t> egl_context;  ///< Current OpenGL context.
    gl::program_t compositor_program;  ///< Shader used to sample imported X11 pixmaps.
    gl::tex_t cursor_texture;  ///< Cursor image texture.
    std::unordered_map<Window, cached_pixmap_t> window_pixmaps;  ///< Imported top-level window pixmaps.
    std::vector<Window> stacking_order;  ///< Top-level windows ordered from bottom to top.
    cached_pixmap_t root_pixmap_cache;  ///< Imported desktop background pixmap.
    std::uint64_t compositor_frame {0};  ///< Monotonic generation used to retire stale window cache entries.
    bool window_tree_dirty {true};  ///< Whether X11 events require a window-tree refresh.
    bool root_pixmap_dirty {true};  ///< Whether the desktop background property must be queried again.
    GLuint compositor_vao {0};  ///< Empty vertex array used by the compositor triangle.
    GLint uv_rect_location {-1};  ///< Shader location for normalized source crop coordinates.
    int cursor_width {0};  ///< Width of the uploaded cursor texture.
    int cursor_height {0};  ///< Height of the uploaded cursor texture.

    /**
     * @brief Construct a GPU X11 display.
     */
    x11_gpu_attr_t():
        x11_attr_t(mem_type_e::nvmm) {
    }

    /**
     * @brief Release XComposite redirection and OpenGL compositor resources.
     */
    ~x11_gpu_attr_t() override {
      if (egl_context) {
        eglMakeCurrent(egl_display.get(), EGL_NO_SURFACE, EGL_NO_SURFACE, std::get<1>(*egl_context->operator->()));
        for (auto &[window, cached] : window_pixmaps) {
          release_cached_pixmap(cached);
        }
        window_pixmaps.clear();
        release_cached_pixmap(root_pixmap_cache);
        if (compositor_vao != 0) {
          gl::ctx.DeleteVertexArrays(1, &compositor_vao);
        }
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
     * @brief Allocate a renderable GBM DMA-BUF used as the compositor target.
     *
     * @return GPU-backed image, or null on allocation failure.
     */
    std::shared_ptr<img_t> alloc_img() override {
      constexpr std::uint32_t drm_abgr8888 = 'A' | ('B' << 8) | ('2' << 16) | ('4' << 24);  ///< DRM ABGR8888 fourcc.
      auto img = std::make_shared<x11_gpu_img_t>();
      if (!gbm_device || !gbm::bo_create || !gbm::bo_get_fd || !gbm::bo_get_stride || !gbm::bo_get_modifier) {
        BOOST_LOG(error) << "X11 GPU compositor GBM device is unavailable"sv;
        return nullptr;
      }
      img->bo.reset(gbm::bo_create(
        gbm_device.get(),
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        drm_abgr8888,
        gbm::bo_use_rendering
      ));
      if (!img->bo) {
        BOOST_LOG(error) << "Could not allocate X11 GPU compositor GBM buffer"sv;
        return nullptr;
      }
      std::fill_n(img->sd.fds, 4, -1);
      img->sd.fds[0] = gbm::bo_get_fd(img->bo.get());
      if (img->sd.fds[0] < 0) {
        BOOST_LOG(error) << "Could not export X11 GPU compositor GBM buffer"sv;
        return nullptr;
      }
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = static_cast<int>(gbm::bo_get_stride(img->bo.get()));
      img->y_invert = true;
      img->sd.width = width;
      img->sd.height = height;
      img->sd.fourcc = drm_abgr8888;
      img->sd.modifier = gbm::bo_get_modifier(img->bo.get());
      img->sd.pitches[0] = static_cast<std::uint32_t>(img->row_pitch);
      img->sd.offsets[0] = 0;
      return img;
    }

    /**
     * @brief Release one cached XComposite pixmap import.
     *
     * @param cached Cache entry to reset.
     */
    void release_cached_pixmap(cached_pixmap_t &cached) {
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      cached.texture = gl::tex_t {};
      if (cached.image != EGL_NO_IMAGE) {
        eglDestroyImage(egl_display.get(), cached.image);
      }
      if (cached.owns_pixmap && cached.pixmap != None) {
        XFreePixmap(xdisplay.get(), cached.pixmap);
      }
      cached.image = EGL_NO_IMAGE;
      cached.pixmap = None;
      cached.width = 0;
      cached.height = 0;
      cached.depth = 0;
      cached.x = 0;
      cached.y = 0;
      cached.seen_frame = 0;
      cached.visible = false;
      cached.owns_pixmap = false;
    }

    /**
     * @brief Import or replace a persistent XComposite pixmap cache entry.
     *
     * @param cached Cache entry to populate.
     * @param pixmap X11 pixmap to import.
     * @param source_width Pixmap width.
     * @param source_height Pixmap height.
     * @param depth X11 drawable depth.
     * @param owns_pixmap Whether this client owns the pixmap XID.
     * @return True when the pixmap is ready for sampling.
     */
    bool cache_pixmap(cached_pixmap_t &cached, Pixmap pixmap, int source_width, int source_height, int depth, bool owns_pixmap) {
      release_cached_pixmap(cached);
      if (pixmap == None) {
        return false;
      }
      gl::tex_t texture;
      EGLImage image = EGL_NO_IMAGE;
      if (!import_pixmap(pixmap, texture, image)) {
        if (owns_pixmap) {
          XFreePixmap(xdisplay.get(), pixmap);
        }
        return false;
      }
      cached.pixmap = pixmap;
      cached.image = image;
      cached.texture = std::move(texture);
      cached.width = source_width;
      cached.height = source_height;
      cached.depth = depth;
      cached.owns_pixmap = owns_pixmap;
      return true;
    }

    /**
     * @brief Name a redirected window pixmap while trapping BadMatch.
     *
     * GNOME may expose viewable helper windows that Mutter did not redirect.
     * XComposite reports those asynchronously, so isolate the single request
     * behind XSync and return None instead of invoking Xlib's fatal handler.
     *
     * @param window X11 window whose redirected backing pixmap is requested.
     * @return Named pixmap, or None when the window is not redirectable.
     */
    Pixmap name_window_pixmap(Window window) {
      std::lock_guard lock {composite_error_mutex};
      XSync(xdisplay.get(), False);
      composite_request_failed = false;
      const auto previous_handler = XSetErrorHandler(trap_composite_error);
      const auto pixmap = x11::composite::NameWindowPixmap(xdisplay.get(), window);
      XSync(xdisplay.get(), False);
      XSetErrorHandler(previous_handler);
      if (composite_request_failed) {
        return None;
      }
      return pixmap;
    }

    /**
     * @brief Refresh the top-level window cache and stacking order.
     *
     * @return True when the X11 window tree was read successfully.
     */
    bool refresh_window_tree() {
      Window root_return = None;
      Window parent_return = None;
      Window *children = nullptr;
      unsigned int child_count = 0;
      if (!XQueryTree(xdisplay.get(), xwindow, &root_return, &parent_return, &children, &child_count)) {
        return false;
      }

      ++compositor_frame;
      stacking_order.clear();
      stacking_order.reserve(child_count);
      for (unsigned int index = 0; index < child_count; ++index) {
        XWindowAttributes attributes {};
        if (!XGetWindowAttributes(xdisplay.get(), children[index], &attributes) ||
            attributes.map_state != IsViewable || attributes.width <= 0 || attributes.height <= 0) {
          continue;
        }

        auto [cached_it, inserted] = window_pixmaps.try_emplace(children[index]);
        auto &cached = cached_it->second;
        const auto recreate = inserted || cached.texture.size() != 1 || cached.width != attributes.width || cached.height != attributes.height || cached.depth != attributes.depth;
        if (recreate) {
          const auto pixmap = name_window_pixmap(children[index]);
          if (!cache_pixmap(cached, pixmap, attributes.width, attributes.height, attributes.depth, true)) {
            window_pixmaps.erase(cached_it);
            continue;
          }
        }
        cached.x = attributes.x;
        cached.y = attributes.y;
        cached.visible = true;
        cached.seen_frame = compositor_frame;
        stacking_order.push_back(children[index]);
      }
      if (children) {
        XFree(children);
      }

      for (auto it = window_pixmaps.begin(); it != window_pixmaps.end();) {
        if (it->second.seen_frame != compositor_frame) {
          release_cached_pixmap(it->second);
          it = window_pixmaps.erase(it);
        } else {
          ++it;
        }
      }
      window_tree_dirty = false;
      return true;
    }

    /**
     * @brief Consume X11 structure notifications and update cached geometry.
     *
     * Window contents update in-place through their redirected pixmaps. Only
     * structure changes need XQueryTree or a new XComposite pixmap import.
     */
    void process_window_events() {
      while (XPending(xdisplay.get()) > 0) {
        XEvent event {};
        XNextEvent(xdisplay.get(), &event);
        switch (event.type) {
          case ConfigureNotify:
            {
              auto cached_it = window_pixmaps.find(event.xconfigure.window);
              if (cached_it == window_pixmaps.end()) {
                window_tree_dirty = true;
                break;
              }
              auto &cached = cached_it->second;
              const auto resized = cached.width != event.xconfigure.width || cached.height != event.xconfigure.height;
              const auto depth = cached.depth;
              if (resized) {
                release_cached_pixmap(cached);
                cached.width = event.xconfigure.width;
                cached.height = event.xconfigure.height;
                cached.depth = depth;
              }
              cached.x = event.xconfigure.x;
              cached.y = event.xconfigure.y;
              cached.visible = true;
              break;
            }
          case UnmapNotify:
          case DestroyNotify:
            {
              const auto window = event.type == UnmapNotify ? event.xunmap.window : event.xdestroywindow.window;
              auto cached_it = window_pixmaps.find(window);
              if (cached_it != window_pixmaps.end()) {
                release_cached_pixmap(cached_it->second);
                window_pixmaps.erase(cached_it);
              }
              std::erase(stacking_order, window);
              break;
            }
          case PropertyNotify:
            if (event.xproperty.window == xwindow) {
              root_pixmap_dirty = true;
            }
            break;
          case CreateNotify:
          case MapNotify:
          case ReparentNotify:
          case CirculateNotify:
            window_tree_dirty = true;
            break;
          default:
            break;
        }
      }
    }

    /**
     * @brief Build a sampleable GL texture from an X11 native pixmap.
     *
     * NVIDIA's EGL native-pixmap import is reliably sampleable as a texture,
     * but it is not guaranteed to be framebuffer-renderable. Keep the imported
     * pixmap on the texture path and render it with the compositor shader.
     *
     * @param pixmap X11 pixmap identifier.
     * @param texture Output texture bound to the EGL image.
     * @param image Output EGL image handle.
     * @return True when the pixmap was imported successfully.
     */
    bool import_pixmap(Pixmap pixmap, gl::tex_t &texture, EGLImage &image) {
      const EGLAttrib image_attributes[] {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      while (gl::ctx.GetError() != GL_NO_ERROR) {
      }
      while (eglGetError() != EGL_SUCCESS) {
      }
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
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      gl::egl_image_target_texture_2d()(GL_TEXTURE_2D, image);
      const auto gl_error = gl::ctx.GetError();
      const auto egl_error = eglGetError();
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      if (gl_error != GL_NO_ERROR || egl_error != EGL_SUCCESS) {
        eglDestroyImage(egl_display.get(), image);
        image = EGL_NO_IMAGE;
        return false;
      }
      return true;
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
    capture_e snapshot(x11_gpu_img_t &img, bool cursor_enabled) {
      if (!egl_context || eglMakeCurrent(egl_display.get(), EGL_NO_SURFACE, EGL_NO_SURFACE, std::get<1>(*egl_context->operator->())) == EGL_FALSE) {
        return capture_e::error;
      }
      if (!img.target_rgb) {
        img.target_rgb = egl::import_source(egl_display.get(), img.sd);
        if (!img.target_rgb) {
          BOOST_LOG(error) << "Could not import X11 GPU compositor GBM buffer into EGL"sv;
          return capture_e::error;
        }
        img.target_framebuffer = gl::frame_buf_t::make(1);
        gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, img.target_framebuffer[0]);
        gl::ctx.FramebufferTexture2D(
          GL_DRAW_FRAMEBUFFER,
          GL_COLOR_ATTACHMENT0,
          GL_TEXTURE_2D,
          (*img.target_rgb)->tex[0],
          0
        );
        const auto target_status = gl::ctx.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        const auto target_gl_error = gl::ctx.GetError();
        gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (target_gl_error != GL_NO_ERROR || target_status != GL_FRAMEBUFFER_COMPLETE) {
          BOOST_LOG(error) << "X11 GPU compositor GBM framebuffer failed; GL="sv << target_gl_error << ", framebuffer="sv << target_status;
          return capture_e::error;
        }
      }

      gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, img.target_framebuffer[0]);
      gl::ctx.Viewport(0, 0, width, height);
      gl::ctx.Disable(GL_SCISSOR_TEST);
      gl::ctx.Disable(GL_BLEND);
      gl::ctx.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      gl::ctx.Clear(GL_COLOR_BUFFER_BIT);
      gl::ctx.UseProgram(compositor_program.handle());
      gl::ctx.BindVertexArray(compositor_vao);
      gl::ctx.ActiveTexture(GL_TEXTURE0);

      auto draw_texture = [&](GLuint texture, int source_width, int source_height, int dst_left, int dst_top, int source_left, int source_top, int copy_width, int copy_height, bool alpha_blend) {
        if (texture == 0 || source_width <= 0 || source_height <= 0 || copy_width <= 0 || copy_height <= 0) {
          return false;
        }
        const auto dst_bottom = height - dst_top - copy_height;
        const auto source_bottom = source_height - source_top - copy_height;
        if (dst_left < 0 || dst_bottom < 0 || source_left < 0 || source_bottom < 0) {
          return false;
        }
        gl::ctx.Viewport(dst_left, dst_bottom, copy_width, copy_height);
        gl::ctx.Uniform4f(
          uv_rect_location,
          static_cast<float>(source_left) / static_cast<float>(source_width),
          static_cast<float>(source_bottom) / static_cast<float>(source_height),
          static_cast<float>(copy_width) / static_cast<float>(source_width),
          static_cast<float>(copy_height) / static_cast<float>(source_height)
        );
        gl::ctx.BindTexture(GL_TEXTURE_2D, texture);
        if (alpha_blend) {
          gl::ctx.Enable(GL_BLEND);
          gl::ctx.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
          gl::ctx.Disable(GL_BLEND);
        }
        gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);
        const auto draw_error = gl::ctx.GetError();
        if (draw_error != GL_NO_ERROR) {
          BOOST_LOG(error) << "X11 GPU compositor draw failed with GL error "sv << draw_error;
          return false;
        }
        return true;
      };

      process_window_events();
      if (window_tree_dirty && !refresh_window_tree()) {
        BOOST_LOG(error) << "X11 GPU compositor could not refresh the window tree"sv;
        return capture_e::error;
      }

      bool rendered_any_pixmap = false;
      if (root_pixmap_dirty || root_pixmap_cache.texture.size() != 1) {
        const auto background = root_pixmap();
        if (background != root_pixmap_cache.pixmap || root_pixmap_cache.texture.size() != 1) {
          cache_pixmap(root_pixmap_cache, background, env_width, env_height, 24, false);
        }
        root_pixmap_dirty = false;
      }
      if (root_pixmap_cache.texture.size() == 1) {
        rendered_any_pixmap |= draw_texture(root_pixmap_cache.texture[0], env_width, env_height, 0, 0, offset_x, offset_y, width, height, false);
      }

      for (const auto window : stacking_order) {
        auto cached_it = window_pixmaps.find(window);
        if (cached_it == window_pixmaps.end() || !cached_it->second.visible) {
          continue;
        }
        auto &cached = cached_it->second;
        if (cached.texture.size() != 1) {
          const auto x = cached.x;
          const auto y = cached.y;
          const auto source_width = cached.width;
          const auto source_height = cached.height;
          const auto depth = cached.depth;
          const auto pixmap = name_window_pixmap(window);
          if (!cache_pixmap(cached, pixmap, source_width, source_height, depth, true)) {
            continue;
          }
          cached.x = x;
          cached.y = y;
          cached.visible = true;
        }
        const auto left = std::max(cached.x - offset_x, 0);
        const auto top = std::max(cached.y - offset_y, 0);
        const auto source_left = std::max(offset_x - cached.x, 0);
        const auto source_top = std::max(offset_y - cached.y, 0);
        const auto copy_width = std::min(cached.width - source_left, width - left);
        const auto copy_height = std::min(cached.height - source_top, height - top);
        rendered_any_pixmap |= draw_texture(
          cached.texture[0], cached.width, cached.height, left, top,
          source_left, source_top, copy_width, copy_height, cached.depth == 32
        );
      }

      if (!rendered_any_pixmap) {
        gl::ctx.BindVertexArray(0);
        gl::ctx.UseProgram(0);
        gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        BOOST_LOG(error) << "X11 GPU compositor could not import any desktop pixmaps"sv;
        return capture_e::error;
      }

      if (cursor_enabled) {
        auto *cursor = x11::fix::GetCursorImage(xdisplay.get());
        if (cursor) {
          const auto cursor_x = static_cast<int>(cursor->x) - static_cast<int>(cursor->xhot) - offset_x;
          const auto cursor_y = static_cast<int>(cursor->y) - static_cast<int>(cursor->yhot) - offset_y;
          if (cursor_x < width && cursor_y < height && cursor_x + cursor->width > 0 && cursor_y + cursor->height > 0) {
            if (cursor_width != cursor->width || cursor_height != cursor->height) {
              cursor_width = cursor->width;
              cursor_height = cursor->height;
              cursor_texture = gl::tex_t::make(1);
              gl::ctx.BindTexture(GL_TEXTURE_2D, cursor_texture[0]);
              gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
              gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
              gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
              gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
              gl::ctx.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cursor_width, cursor_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, cursor->pixels);
            } else {
              gl::ctx.BindTexture(GL_TEXTURE_2D, cursor_texture[0]);
              gl::ctx.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cursor_width, cursor_height, GL_BGRA, GL_UNSIGNED_BYTE, cursor->pixels);
            }
            gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
            const auto left = std::max(cursor_x, 0);
            const auto top = std::max(cursor_y, 0);
            const auto source_left = std::max(-cursor_x, 0);
            const auto source_top = std::max(-cursor_y, 0);
            const auto copy_width = std::min(static_cast<int>(cursor->width) - source_left, width - left);
            const auto copy_height = std::min(static_cast<int>(cursor->height) - source_top, height - top);
            draw_texture(cursor_texture[0], cursor_width, cursor_height, left, top, source_left, source_top, copy_width, copy_height, true);
          }
          x11::Free(cursor);
        }
      }

      gl::ctx.Disable(GL_BLEND);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      gl::ctx.BindVertexArray(0);
      gl::ctx.UseProgram(0);
      gl::ctx.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      // Submit rendering without stalling the capture thread. The exported
      // NvBufSurface DMA-BUF carries the NVIDIA driver's implicit fence into
      // the VIC import path.
      gl::ctx.Flush();
      img.frame_timestamp = std::chrono::steady_clock::now();
      img.data = nullptr;
      return capture_e::ok;
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
      if (x11_attr_t::init(display_name, config)) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: base X11 display initialization"sv;
        return -1;
      }
      if (x11::composite::init()) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: libXcomposite or required symbols unavailable"sv;
        return -1;
      }
      if (gbm::init()) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: libgbm unavailable"sv;
        return -1;
      }
      gbm_fd = file_t {::open("/dev/dri/by-path/platform-13800000.display-render", O_RDWR | O_CLOEXEC)};  // NOSONAR(cpp:S1874): `_sopen_s` not available
      if (gbm_fd.el < 0) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: NVIDIA display render node unavailable"sv;
        return -1;
      }
      gbm_device.reset(gbm::create_device(gbm_fd.el));
      if (!gbm_device) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: NVIDIA GBM device creation"sv;
        return -1;
      }

      // Do not redirect the root subwindows here. On a normal composited X11
      // desktop (GNOME/Mutter, KDE/KWin, etc.) the compositor already owns the
      // redirection. Trying to redirect the root again can conflict with that
      // owner. XCompositeNameWindowPixmap can name those already-redirected
      // windows, which is the same model used by other GPU X11 capture tools.
      egl_display = egl::make_display(xdisplay.get());
      if (!egl_display) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: EGL display initialization"sv;
        return -1;
      }
      egl_context = egl::make_ctx(egl_display.get());
      if (!egl_context) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: OpenGL context creation"sv;
        return -1;
      }
      if (!gl::egl_image_target_texture_2d()) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: glEGLImageTargetTexture2DOES unavailable"sv;
        return -1;
      }
      const auto vertex = gl::shader_t::compile(
        "#version 330\n"
        "out vec2 texcoord;\n"
        "uniform vec4 uv_rect;\n"
        "void main() {\n"
        "  const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
        "  vec2 uv = (positions[gl_VertexID] + 1.0) * 0.5;\n"
        "  texcoord = uv_rect.xy + uv * uv_rect.zw;\n"
        "  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);\n"
        "}\n",
        GL_VERTEX_SHADER
      );
      if (vertex.has_right()) {
        BOOST_LOG(error) << "X11 GPU compositor vertex shader failed: "sv << vertex.right();
        return -1;
      }
      const auto fragment = gl::shader_t::compile(
        "#version 330\n"
        "in vec2 texcoord;\n"
        "out vec4 color;\n"
        "uniform sampler2D source_texture;\n"
        "void main() { color = texture(source_texture, texcoord); }\n",
        GL_FRAGMENT_SHADER
      );
      if (fragment.has_right()) {
        BOOST_LOG(error) << "X11 GPU compositor fragment shader failed: "sv << fragment.right();
        return -1;
      }
      auto program = gl::program_t::link(vertex.left(), fragment.left());
      if (program.has_right()) {
        BOOST_LOG(error) << "X11 GPU compositor shader link failed: "sv << program.right();
        return -1;
      }
      compositor_program = std::move(program.left());
      uv_rect_location = gl::ctx.GetUniformLocation(compositor_program.handle(), "uv_rect");
      const auto source_texture_location = gl::ctx.GetUniformLocation(compositor_program.handle(), "source_texture");
      if (uv_rect_location < 0 || source_texture_location < 0) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: shader uniforms unavailable"sv;
        return -1;
      }
      gl::ctx.UseProgram(compositor_program.handle());
      gl::ctx.Uniform1i(source_texture_location, 0);
      gl::ctx.UseProgram(0);
      gl::ctx.GenVertexArrays(1, &compositor_vao);
      if (compositor_vao == 0) {
        BOOST_LOG(error) << "X11 GPU compositor init failed: vertex array creation"sv;
        return -1;
      }
      const auto init_gl_error = gl::ctx.GetError();
      if (init_gl_error != GL_NO_ERROR) {
        BOOST_LOG(error) << "X11 GPU compositor init failed with GL error "sv << init_gl_error;
        return -1;
      }
      XSelectInput(
        xdisplay.get(), xwindow,
        StructureNotifyMask | SubstructureNotifyMask | PropertyChangeMask
      );
      XFlush(xdisplay.get());
      BOOST_LOG(info) << "X11 GPU compositor enabled through sampled EGL native pixmaps"sv;
      return 0;
    }

    /**
     * @brief Render the encoder probe frame through the GPU compositor.
     *
     * The base X11 implementation uses XGetImage and assumes an x11_img_t.
     * GPU images must instead be rendered into their NvBufSurface so the
     * Jetson encoder probe receives the same DMA-BUF path as a live stream.
     *
     * @param img GPU-backed image allocated by alloc_img().
     * @return Zero when the DMA-BUF frame is ready; otherwise negative one.
     */
    int dummy_img(img_t *img) override {
      auto *gpu_img = dynamic_cast<x11_gpu_img_t *>(img);
      if (!gpu_img) {
        return -1;
      }
      return snapshot(*gpu_img, true) == capture_e::ok ? 0 : -1;
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
