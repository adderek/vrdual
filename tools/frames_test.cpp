// vrdual-frames-test DIR: frame file reader (common/vrdual_frames.hpp) against
// a fake producer. Checks that a producer re-init with a bigger size between
// eye_map and eye_read is refused (no read past the mapping), the remap then
// reads the new size, and pair_newest reuses its buffers (no per-frame
// allocation). Exit 0 = pass.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <set>
#include <string>

#include "../common/vrdual_frames.hpp"

struct Producer {
    std::string path;
    int fd = -1;
    vrdual_shm_header* h = nullptr;
    size_t bytes = 0;
    void init(uint32_t w, uint32_t hh) {  // like the layer's shm_open_out: grow only
        uint64_t slot = uint64_t(w) * hh * 4;
        size_t b = vrdual_file_bytes(slot);
        if (fd < 0) fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (h) munmap(h, bytes);
        if (ftruncate(fd, b) != 0) { perror("ftruncate"); exit(1); }
        h = (vrdual_shm_header*)mmap(nullptr, b, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        bytes = b;
        h->magic = VRDUAL_MAGIC;
        h->version = VRDUAL_VERSION;
        h->width = w; h->height = hh; h->stride = w * 4; h->pixfmt = VRDUAL_FMT_BGRA8;
        h->slot_bytes = slot;
        h->last_frame = 0;
        for (auto& s : h->slots) { s.seq = 0; s.frame = ~0ull; }
    }
    void publish(uint64_t F) {
        auto& s = h->slots[F % VRDUAL_SLOTS];
        s.seq++;
        s.frame = F;
        memset((char*)h + VRDUAL_HDR_BYTES + (F % VRDUAL_SLOTS) * h->slot_bytes, int(F & 0xff), h->slot_bytes);
        s.present_ns = 1;
        s.seq++;
        h->last_frame = F + 1;
    }
};

static int fails = 0;
static void expect(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    Producer pl, pr;
    pl.path = std::string(argv[1]) + "/l";
    pr.path = std::string(argv[1]) + "/r";
    pl.init(64, 32);
    pr.init(64, 32);
    vrdual::Eye L{0, pl.path}, R{1, pr.path};
    L.quiet = R.quiet = true;
    expect(vrdual::eye_map(L) && vrdual::eye_map(R), "map both eyes");
    std::set<const uint8_t*> bufs;
    uint64_t F = 0, shown = 0;
    bool all = true;
    for (uint64_t f = 0; f < 10; ++f) {
        pl.publish(f);
        pr.publish(f);
        all &= vrdual::pair_newest(L, R, shown, &F) && F == f && L.px.size() == 64 * 32 * 4 && L.px[0] == f;
        shown = F + 1;
        bufs.insert(L.px.data());
    }
    expect(all, "pairs frames 0..9");
    expect(bufs.size() <= 2, "pair_newest reuses two buffers per eye (no allocation per frame)");

    // Re-init bigger behind the reader's back: slot 1 of the new layout lies
    // far past the old mapping.
    pl.init(640, 320);
    pl.publish(1);
    std::vector<uint8_t> px;
    expect(!vrdual::eye_read(L, 1, px), "eye_read refuses a frame of a re-initialised (bigger) file before the remap");
    expect(vrdual::eye_map(L) && L.w == 640 && vrdual::eye_read(L, 1, px) && px.size() == 640 * 320 * 4 && px.back() == 1,
           "after eye_map: reads the new size");
    printf("%s\n", fails ? "frames test: FAILED" : "frames test: all passed");
    return fails ? 1 : 0;
}
