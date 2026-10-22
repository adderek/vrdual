// vrdual-input-test: checks the input Mapper with scripted controller states,
// then a uinput round trip with a virtual pad (read back through evdev when
// the event node is accessible; joysticks get a uaccess ACL on logind seats).
// Exit 0 = all good. Never creates a mouse or keyboard (would move the desktop).
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>

#include "../common/vrdual_frames.hpp"
#include "../common/vrdual_input.hpp"

using namespace vrdual;

static int fails = 0;
static void expect(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}
static bool has(const std::vector<InEvent>& ev, InEvent e) {
    for (auto& x : ev) if (x == e) return true;
    return false;
}
static int rel(const std::vector<InEvent>& ev, int code) {
    int s = 0;
    for (auto& x : ev) if (x.dev == DEV_MOUSE && x.type == EV_REL && x.code == code) s += x.value;
    return s;
}

static InputCfg cfg(const char* input, const char* mode = "stick") {
    InputCfg c;
    c.input = input;
    c.mouse_mode = mode;
    for (int& m : c.map) m = -1;
    return c;
}

int main() {
    {
        Mapper m(cfg("gamepad"));
        Controls c;
        c.a = true; c.lx = 1; c.ly = 1; c.rt = 1;
        auto ev = m.step(c, 0.01);
        expect(has(ev, {DEV_PAD, EV_KEY, BTN_A, 1}), "gamepad: A -> BTN_A");
        expect(has(ev, {DEV_PAD, EV_ABS, ABS_X, 32767}) && has(ev, {DEV_PAD, EV_ABS, ABS_Y, -32767}), "gamepad: left stick, y flipped");
        expect(has(ev, {DEV_PAD, EV_ABS, ABS_RZ, 255}), "gamepad: right trigger axis");
        c.a = false;
        ev = m.step(c, 0.01);
        expect(has(ev, {DEV_PAD, EV_KEY, BTN_A, 0}) && ev.size() == 1, "gamepad: release only, unchanged axes not resent");
    }
    {
        Mapper m(cfg("mouse"));
        Controls c;
        c.rx = 1; c.rt = 1; c.ly = 1;
        auto ev = m.step(c, 0.1);
        expect(rel(ev, REL_X) == 150, "mouse stick: full right 0.1 s -> 150 counts");
        expect(has(ev, {DEV_MOUSE, EV_KEY, BTN_LEFT, 1}), "mouse: right trigger -> left button");
        expect(has(ev, {DEV_KBD, EV_KEY, KEY_W, 1}), "mouse: left stick up -> W");
        c = Controls{};
        c.rx = 0.1f;  // inside the dead zone
        ev = m.step(c, 0.1);
        expect(rel(ev, REL_X) == 0 && has(ev, {DEV_MOUSE, EV_KEY, BTN_LEFT, 0}) && has(ev, {DEV_KBD, EV_KEY, KEY_W, 0}),
               "mouse: dead zone, releases");
        // menu tap -> motion mode
        c = Controls{}; c.menu = true; m.step(c, 0.1);
        c.menu = false; m.step(c, 0.1);
        expect(!strcmp(m.mouse_mode(), "motion"), "menu tap cycles stick -> motion");
        c.hand_ok = true; c.hand_yaw = 0; m.step(c, 0.01);
        c.hand_yaw = -10; ev = m.step(c, 0.01);  // turned right
        expect(rel(ev, REL_X) == 200, "motion: 10 deg right -> 200 counts");
        c.lg = 1; c.hand_yaw = -20; ev = m.step(c, 0.01);
        expect(rel(ev, REL_X) == 0 && ev.empty(), "motion: left grip = clutch, no key");
        c = Controls{}; c.menu = true; m.step(c, 0.1);
        c.menu = false; m.step(c, 0.1);
        expect(!strcmp(m.mouse_mode(), "aim"), "menu tap cycles motion -> aim");
        c.aim_ok = true; c.aim_u = 0.5f; c.aim_v = 0.25f;
        ev = m.step(c, 0.01);
        expect(has(ev, {DEV_TABLET, EV_ABS, ABS_X, 32768}) && has(ev, {DEV_TABLET, EV_ABS, ABS_Y, 16384}), "aim: ray -> absolute pointer");
    }
    {
        InputCfg k = cfg("mouse");
        k.map[B_A] = input_code("KEY_F5");
        k.map[B_B] = 0;
        expect(k.map[B_A] == KEY_F5 && input_code("btn_south") == BTN_SOUTH, "code names resolve (KEY_F5, btn_south)");
        Mapper m(k);
        Controls c; c.a = c.b = true;
        auto ev = m.step(c, 0.01);
        expect(has(ev, {DEV_KBD, EV_KEY, KEY_F5, 1}) && ev.size() == 1, "map_a = KEY_F5, map_b = none");
    }
    {
        InputCfg k = cfg("off");
        k.head_look = "edge";
        Mapper m(k);
        Controls c; c.head_ok = true; c.head_yaw = 10;
        m.step(c, 0.5);                       // reference = 10 deg
        c.head_yaw = 40;                      // 30 deg left: 15 past the dead zone
        auto ev = m.step(c, 0.5);             // 15/45*120 = 40 deg/s * 0.5 s * 10 counts/deg
        expect(rel(ev, REL_X) == -200, "head edge: 30 deg left -> -200 counts in 0.5 s");
        c.head_yaw = 20; ev = m.step(c, 0.5);
        expect(rel(ev, REL_X) == 0, "head edge: inside the dead zone -> nothing");
    }
    {
        InputCfg k = cfg("off");
        k.head_look = "mouse";
        Mapper m(k);
        Controls c; c.head_ok = true; c.head_yaw = 179; m.step(c, 0.01);
        c.head_yaw = -176; auto ev = m.step(c, 0.01);  // 5 deg left across the wrap
        expect(rel(ev, REL_X) == -50, "head mouse: 5 deg left across +-180 -> -50");
    }
    {
        ScreenGrab g;
        Controls c;
        c.hands_ok = true;
        c.lg = 1;
        float l[3] = {-0.2f, 0, -0.5f}, r[3] = {0.2f, 0, -0.5f};
        memcpy(c.hand_p[0], l, sizeof l); memcpy(c.hand_p[1], r, sizeof r);
        expect(!g.step(c, 0.01), "screen grab: one grip -> no grab");
        c.rg = 1;
        expect(g.step(c, 0.01) && g.off[0] == 0 && g.sx == 1 && g.rot[3] == 1, "screen grab: both grips -> grab, no jump");
        c.hand_p[0][0] = -0.2f; c.hand_p[1][0] = 0.4f; c.hand_p[0][2] = c.hand_p[1][2] = -0.6f;  // midpoint +0.1 x, -0.1 z; 0.6 apart
        g.step(c, 0.01);
        expect(fabsf(g.off[0] - 0.1f) < 1e-5f && fabsf(g.off[2] + 0.1f) < 1e-5f && fabsf(g.sx - 1.5f) < 1e-4f && fabsf(g.sy - 1.5f) < 1e-4f,
               "screen grab: midpoint drags, distance scales (0.4 -> 0.6 m: x1.5)");
        c.lg = c.rg = 0;
        expect(!g.step(c, 0.01) && fabsf(g.off[0] - 0.1f) < 1e-5f, "screen grab: release keeps the offset");
        c.lg = c.rg = 1; c.hand_p[0][0] = 1; c.hand_p[1][0] = 1.6f;
        g.step(c, 0.01);
        expect(fabsf(g.off[0] - 0.1f) < 1e-5f && fabsf(g.sx - 1.5f) < 1e-4f, "screen grab: regrab elsewhere, no jump");
        // turn the hands' line 90 deg about the midpoint (x axis -> -z axis): yaw left 90 deg
        c.hand_p[0][0] = 1.3f; c.hand_p[1][0] = 1.3f; c.hand_p[0][2] = -0.3f; c.hand_p[1][2] = -0.9f;
        g.step(c, 0.01);
        expect(fabsf(g.rot[1] - 0.70711f) < 1e-3f && fabsf(g.rot[3] - 0.70711f) < 1e-3f && fabsf(g.off[0] - 0.1f) < 1e-5f,
               "screen grab: hands' line turned 90 deg -> picture yawed 90 deg, centre kept");
        c.hand_p[0][2] = -0.31f; c.hand_p[1][2] = -0.89f; c.hand_p[0][0] = 1.32f;  // ~92 deg, slightly tilted
        g.step(c, 0.01);
        expect(fabsf(g.rot[1] - 0.70711f) < 1e-4f && fabsf(g.rot[3] - 0.70711f) < 1e-4f, "screen grab: 92 deg snaps to 90 deg");
        c.rt = 1;
        g.step(c, 0.01);
        expect(fabsf(g.rot[1] - 0.70711f) > 5e-3f, "screen grab: trigger held -> free turn, no snap");
        c.rt = 0;
        g.step(c, 0.01);
        expect(fabsf(g.rot[1] - 0.70711f) > 5e-3f, "screen grab: trigger let go, still grabbing -> stays free");
        c.lg = c.rg = 0; g.step(c, 0.01);
        c.lg = c.rg = 1; g.step(c, 0.01);
        c.hand_p[0][0] = 1.31f; g.step(c, 0.01);
        expect(fabsf(g.rot[1] - 0.70711f) < 1e-4f && fabsf(g.rot[3] - 0.70711f) < 1e-4f, "screen grab: regrab -> snaps again");
        c.hand_p[0][2] = -0.3f; c.hand_p[1][2] = -0.9f; c.hand_p[0][0] = 1.3f;
        g.step(c, 0.01);
        c.rx = 1;  // stretch width for 1 s
        for (int i = 0; i < 20; ++i) g.step(c, 0.05);
        expect(g.sx > 1.5f * 1.6f && g.sx < 1.5f * 1.7f && fabsf(g.sy - 1.5f) < 1e-4f, "screen grab: right stick x stretches width only");
        {  // left stick: orbit 30 deg right about the viewer; base yawed 90 deg left, snap in its frame
            ScreenGrab o;
            qaxis(1, 1.5707963f, o.basis);
            Controls d;
            d.hands_ok = true; d.lg = d.rg = 1;
            d.hand_p[0][0] = -0.2f; d.hand_p[1][0] = 0.2f;
            o.off[0] = 0.5f;
            o.step(d, 0.01);
            d.lx = 1;
            for (int i = 0; i < 10; ++i) o.step(d, 0.05);  // 60 deg/s * 0.5 s
            float e[3] = {0.5f * cosf(0.5235988f), 0, 0.5f * sinf(0.5235988f)};  // (0.5, 0, 0) yawed 30 deg right
            expect(fabsf(o.orbit[0] + 0.5235988f) < 1e-4f && fabsf(o.off[0] - e[0]) < 1e-4f && fabsf(o.off[2] - e[2]) < 1e-4f &&
                   fabsf(o.rot[3] - 1) < 1e-5f, "screen grab: left stick orbits 30 deg, offset turns along, picture still faces the viewer");
        }
        c = Controls{};
        c.menu = true;
        for (int i = 0; i < 9; ++i) g.step(c, 0.1);
        int before = g.recentres;
        for (int i = 0; i < 20; ++i) g.step(c, 0.1);
        expect(before == 0 && g.recentres == 1, "screen: menu held 1 s -> one recentre");
    }
    {
        Tuner t;
        t.v[0] = t.start[0] = 50; t.v[1] = t.start[1] = 0; t.v[2] = t.start[2] = 90;
        Controls c;
        c.ly = 1;
        int r = 0;
        for (int i = 0; i < 10; ++i) r |= t.step(c, 0.1);
        expect(std::fabs(t.v[0] - 100) < 0.5f && (r & TUNE_CHANGED), "tune: left stick up 1 s -> world_scale x2");
        c.ly = 0;
        expect(t.step(c, 0.1) & TUNE_SETTLED, "tune: stick released -> settled");
        c.ry = -1;
        t.step(c, 0.1);
        expect(t.v[1] > 90 && t.v[1] <= 100, "tune: right stick down from infinity -> hud_depth 100 m");
        for (int i = 0; i < 120; ++i) t.step(c, 0.1);
        expect(std::fabs(t.v[1] - 0.1f) < 1e-3f, "tune: hud_depth stops at 0.1 m");
        c.ry = 1;
        for (int i = 0; i < 120; ++i) t.step(c, 0.1);
        expect(t.v[1] == 0, "tune: hud_depth past 100 m -> 0 (infinity)");
        c.ry = 0; c.rx = 1; c.rg = 1;
        float h = t.v[2];
        t.step(c, 0.5);
        expect(t.v[2] == h, "tune: grip held -> sticks ignored");
        c.rg = 0;
        t.step(c, 0.5);
        expect(std::fabs(t.v[2] - 100) < 0.01f, "tune: right stick x -> hfov 20 deg/s");
        c.rx = 0; c.a = true;
        expect((t.step(c, 0.1) & TUNE_SAVE) && !(t.step(c, 0.1) & TUNE_SAVE), "tune: a -> one save");
        c.a = false;
        t.step(c, 0.1);
        c.ly = 1;
        t.step(c, 0.5);
        c.ly = 0; c.b = true;
        r = t.step(c, 0.1);
        expect((r & TUNE_RESET) && t.undone_to == Tuner::UNDO_SAVED && std::fabs(t.v[2] - 100) < 0.01f && t.v[1] == 0,
               "tune: change after save, b -> saved values");
        c.b = false; t.step(c, 0.1); c.b = true;
        r = t.step(c, 0.1);
        expect((r & TUNE_RESET) && t.undone_to == Tuner::UNDO_START && t.v[0] == 50 && t.v[1] == 0 && t.v[2] == 90, "tune: b again -> start values");
        c.b = false; t.step(c, 0.1); c.b = true;
        r = t.step(c, 0.1);
        expect((r & TUNE_RESET) && t.undone_to == Tuner::UNDO_DEFAULTS && t.v[0] == 1 && t.v[2] == 90, "tune: b third time -> defaults");
        Tuner u;
        u.v[0] = u.start[0] = 50;
        u.v[0] = 70;
        c.b = true;
        expect((u.step(c, 0.1) & TUNE_RESET) && u.v[0] == 50, "tune: nothing saved, b -> start values");
    }

    {  // cursor depth: the scene's disparity under the pointer (block matching)
        const uint32_t W = 320, H = 64;
        std::vector<uint8_t> base(W * H), L(W * H * 4), R(W * H * 4);
        uint32_t r = 12345;
        for (auto& v : base) { r = r * 1103515245 + 12345; v = uint8_t(r >> 24); }
        auto make = [&](std::vector<uint8_t>& img, int shift) {  // view x shown at x + shift
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    int sx = std::clamp(int(x) - shift, 0, int(W) - 1);
                    uint8_t v = base[y * W + sx];
                    for (int c = 0; c < 4; ++c) img[(y * W + x) * 4 + c] = v;
                }
        };
        make(L, 7); make(R, -7);
        int d = -99;
        bool ok = scene_disparity(L.data(), R.data(), W, H, 160, 32, 0.5f, -0.5f, 40, &d);
        expect(ok && std::abs(d - 14) <= 1, "cursor: textured scene, disparity 14 px found (13 = same shifts)");
        std::fill(L.begin(), L.end(), 128); std::fill(R.begin(), R.end(), 128);
        expect(!scene_disparity(L.data(), R.data(), W, H, 160, 32, 0.5f, -0.5f, 40, &d), "cursor: flat patch -> no depth");
    }

    {  // menu + a / b switches the control mode in every mode; the game sees neither button
        Mapper m(cfg("off"));
        Controls c;
        c.menu = true; auto ev = m.step(c, 0.01);
        c.a = true; ev = m.step(c, 0.01);
        expect(m.input() == "gamepad" && m.changes == 1 && std::none_of(ev.begin(), ev.end(), [](const InEvent& e) { return e.type == EV_KEY; }),
               "off: menu + a -> gamepad, no key sent");
        c.a = false; ev = m.step(c, 0.01);
        expect(!has(ev, {DEV_PAD, EV_KEY, BTN_A, 0}), "chord: a's release not sent");
        c.menu = false; ev = m.step(c, 0.01);
        expect(!has(ev, {DEV_PAD, EV_KEY, BTN_START, 1}), "chord: menu release is no Start");
        // gamepad: a menu tap is Start, pressed on release, released on the next step
        c.menu = true; ev = m.step(c, 0.01);
        expect(!has(ev, {DEV_PAD, EV_KEY, BTN_START, 1}), "gamepad: menu down sends nothing yet");
        c.menu = false; ev = m.step(c, 0.01);
        expect(has(ev, {DEV_PAD, EV_KEY, BTN_START, 1}), "gamepad: menu tap -> Start on release");
        ev = m.step(c, 0.01);
        expect(has(ev, {DEV_PAD, EV_KEY, BTN_START, 0}), "gamepad: Start released on the next step");
        // held: no Start after a 1 s hold (recentre)
        c.menu = true; m.step(c, 0.6); m.step(c, 0.6);
        c.menu = false; ev = m.step(c, 0.01);
        expect(!has(ev, {DEV_PAD, EV_KEY, BTN_START, 1}), "gamepad: menu held 1 s -> no Start");
        // switching away releases what the old mode holds
        c.x = true; c.lx = 1; m.step(c, 0.01);
        c.menu = true; m.step(c, 0.01);
        c.a = true; ev = m.step(c, 0.01);
        expect(m.controls() == "mouse: stick" && has(ev, {DEV_PAD, EV_KEY, BTN_X, 0}) && has(ev, {DEV_PAD, EV_ABS, ABS_X, 0}),
               "gamepad -> mouse: X released, stick centred");
        c = Controls{}; m.step(c, 0.01);
        c.rt = 1; m.step(c, 0.01);  // left button held
        c.menu = true; m.step(c, 0.01);
        c.b = true; ev = m.step(c, 0.01);
        expect(m.input() == "gamepad" && has(ev, {DEV_MOUSE, EV_KEY, BTN_LEFT, 0}), "mouse -> back: left button released");
        c = Controls{}; m.step(c, 0.01);
        // b from off wraps to tune; a walks through all six
        Mapper w(cfg("off"));
        auto chord = [&](bool next) {
            Controls k; k.menu = true; w.step(k, 0.01);
            (next ? k.a : k.b) = true; w.step(k, 0.01);
            w.step(Controls{}, 0.01);
        };
        chord(false);
        expect(w.input() == "tune", "off: menu + b -> tune");
        std::string seen;
        for (int i = 0; i < 6; ++i) { chord(true); seen += w.controls() + ","; }
        expect(seen == "off,gamepad,mouse: stick,mouse: motion,mouse: aim,tune,", ("cycle: " + seen).c_str());
    }
    {  // stereo window at the edges (common/vrdual_frames.hpp)
        int l = edge_band(90, -0.034f, 1, 1920), r = edge_band(90, 0.034f, 1, 1920);
        expect(l == -33 && r == 33, "edge band: ipd 68 mm, hfov 90, things at 1 m -> 33 px, left eye at the left edge");
        expect(edge_band(90, -0.034f, 0.5f, 1920) == -65, "edge band: nearer things -> wider (0.5 m: 65 px)");
        expect(edge_band(90, 0, 1, 1920) == 0, "edge band: unshifted eye (anchor left/right) -> none");
        std::vector<uint8_t> img(40 * 2 * 4, 200);
        edge_mask(img.data(), 40, 2, -5, false);
        auto px = [&](int x, int y) { return img[(size_t(y) * 40 + x) * 4]; };
        bool black = true;
        for (int x = 0; x < 5; ++x) black &= px(x, 0) == 0 && px(x, 1) == 0;
        bool ramp = true;
        for (int x = 5; x < 9; ++x) ramp &= px(x, 0) < px(x + 1, 0) && px(x + 1, 0) < 200;
        bool rest = true;
        for (int x = 10; x < 40; ++x) rest &= px(x, 0) == 200;
        expect(black && ramp && rest && img[3] == 200, "edge mask: 5 px black, fade over 5 px, rest and alpha untouched");
        std::vector<uint8_t> b(40 * 4, 200);
        edge_mask(b.data(), 40, 1, 5, true);
        expect(b[39 * 4] == 0 && b[0] == 0 && b[20 * 4] == 200, "edge mask both: right band + mirrored left band");
    }
    // uinput round trip with a virtual pad
    {
        // a running vrdual-xr has a pad of the same name: only a node that appears now is ours
        auto pad_nodes = [] {
            std::set<std::string> s;
            if (DIR* d = opendir("/sys/class/input")) {
                while (dirent* e = readdir(d)) {
                    if (strncmp(e->d_name, "event", 5)) continue;
                    std::ifstream f(std::string("/sys/class/input/") + e->d_name + "/device/name");
                    std::string n;
                    std::getline(f, n);
                    // a real 360 pad would be on a usb path; virtual devices live under /devices/virtual
                    char buf[512];
                    ssize_t l = readlink((std::string("/sys/class/input/") + e->d_name).c_str(), buf, sizeof buf - 1);
                    if (n == Uinput::name(DEV_PAD) && l > 0 && std::string(buf, l).find("virtual") != std::string::npos)
                        s.insert(e->d_name);
                }
                closedir(d);
            }
            return s;
        };
        std::set<std::string> before = pad_nodes();
        Uinput u;
        bool made = u.emit({{DEV_PAD, EV_ABS, ABS_X, 0}});
        if (!made && u.error().find("No such device") != std::string::npos) {
            printf("skip  uinput: %s\n", u.error().c_str());
            printf("%s\n", fails ? "input test: FAILED" : "input test: all passed (uinput skipped)");
            return fails ? 1 : 0;
        }
        expect(made, "uinput: virtual pad created");
        std::string node;
        for (int i = 0; i < 40 && node.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            for (const std::string& e : pad_nodes())
                if (!before.count(e)) node = "/dev/input/" + e;
        }
        int fd = node.empty() ? -1 : open(node.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            printf("skip  uinput read back (%s)\n", node.empty() ? "no event node" : strerror(errno));
        } else {
            u.emit({{DEV_PAD, EV_KEY, BTN_A, 1}});
            bool seen = false;
            for (int i = 0; i < 40 && !seen; ++i) {
                input_event e;
                while (read(fd, &e, sizeof e) == (ssize_t)sizeof e)
                    if (e.type == EV_KEY && e.code == BTN_A && e.value == 1) seen = true;
                if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            u.emit({{DEV_PAD, EV_KEY, BTN_A, 0}});
            close(fd);
            expect(seen, "uinput: BTN_A read back through evdev");
        }
    }
    printf("%s\n", fails ? "input test: FAILED" : "input test: all passed");
    return fails ? 1 : 0;
}
