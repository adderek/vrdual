// vrdual-gamefix-test: layer/vrdual_gamefix.hpp without a game. The culling
// planes against a known projection (plain and widened), the byte-checked
// patch on a page of our own (applied / already / unknown build / unmapped),
// and in the 32-bit build the jmp into the hook the way the game calls it.
// Exit 0 = all good.
#include <sys/mman.h>

#include <cstdio>

#include "../layer/vrdual_gamefix.hpp"

static int fails = 0;
static void expect(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}
static void nolog(const char*, ...) {}

// D3D left-handed perspective (D3DXMatrixPerspectiveFovLH), row vectors.
static void persp(float* m, float fovy, float aspect, float zn, float zf) {
    float ys = 1 / tanf(fovy / 2), xs = ys / aspect;
    float r[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    memcpy(m, r, sizeof r);
}
static float dist(const float* pl, float x, float y, float z) { return pl[0] * x + pl[1] * y + pl[2] * z + pl[3]; }
static bool inside(const float* p, float x, float y, float z) {
    for (int i = 0; i < 6; ++i)
        if (dist(p + 4 * i, x, y, z) < -1e-4f) return false;
    return true;
}

int main() {
    float m[16], p[24], w[24];
    const float fovy = 1.0f, aspect = 16.f / 9, z = 10;
    persp(m, fovy, aspect, 1, 100);
    float ty = tanf(fovy / 2), tx = ty * aspect;  // half extents at depth 1
    gamefix::clip_planes(m, p, 1);
    expect(fabsf(dist(p + 8, tx * z, 0, z)) < 1e-4f && fabsf(dist(p + 12, -tx * z, 0, z)) < 1e-4f && fabsf(dist(p + 20, 0, ty * z, z)) < 1e-4f,
           "planes: right / left / top edge of the view lie on their planes");
    expect(inside(p, 0, 0, z) && !inside(p, 1.5f * tx * z, 0, z) && !inside(p, 0, -1.5f * ty * z, z) && !inside(p, 0, 0, 0.5f) && !inside(p, 0, 0, 200),
           "planes: centre inside; 1.5x past the side, below the bottom, before near, past far outside");
    gamefix::clip_planes(m, w, 2);
    expect(inside(w, 1.9f * tx * z, 0, z) && inside(w, 0, -1.9f * ty * z, z) && !inside(w, 2.1f * tx * z, 0, z) && fabsf(dist(w, 0, 0, 100)) < 1e-3f && fabsf(dist(w + 4, 0, 0, 1)) < 1e-4f,
           "cull_widen 2: twice the half extents inside, near / far unchanged");

    long pg = sysconf(_SC_PAGESIZE);
    auto* page = (uint8_t*)mmap(nullptr, pg, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const char* orig = "53575683ec10e8000000005b81c3";
    uint8_t ob[16], repl[5] = {0xe9, 1, 2, 3, 4};
    size_t n = gamefix::unhex(orig, ob, sizeof ob);
    memcpy(page, ob, n);
    auto r1 = gamefix::apply(page, orig, repl, 5);
    bool body = !memcmp(page, repl, 5) && !memcmp(page + 5, ob + 5, n - 5);
    auto r2 = gamefix::apply(page, orig, repl, 5);
    uint8_t other[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    auto r3 = gamefix::apply(page, orig, other, 5);
    expect(r1 == gamefix::APPLIED && body && r2 == gamefix::ALREADY && r3 == gamefix::MISMATCH,
           "patch: applied over the expected bytes only, again = already there, other bytes = unknown build");
    void* hole = mmap(nullptr, pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(hole, pg);
    expect(gamefix::apply((uint8_t*)hole, orig, repl, 5) == gamefix::UNMAPPED && gamefix::unhex("4g", ob, 16) == 0,
           "patch: unmapped address refused without a fault; bad hex refused");
    gamefix::run("not-a-game", 2, nolog);  // must not touch anything

#if defined(__i386__)
    mprotect(page, pg, PROT_READ | PROT_WRITE | PROT_EXEC);  // apply() left it read + execute
    memcpy(page + 64, ob, n);  // a fresh "game function" at page + 64
    uint8_t j[5];
    gamefix::jmp5(j, uintptr_t(page + 64), uintptr_t(&gamefix::vrdual_civ5_clip_planes));
    gamefix::g_widen = 2;
    auto r4 = gamefix::apply(page + 64, orig, j, 5);
    float h[24] = {};
    // called like the game: cdecl, stack misaligned by 4 on purpose
    asm volatile("sub $4, %%esp\n\tpush %2\n\tpush %1\n\tcall *%0\n\tadd $12, %%esp" : : "r"(page + 64), "r"(m), "r"(h) : "eax", "ecx", "edx", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
    uint32_t c0 = gamefix::g_scene_calls.load();
    asm volatile("sub $4, %%esp\n\tpush %2\n\tpush %1\n\tcall *%0\n\tadd $12, %%esp" : : "r"(page + 64), "r"(m), "r"(h) : "eax", "ecx", "edx", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
    expect(r4 == gamefix::APPLIED && !memcmp(h, w, sizeof h) && gamefix::g_scene_calls.load() == c0 + 1,
           "32-bit: jmp into the hook, called with a misaligned stack, gives the widened planes and counts a 3D view frame");
#endif
    return fails ? 1 : 0;
}
