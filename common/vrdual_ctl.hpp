// Access to the live control block (see vrdual_shm.h) + the eye math shared by
// the layer and the tools.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
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
            c->version = 1;
            c->seq = 0;
            c->ipd = seed->f("ipd");
            c->ipd_near = seed->f("ipd_near");
            c->near_dist = seed->f("near_dist");
            c->far_dist = seed->f("far_dist");
            c->world_scale = seed->f("world_scale");
            c->hfov = seed->f("hfov");
            c->anchor = vrdual_anchor_parse(seed->s("anchor").c_str());
            c->live = seed->i("live") != 0;
            snprintf(c->app, sizeof c->app, "%.63s", seed->app.c_str());
            __sync_synchronize();
            c->magic = VRDUAL_CTL_MAGIC;
            if (created) *created = true;
            return c;
        }
    }
    fd = open(ctl_path(session).c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return nullptr;
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

static const char* const kLiveKeys[] = {"ipd", "ipd_near", "near_dist", "far_dist", "anchor", "hfov", "world_scale"};

inline std::string live_value(const vrdual_ctl& c, const std::string& k) {
    char b[64];
    if (k == "anchor") return vrdual_anchor_name(c.anchor);
    float v = k == "ipd" ? c.ipd : k == "ipd_near" ? c.ipd_near : k == "near_dist" ? c.near_dist
            : k == "far_dist" ? c.far_dist : k == "hfov" ? c.hfov : c.world_scale;
    snprintf(b, sizeof b, "%g", v);
    return b;
}

// Writes the live values into [app:NAME] (or [global]); returns the section, "" on error.
inline std::string ctl_save(const vrdual_ctl& v, bool global) {
    std::string sec = global || !v.app[0] ? "global" : std::string("app:") + v.app;
    for (auto k : kLiveKeys)
        if (!save_key(config_path(), sec, k, live_value(v, k))) return "";
    return sec;
}

// Per-eye shader parameters, read by the patched vertex shader as a vec4:
//   x = clip_dx for far objects, y = clip_dx for near objects,
//   z = far distance in view-space W units, w = 1 / (far - near)
// offset(W) = mix(x, y, clamp((z - W) * w, 0, 1))
struct EyeParams { float v[4]; };

inline EyeParams eye_params(const vrdual_ctl& c, int eye) {
    float p00 = 1.0f / tanf(c.hfov * 0.5f * float(M_PI) / 180.0f);
    const char* an = vrdual_anchor_name(c.anchor);
    float far_ = -p00 * c.world_scale * eye_dx(an, eye, c.ipd);
    float ipdn = c.ipd_near > 0 ? c.ipd_near : c.ipd;
    float near_ = -p00 * c.world_scale * eye_dx(an, eye, ipdn);
    float wf = c.far_dist * c.world_scale, wn = c.near_dist * c.world_scale;
    float inv = wf > wn ? 1.0f / (wf - wn) : 0.0f;
    return {{far_, near_, wf, inv}};
}

}  // namespace vrdual
