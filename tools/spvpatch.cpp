// Offline driver for layer/spirv_patch.hpp (validate patched output with spirv-val).
//   spvpatch in.spv out.spv baked A [B WF INV]
//   spvpatch in.spv out.spv live 0xADDRESS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../layer/spirv_patch.hpp"

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s in.spv out.spv baked A [B WF INV] | live 0xADDR\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    std::vector<uint32_t> w;
    uint32_t x;
    while (fread(&x, 4, 1, f) == 1) w.push_back(x);
    fclose(f);
    spv::PatchOptions o;
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
