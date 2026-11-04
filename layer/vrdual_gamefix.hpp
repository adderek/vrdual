// Game fixes: byte-checked patches of known game binaries, applied once when
// the layer loads its config (vkCreateInstance; the layer is in the game's
// process anyway, so no preload or file change). Each patch names the exe, a
// fixed address (non-PIE executables only) and the bytes expected there; any
// other bytes (another build, an already patched file) leave the game as it
// is. Off with game_fixes = 0. Self-contained (tools/gamefix_test.cpp).
//
// Civilization V, native Linux port (Civ5XP, 32-bit, Aspyr):
//   threads  JobManager_GetNumberThreads() returns 16 once initialised.
//            Each thread entering the TBB job system gets an index from an
//            unbounded counter, the per-thread render data has n slots (n =
//            MaxSimultaneousThreads clamped to 3..16); two threads more than n
//            enter and a render job on one reads a null slot (crash after
//            minutes). 16 is the port's own maximum (the landmark renderer
//            holds 16 slots inline), so keep MaxSimultaneousThreads <= ~12.
//   cull     FGXGetClipPlanesFromMatrix(proj, planes), the source of every
//            culling frustum (visible map region, terrain cells, shadows),
//            replaced by the same extraction from a projection whose x and y
//            are divided by cull_widen: the game submits what a cull_widen
//            times wider view (tan of the half angles) holds, while drawing
//            with its own projection. head_look = clip / scroll then has
//            something to show past the screen edge. Hooked also without
//            cull_widen (same planes then): each call counts in
//            g_scene_calls, which tells the layer when the game draws its 3D
//            view (menus, loading screens and videos call it never).
//   streams  with cull_widen: CRenderer9::CreateCommandStream(commands, n,
//            constant bytes, flags), the per-frame buffers of every render
//            system, built with commands, n and constant bytes times
//            ceil(cull_widen^2) (the culled area): the fog of war's 1 MB of
//            dynamic constants overflowed at 1.8 (the game's bounds check,
//            ud2 in FGXConstantSet::LockDynamic: SIGILL).
#pragma once
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace gamefix {

using Log = void (*)(const char* fmt, ...);

// Hex string -> bytes; returns the count (0 on a bad string).
inline size_t unhex(const char* h, uint8_t* out, size_t cap) {
    size_t n = 0;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (; h[0] && h[1] && n < cap; h += 2) {
        int a = v(h[0]), b = v(h[1]);
        if (a < 0 || b < 0) return 0;
        out[n++] = uint8_t(a << 4 | b);
    }
    return *h ? 0 : n;
}

// Writes `len` bytes at `at` (code): page made writable for the copy, then
// read + execute again.
inline bool poke(void* at, const uint8_t* bytes, size_t len) {
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = uintptr_t(at), lo = a & ~uintptr_t(pg - 1), hi = (a + len + pg - 1) & ~uintptr_t(pg - 1);
    if (mprotect((void*)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    memcpy(at, bytes, len);
    mprotect((void*)lo, hi - lo, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char*)at, (char*)at + len);
    return true;
}

enum Result { APPLIED, ALREADY, MISMATCH, FAILED, UNMAPPED };

// [at, at + len) mapped in this process (an exe of the same name but another
// layout must not fault here).
inline bool mapped(const void* at, size_t len) {
    long pg = sysconf(_SC_PAGESIZE);
    unsigned char v;
    for (uintptr_t a = uintptr_t(at) & ~uintptr_t(pg - 1); a < uintptr_t(at) + len; a += pg)
        if (mincore((void*)a, pg, &v)) return false;
    return true;
}

// Replaces `orig` at `at` with `repl` (same length or shorter: only that
// many bytes written). ALREADY when `repl` is there, MISMATCH for anything
// else.
inline Result apply(uint8_t* at, const char* orig_hex, const uint8_t* repl, size_t rlen) {
    uint8_t orig[64];
    size_t n = unhex(orig_hex, orig, sizeof orig);
    if (!n || rlen > n) return FAILED;
    if (!mapped(at, n)) return UNMAPPED;
    if (!memcmp(at, repl, rlen)) return ALREADY;
    if (memcmp(at, orig, n)) return MISMATCH;
    return poke(at, repl, rlen) ? APPLIED : FAILED;
}

// jmp rel32 from `at` to `to` (5 bytes).
inline void jmp5(uint8_t out[5], uintptr_t at, uintptr_t to) {
    out[0] = 0xe9;
    int32_t rel = int32_t(to - (at + 5));
    memcpy(out + 1, &rel, 4);
}

// FGXGetClipPlanesFromMatrix as the game does it (D3D row vectors: clip.x =
// v . column 0), with columns 0 and 1 divided by `widen`. Planes: far, near,
// right, left, bottom, top; each normalised by its xyz length.
inline void clip_planes(const float* m, float* p, float widen) {
    float k = widen > 0 ? 1 / widen : 1;
    for (int r = 0; r < 4; ++r) {
        float c0 = m[r * 4] * k, c1 = m[r * 4 + 1] * k, c2 = m[r * 4 + 2], c3 = m[r * 4 + 3];
        p[r] = c3 - c2;
        p[4 + r] = c2;
        p[8 + r] = c3 - c0;
        p[12 + r] = c0 + c3;
        p[16 + r] = c1 + c3;
        p[20 + r] = c3 - c1;
    }
    for (int i = 0; i < 6; ++i) {
        float* q = p + 4 * i;
        float s = 1.0f / sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        for (int j = 0; j < 4; ++j) q[j] *= s;
    }
}

inline float g_widen = 1;
inline std::atomic<uint32_t> g_scene_calls{0};
inline bool g_scene_hook = false;  // g_scene_calls counts (the hook is in)
inline unsigned g_stream_k = 1;
#if defined(__i386__)
// CRenderer9::CreateCommandStream as the game does it (operator new, the
// CCommandStream constructor at its fixed addresses), the sizes scaled.
extern "C" __attribute__((force_align_arg_pointer, used)) inline void* vrdual_civ5_create_stream(void*, unsigned a, unsigned b, unsigned c, unsigned d) {
    auto game_new = (void* (*)(unsigned))0x8467960;
    auto ctor = (void (*)(void*, unsigned, unsigned, unsigned, unsigned))0x8cf1af0;
    void* p = game_new(0x1058);
    ctor(p, a * g_stream_k, b * g_stream_k, c * g_stream_k, d);
    return p;
}
// Called by the game in place of FGXGetClipPlanesFromMatrix (cdecl, same
// arguments). The game's stack may be only 4-byte aligned.
extern "C" __attribute__((force_align_arg_pointer, used)) inline void vrdual_civ5_clip_planes(const float* m, float* p) {
    g_scene_calls.fetch_add(1, std::memory_order_relaxed);
    clip_planes(m, p, g_widen);
}
#endif

inline const char* result_str(Result r) {
    return r == APPLIED ? "applied" : r == ALREADY ? "already in the binary" : r == MISMATCH ? "unknown build, not applied" : r == UNMAPPED ? "address not mapped (unknown build), not applied" : "mprotect failed";
}

// All fixes for this process (exe = basename of /proc/self/exe).
inline void run(const std::string& exe, float cull_widen, Log log) {
#if defined(__i386__)
    if (exe != "Civ5XP") return;
    static const uint8_t threads[] = {0xa1, 0x08, 0x14, 0x0a, 0x10, 0x85, 0xc0, 0x74, 0x05, 0xb8, 0x10, 0, 0, 0, 0xc3, 0x90, 0x90, 0x90, 0x90};
    Result r = apply((uint8_t*)0x8dc282b, "e8000000005881c0d04754018b8008a4d905c3", threads, sizeof threads);
    log("game fix Civ V threads (per-thread render data for 16 threads): %s", result_str(r));
    g_widen = cull_widen > 1 ? cull_widen : 1.0f;
    uint8_t j[5];
    jmp5(j, 0x8d017a0, uintptr_t(&vrdual_civ5_clip_planes));
    r = apply((uint8_t*)0x8d017a0, "53575683ec10e8000000005b81c3555860018b7424248b442420", j, 5);
    g_scene_hook = r == APPLIED || r == ALREADY;
    log("game fix Civ V culling frustum (cull_widen %.2f; 3D view frames counted for the head turn): %s", g_widen, result_str(r));
    if (cull_widen > 1) {
        g_stream_k = unsigned(ceilf(cull_widen * cull_widen - 1e-3f));
        jmp5(j, 0x8d1052c, uintptr_t(&vrdual_civ5_create_stream));
        r = apply((uint8_t*)0x8d1052c, "5553575683ec1ce8000000005b81c3c86a5f01", j, 5);
        log("game fix Civ V render streams x%u for cull_widen (per-frame command / constant buffers): %s", g_stream_k, result_str(r));
    }
#else
    (void)exe; (void)cull_widen; (void)log;
#endif
}

}  // namespace gamefix
