// System cursor -> frame header (v4, see vrdual_shm.h). Games that rely on
// the X11 cursor (Hospital Tycoon, strategy games) show none in the HMD: the
// swapchain never contains it. Read-only queries on the layer's own xcb
// connection (libxcb + libxcb-xfixes via dlopen; xcb reports errors per
// request, so no process-wide Xlib error handler that would fight Wine's).
// Called under g_shm_lock, once per exported frame: pointer, window size and
// cursor image go out as three pipelined requests (one round trip).
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

// tools/frame_hash.py --cursor reads the fields at this offset
static_assert(offsetof(vrdual_shm_header, cursor_seq) == 304, "update tools/frame_hash.py");

namespace cursor {

// vkCreateXlibSurfaceKHR / vkCreateXcbSurfaceKHR create infos (the X headers'
// macros stay out of the layer).
struct XlibSurfaceCI { VkStructureType sType; const void* pNext; VkFlags flags; void* dpy; unsigned long window; };
struct XcbSurfaceCI { VkStructureType sType; const void* pNext; VkFlags flags; void* connection; uint32_t window; };
using PFN_CreateSurface = VkResult(VKAPI_PTR*)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);

static std::mutex& g_lock = *new std::mutex;
static auto& g_windows = *new std::unordered_map<uint64_t, uint32_t>;  // VkSurfaceKHR -> X window

static void remember(VkSurfaceKHR s, uint32_t win) {
    std::lock_guard<std::mutex> l(g_lock);
    g_windows[uint64_t(s)] = win;
}
static uint32_t window_of(VkSurfaceKHR s) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_windows.find(uint64_t(s));
    return it == g_windows.end() ? 0 : it->second;
}

struct X {
#define XF(n) decltype(&::xcb_##n) n = nullptr
    XF(connect); XF(connection_has_error); XF(query_pointer); XF(query_pointer_reply);
    XF(get_geometry); XF(get_geometry_reply); XF(get_setup); XF(setup_roots_iterator);
    XF(xfixes_query_version); XF(xfixes_query_version_reply); XF(xfixes_get_cursor_image);
    XF(xfixes_get_cursor_image_reply); XF(xfixes_get_cursor_image_cursor_image);
#undef XF
    xcb_connection_t* c = nullptr;
    uint32_t root = 0;
    bool tried = false;

    bool init() {
        if (tried) return c != nullptr;
        tried = true;
        void* a = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void* b = dlopen("libxcb-xfixes.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!a || !b) { logf("cursor: libxcb / libxcb-xfixes not found, no cursor in the HMD"); return false; }
#define XL(h, n) n = (decltype(n))dlsym(h, "xcb_" #n)
        XL(a, connect); XL(a, connection_has_error); XL(a, query_pointer); XL(a, query_pointer_reply);
        XL(a, get_geometry); XL(a, get_geometry_reply); XL(a, get_setup); XL(a, setup_roots_iterator);
        XL(b, xfixes_query_version); XL(b, xfixes_query_version_reply); XL(b, xfixes_get_cursor_image);
        XL(b, xfixes_get_cursor_image_reply); XL(b, xfixes_get_cursor_image_cursor_image);
#undef XL
        if (!connect || !xfixes_get_cursor_image_cursor_image || !setup_roots_iterator) return false;
        xcb_connection_t* cn = connect(nullptr, nullptr);
        if (!cn || connection_has_error(cn)) { logf("cursor: cannot connect to X (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : ""); return false; }
        // XFixes needs the version handshake before any other request
        auto* v = xfixes_query_version_reply(cn, xfixes_query_version(cn, 4, 0), nullptr);
        if (!v) { logf("cursor: X server without XFixes"); return false; }
        free(v);
        c = cn;
        root = setup_roots_iterator(get_setup(cn)).data->root;
        logf("cursor: X11 cursor exported (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return true;
    }
};

// Pointer over `win` scaled to the swapchain extent, plus the cursor image,
// into every open frame header. Window unknown under gamescope: its WSI layer
// (64-bit, above ours) turns the app's X11 surface into a Wayland one; the
// game fills gamescope's nested X screen, so the root window stands in.
static void update(uint32_t win, VkExtent2D ext, vrdual_shm_header* const* hdrs, int n) {
    static X& x = *new X;
    static const bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    if ((!win && !gamescope) || !x.init()) return;
    if (!win) {
        static std::once_flag once;
        std::call_once(once, [] { logf("cursor: surface window unknown (gamescope WSI), pointer over the root window"); });
        win = x.root;
    }
    auto qp = x.query_pointer(x.c, win);
    auto gg = x.get_geometry(x.c, win);
    auto ci = x.xfixes_get_cursor_image(x.c);
    xcb_query_pointer_reply_t* p = x.query_pointer_reply(x.c, qp, nullptr);
    xcb_get_geometry_reply_t* g = x.get_geometry_reply(x.c, gg, nullptr);
    xcb_xfixes_get_cursor_image_reply_t* im = x.xfixes_get_cursor_image_reply(x.c, ci, nullptr);
    if (p && g && im && g->width && g->height) {
        int32_t cx = int32_t(int64_t(p->win_x) * ext.width / g->width), cy = int32_t(int64_t(p->win_y) * ext.height / g->height);
        uint32_t w = std::min<uint32_t>(im->width, VRDUAL_CURSOR_MAX), h = std::min<uint32_t>(im->height, VRDUAL_CURSOR_MAX);
        const uint32_t* px = x.xfixes_get_cursor_image_cursor_image(im);
        for (int i = 0; i < n; ++i) {
            vrdual_shm_header* hd = hdrs[i];
            if (!hd) continue;
            hd->cursor_seq++;
            __sync_synchronize();
            hd->cursor_x = cx;
            hd->cursor_y = cy;
            if (hd->cursor_serial != im->cursor_serial || hd->cursor_w != w || hd->cursor_h != h) {
                auto* dst = (uint32_t*)((char*)hd + VRDUAL_CURSOR_OFF);
                for (uint32_t y = 0; y < h; ++y) memcpy(dst + y * w, px + y * im->width, w * 4);
                hd->cursor_serial = im->cursor_serial;
                hd->cursor_w = w;
                hd->cursor_h = h;
                hd->cursor_hot_x = im->xhot;
                hd->cursor_hot_y = im->yhot;
            }
            __sync_synchronize();
            hd->cursor_seq++;
        }
    }
    free(p);
    free(g);
    free(im);
}

}  // namespace cursor
