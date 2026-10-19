// Shared-memory frame transport between the per-eye layer instances and the
// compositor. One file per eye: /dev/shm/vrdual-<session>-<eye>.
//
// Layout: [vrdual_shm_header][slot 0 pixels][slot 1 pixels]...[slot N-1 pixels]
// Frame F is written to slot F % VRDUAL_SLOTS. Each slot has a seqlock
// (odd = being written) so the compositor never shows a torn frame, and keeps
// the frame index so the compositor can pair left/right frames with the same F.
#pragma once
#include <stdint.h>

#define VRDUAL_MAGIC 0x56524455u /* "VRDU" */
#define VRDUAL_VERSION 2u
#define VRDUAL_SLOTS 4u

enum vrdual_pixfmt {
    VRDUAL_FMT_BGRA8 = 1,
    VRDUAL_FMT_RGBA8 = 2,
};

struct vrdual_slot {
    volatile uint64_t seq;   // odd while writing
    volatile uint64_t frame; // frame index stored in this slot
    uint64_t present_ns;     // CLOCK_MONOTONIC at publish
};

struct vrdual_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width, height, stride; // stride in bytes
    uint32_t pixfmt;                // vrdual_pixfmt
    uint32_t eye;                   // 0 left, 1 right
    uint32_t pad0;
    uint64_t slot_bytes;
    volatile uint64_t last_frame;   // highest published frame (+1, 0 = none)
    struct vrdual_slot slots[VRDUAL_SLOTS];
    char gpu_name[64];
    char pci[32];
    char app[64];                   // v2: producing app (profile key: VkApplicationInfo name, else exe)
};

#define VRDUAL_HDR_BYTES 4096u

static inline uint64_t vrdual_file_bytes(uint64_t slot_bytes) {
    return VRDUAL_HDR_BYTES + slot_bytes * VRDUAL_SLOTS;
}

// Live control block: /dev/shm/vrdual-<session>-ctl. Created and seeded by the
// first layer instance from its config; the compositor and vrdual-ctl edit it;
// both layers poll `seq` once per frame and push new values to the GPU.
#define VRDUAL_CTL_MAGIC 0x56524443u /* "VRDC" */

enum vrdual_anchor { VRDUAL_ANCHOR_CENTER = 0, VRDUAL_ANCHOR_LEFT = 1, VRDUAL_ANCHOR_RIGHT = 2 };

struct vrdual_ctl {
    uint32_t magic;
    uint32_t version;
    volatile uint64_t seq;  // odd while a writer is updating
    float ipd;              // meters, far objects
    float ipd_near;         // meters, 0 = same as ipd
    float near_dist, far_dist;  // meters
    float world_scale;      // app units per meter
    float hfov;             // degrees
    int32_t anchor;         // vrdual_anchor
    uint32_t live;          // 1 if the layers can apply changes without restart
    char app[64];           // app name (profile key) the session was seeded for
    // version 2: head offset from vrdual-xr (meters, soft-clamped, in the frame
    // of the start direction: x right, y up, z forward); 0 when no HMD drives it
    float head_x, head_y, head_z;
    // version 3: HUD (w == 1 draws) shown at this distance, meters; 0 = infinity
    float hud_depth;
};

static inline const char* vrdual_anchor_name(int a) {
    return a == VRDUAL_ANCHOR_LEFT ? "left" : a == VRDUAL_ANCHOR_RIGHT ? "right" : "center";
}
static inline int vrdual_anchor_parse(const char* s) {
    return s && s[0] == 'l' ? VRDUAL_ANCHOR_LEFT : s && s[0] == 'r' ? VRDUAL_ANCHOR_RIGHT : VRDUAL_ANCHOR_CENTER;
}
