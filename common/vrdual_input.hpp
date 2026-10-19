// vrdual input: controller state -> what the game understands, through uinput.
//
//   input = gamepad   virtual Xbox 360 pad (games with controller support)
//   input = mouse     mouse + keyboard (games without), mouse_mode:
//                       stick   right stick deflection -> pointer velocity
//                       motion  right controller rotation -> relative motion,
//                               left grip held = clutch (re-centre the hand)
//                       aim     right controller ray -> absolute pointer
//   head_look         mouse: head yaw/pitch deltas -> relative mouse (1:1)
//                     edge:  past head_deadzone from the start direction,
//                            the angle sets a turn rate
//
// Buttons are remapped with map_<button> = KEY_*|BTN_*|none in [global] or
// [app:NAME]; buttons: a b x y l3 r3 lt rt lg rg menu. In mouse mode the menu
// button (unmapped) cycles mouse_mode; held 1 s it re-centres head_look=edge.
//
// Mapper is pure (state in, events out) so it can be tested without devices;
// Uinput creates the virtual devices on first use. Header-only.
#pragma once
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vrdual_config.hpp"

namespace vrdual {

static const struct { const char* n; int c; } kInputNames[] = {
#include "input_names.hpp"
};

inline int input_code(const std::string& name) {
    for (auto& e : kInputNames)
        if (lower(name) == lower(e.n)) return e.c;
    return -1;
}

// One snapshot of the controllers (OpenXR conventions: y up, yaw positive = left).
struct Controls {
    float lx = 0, ly = 0, rx = 0, ry = 0;  // sticks, -1..1
    float lt = 0, rt = 0, lg = 0, rg = 0;  // triggers / grips, 0..1
    bool a = false, b = false, x = false, y = false, l3 = false, r3 = false, menu = false;
    bool aim_ok = false;                    // aim ray hits the virtual screen
    float aim_u = 0, aim_v = 0;             // 0..1, v down
    bool hand_ok = false;                   // right controller orientation valid
    float hand_yaw = 0, hand_pitch = 0;     // degrees
    bool head_ok = false;
    float head_yaw = 0, head_pitch = 0;     // degrees
    float head_px = 0, head_py = 0, head_pz = 0;  // meters, OpenXR LOCAL space
    bool hands_ok = false;                  // both grip poses valid
    float hand_p[2][3] = {};                // left, right grip position, meters, screen space
};

// Two-hand grab of the cinema screen (vrdual-xr, screen = flat|curved): both
// grips held -> the hands' midpoint drags the screen, their distance scales
// its width. Pure, like Mapper. The caller hides both grips from the game
// while step() returns true.
struct ScreenGrab {
    float off[3] = {0, 0, 0};  // screen offset from its configured place, meters
    float fov = 60;            // current width, degrees
    bool both = false, moved = false;
    float m0[3] = {}, off0[3] = {}, s0 = 0, fov0 = 0;

    bool step(const Controls& c) {
        bool now = c.hands_ok && c.lg > 0.8f && c.rg > 0.8f;
        const float *l = c.hand_p[0], *r = c.hand_p[1];
        float m[3] = {(l[0] + r[0]) / 2, (l[1] + r[1]) / 2, (l[2] + r[2]) / 2};
        float s = std::sqrt((l[0] - r[0]) * (l[0] - r[0]) + (l[1] - r[1]) * (l[1] - r[1]) + (l[2] - r[2]) * (l[2] - r[2]));
        if (now && !both) {
            for (int k = 0; k < 3; ++k) { m0[k] = m[k]; off0[k] = off[k]; }
            s0 = s;
            fov0 = fov;
        } else if (now) {
            for (int k = 0; k < 3; ++k) off[k] = off0[k] + m[k] - m0[k];
            if (s0 > 0.05f) fov = std::fmin(170.0f, std::fmax(10.0f, fov0 * s / s0));
            moved = true;
        }
        both = now;
        return now;
    }
};

enum InDev { DEV_PAD, DEV_MOUSE, DEV_TABLET, DEV_KBD };
struct InEvent {
    int dev, type, code, value;
    bool operator==(const InEvent& o) const { return dev == o.dev && type == o.type && code == o.code && value == o.value; }
};

enum Btn { B_A, B_B, B_X, B_Y, B_L3, B_R3, B_LT, B_RT, B_LG, B_RG, B_MENU, B_COUNT };
static const char* const kBtnNames[B_COUNT] = {"a", "b", "x", "y", "l3", "r3", "lt", "rt", "lg", "rg", "menu"};

struct InputCfg {
    std::string input = "off", mouse_mode = "stick", lstick = "wasd", head_look = "off";
    float speed = 1, curve = 2.5f, deadzone = 0.15f;
    float head_dz = 15, head_rate = 120, head_cpd = 10;
    bool lean = false;
    float lean_max = 0.1f;
    int map[B_COUNT];  // code, 0 = none, -1 = mode default
};

// Settings + raw map_* keys of [global] and [app:NAME].
inline InputCfg load_input_cfg(const std::string& app) {
    Settings st = load_settings({app});
    InputCfg c;
    c.input = st.s("input");
    c.mouse_mode = st.s("mouse_mode");
    c.lstick = st.s("lstick");
    c.head_look = st.s("head_look");
    c.speed = st.f("mouse_speed");
    c.curve = st.f("mouse_curve");
    c.deadzone = st.f("deadzone");
    c.head_dz = st.f("head_deadzone");
    c.head_rate = st.f("head_rate");
    c.head_cpd = st.f("head_counts_per_deg");
    c.lean = st.s("head_lean") == "on";
    c.lean_max = st.f("lean_max");
    for (int& m : c.map) m = -1;
    Ini ini = parse_ini(config_path());
    for (std::string sec : {std::string("global"), "app:" + lower(st.profile)}) {
        auto it = ini.find(sec);
        if (it == ini.end()) continue;
        for (int b = 0; b < B_COUNT; ++b) {
            auto kv = it->second.find(std::string("map_") + kBtnNames[b]);
            if (kv == it->second.end()) continue;
            int code = lower(kv->second) == "none" ? 0 : input_code(kv->second);
            if (code < 0) fprintf(stderr, "[input] map_%s: unknown code '%s'\n", kBtnNames[b], kv->second.c_str());
            else c.map[b] = code;
        }
    }
    return c;
}

inline float wrap180(float a) {
    while (a > 180) a -= 360;
    while (a < -180) a += 360;
    return a;
}

class Mapper {
public:
    explicit Mapper(const InputCfg& c) : cfg_(c) {
        mode_ = cfg_.mouse_mode == "motion" ? 1 : cfg_.mouse_mode == "aim" ? 2 : 0;
    }
    const char* mouse_mode() const { return mode_ == 1 ? "motion" : mode_ == 2 ? "aim" : "stick"; }
    int recentres = 0;  // menu held 1 s; vrdual-xr also re-centres head_lean on it

    // Code a button sends in the current input mode (0 = nothing).
    int code_of(int b) const {
        if (cfg_.map[b] >= 0) return cfg_.map[b];
        if (cfg_.input == "gamepad") {
            static const int pad[B_COUNT] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_THUMBL, BTN_THUMBR, 0, 0, BTN_TL, BTN_TR, BTN_START};
            return pad[b];  // triggers are axes
        }
        if (cfg_.input == "mouse") {
            static const int mouse[B_COUNT] = {KEY_ENTER, KEY_ESC, KEY_SPACE, KEY_TAB, KEY_LEFTALT, BTN_MIDDLE,
                                               KEY_LEFTSHIFT, BTN_LEFT, KEY_LEFTCTRL, BTN_RIGHT, 0};
            if (b == B_LG && mode_ == 1) return 0;  // clutch
            return mouse[b];
        }
        return 0;
    }

    std::vector<InEvent> step(const Controls& c, double dt) {
        std::vector<InEvent> ev;
        bool now[B_COUNT] = {c.a, c.b, c.x, c.y, c.l3, c.r3, hyst(B_LT, c.lt), hyst(B_RT, c.rt), hyst(B_LG, c.lg), hyst(B_RG, c.rg), c.menu};
        menu_hold_ = now[B_MENU] ? menu_hold_ + dt : 0;
        if (cfg_.input == "gamepad") pad_axes(c, ev);
        if (cfg_.input != "off") {
            for (int b = 0; b < B_COUNT; ++b) {
                if (now[b] == prev_[b]) continue;
                int code = down_code_[b];  // release what was pressed, even if the mode changed meanwhile
                if (now[b]) code = down_code_[b] = code_of(b);
                if (code > 0) key(ev, code, now[b]);
                if (!now[b]) {
                    if (b == B_MENU && code <= 0 && cfg_.input == "mouse" && !recentred_) {
                        mode_ = (mode_ + 1) % 3;
                        fprintf(stderr, "[input] mouse mode: %s\n", mouse_mode());
                    }
                    down_code_[b] = 0;
                }
            }
        }
        if (cfg_.input == "mouse") {
            lstick_keys(c, ev);
            pointer(c, dt, ev);
        }
        if (!now[B_MENU]) recentred_ = false;
        if (menu_hold_ >= 1.0 && !recentred_) {
            head_ref_ok_ = false;  // edge: the current direction becomes the centre
            recentred_ = true;
            ++recentres;
            fprintf(stderr, "[input] head look re-centred\n");
        }
        head(c, dt);
        flush_rel(ev);
        for (int b = 0; b < B_COUNT; ++b) prev_[b] = now[b];
        return ev;
    }

private:
    InputCfg cfg_;
    int mode_ = 0;
    bool prev_[B_COUNT] = {};
    int down_code_[B_COUNT] = {};
    bool an_[B_COUNT] = {};  // analog buttons' current state (hysteresis)
    double menu_hold_ = 0;
    bool recentred_ = false;
    std::map<int, int> axis_;  // last sent pad axes
    bool dir_[4] = {};         // left stick directions held (mouse mode)
    float fx_ = 0, fy_ = 0, fw_ = 0;  // sub-count remainders
    int tab_u_ = -1, tab_v_ = -1;
    bool hand_prev_ok_ = false;
    float hand_yaw_ = 0, hand_pitch_ = 0;
    bool head_prev_ok_ = false, head_ref_ok_ = false;
    float head_yaw_ = 0, head_pitch_ = 0, ref_yaw_ = 0, ref_pitch_ = 0;

    bool hyst(int b, float v) { return an_[b] = an_[b] ? v > 0.45f : v > 0.55f; }
    static void key(std::vector<InEvent>& ev, int code, bool down) {
        int dev = code >= BTN_MOUSE && code < BTN_JOYSTICK ? DEV_MOUSE : code >= BTN_JOYSTICK && code < BTN_DIGI ? DEV_PAD : DEV_KBD;
        ev.push_back({dev, EV_KEY, code, down ? 1 : 0});
    }
    void axis(std::vector<InEvent>& ev, int code, int v) {
        auto it = axis_.find(code);
        if (it != axis_.end() && it->second == v) return;
        axis_[code] = v;
        ev.push_back({DEV_PAD, EV_ABS, code, v});
    }
    void pad_axes(const Controls& c, std::vector<InEvent>& ev) {
        auto s = [](float v) { return int(lrintf(std::fmax(-1.f, std::fmin(1.f, v)) * 32767)); };
        axis(ev, ABS_X, s(c.lx));
        axis(ev, ABS_Y, s(-c.ly));  // pads report y down
        axis(ev, ABS_RX, s(c.rx));
        axis(ev, ABS_RY, s(-c.ry));
        axis(ev, ABS_Z, int(lrintf(std::fmin(1.f, c.lt) * 255)));
        axis(ev, ABS_RZ, int(lrintf(std::fmin(1.f, c.rt) * 255)));
    }
    void lstick_keys(const Controls& c, std::vector<InEvent>& ev) {
        if (cfg_.lstick == "none") return;
        bool arrows = cfg_.lstick == "arrows";
        const int keys[4] = {arrows ? KEY_UP : KEY_W, arrows ? KEY_DOWN : KEY_S, arrows ? KEY_LEFT : KEY_A, arrows ? KEY_RIGHT : KEY_D};
        bool want[4] = {c.ly > 0.5f, c.ly < -0.5f, c.lx < -0.5f, c.lx > 0.5f};
        for (int i = 0; i < 4; ++i)
            if (want[i] != dir_[i]) { key(ev, keys[i], want[i]); dir_[i] = want[i]; }
    }
    float shape(float v) const {
        float a = std::fabs(v);
        if (a <= cfg_.deadzone) return 0;
        return std::copysign(std::pow((a - cfg_.deadzone) / (1 - cfg_.deadzone), cfg_.curve), v);
    }
    void pointer(const Controls& c, double dt, std::vector<InEvent>& ev) {
        if (mode_ == 0) {  // stick: up to 1500 counts/s
            fx_ += float(shape(c.rx) * 1500 * cfg_.speed * dt);
            fy_ -= float(shape(c.ry) * 1500 * cfg_.speed * dt);
        } else {
            fw_ += float(shape(c.ry) * 10 * dt);  // wheel, up to 10 notches/s
        }
        if (mode_ == 1) {  // motion: 20 counts per degree of hand rotation
            bool clutch = an_[B_LG];
            if (c.hand_ok && hand_prev_ok_ && !clutch) {
                fx_ -= wrap180(c.hand_yaw - hand_yaw_) * 20 * cfg_.speed;
                fy_ -= (c.hand_pitch - hand_pitch_) * 20 * cfg_.speed;
            }
            hand_prev_ok_ = c.hand_ok;
            hand_yaw_ = c.hand_yaw;
            hand_pitch_ = c.hand_pitch;
        }
        if (mode_ == 2 && c.aim_ok) {
            int u = int(lrintf(c.aim_u * 65535)), v = int(lrintf(c.aim_v * 65535));
            if (u != tab_u_ || v != tab_v_) {
                ev.push_back({DEV_TABLET, EV_ABS, ABS_X, u});
                ev.push_back({DEV_TABLET, EV_ABS, ABS_Y, v});
                tab_u_ = u;
                tab_v_ = v;
            }
        }
    }
    void head(const Controls& c, double dt) {
        if (cfg_.head_look == "mouse" && c.head_ok && head_prev_ok_) {
            fx_ -= wrap180(c.head_yaw - head_yaw_) * cfg_.head_cpd;
            fy_ -= (c.head_pitch - head_pitch_) * cfg_.head_cpd;
        }
        if (cfg_.head_look == "edge" && c.head_ok) {
            if (!head_ref_ok_) { ref_yaw_ = c.head_yaw; ref_pitch_ = c.head_pitch; head_ref_ok_ = true; }
            auto rate = [&](float a) {  // deg/s of game turn for a head angle a past the centre
                float over = std::fabs(a) - cfg_.head_dz;
                return over <= 0 ? 0.f : std::copysign(std::fmin(over / 45.f, 1.f) * cfg_.head_rate, a);
            };
            fx_ -= float(rate(wrap180(c.head_yaw - ref_yaw_)) * dt * cfg_.head_cpd);
            fy_ -= float(rate(c.head_pitch - ref_pitch_) * dt * cfg_.head_cpd);
        }
        head_prev_ok_ = c.head_ok;
        head_yaw_ = c.head_yaw;
        head_pitch_ = c.head_pitch;
    }
    void flush_rel(std::vector<InEvent>& ev) {
        int dx = int(fx_), dy = int(fy_), dw = int(fw_);
        fx_ -= dx;
        fy_ -= dy;
        fw_ -= dw;
        if (dx) ev.push_back({DEV_MOUSE, EV_REL, REL_X, dx});
        if (dy) ev.push_back({DEV_MOUSE, EV_REL, REL_Y, dy});
        if (dw) ev.push_back({DEV_MOUSE, EV_REL, REL_WHEEL, dw});
    }
};

// Virtual devices, created on first event for them.
class Uinput {
public:
    ~Uinput() {
        for (int& fd : fd_)
            if (fd >= 0) { ioctl(fd, UI_DEV_DESTROY); close(fd); }
    }
    bool emit(const std::vector<InEvent>& ev) {
        bool used[4] = {};
        for (auto& e : ev) {
            if (!open_dev(e.dev)) continue;
            write_ev(fd_[e.dev], e.type, e.code, e.value);
            used[e.dev] = true;
        }
        for (int d = 0; d < 4; ++d)
            if (used[d]) write_ev(fd_[d], EV_SYN, SYN_REPORT, 0);
        return !failed_;
    }
    const std::string& error() const { return err_; }
    static const char* name(int dev) {
        static const char* const n[4] = {"Microsoft X-Box 360 pad", "vrdual mouse", "vrdual pointer", "vrdual keyboard"};
        return n[dev];
    }

private:
    int fd_[4] = {-1, -1, -1, -1};
    bool failed_ = false;
    std::string err_;

    static void write_ev(int fd, int type, int code, int value) {
        input_event e{};
        e.type = (unsigned short)type;
        e.code = (unsigned short)code;
        e.value = value;
        if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) {}
    }
    static void abs_setup(int fd, int code, int lo, int hi, int flat) {
        uinput_abs_setup a{};
        a.code = (unsigned short)code;
        a.absinfo.minimum = lo;
        a.absinfo.maximum = hi;
        a.absinfo.flat = flat;
        ioctl(fd, UI_SET_ABSBIT, code);
        ioctl(fd, UI_ABS_SETUP, &a);
    }
    bool open_dev(int dev) {
        if (fd_[dev] >= 0) return true;
        if (failed_) return false;
        int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            err_ = std::string("/dev/uinput: ") + strerror(errno) +
                   (errno == ENODEV ? " (uinput module not loaded; after a kernel update: reboot)" : "");
            fprintf(stderr, "[input] %s — no virtual devices\n", err_.c_str());
            failed_ = true;
            return false;
        }
        uinput_setup us{};
        us.id.bustype = BUS_USB;
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        if (dev == DEV_PAD) {
            us.id.vendor = 0x045e;  // Xbox 360 pad: known to SDL / Steam Input
            us.id.product = 0x028e;
            us.id.version = 0x0110;
            for (int b : {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR}) ioctl(fd, UI_SET_KEYBIT, b);
            ioctl(fd, UI_SET_EVBIT, EV_ABS);
            for (int a : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) abs_setup(fd, a, -32768, 32767, 128);
            for (int a : {ABS_Z, ABS_RZ}) abs_setup(fd, a, 0, 255, 0);
            for (int a : {ABS_HAT0X, ABS_HAT0Y}) abs_setup(fd, a, -1, 1, 0);
        } else if (dev == DEV_MOUSE) {
            us.id.vendor = 0x1d6b;
            us.id.product = 0x7672;
            for (int b = BTN_LEFT; b <= BTN_TASK; ++b) ioctl(fd, UI_SET_KEYBIT, b);
            ioctl(fd, UI_SET_EVBIT, EV_REL);
            for (int r : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL}) ioctl(fd, UI_SET_RELBIT, r);
        } else if (dev == DEV_TABLET) {  // absolute pointer, like a VM tablet; maps to the whole desktop
            us.id.vendor = 0x1d6b;
            us.id.product = 0x7673;
            for (int b : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) ioctl(fd, UI_SET_KEYBIT, b);
            ioctl(fd, UI_SET_EVBIT, EV_ABS);
            for (int a : {ABS_X, ABS_Y}) abs_setup(fd, a, 0, 65535, 0);
        } else {
            us.id.vendor = 0x1d6b;
            us.id.product = 0x7674;
            for (int k = KEY_ESC; k < BTN_MISC; ++k) ioctl(fd, UI_SET_KEYBIT, k);
        }
        snprintf(us.name, sizeof us.name, "%s", name(dev));
        if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
            fprintf(stderr, "[input] creating '%s' failed: %s\n", name(dev), strerror(errno));
            close(fd);
            failed_ = true;
            return false;
        }
        fprintf(stderr, "[input] virtual device: %s\n", name(dev));
        fd_[dev] = fd;
        return true;
    }
};

}  // namespace vrdual
