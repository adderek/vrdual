// Reading the per-eye frame files (see vrdual_shm.h) and pairing L[F] with
// R[F]. Shared by the SBS compositor and the OpenXR output.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
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
};

inline double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// (Re)map when the producer appears or re-creates its swapchain.
inline bool eye_map(Eye& e) {
    if (e.hdr && e.hdr->magic == VRDUAL_MAGIC && e.hdr->width == e.w && e.hdr->height == e.h &&
        vrdual_file_bytes(e.hdr->slot_bytes) <= e.bytes)
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
    e.hdr = h;
    e.bytes = st.st_size;
    e.ino = st.st_ino;
    e.w = h->width; e.h = h->height; e.fmt = h->pixfmt;
    e.remapped = true;
    fprintf(stderr, "[compositor] %s eye: %ux%u on %s (pci %s)\n", e.eye ? "right" : "left", e.w, e.h, h->gpu_name, h->pci);
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

}  // namespace vrdual
