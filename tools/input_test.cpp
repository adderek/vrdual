// vrdual-input-test: checks the input Mapper with scripted controller states,
// then a uinput round trip with a virtual pad (read back through evdev when
// the event node is accessible; joysticks get a uaccess ACL on logind seats).
// Exit 0 = all good. Never creates a mouse or keyboard (would move the desktop).
#include <dirent.h>

#include <chrono>
#include <fstream>
#include <thread>

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

    // uinput round trip with a virtual pad
    {
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
            if (DIR* d = opendir("/sys/class/input")) {
                while (dirent* e = readdir(d)) {
                    if (strncmp(e->d_name, "event", 5)) continue;
                    std::ifstream f(std::string("/sys/class/input/") + e->d_name + "/device/name");
                    std::string n;
                    std::getline(f, n);
                    if (n == Uinput::name(DEV_PAD)) {
                        // ours: a real 360 pad would be on a usb path; virtual devices live under /devices/virtual
                        char buf[512];
                        ssize_t l = readlink((std::string("/sys/class/input/") + e->d_name).c_str(), buf, sizeof buf - 1);
                        if (l > 0 && std::string(buf, l).find("virtual") != std::string::npos) node = std::string("/dev/input/") + e->d_name;
                    }
                }
                closedir(d);
            }
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
