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
// Buttons are remapped with map_<mode>_<button> = KEY_*|BTN_*|none (mode:
// gamepad, mouse) or map_<button> (both modes) in [global] or [app:NAME];
// buttons: a b x y l3 r3 lt rt lg rg menu. Most specific wins: [app:NAME]
// map_<mode>_<button>, [app:NAME] map_<button>, [global] ..., built-in
// (kMapDefault). vrdual-xr reloads them when the config file changes. In mouse mode the menu
// button (unmapped) cycles mouse_mode; held 1 s it re-centres head_look=edge.
// Menu held + a / b: next / previous control mode (off, gamepad, mouse stick,
// motion, aim, tune), in every mode; so menu's own code (gamepad: Start) is
// sent as a tap when it is released without a chord and before 1 s.
//
// Mapper is pure (state in, events out) so it can be tested without devices;
// Uinput creates the virtual devices on first use. Header-only.
#pragma once
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
inline const char* input_name(int code) {  // "none" for 0; of aliases the last defined (BTN_A, not BTN_GAMEPAD)
    if (code == 0) return "none";
    const char* n = "?";
    for (auto& e : kInputNames)
        if (e.c == code && strcmp(e.n, "KEY_MIN_INTERESTING")) n = e.n;
    return n;
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

// Quaternions as float[4] = x, y, z, w (OpenXR order).
inline void qmul(const float a[4], const float b[4], float o[4]) {
    float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    memcpy(o, r, sizeof r);
}
// Shortest rotation taking direction u to direction v.
inline void qarc(const float u[3], const float v[3], float o[4]) {
    float lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), lv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    if (lu < 1e-6f || lv < 1e-6f) return;
    float d = (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) / (lu * lv);
    if (d < -0.9999f) return;  // opposite: ambiguous axis, keep
    float c[3] = {(u[1] * v[2] - u[2] * v[1]) / (lu * lv), (u[2] * v[0] - u[0] * v[2]) / (lu * lv), (u[0] * v[1] - u[1] * v[0]) / (lu * lv)};
    float w = 1 + d, n = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + w * w);
    o[0] = c[0] / n; o[1] = c[1] / n; o[2] = c[2] / n; o[3] = w / n;
}

// Rotation by angle (rad) about axis k (0 = x, 1 = y, 2 = z).
inline void qaxis(int k, float a, float o[4]) {
    o[0] = o[1] = o[2] = 0;
    o[k] = std::sin(a / 2);
    o[3] = std::cos(a / 2);
}
inline void qinv(const float q[4], float o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
inline void qrotv(const float q[4], const float v[3], float o[3]) {
    float p[4] = {v[0], v[1], v[2], 0}, qi[4], t[4];
    qinv(q, qi);
    qmul(q, p, t);
    qmul(t, qi, t);
    o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
}
// Round q's yaw (y), pitch (x) and roll (z), applied in that order, to step rad.
inline void qsnap(float q[4], float step) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float yaw = std::atan2(2 * (x * z + w * y), 1 - 2 * (x * x + y * y));
    float pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, -2 * (y * z - w * x))));
    float roll = std::atan2(2 * (x * y + w * z), 1 - 2 * (x * x + z * z));
    auto r = [&](float a) { return std::round(a / step) * step; };
    float a[4], b[4];
    qaxis(1, r(yaw), a);
    qaxis(0, r(pitch), b);
    qmul(a, b, a);
    qaxis(2, r(roll), b);
    qmul(a, b, q);
}

// Screen placement from the controllers (vrdual-xr). Pure, like Mapper.
//   both grips held: the hands' midpoint drags the screen, the line between
//   them turns it (about its centre), their distance scales it; the turn
//   snaps to 15 deg steps (yaw / pitch / roll relative to the base, so level
//   stays level) unless a trigger was pressed during this grab. Meanwhile the right stick
//   stretches width (x) and height (y) separately and the left stick orbits
//   the screen around the viewer (yaw / pitch about the base). The caller
//   sets basis (the base orientation in the hands' space), adds orbit_q() to
//   it and hides grips, triggers and sticks from the game while step()
//   returns true. move = false (vrdual-xr: every mode but tune): no grab,
//   the grips stay the game's; the menu hold still counts.
//   menu held 1 s: ++recentres (vrdual-xr: world lock -> screen in front of
//   the current gaze; head lock -> freeze the screen in place / follow again)
struct ScreenGrab {
    float off[3] = {0, 0, 0};    // centre offset, meters, in the screen's space
    float rot[4] = {0, 0, 0, 1}; // extra rotation about the centre
    float sx = 1, sy = 1;        // width / height scale
    float orbit[2] = {0, 0};     // yaw (left +), pitch (up +) about the viewer, rad
    float basis[4] = {0, 0, 0, 1};
    // turn: left trigger held -> the hands' twist and the left stick rotate
    // (without it a grab only moves and scales: no accidental yaw / pivot)
    // fine: right trigger pressed during this grab -> no snap until release
    bool both = false, fine = false, turn = false;
    int recentres = 0;
    double menu_t = 0;
    float m0[3] = {}, off0[3] = {}, v0[3] = {}, rot0[4] = {0, 0, 0, 1}, s0 = 0, sx0 = 1, sy0 = 1;

    static constexpr float kSnap = 0.2617994f;  // 15 deg
    static constexpr float kOrbitRate = 1.0471976f;  // 60 deg/s at full deflection

    void orbit_q(float o[4]) const {
        float p[4];
        qaxis(1, orbit[0], o);
        qaxis(0, orbit[1], p);
        qmul(o, p, o);
    }
    void reset_pose() {
        memset(off, 0, sizeof off);
        rot[0] = rot[1] = rot[2] = 0; rot[3] = 1;
        orbit[0] = orbit[1] = 0;
    }
    bool step(const Controls& c, double dt, bool move = true) {
        if (c.menu) {
            if (menu_t < 1 && menu_t + dt >= 1) ++recentres;
            menu_t += dt;
        } else {
            menu_t = 0;
        }
        bool now = move && c.hands_ok && c.lg > 0.8f && c.rg > 0.8f;
        const float *l = c.hand_p[0], *r = c.hand_p[1];
        float m[3] = {(l[0] + r[0]) / 2, (l[1] + r[1]) / 2, (l[2] + r[2]) / 2};
        float v[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]};
        float s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        auto regrab = [&] {
            for (int k = 0; k < 3; ++k) { m0[k] = m[k]; off0[k] = off[k]; v0[k] = v[k]; }
            memcpy(rot0, rot, sizeof rot);
            s0 = s;
            sx0 = sx;
            sy0 = sy;
        };
        bool t = c.lt > 0.5f;
        if (now && !both) {
            regrab();
            fine = false;
        } else if (now) {
            if (t != turn) regrab();  // rotation on / off: no jump
            float b[4], bi[4], q[4];
            float dz = 0.2f;
            float oy = t && std::fabs(c.lx) > dz ? -c.lx * kOrbitRate * float(dt) : 0, op = t && std::fabs(c.ly) > dz ? c.ly * kOrbitRate * float(dt) : 0;
            if (oy != 0 || op != 0) {  // R = B' B^-1 about the viewer: off -> R off, rot -> R rot R^-1
                float B[4], B1[4], R[4], Ri[4];
                orbit_q(q); qmul(basis, q, B); qinv(B, bi);
                orbit[0] += oy;
                orbit[1] = std::fmax(-1.4f, std::fmin(1.4f, orbit[1] + op));
                orbit_q(q); qmul(basis, q, B1);
                qmul(B1, bi, R); qinv(R, Ri);
                qrotv(R, off, off);
                qmul(R, rot, rot); qmul(rot, Ri, rot);
                regrab();  // hand motion keeps working from here
            }
            for (int k = 0; k < 3; ++k) off[k] = off0[k] + m[k] - m0[k];
            if (t) {
                qarc(v0, v, q);
                qmul(q, rot0, rot);
            }
            fine |= c.rt > 0.5f;
            if (t && !fine) {  // snap in the base frame: B^-1 rot B
                orbit_q(q); qmul(basis, q, b); qinv(b, bi);
                qmul(bi, rot, q); qmul(q, b, q);
                qsnap(q, kSnap);
                qmul(b, q, q); qmul(q, bi, rot);
            }
            auto stretch = [&](float a) { return std::fabs(a) > 0.2f ? 1 + 0.5f * a * float(dt) : 1.0f; };  // 50 %/s at full deflection
            sx0 *= stretch(c.rx);
            sy0 *= stretch(c.ry);
            float k = s0 > 0.05f ? s / s0 : 1;
            sx = std::fmin(10.0f, std::fmax(0.1f, sx0 * k));
            sy = std::fmin(10.0f, std::fmax(0.1f, sy0 * k));
        }
        both = now;
        turn = t;
        return now;
    }
};

// input = tune: the controllers change the live stereo settings instead of
// driving the game (vrdual-xr, while no grip or trigger is held):
//   left stick y   world_scale  x2 per second at full deflection (up = more depth)
//   right stick y  hud_depth    x2 per second (up = farther; past 100 m = 0 = infinity)
//   right stick x  hfov         20 deg/s; with screen set (flat / curved
//                  screen, where hfov only scales the depth) screen_fov instead
//   a              save them into [app:NAME]
//   b              undo: back to the last saved values; pressed again, to the
//                  values the game started with, then to the built-in defaults
// step() returns TUNE_* bits (undone_to says where b went); the caller seeds
// v[] and start[] from the control block (screen_fov: from the screen).
enum { TUNE_CHANGED = 1, TUNE_SETTLED = 2, TUNE_SAVE = 4, TUNE_RESET = 8 };
struct Tuner {
    enum { WORLD_SCALE, HUD_DEPTH, HFOV, SCREEN_FOV, N };
    float v[N] = {1, 0, 90, 60};
    float start[N] = {1, 0, 90, 60};
    float saved[N] = {NAN, NAN, NAN, NAN};  // NAN = not saved in this session
    enum { UNDO_SAVED, UNDO_START, UNDO_DEFAULTS } undone_to = UNDO_SAVED;
    bool moving = false, a = false, b = false, undid = false;
    bool screen = false;  // right stick x = screen_fov
    float dz = 0.15f;

    int step(const Controls& c, double dt) {
        int r = 0;
        bool idle = c.lg < 0.5f && c.rg < 0.5f && c.lt < 0.5f && c.rt < 0.5f;
        auto rate = [&](float s) {  // signed, 0..1, quadratic past the dead zone: fine near the centre
            float m = (std::fabs(s) - dz) / (1 - dz);
            return idle && m > 0 ? std::copysign(m * m, s) : 0.0f;
        };
        float ws = rate(c.ly), hd = rate(c.ry), fv = rate(c.rx);
        float old[N];
        memcpy(old, v, sizeof v);
        if (ws != 0) v[WORLD_SCALE] = std::fmin(10000.0f, std::fmax(0.01f, v[WORLD_SCALE] * std::exp2(ws * float(dt))));
        if (hd < 0 && v[HUD_DEPTH] <= 0) v[HUD_DEPTH] = 100;  // in from infinity
        if (hd != 0 && v[HUD_DEPTH] > 0) {
            v[HUD_DEPTH] = std::fmax(0.1f, v[HUD_DEPTH] * std::exp2(hd * float(dt)));
            if (v[HUD_DEPTH] > 100) v[HUD_DEPTH] = 0;
        }
        if (fv != 0 && !screen) v[HFOV] = std::fmin(170.0f, std::fmax(20.0f, v[HFOV] + 20 * fv * float(dt)));
        if (fv != 0 && screen) v[SCREEN_FOV] = std::fmin(170.0f, std::fmax(10.0f, v[SCREEN_FOV] + 20 * fv * float(dt)));
        bool now = ws != 0 || hd != 0 || fv != 0;
        if (memcmp(old, v, sizeof v)) r |= TUNE_CHANGED;
        if (moving && !now) r |= TUNE_SETTLED;
        moving = now;
        if (idle && c.a && !a) {
            memcpy(saved, v, sizeof v);
            r |= TUNE_SAVE;
        }
        if (idle && c.b && !b) {
            // first press: saved; each press with nothing moved since: one step further
            static const float defaults[N] = {1, 0, 90, 60};
            const float* to[3] = {saved, start, defaults};
            int i = undid && !memcmp(v, to[undone_to], sizeof v) ? undone_to + 1 : 0;
            while (i < UNDO_DEFAULTS && (to[i][0] != to[i][0] || !memcmp(v, to[i], sizeof v))) ++i;  // unsaved or no-op
            if (i > UNDO_DEFAULTS) i = UNDO_SAVED + (saved[0] != saved[0]);  // wrap around
            undone_to = decltype(undone_to)(i);
            memcpy(v, to[i], sizeof v);
            undid = true;
            r |= TUNE_RESET | TUNE_CHANGED;
        }
        a = c.a;
        b = c.b;
        return r;
    }
};

enum InDev { DEV_PAD, DEV_MOUSE, DEV_TABLET, DEV_KBD };
struct InEvent {
    int dev, type, code, value;
    bool operator==(const InEvent& o) const { return dev == o.dev && type == o.type && code == o.code && value == o.value; }
};

enum Btn { B_A, B_B, B_X, B_Y, B_L3, B_R3, B_LT, B_RT, B_LG, B_RG, B_MENU, B_COUNT };
static const char* const kBtnNames[B_COUNT] = {"a", "b", "x", "y", "l3", "r3", "lt", "rt", "lg", "rg", "menu"};
enum MapMode { M_GAMEPAD, M_MOUSE, M_COUNT };
static const char* const kMapModes[M_COUNT] = {"gamepad", "mouse"};
// Built-in button codes per mode. Gamepad triggers are axes (0 = no button);
// mouse menu 0 = cycles mouse_mode; mouse lg is the clutch in motion mode.
static const int kMapDefault[M_COUNT][B_COUNT] = {
    {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_THUMBL, BTN_THUMBR, 0, 0, BTN_TL, BTN_TR, BTN_START},
    {KEY_ENTER, KEY_ESC, KEY_SPACE, KEY_TAB, KEY_LEFTALT, BTN_MIDDLE, KEY_LEFTSHIFT, BTN_LEFT, KEY_LEFTCTRL, BTN_RIGHT, 0},
};

struct InputCfg {
    std::string input = "off", mouse_mode = "stick", lstick = "wasd", head_look = "off";
    float speed = 1, curve = 2.5f, deadzone = 0.15f;
    float head_dz = 15, head_rate = 120, head_cpd = 10;
    bool lean = false;
    float lean_max = 0.1f;
    int map[M_COUNT][B_COUNT];  // code, 0 = none, -1 = mode default
    std::string map_src[M_COUNT][B_COUNT];  // "[app:NAME] map_mouse_a", "" = built-in
    std::string input_src;  // where input came from (load_input_cfg)
};

// Control mode last used per game (vrdual-xr saves it on every switch):
// $XDG_STATE_HOME/vrdual/input_last.tsv, else ~/.local/state/vrdual/, lines
// "app<TAB>input<TAB>mouse_mode". Ranks above [global] and the built-in
// profiles, below [app:NAME] input and VRDUAL_INPUT.
inline std::string input_last_path() {
    const char* x = getenv("XDG_STATE_HOME");
    const char* h = getenv("HOME");
    std::string d = x && *x ? std::string(x) : std::string(h ? h : "/tmp") + "/.local/state";
    return d + "/vrdual/input_last.tsv";
}
inline std::map<std::string, std::pair<std::string, std::string>> input_last_read() {
    std::map<std::string, std::pair<std::string, std::string>> m;
    FILE* f = fopen(input_last_path().c_str(), "r");
    if (!f) return m;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
        if (b != std::string::npos) m[l.substr(0, a)] = {l.substr(a + 1, b - a - 1), l.substr(b + 1)};
    }
    fclose(f);
    return m;
}
inline bool input_last_save(const std::string& app, const std::string& input, const std::string& mouse_mode) {
    if (app.empty()) return false;
    auto m = input_last_read();
    m[app] = {input, mouse_mode};
    std::string p = input_last_path(), d = p.substr(0, p.rfind('/'));
    mkdir(d.substr(0, d.rfind('/')).c_str(), 0755);
    mkdir(d.c_str(), 0755);
    std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (auto& kv : m) fprintf(f, "%s\t%s\t%s\n", kv.first.c_str(), kv.second.first.c_str(), kv.second.second.c_str());
    bool ok = fclose(f) == 0;
    return ok && rename(tmp.c_str(), p.c_str()) == 0;
}

// map_* keys of [global] and [app:PROFILE], least specific first (see top).
inline void load_input_maps(InputCfg& c, const std::string& profile) {
    for (int m = 0; m < M_COUNT; ++m)
        for (int b = 0; b < B_COUNT; ++b) c.map[m][b] = -1, c.map_src[m][b].clear();
    Ini ini = parse_ini(config_path());
    for (std::string sec : {std::string("global"), "app:" + lower(profile)}) {
        auto it = ini.find(sec);
        if (it == ini.end()) continue;
        for (int spec = 0; spec < 2; ++spec)
            for (int m = 0; m < M_COUNT; ++m)
                for (int b = 0; b < B_COUNT; ++b) {
                    std::string k = std::string("map_") + (spec ? std::string(kMapModes[m]) + "_" : "") + kBtnNames[b];
                    auto kv = it->second.find(k);
                    if (kv == it->second.end()) continue;
                    int code = lower(kv->second) == "none" ? 0 : input_code(kv->second);
                    if (code < 0) { if (m == 0 || spec) fprintf(stderr, "[input] %s: unknown code '%s'\n", k.c_str(), kv->second.c_str()); }
                    else c.map[m][b] = code, c.map_src[m][b] = "[" + sec + "] " + k;
                }
    }
}

// Settings + raw map_* keys of [global] and [app:NAME].
inline InputCfg load_input_cfg(const std::string& app) {
    Settings st = load_settings({app});
    InputCfg c;
    c.input = st.s("input");
    c.mouse_mode = st.s("mouse_mode");
    c.input_src = st.src["input"];
    // set by the user for this game ([app:NAME]) or env: wins over the last used mode
    auto pinned = [&](const char* k) {
        const std::string& s = st.src[k];
        return s.rfind("env ", 0) == 0 || (s.find(" [app:") != std::string::npos && s.rfind("built-in", 0) != 0);
    };
    if (!pinned("input")) {
        auto m = input_last_read();
        auto it = m.find(app);
        if (it != m.end() && !it->second.first.empty()) {
            c.input = it->second.first;
            if (!pinned("mouse_mode") && !it->second.second.empty()) c.mouse_mode = it->second.second;
            c.input_src = "last used (" + input_last_path() + ")";
        }
    }
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
    load_input_maps(c, st.profile);
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
    const std::string& input() const { return cfg_.input; }
    // "gamepad", "mouse: aim", ... for notices
    std::string controls() const { return cfg_.input == "mouse" ? std::string("mouse: ") + mouse_mode() : cfg_.input; }
    int recentres = 0;  // menu held 1 s; vrdual-xr also re-centres head_lean on it
    int changes = 0;    // control mode changed (menu + a/b, or menu tap in mouse mode)

    // Code a button sends in the current input mode (0 = nothing).
    int code_of(int b) const {
        int m = cfg_.input == "gamepad" ? M_GAMEPAD : cfg_.input == "mouse" ? M_MOUSE : -1;
        if (m < 0) return 0;
        if (cfg_.map[m][b] >= 0) return cfg_.map[m][b];
        if (m == M_MOUSE && b == B_LG && mode_ == 1) return 0;  // clutch
        return kMapDefault[m][b];
    }
    // New button maps (config reloaded); held buttons release what they pressed.
    void remap(const InputCfg& c) { memcpy(cfg_.map, c.map, sizeof cfg_.map); }

    std::vector<InEvent> step(const Controls& c, double dt) {
        std::vector<InEvent> ev;
        bool now[B_COUNT] = {c.a, c.b, c.x, c.y, c.l3, c.r3, hyst(B_LT, c.lt), hyst(B_RT, c.rt), hyst(B_LG, c.lg), hyst(B_RG, c.rg), c.menu};
        if (menu_up_) key(ev, menu_up_, false), menu_up_ = 0;  // second half of a menu tap
        menu_hold_ = now[B_MENU] ? menu_hold_ + dt : 0;
        if (now[B_MENU])  // chord: menu held + a / b switches the control mode; the game sees neither
            for (int b : {B_A, B_B})
                if (now[b] && !prev_[b]) {
                    cycle(b == B_A ? 1 : -1, ev);
                    swallow_[b] = chord_ = true;
                }
        if (cfg_.input == "gamepad") pad_axes(c, ev);
        for (int b = 0; b < B_COUNT; ++b) {
            if (now[b] == prev_[b]) continue;
            if (swallow_[b]) { swallow_[b] = now[b]; continue; }
            if (b == B_MENU) {  // acts on release: a tap, not a chord or a hold
                if (!now[b] && !chord_ && !recentred_) {
                    int code = code_of(b);
                    if (code > 0) key(ev, code, true), menu_up_ = code;
                    else if (cfg_.input == "mouse") {
                        mode_ = (mode_ + 1) % 3;
                        ++changes;
                        fprintf(stderr, "[input] mouse mode: %s\n", mouse_mode());
                    }
                }
                if (!now[b]) chord_ = false;
                continue;
            }
            int code = down_code_[b];  // release what was pressed, even if the mode changed meanwhile
            if (now[b]) code = down_code_[b] = code_of(b);
            if (code > 0) key(ev, code, now[b]);
            if (!now[b]) down_code_[b] = 0;
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
    bool swallow_[B_COUNT] = {};  // pressed as part of a chord: its release is not the game's either
    bool chord_ = false;          // a chord happened during this menu hold
    int menu_up_ = 0;             // menu tap: code to release on the next step

    // Next (+1) / previous (-1) control mode. Releases whatever the old mode
    // holds down (keys, buttons, pad axes) so nothing sticks.
    void cycle(int dir, std::vector<InEvent>& ev) {
        static const char* const kIn[] = {"off", "gamepad", "mouse", "mouse", "mouse", "tune"};
        int pos = cfg_.input == "gamepad" ? 1 : cfg_.input == "mouse" ? 2 + mode_ : cfg_.input == "tune" ? 5 : 0;
        for (int b = 0; b < B_COUNT; ++b)
            if (down_code_[b] > 0) key(ev, down_code_[b], false), down_code_[b] = 0;
        for (int i = 0; i < 4; ++i)
            if (dir_[i]) key(ev, lstick_key(i), false), dir_[i] = false;
        if (cfg_.input == "gamepad")
            for (int a : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ}) axis(ev, a, 0);
        pos = (pos + dir + 6) % 6;
        cfg_.input = kIn[pos];
        if (pos >= 2 && pos <= 4) mode_ = pos - 2;
        hand_prev_ok_ = false;
        ++changes;
        fprintf(stderr, "[input] controls: %s\n", controls().c_str());
    }
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
    int lstick_key(int i) const {  // up down left right
        bool arrows = cfg_.lstick == "arrows";
        const int keys[4] = {arrows ? KEY_UP : KEY_W, arrows ? KEY_DOWN : KEY_S, arrows ? KEY_LEFT : KEY_A, arrows ? KEY_RIGHT : KEY_D};
        return keys[i];
    }
    void lstick_keys(const Controls& c, std::vector<InEvent>& ev) {
        if (cfg_.lstick == "none") return;
        bool want[4] = {c.ly > 0.5f, c.ly < -0.5f, c.lx < -0.5f, c.lx > 0.5f};
        for (int i = 0; i < 4; ++i)
            if (want[i] != dir_[i]) { key(ev, lstick_key(i), want[i]); dir_[i] = want[i]; }
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
