// Reading the per-eye frame files (see vrdual_shm.h) and pairing L[F] with
// R[F]. Shared by the SBS compositor and the OpenXR output.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vrdual_shm.h"

namespace vrdual {

struct Eye {
    int eye;
    std::string name;
    vrdual_shm_header* hdr = nullptr;
    size_t bytes = 0;
    uint32_t w = 0, h = 0, fmt = 0;
    std::vector<uint8_t> px;  // last paired frame
    bool remapped = false;    // set when the size changed; the user clears it
    ino_t ino = 0;            // of the mapped file (a re-created dir/file = new inode)
    bool quiet = false;       // probe (another dir, every second): no mapping line
};

inline double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// (Re)map when the producer appears or re-creates its swapchain (size or
// format: the next game may write the same file).
inline bool eye_map(Eye& e) {
    if (e.hdr && e.hdr->magic == VRDUAL_MAGIC && e.hdr->width == e.w && e.hdr->height == e.h &&
        e.hdr->pixfmt == e.fmt && vrdual_file_bytes(e.hdr->slot_bytes) <= e.bytes)
        return true;
    if (e.hdr) { munmap(e.hdr, e.bytes); e.hdr = nullptr; }
    int fd = open(e.name.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size >= (off_t)VRDUAL_HDR_BYTES;
    void* p = ok ? mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) return false;
    auto* h = (vrdual_shm_header*)p;
    if (h->magic != VRDUAL_MAGIC || vrdual_file_bytes(h->slot_bytes) > (uint64_t)st.st_size) { munmap(p, st.st_size); return false; }
    if (h->version != VRDUAL_VERSION) {  // other header layout: an old layer copy (Steam) or an old reader
        static uint32_t told = 0;
        if (told != h->version)
            fprintf(stderr, "[compositor] %s: frame file version %u, this build reads %u (game runs an old layer? vrdual steam-install)\n",
                    e.name.c_str(), h->version, VRDUAL_VERSION);
        told = h->version;
        munmap(p, st.st_size);
        return false;
    }
    e.hdr = h;
    e.bytes = st.st_size;
    e.ino = st.st_ino;
    e.w = h->width; e.h = h->height; e.fmt = h->pixfmt;
    e.remapped = true;
    if (!e.quiet) fprintf(stderr, "[compositor] %s eye: %ux%u on %s (pci %s)\n", e.eye ? "right" : "left", e.w, e.h, h->gpu_name, h->pci);
    return true;
}

// Copies frame F out of its slot; false if the slot no longer holds F or was torn.
inline bool eye_read(Eye& e, uint64_t F, std::vector<uint8_t>& dst) {
    const vrdual_slot& s = e.hdr->slots[F % VRDUAL_SLOTS];
    uint64_t s0 = s.seq;
    __sync_synchronize();
    if ((s0 & 1) || s.frame != F) return false;
    size_t n = size_t(e.w) * e.h * 4;
    dst.resize(n);
    memcpy(dst.data(), (const char*)e.hdr + VRDUAL_HDR_BYTES + (F % VRDUAL_SLOTS) * e.hdr->slot_bytes, n);
    __sync_synchronize();
    return s.seq == s0 && s.frame == F;
}

// Seconds since the eye's newest frame was published (inf if none). Producers
// stamp CLOCK_MONOTONIC, which is shared with Flatpak sandboxes.
inline double eye_age_s(const Eye& e) {
    uint64_t last = e.hdr ? e.hdr->last_frame : 0;
    if (!last) return 1e9;
    return now_s() - e.hdr->slots[(last - 1) % VRDUAL_SLOTS].present_ns * 1e-9;
}

// Newest frame index present in both eyes, copied into L.px / R.px.
inline bool pair_newest(Eye& L, Eye& R, uint64_t after, uint64_t* out) {
    uint64_t ll = L.hdr->last_frame, rl = R.hdr->last_frame;
    if (!ll || !rl) return false;
    uint64_t top = (ll < rl ? ll : rl) - 1;
    for (uint64_t k = 0; k < VRDUAL_SLOTS && top >= k; ++k) {
        uint64_t F = top - k;
        if (F + 1 <= after) return false;
        std::vector<uint8_t> a, b;
        if (eye_read(L, F, a) && eye_read(R, F, b)) {
            L.px.swap(a);
            R.px.swap(b);
            *out = F;
            return true;
        }
    }
    return false;
}

// The game's X11 cursor (header v4), read under its seqlock. The image is
// copied only when its serial or size changes.
struct Cursor {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0, hot_x = 0, hot_y = 0, serial = 0;
    std::vector<uint32_t> px;  // premultiplied ARGB32
};
inline bool cursor_read(const Eye& e, Cursor& c) {
    const vrdual_shm_header* h = e.hdr;
    if (!h) return false;
    for (int t = 0; t < 3; ++t) {
        uint32_t s0 = h->cursor_seq;
        if (s0 & 1) continue;
        __sync_synchronize();
        Cursor n;
        n.x = h->cursor_x; n.y = h->cursor_y;
        n.w = h->cursor_w < VRDUAL_CURSOR_MAX ? h->cursor_w : VRDUAL_CURSOR_MAX;
        n.h = h->cursor_h < VRDUAL_CURSOR_MAX ? h->cursor_h : VRDUAL_CURSOR_MAX;
        n.hot_x = h->cursor_hot_x; n.hot_y = h->cursor_hot_y; n.serial = h->cursor_serial;
        const uint32_t* src = (const uint32_t*)((const char*)h + VRDUAL_CURSOR_OFF);
        if (n.serial == c.serial && n.w == c.w && n.h == c.h) n.px = c.px;
        else n.px.assign(src, src + n.w * n.h);
        __sync_synchronize();
        if (h->cursor_seq != s0) continue;
        c = std::move(n);
        return c.w && c.h;
    }
    return false;
}

// Disparity of the scene under the cursor: the patch around (cx, cy) is found
// in both eye images (4-byte pixels, any channel order) by block matching. A
// point of the game's own view at x appears at x + D * kl in the left image
// and x + D * kr in the right one (kl, kr: the eyes' shift factors, kl - kr = 1;
// anchor center: +0.5 / -0.5). Returns false when the patch has no texture or
// the match is ambiguous; *d = D in pixels (0 = infinity, > 0 nearer).
inline bool scene_disparity(const uint8_t* L, const uint8_t* R, uint32_t w, uint32_t h, int cx, int cy, float kl, float kr, int dmax, int* d) {
    const int P = 12;  // patch half size
    if (cy - P < 0 || cy + P >= int(h)) return false;
    auto lum = [](const uint8_t* p) { return int(p[0]) + 2 * int(p[1]) + int(p[2]); };
    std::vector<std::pair<int, long>> sads;
    long sum = 0;
    for (int D = -4; D <= dmax; ++D) {
        int xl = cx + int(lroundf(D * kl)), xr = cx + int(lroundf(D * kr));
        if (xl - P < 0 || xr - P < 0 || xl + P >= int(w) || xr + P >= int(w)) continue;
        long sad = 0;
        for (int y = -P; y <= P; ++y) {
            const uint8_t* a = L + (size_t(cy + y) * w + xl - P) * 4;
            const uint8_t* b = R + (size_t(cy + y) * w + xr - P) * 4;
            for (int x = 0; x <= 2 * P; ++x) sad += std::abs(lum(a + x * 4) - lum(b + x * 4));
        }
        sads.push_back({D, sad});
        sum += sad;
    }
    if (sads.size() < 3) return false;
    long best = -1, second = -1;
    int bd = 0;
    for (auto& s : sads) if (best < 0 || s.second < best) { best = s.second; bd = s.first; }
    for (auto& s : sads) if (std::abs(s.first - bd) > 2 && (second < 0 || s.second < second)) second = s.second;
    long mean = sum / long(sads.size());
    // flat patch (everything matches) or a repeating texture (two minima)
    if (mean < long(2 * P + 1) * (2 * P + 1) * 8 || best * 2 > mean || (second >= 0 && best * 10 > second * 8)) return false;
    *d = bd;
    return true;
}

// Blends the cursor over a w x h image (BGRA or RGBA bytes), hotspot at its
// position shifted right by dx pixels.
// Stereo window at the picture's edges. An eye camera moved by dx shifts what
// it sees by -P00*dx/depth in NDC, near things most. At the edge the shifted
// view shows a strip the game never drew for its own camera (culled objects,
// pop-in, clear colour), and a near object cut by the edge is seen by one eye
// only (flicker). Masking that strip is the "floating window" of 3D cinema.
// Returns the strip for an eye camera at dx metres (signed, + = right, head
// offset included): width in pixels for things at `dist` metres, negative =
// at the left edge (camera moved left: the view shifts right).
inline int edge_band(float hfov_deg, float dx, float dist, uint32_t w) {
    if (dist <= 0 || w == 0) return 0;
    float p00 = 1.0f / tanf(hfov_deg * 0.5f * float(M_PI) / 180.0f);
    float px = p00 * fabsf(dx) / dist * float(w) / 2;
    int b = int(lroundf(std::fmin(px, float(w) / 4)));
    return dx < 0 ? -b : b;
}

// Darkens the strip: `band` columns black at the edge, then a linear fade over
// as many more. band < 0: left edge, > 0: right edge; both = also the other edge.
inline void edge_mask(uint8_t* img, uint32_t w, uint32_t h, int band, bool both) {
    int b = band < 0 ? -band : band;
    if (b == 0 || w == 0) return;
    for (int side = 0; side < 2; ++side) {
        bool left = (band < 0) != (side == 1);
        if (side == 1 && !both) break;
        for (int i = 0; i < 2 * b && i < int(w); ++i) {
            int x = left ? i : int(w) - 1 - i;
            unsigned k = i < b ? 0 : unsigned(256 * (i - b + 1) / (b + 1));  // 0 = black .. 256 = untouched
            for (uint32_t y = 0; y < h; ++y) {
                uint8_t* p = img + (size_t(y) * w + size_t(x)) * 4;
                p[0] = uint8_t(p[0] * k >> 8), p[1] = uint8_t(p[1] * k >> 8), p[2] = uint8_t(p[2] * k >> 8);
            }
        }
    }
}

inline void cursor_draw(uint8_t* img, uint32_t w, uint32_t h, bool bgra, const Cursor& c, int dx) {
    int x0 = c.x + dx - int(c.hot_x), y0 = c.y - int(c.hot_y);
    for (uint32_t y = 0; y < c.h; ++y) {
        int yy = y0 + int(y);
        if (yy < 0 || yy >= int(h)) continue;
        for (uint32_t x = 0; x < c.w; ++x) {
            int xx = x0 + int(x);
            if (xx < 0 || xx >= int(w)) continue;
            uint32_t p = c.px[y * c.w + x], a = p >> 24;
            if (!a) continue;
            uint8_t* d = img + (size_t(yy) * w + xx) * 4;
            uint32_t b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;  // premultiplied
            uint32_t s0 = bgra ? b : r, s2 = bgra ? r : b;
            d[0] = uint8_t(s0 + d[0] * (255 - a) / 255);
            d[1] = uint8_t(g + d[1] * (255 - a) / 255);
            d[2] = uint8_t(s2 + d[2] * (255 - a) / 255);
        }
    }
}

}  // namespace vrdual
