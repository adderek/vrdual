// System cursor -> frame header (v4, see vrdual_shm.h). Games that rely on
// the X11 cursor (Hospital Tycoon, strategy games) show none in the HMD: the
// swapchain never contains it. Read-only queries on the layer's own xcb
// connection (libxcb + libxcb-xfixes via dlopen; xcb reports errors per
// request, so no process-wide Xlib error handler that would fight Wine's).
// Called under g_shm_lock, once per exported frame: pointer, window size and
// cursor image go out as three pipelined requests (one round trip), with the
// input focus: is the game's window (or one of its ancestors / children) the
// focused one (VRDUAL_SLOT_UNFOCUSED, vrdual-xr's keys go to the focused
// window). input_focus = on / off forces the answer.
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xtest.h>
#include <linux/input-event-codes.h>

// tools/frame_hash.py --cursor reads the fields at this offset
static_assert(offsetof(vrdual_shm_header, cursor_seq) == 304, "update tools/frame_hash.py");
static_assert(offsetof(vrdual_shm_header, slot_flags) == 376, "update tools/frame_hash.py");
static_assert(offsetof(vrdual_ctl, xin_head) == 168 && offsetof(vrdual_ctl, xin) == 176, "update the XTEST check in selftest (_)");

namespace cursor {

// vkCreateXlibSurfaceKHR / vkCreateXcbSurfaceKHR create infos (the X headers'
// macros stay out of the layer).
struct XlibSurfaceCI { VkStructureType sType; const void* pNext; VkFlags flags; void* dpy; unsigned long window; };
struct XcbSurfaceCI { VkStructureType sType; const void* pNext; VkFlags flags; void* connection; uint32_t window; };
using PFN_CreateSurface = VkResult(VKAPI_PTR*)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);

static std::mutex& g_lock = *new std::mutex;
static std::atomic<bool> g_unfocused{false};  // last answer; false while unknown
static std::atomic<bool> g_xtest{false};      // this layer injects vrdual-xr's keys and mouse (VRDUAL_SLOT_XTEST)
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
    XF(get_input_focus); XF(get_input_focus_reply); XF(query_tree); XF(query_tree_reply);
#undef XF
    decltype(&::xcb_test_fake_input) fake_input = nullptr;  // libxcb-xtest, optional
    xcb_connection_t* c = nullptr;
    uint32_t root = 0;
    uint16_t root_w = 0, root_h = 0;
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
        XL(a, get_input_focus); XL(a, get_input_focus_reply); XL(a, query_tree); XL(a, query_tree_reply);
#undef XL
        if (!connect || !xfixes_get_cursor_image_cursor_image || !setup_roots_iterator || !get_input_focus_reply || !query_tree_reply) return false;
        xcb_connection_t* cn = connect(nullptr, nullptr);
        if (!cn || connection_has_error(cn)) { logf("cursor: cannot connect to X (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : ""); return false; }
        // XFixes needs the version handshake before any other request
        auto* v = xfixes_query_version_reply(cn, xfixes_query_version(cn, 4, 0), nullptr);
        if (!v) { logf("cursor: X server without XFixes"); return false; }
        free(v);
        c = cn;
        xcb_screen_t* scr = setup_roots_iterator(get_setup(cn)).data;
        root = scr->root, root_w = scr->width_in_pixels, root_h = scr->height_in_pixels;
        if (void* t = dlopen("libxcb-xtest.so.0", RTLD_NOW | RTLD_LOCAL)) fake_input = (decltype(fake_input))dlsym(t, "xcb_test_fake_input");
        logf("cursor: X11 cursor exported (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return true;
    }
};

// Is `a` == `b` or an ancestor of it? Walks up from b (a few round trips,
// only when the focus or the window changed).
static bool above(X& x, uint32_t a, uint32_t b) {
    for (int i = 0; i < 16 && b && b != x.root; ++i) {
        if (a == b) return true;
        xcb_query_tree_reply_t* t = x.query_tree_reply(x.c, x.query_tree(x.c, b), nullptr);
        b = t ? t->parent : 0;
        free(t);
    }
    return a == b;
}

static void focus(X& x, uint32_t win, xcb_get_input_focus_reply_t* f, int force) {
    static uint32_t last_win = 0, last_focus = 0;
    static bool last = true, told = false;
    bool on = true;
    if (force >= 0) on = force;
    else if (!f) return;
    else if (win == x.root) on = true;  // gamescope: its own X screen holds only the game
    else if (f->focus == last_focus && win == last_win) on = last;
    else on = f->focus == XCB_INPUT_FOCUS_POINTER_ROOT || above(x, win, f->focus) || above(x, f->focus, win);
    if (f) last_focus = f->focus;
    last_win = win;
    static const bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    if (gamescope && force < 0 && !told && !g_xtest)
        logf("focus: inside gamescope the game always has its X server's focus; gamescope's own focus on the desktop is not visible here: vrdual-xr input goes to the active desktop window unless input_route = auto (XTEST into the game)");
    else if (on != last || !told) logf("focus: the game window %s the input focus%s", on ? "has" : "does not have", on ? "" : " (vrdual-xr sends no input)");
    last = on;
    told = true;
    g_unfocused = !on;
}

// Pointer over `win` scaled to the swapchain extent, plus the cursor image,
// into every open frame header. Window unknown under gamescope: its WSI layer
// (64-bit, above ours) turns the app's X11 surface into a Wayland one; the
// game fills gamescope's nested X screen, so the root window stands in.
// vrdual-xr's keys and mouse (vrdual_ctl.xin) into this X server with XTEST:
// a game inside gamescope gets them whatever window is active on the desktop
// (uinput goes to the desktop's focused window, and gamescope's focus is not
// visible). Evdev key codes + 8 = X keycodes (Xwayland's keymap). Sent once
// per present, in order; relative motion summed until the next button / key.
// route: input_route xtest 1 / uinput 0 / auto -1 (xtest inside gamescope).
// VRDUAL_XTEST_DUMP=FILE: write the requests there instead of sending them
// (selftest: never into the desktop's X server).
static void inject(X& x, const vrdual_ctl* c, int route, bool gamescope) {
    static const char* dump_path = getenv("VRDUAL_XTEST_DUMP");
    static FILE* dump = dump_path && *dump_path ? fopen(dump_path, "a") : nullptr;
    static uint32_t tail = 0;
    static bool on = false;
    bool want = c && (route == 1 || (route < 0 && gamescope)) && (x.fake_input || dump);
    if (want != on) {
        if (want) tail = c->xin_head;  // nothing from before
        logf("input: %s", want ? (dump ? "XTEST requests from vrdual-xr dumped to VRDUAL_XTEST_DUMP (not sent)"
                                      : "keys and mouse from vrdual-xr injected with XTEST into this X server (reach the game whatever window is active on the desktop)")
                               : "XTEST injection off (vrdual-xr uses uinput)");
        if (route != 0 && !x.fake_input && !dump) logf("input: libxcb-xtest not found, no XTEST");
        on = want;
        g_xtest = on;
    }
    if (!on) return;
    uint32_t h = c->xin_head;
    __sync_synchronize();
    if (h - tail > VRDUAL_XIN_N) tail = h - VRDUAL_XIN_N;
    int mx = 0, my = 0;
    auto send = [&](uint8_t type, uint8_t detail, uint32_t root, int16_t rx, int16_t ry) {
        if (dump) fprintf(dump, "%u %u %d %d\n", type, detail, rx, ry);
        else x.fake_input(x.c, type, detail, XCB_CURRENT_TIME, root, rx, ry, 0);
    };
    auto flush_motion = [&] {
        if (mx || my) send(XCB_MOTION_NOTIFY, 1, XCB_NONE, int16_t(mx), int16_t(my));  // detail 1: relative
        mx = my = 0;
    };
    for (; tail != h; ++tail) {
        auto e = c->xin[tail % VRDUAL_XIN_N];
        if (e.type == EV_REL && (e.code == REL_X || e.code == REL_Y)) {
            (e.code == REL_X ? mx : my) += e.value;
            continue;
        }
        flush_motion();
        if (e.type == EV_KEY && e.code < BTN_MISC) {
            send(e.value ? XCB_KEY_PRESS : XCB_KEY_RELEASE, uint8_t(e.code + 8), XCB_NONE, 0, 0);
        } else if (e.type == EV_KEY) {
            int b = e.code == BTN_LEFT ? 1 : e.code == BTN_MIDDLE ? 2 : e.code == BTN_RIGHT ? 3 : e.code == BTN_SIDE ? 8 : e.code == BTN_EXTRA ? 9 : 0;
            if (b) send(e.value ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, uint8_t(b), XCB_NONE, 0, 0);
        } else if (e.type == EV_REL && (e.code == REL_WHEEL || e.code == REL_HWHEEL)) {
            int b = e.code == REL_WHEEL ? (e.value > 0 ? 4 : 5) : (e.value > 0 ? 7 : 6);
            for (int i = 0; i < std::min(abs(e.value), 8); ++i) send(XCB_BUTTON_PRESS, uint8_t(b), XCB_NONE, 0, 0), send(XCB_BUTTON_RELEASE, uint8_t(b), XCB_NONE, 0, 0);
        } else if (e.type == EV_ABS && (e.code == ABS_X || e.code == ABS_Y) && x.root_w) {  // tablet: 0..65535 over the screen
            static int ax = 0, ay = 0;
            (e.code == ABS_X ? ax : ay) = e.value;
            send(XCB_MOTION_NOTIFY, 0, x.root, int16_t(int64_t(ax) * x.root_w / 65536), int16_t(int64_t(ay) * x.root_h / 65536));
        }
    }
    flush_motion();
    if (dump) fflush(dump);
}

// force: input_focus on (1) / off (0), auto -1; route: see inject; xin: the
// control block when this process injects (one layer per session: eye 0)
static void update(uint32_t win, VkExtent2D ext, vrdual_shm_header* const* hdrs, int n, int force, int route = 0, const vrdual_ctl* xin = nullptr) {
    static X& x = *new X;
    static const bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    if (force >= 0) focus(x, win, nullptr, force);  // no X needed
    if ((!win && !gamescope) || !x.init()) return;
    inject(x, xin, route, gamescope);
    if (!win) {
        static std::once_flag once;
        std::call_once(once, [] { logf("cursor: surface window unknown (gamescope WSI), pointer over the root window"); });
        win = x.root;
    }
    auto qp = x.query_pointer(x.c, win);
    auto gg = x.get_geometry(x.c, win);
    auto ci = x.xfixes_get_cursor_image(x.c);
    auto fc = x.get_input_focus(x.c);
    xcb_query_pointer_reply_t* p = x.query_pointer_reply(x.c, qp, nullptr);
    xcb_get_geometry_reply_t* g = x.get_geometry_reply(x.c, gg, nullptr);
    xcb_xfixes_get_cursor_image_reply_t* im = x.xfixes_get_cursor_image_reply(x.c, ci, nullptr);
    xcb_get_input_focus_reply_t* f = x.get_input_focus_reply(x.c, fc, nullptr);
    if (force < 0) focus(x, win, f, force);
    free(f);
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
