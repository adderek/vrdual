// Offline driver for layer/spirv_patch.hpp (validate patched output with spirv-val).
//   spvpatch in.spv out.spv baked A [B WF INV]
//   spvpatch in.spv out.spv live 0xADDRESS
//   ... [hud|hide] after the mode: shader rule variants
//   spvpatch --hash in.spv: the shader key (16 hex digits) the layer uses
//   spvpatch --fuzz N in.spv...: N random mutations (word flips, opcode/word
//     count edits, truncation) of each input through every patch mode; built
//     with ASan as spvpatch-asan, a read past an instruction or the buffer aborts
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../layer/spirv_patch.hpp"

static std::vector<uint32_t> load(const char* path) {
    std::vector<uint32_t> w;
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint32_t x;
    while (fread(&x, 4, 1, f) == 1) w.push_back(x);
    fclose(f);
    return w;
}

static int fuzz(long n, int nfiles, char** files) {
    uint64_t r = 0x9e3779b97f4a7c15ull;
    auto rnd = [&]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return r; };
    long patched = 0;
    for (int fi = 0; fi < nfiles; ++fi) {
        const std::vector<uint32_t> base = load(files[fi]);
        for (long it = 0; it < n; ++it) {
            std::vector<uint32_t> w = base;
            for (int m = int(rnd() % 4) + 1; m-- > 0 && w.size() > 5;) {
                size_t i = 5 + rnd() % (w.size() - 5);
                switch (rnd() % 4) {
                case 0: w[i] ^= 1u << (rnd() % 32); break;
                case 1: w[i] = (w[i] & 0xffff) | uint32_t(rnd() % 6) << 16; break;  // word count 0..5
                case 2: w[i] = uint32_t(rnd()); break;
                case 3: w.resize(i); break;
                }
            }
            // copy into an exact-size heap buffer so ASan sees reads past its end
            spv::PatchOptions o;
            switch (it % 4) {
            case 0: o.params[0] = o.params[1] = 0.03f; break;
            case 1: o.addr = 0x123456789a0ull; break;
            case 2: o.addr = 0x123456789a0ull; o.hud = true; break;
            case 3: o.hide = true; break;
            }
            std::vector<uint32_t> in(w.begin(), w.end());
            in.shrink_to_fit();
            patched += spv::patch_vertex(in, o).patched;
        }
    }
    printf("fuzz: %ld mutants per file, %ld patched, no crash\n", n, patched);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "--fuzz")) return fuzz(atol(argv[2]), argc - 3, argv + 3);
    if (argc == 3 && !strcmp(argv[1], "--hash")) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 1; }
        std::vector<uint32_t> w;
        uint32_t x;
        while (fread(&x, 4, 1, f) == 1) w.push_back(x);
        fclose(f);
        printf("%016llx\n", (unsigned long long)spv::shader_hash(w.data(), w.size()));
        return 0;
    }
    if (argc < 5) { fprintf(stderr, "usage: %s in.spv out.spv baked A [B WF INV] | live 0xADDR\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    std::vector<uint32_t> w;
    uint32_t x;
    while (fread(&x, 4, 1, f) == 1) w.push_back(x);
    fclose(f);
    spv::PatchOptions o;
    o.key = uint32_t(spv::shader_hash(w.data(), w.size()) >> 32);
    if (!strcmp(argv[argc - 1], "hud")) { o.hud = true; --argc; }
    else if (!strcmp(argv[argc - 1], "hide")) { o.hide = true; --argc; }
    if (!strcmp(argv[3], "live")) o.addr = strtoull(argv[4], nullptr, 0);
    else {
        o.params[0] = o.params[1] = strtof(argv[4], nullptr);
        for (int k = 1; k < 4 && 4 + k < argc; ++k) o.params[k] = strtof(argv[4 + k], nullptr);
    }
    auto r = spv::patch_vertex(w, o);
    if (!r.patched) { fprintf(stderr, "not patched: %s\n", r.why.c_str()); return 3; }
    f = fopen(argv[2], "wb");
    fwrite(w.data(), 4, w.size(), f);
    fclose(f);
    return 0;
}
