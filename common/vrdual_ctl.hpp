// Access to the live control block (see vrdual_shm.h) + the eye math shared by
// the layer and the tools.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <string>

#include "vrdual_config.hpp"
#include "vrdual_shm.h"

namespace vrdual {

// Directory of the frame files and the control block. /dev/shm unless
// VRDUAL_SHM_DIR says otherwise (a Flatpak app has a private /dev/shm, which
// the host sees as /dev/shm/flatpak-<app id>-XXXXXX).
inline std::string shm_dir() {
    const char* d = getenv("VRDUAL_SHM_DIR");
    return d && *d ? d : "/dev/shm";
}
inline std::string ctl_path(const std::string& session) { return shm_dir() + "/vrdual-" + session + "-ctl"; }

// Live values from config (creation, and a new game joining an old block).
inline void ctl_seed(vrdual_ctl& c, const Settings& s) {
    c.version = 7;
    c.hunt_mode = 0;  // a hunt belongs to the game it ran on
    c.hud_mode = 0;   // so does a hidden HUD (a screenshot)
    c.ipd = s.f("ipd");
    c.ipd_near = s.f("ipd_near");
    c.near_dist = s.f("near_dist");
    c.far_dist = s.f("far_dist");
    c.world_scale = s.f("world_scale");
    c.hfov = s.f("hfov");
    c.anchor = vrdual_anchor_parse(s.s("anchor").c_str());
    c.live = s.i("live") != 0;
    c.hud_depth = s.f("hud_depth");
    snprintf(c.app, sizeof c.app, "%.63s", s.app.c_str());
}

// Opens the session's control block. With `seed` non-null the block is created
// if missing and filled from `seed` (exactly one creator wins via O_EXCL).
inline vrdual_ctl* ctl_open(const std::string& session, const Settings* seed, bool* created = nullptr) {
    if (created) *created = false;
    int fd = -1;
    if (seed) {
        fd = open(ctl_path(session).c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (ftruncate(fd, sizeof(vrdual_ctl)) != 0) { close(fd); return nullptr; }
            auto* c = (vrdual_ctl*)mmap(nullptr, sizeof(vrdual_ctl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (c == MAP_FAILED) return nullptr;
            c->seq = 0;
            ctl_seed(*c, *seed);
            __sync_synchronize();
            c->magic = VRDUAL_CTL_MAGIC;
            if (created) *created = true;
            return c;
        }
    }
    fd = open(ctl_path(session).c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st;  // a block from an older build is shorter: grow it (new fields read 0)
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(vrdual_ctl) && ftruncate(fd, sizeof(vrdual_ctl)) != 0) { close(fd); return nullptr; }
    auto* c = (vrdual_ctl*)mmap(nullptr, sizeof(vrdual_ctl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c == MAP_FAILED) return nullptr;
    for (int i = 0; i < 200 && c->magic != VRDUAL_CTL_MAGIC; ++i) usleep(1000);  // creator still seeding
    if (c->magic != VRDUAL_CTL_MAGIC) { munmap(c, sizeof *c); return nullptr; }
    return c;
}

// Consistent copy (seqlock reader).
inline vrdual_ctl ctl_read(const vrdual_ctl* c) {
    vrdual_ctl out;
    for (;;) {
        uint64_t s0 = c->seq;
        __sync_synchronize();
        memcpy((void*)&out, (const void*)c, sizeof out);
        __sync_synchronize();
        if (!(s0 & 1) && c->seq == s0) return out;
        usleep(50);
    }
}

// Seqlock writer: fn(vrdual_ctl&) mutates the block. Writers are rare (a key
// press, a CLI call), so a lost race between two writers is acceptable.
template <typename F> inline void ctl_write(vrdual_ctl* c, F fn) {
    c->seq++;
    __sync_synchronize();
    fn(*c);
    __sync_synchronize();
    c->seq++;
}

static const char* const kLiveKeys[] = {"ipd", "ipd_near", "near_dist", "far_dist", "anchor", "hfov", "world_scale", "hud_depth"};
// Live but never saved: the head offset (vrdual-xr writes it every frame) and
// hud (show / hide, for screenshots).
static const char* const kHeadKeys[] = {"head_x", "head_y", "head_z", "head_yaw", "head_pitch", "head_roll", "hud_yaw", "hud_pitch", "hud_roll", "hud"};

inline std::string live_value(const vrdual_ctl& c, const std::string& k) {
    char b[64];
    if (k == "anchor") return vrdual_anchor_name(c.anchor);
    if (k == "hud") return c.hud_mode == 1 ? "hide" : "show";
    float v = k == "ipd" ? c.ipd : k == "ipd_near" ? c.ipd_near : k == "near_dist" ? c.near_dist
            : k == "far_dist" ? c.far_dist : k == "hfov" ? c.hfov : k == "head_x" ? c.head_x
            : k == "head_y" ? c.head_y : k == "head_z" ? c.head_z : k == "head_yaw" ? c.head_yaw
            : k == "head_pitch" ? c.head_pitch : k == "head_roll" ? c.head_roll : k == "hud_yaw" ? c.hud_yaw
            : k == "hud_pitch" ? c.hud_pitch : k == "hud_roll" ? c.hud_roll : k == "hud_depth" ? c.hud_depth : c.world_scale;
    snprintf(b, sizeof b, "%g", v);
    return b;
}

// Writes the live values into [app:NAME] (or [global]); returns the section, "" on error.
// The eye keys (ipd, ipd_near, near/far_dist) belong to the person, not the
// game: only a [global] save writes them, so a profile never pins an old IPD.
inline std::string ctl_save(const vrdual_ctl& v, bool global) {
    std::string sec = global || !v.app[0] ? "global" : std::string("app:") + v.app;
    std::vector<std::pair<std::string, std::string>> kvs;
    for (auto k : kLiveKeys) {
        if (sec != "global" && (!strcmp(k, "ipd") || !strcmp(k, "ipd_near") || !strcmp(k, "near_dist") || !strcmp(k, "far_dist"))) continue;
        kvs.push_back({k, live_value(v, k)});
    }
    return save_keys(config_path(), sec, kvs) ? sec : "";
}

// Per-eye shader parameters, read by the patched vertex shader:
// vec4 #0  x = clip_dx for far objects, y = clip_dx for near objects,
//          z = far distance in view-space W units, w = 1 / (far - near)
//          offset(W) = mix(x, y, clamp((z - W) * w, 0, 1))
// vec4 #1  head offset in clip units added to x, y, w (live mode only):
//          camera moved by (hx, hy, hz) => clip.x -= P00*hx, clip.y -= P11*hy,
//          clip.w -= hz (w = view depth). P11 = y_sign * P00 * aspect; y_sign
//          is +1 for y-up clip space (GL via Zink, D3D via DXVK), -1 for
//          Vulkan apps that flip y in the projection.
//          w = clip_dx added to w == 1 vertices (HUD): the NDC disparity of a
//          point at hud_depth, -P00 * eye_dx / hud_depth (0 = HUD at infinity).
// uvec4 at +32: hunt_lo, hunt_hi, hunt_mode, hud_mode (live mode only).
// vec4 #2..#4 (at +48, after the hunt uvec4 at +32; live mode only): rows of
//          M = D * R^T * D^-1, D = diag(P00, P11, 1), applied to (x, y, w)
//          after the head offset and before the eye shift: the head rotation
//          R (yaw, pitch, roll) for a perspective P whose w is view depth. M
//          is the identity without rotation (bit-exact result).
// vec4 #5..#7 (v[20..31], at +112, after the auto_noshift flag at +96): rows
//          of M_hud, the same for the HUD turn (hud_yaw / pitch / roll),
//          applied to w == 1 vertices (x, y, w) before the HUD disparity. The
//          HUD gets the scene's homography up to hud_follow, so a cursor over
//          either lands at the same place.
struct EyeParams { float v[32]; };

// R = Ry(yaw) Rx(pitch) Rz(roll) in x right, y up, z forward; M as above.
inline void head_rot(const vrdual_ctl& c, float p00, float p11, float m[12]) {
    const float k = float(M_PI) / 180.0f;
    float cy = cosf(c.head_yaw * k), sy = sinf(c.head_yaw * k), cp = cosf(c.head_pitch * k), sp = sinf(c.head_pitch * k);
    float cr = cosf(c.head_roll * k), sr = sinf(c.head_roll * k);
    const float Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}, Rx[3][3] = {{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
    const float Rz[3][3] = {{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}};
    float t[3][3], R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = Ry[i][0] * Rx[0][j] + Ry[i][1] * Rx[1][j] + Ry[i][2] * Rx[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R[i][j] = t[i][0] * Rz[0][j] + t[i][1] * Rz[1][j] + t[i][2] * Rz[2][j];
    const float D[3] = {p00, p11, 1};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[4 * i + j] = i == j ? R[j][i] : D[i] * R[j][i] / D[j];
        m[4 * i + 3] = 0;
    }
}

inline EyeParams eye_params(const vrdual_ctl& c, int eye, float aspect = 16.0f / 9, float y_sign = 1) {
    float p00 = 1.0f / tanf(c.hfov * 0.5f * float(M_PI) / 180.0f);
    const char* an = vrdual_anchor_name(c.anchor);
    float far_ = -p00 * c.world_scale * eye_dx(an, eye, c.ipd);
    float ipdn = c.ipd_near > 0 ? c.ipd_near : c.ipd;
    float near_ = -p00 * c.world_scale * eye_dx(an, eye, ipdn);
    float wf = c.far_dist * c.world_scale, wn = c.near_dist * c.world_scale;
    float inv = wf > wn ? 1.0f / (wf - wn) : 0.0f;
    float p11 = y_sign * p00 * aspect;
    EyeParams e{{far_, near_, wf, inv, -p00 * c.world_scale * c.head_x, -p11 * c.world_scale * c.head_y,
                 -c.world_scale * c.head_z, c.hud_depth > 0 ? -p00 * eye_dx(an, eye, c.ipd) / c.hud_depth : 0}};
    head_rot(c, p00, p11, e.v + 8);
    vrdual_ctl h = c;
    h.head_yaw = c.hud_yaw, h.head_pitch = c.hud_pitch, h.head_roll = c.hud_roll;
    head_rot(h, p00, p11, e.v + 20);
    return e;
}

// Where a point the game shows at pixel (x, y) of a w x h eye image appears
// after the head turn: (sx, sy) for the scene (homography M of eye_params),
// (hx, hy) for the HUD (M_hud). vrdual-xr draws the game's cursor there.
inline void turned_px(const EyeParams& e, uint32_t w, uint32_t h, int x, int y, int& sx, int& sy, int& hx, int& hy) {
    float nx = (x + 0.5f) * 2 / w - 1, ny = 1 - (y + 0.5f) * 2 / h;
    auto map = [&](const float* m, int& ox, int& oy) {
        float X = m[0] * nx + m[1] * ny + m[2], Y = m[4] * nx + m[5] * ny + m[6], W = m[8] * nx + m[9] * ny + m[10];
        ox = x, oy = y;
        if (W > 1e-6f) ox = int(lroundf((X / W + 1) * w / 2 - 0.5f)), oy = int(lroundf((1 - Y / W) * h / 2 - 0.5f));
    };
    map(e.v + 8, sx, sy);
    map(e.v + 20, hx, hy);
}

}  // namespace vrdual
