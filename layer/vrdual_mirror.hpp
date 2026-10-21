// Mirror mode (VRDUAL_EYE=both): one app process, two GPUs.
//
// The app sees only device A (left eye). At vkCreateDevice the layer opens the
// same Vulkan driver (ICD) directly — bypassing the loader, so nothing in the
// app or the layer chain ever sees it — and creates twin device B on the
// right-eye GPU. Every device-level call is then executed on A as usual and
// replayed on B with all handles translated to their B twins
// (mirror_gen.hpp, generated from vk.xml). Both GPUs are the same model with
// the same driver, so memory types, alignments and formats are identical and
// sizes/offsets carry over unchanged.
//
// What needs care (hand-written here):
//   * host-visible memory: the app writes through a mapping of A; at every
//     queue submit the bytes it changed are copied to B (shadow + diff, below);
//   * sync: whenever the app learns that A finished something (fence, timeline
//     value), B is waited on too, so B never reads memory the app reuses;
//   * WSI: B has no swapchain. Swapchain images get plain B images as twins,
//     acquire signals B's twin semaphore/fence with an empty submit, present
//     copies the B image into the eye-1 frame file;
//   * shaders: modules are created separately on A and B, each patched with
//     its own eye's parameter buffer.
// If B cannot follow (failed call, untranslatable command) the mirror stops and
// the app keeps running on A; the right eye freezes, with a log line.
namespace mirror {

// ------------------------------------------------------------ handle map ----
static std::shared_mutex& g_hlock = *new std::shared_mutex;
static std::unordered_map<uint64_t, uint64_t>& g_hmap = *new std::unordered_map<uint64_t, uint64_t>;  // A handle -> B handle

template <typename T> static uint64_t h2u(T h) {
    if constexpr (std::is_pointer_v<T>) return (uint64_t)(uintptr_t)h;
    else return (uint64_t)h;
}
template <typename T> static T u2h(uint64_t u) {
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)u;
    else return (T)u;
}

static void log_miss(uint64_t h) {
    static std::atomic<int> n{0};
    int k = n++;
    if (k < 20) logf("mirror: no twin for handle 0x%llx%s", (unsigned long long)h, k == 19 ? " (further misses not logged)" : "");
}

template <typename T> static T xl_handle(T a) {
    if (!a) return a;
    uint64_t k = h2u(a);
    {
        std::shared_lock<std::shared_mutex> l(g_hlock);
        auto it = g_hmap.find(k);
        if (it != g_hmap.end()) return u2h<T>(it->second);
    }
    log_miss(k);
    return T{};
}

template <typename T> static void record_handle(T a, T b) {
    if (!a) return;
    std::unique_lock<std::shared_mutex> l(g_hlock);
    g_hmap[h2u(a)] = h2u(b);
}

template <typename T> static void forget_handle(T a) {
    if (!a) return;
    std::unique_lock<std::shared_mutex> l(g_hlock);
    g_hmap.erase(h2u(a));
}

// ---------------------------------------------------------------- device ----
// Host-visible allocation. Non-cached types (write-combined/BAR, written by
// the app, never read) are mapped through a CPU shadow: the app writes the
// shadow, submit copies changed words to A and B. Cached types are read back
// by the app, so it gets A's real mapping and only the changes go to B.
struct Mem {
    uint64_t bmem = 0;
    VkDeviceSize size = 0;
    bool cached = false, coherent = true;  // cached: the app gets A's own mapping (no shadow)
    bool readback = false;                 // HOST_CACHED type: the app reads GPU results from it
    bool mapped = false;
    char* amap = nullptr;    // null while A is mapped only during pushes (transient)
    char* bmap = nullptr;    // same for B
    bool a_released = false; // transient: A unmapped right after the app's vkMapMemory
    VkDeviceSize off = 0, len = 0;
    char* shadow = nullptr;  // what the app maps (not for `cached`)
    char* last = nullptr;    // what A and B hold, as last pushed
    size_t cap = 0;          // bytes mmap'd for shadow/last
    VkDeviceSize cap_off = 0;
    bool tracked = false;    // shadow registered for dirty tracking
    int wp = -1;             // cached: write-tracking slot
    bool synced = false;     // tracked and fully pushed once: scans list every later write
};

// GPU writes into host-visible memory (copies, fills, query results): byte
// range of an A allocation; end may lie past it (copy to the end of a buffer).
struct MemRange { uint64_t mem; VkDeviceSize off, end; };

// A submit whose command buffers write host-visible memory: those ranges are
// copied from A into the app's shadows once the app can know it finished.
struct Writeback {
    VkFence fence = VK_NULL_HANDLE;  // the app's, or ours when it passed none
    bool ours = false;
    std::vector<std::pair<VkSemaphore, uint64_t>> sig;  // values the submit signals
    std::vector<MemRange> r;
};

struct Template {
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    size_t size = 0;  // bytes of pData read by an update
};

struct MDev {
    Dispatch a, b;
    DeviceData* dd = nullptr;
    VkDevice bdev = VK_NULL_HANDLE;
    VkInstance binst = VK_NULL_HANDLE;
    VkPhysicalDevice bphys = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr icd_gipa = nullptr;
    VkPhysicalDeviceMemoryProperties amp{}, bmp{};
    std::string bname, bpci;
    std::atomic<bool> live{true};
    bool replay = false;  // device addresses: captured on A, replayed on B (same values)
    EyeBuf bep;
    CopyRes bcr;
    VkQueue bqueue_any = VK_NULL_HANDLE;
    std::mutex qlock;  // our own extra submits on B vs the app's
    std::mutex lock;   // tables below
    std::unordered_map<uint64_t, Mem> mems;
    std::unordered_map<uint64_t, Template> templates;
    std::unordered_set<VkCommandBuffer> secondary;
    std::unordered_set<uint64_t> pending;  // fences with a B submit not yet waited for
    uint64_t pushed_bytes = 0, push_ns = 0, pushes = 0, diffed_bytes = 0;
    // where a push spends its time (per 600 pushes, logged with the memory sync line)
    uint64_t full_bytes = 0, full_cached_bytes = 0, full_ns = 0, scan_ns = 0, map_ns = 0;
    std::vector<std::pair<size_t, size_t>> ranges;  // push_one scratch (offset, length)
    // write-back (GPU results into the shadows the app reads)
    struct BufBind { uint64_t mem = 0; VkDeviceSize off = 0, size = 0; };
    std::unordered_map<uint64_t, BufBind> bufs;                    // A buffer -> memory binding
    std::unordered_map<uint64_t, VkFormat> imgs;                   // A image -> format (copy extents)
    std::unordered_map<VkCommandBuffer, std::vector<MemRange>> cbw;  // ranges a command buffer writes
    std::vector<Writeback> wbs;                                    // submitted, not yet copied back
    std::vector<VkFence> fence_pool;                               // our fences for submits without one
    uint64_t wb_bytes = 0;
};

static MDev* mdev_of(void* k) {
    DeviceData* dd = dev_of(k);
    return dd ? dd->md : nullptr;
}

static void mirror_fail(MDev* md, const char* why) {
    if (md->live.exchange(false)) logf("mirror STOPPED: %s — the right eye freezes, the app continues on the left GPU", why);
}

// Per-call scratch: arena for translated structs + translation status.
struct Ctx {
    MDev* md = nullptr;
    bool bad = false;
    std::vector<std::pair<char*, size_t>> blocks;
    size_t used = 0;

    template <typename T> T* alloc(size_t n) {
        size_t sz = (sizeof(T) * (n ? n : 1) + 15) & ~size_t(15);
        if (blocks.empty() || used + sz > blocks.back().second) {
            size_t cap = std::max<size_t>(sz, 256u << 10);
            blocks.push_back({(char*)malloc(cap), cap});
            used = 0;
        }
        T* p = (T*)(blocks.back().first + used);
        used += sz;
        memset((void*)p, 0, sz);
        return p;
    }
    template <typename T> T xl(T a) { return xl_handle(a); }
    template <typename T> void record(T a, T b) { record_handle(a, b); }
    bool ok() const { return !bad; }
    void fail(const char* why) {
        bad = true;
        logf("mirror: %s", why);
    }
    void unknown_stype(VkStructureType st) {
        static std::mutex m;
        static std::unordered_set<int> seen;
        std::lock_guard<std::mutex> l(m);
        if (seen.insert(int(st)).second) logf("mirror: pNext sType %d not translated (passed through)", int(st));
    }
};

// VRDUAL_MIRROR_TRACE=N: log the first N mirrored calls (debugging aid).
static long g_trace = atol(env("VRDUAL_MIRROR_TRACE", "0"));
static void trace_call(const char* name) {
    static std::atomic<long> n{0};
    if (n++ < g_trace) logf("call %s", name);
}

static thread_local Ctx t_ctx;
static Ctx& ctx_begin(MDev* md) {
    t_ctx.md = md;
    t_ctx.bad = false;
    return t_ctx;
}
static void ctx_end(Ctx& c) {
    while (c.blocks.size() > 1) {
        free(c.blocks.front().first);
        c.blocks.erase(c.blocks.begin());
    }
    c.used = 0;
}

// ------------------------------------------------------------ write-back ----
// The app maps host-visible memory through a shadow and reads it there, so
// what A's GPU writes into such memory (readback copies, fills, query
// results) must be copied into the shadow before the app can look: the
// commands are recorded per command buffer (hooks called by the generated
// wrappers, also after a mirror stop), a submit keeps their ranges with a
// fence, and the sync calls through which the app learns of completion copy
// them back first. Shader stores into host memory are not tracked (DXVK reads
// back with copies).
static constexpr VkDeviceSize kToEnd = ~VkDeviceSize(0);

static void wb_note(MDev* md, VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize end) {
    std::lock_guard<std::mutex> l(md->lock);
    auto b = md->bufs.find(h2u(buf));
    if (b == md->bufs.end() || !b->second.mem || !md->mems.count(b->second.mem)) return;  // not host-visible
    VkDeviceSize o = b->second.off;
    md->cbw[cb].push_back({b->second.mem, o + off, end == kToEnd ? o + b->second.size : o + end});
}

static void hook_CmdCopyBuffer(MDev* md, VkCommandBuffer cb, VkBuffer, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    for (uint32_t i = 0; i < n; ++i) wb_note(md, cb, dst, r[i].dstOffset, r[i].dstOffset + r[i].size);
}
static void hook_CmdCopyBuffer2(MDev* md, VkCommandBuffer cb, const VkCopyBufferInfo2* ci) {
    for (uint32_t i = 0; i < ci->regionCount; ++i)
        wb_note(md, cb, ci->dstBuffer, ci->pRegions[i].dstOffset, ci->pRegions[i].dstOffset + ci->pRegions[i].size);
}
// Texel block of a format as copied to a buffer: width, height, bytes (for
// the given aspect). false = not known here: the copy then counts to the
// buffer's end.
static bool fmt_block(VkFormat f, VkImageAspectFlags aspect, uint32_t& bw, uint32_t& bh, uint32_t& bytes) {
    bw = bh = 1;
    if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) return bytes = 1, true;
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_S8_UINT:
        return bytes = 1, true;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
        return bytes = 2, true;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return bytes = 4, true;  // depth aspects of D24/D32 formats copy as 4 bytes
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
        return bytes = 8, true;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return bytes = 16, true;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
        return bw = bh = 4, bytes = 8, true;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
        return bw = bh = 4, bytes = 16, true;
    default:
        return false;
    }
}

// Byte range [off, end) an image -> buffer copy region writes, kToEnd if the
// format is unknown here.
static void copy_extent(VkFormat f, const VkBufferImageCopy& r, VkDeviceSize& off, VkDeviceSize& end) {
    off = r.bufferOffset;
    uint32_t bw, bh, bytes;
    if (!fmt_block(f, r.imageSubresource.aspectMask, bw, bh, bytes)) { end = kToEnd; return; }
    VkDeviceSize row = (VkDeviceSize(r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width) + bw - 1) / bw;
    VkDeviceSize rows = (VkDeviceSize(r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height) + bh - 1) / bh;
    VkDeviceSize slices = VkDeviceSize(std::max(1u, r.imageExtent.depth)) * std::max(1u, r.imageSubresource.layerCount);
    VkDeviceSize w = (r.imageExtent.width + bw - 1) / bw, h = (r.imageExtent.height + bh - 1) / bh;
    if (!w || !h) { end = off; return; }
    end = off + bytes * ((slices - 1) * row * rows + (h - 1) * row + w);
}

static VkFormat img_format(MDev* md, VkImage img) {
    std::lock_guard<std::mutex> l(md->lock);
    auto it = md->imgs.find(h2u(img));
    return it == md->imgs.end() ? VK_FORMAT_UNDEFINED : it->second;
}
static void hook_after_CreateImage(MDev* md, VkDevice, const VkImageCreateInfo* ci, const VkAllocationCallbacks*, VkImage* img, VkResult r) {
    if (r < 0) return;
    std::lock_guard<std::mutex> l(md->lock);
    md->imgs[h2u(*img)] = ci->format;
}
static void hook_DestroyImage(MDev* md, VkDevice, VkImage img, const VkAllocationCallbacks*) {
    std::lock_guard<std::mutex> l(md->lock);
    md->imgs.erase(h2u(img));
}
static void hook_CmdCopyImageToBuffer(MDev* md, VkCommandBuffer cb, VkImage img, VkImageLayout, VkBuffer dst, uint32_t n, const VkBufferImageCopy* r) {
    VkFormat f = img_format(md, img);
    for (uint32_t i = 0; i < n; ++i) {
        VkDeviceSize o, e;
        copy_extent(f, r[i], o, e);
        wb_note(md, cb, dst, o, e);
    }
}
static void hook_CmdCopyImageToBuffer2(MDev* md, VkCommandBuffer cb, const VkCopyImageToBufferInfo2* ci) {
    VkFormat f = img_format(md, ci->srcImage);
    for (uint32_t i = 0; i < ci->regionCount; ++i) {
        const VkBufferImageCopy2& r2 = ci->pRegions[i];
        VkBufferImageCopy r{r2.bufferOffset, r2.bufferRowLength, r2.bufferImageHeight, r2.imageSubresource, r2.imageOffset, r2.imageExtent};
        VkDeviceSize o, e;
        copy_extent(f, r, o, e);
        wb_note(md, cb, ci->dstBuffer, o, e);
    }
}
static void hook_CmdFillBuffer(MDev* md, VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t) {
    wb_note(md, cb, dst, off, size == VK_WHOLE_SIZE ? kToEnd : off + size);
}
static void hook_CmdUpdateBuffer(MDev* md, VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void*) {
    wb_note(md, cb, dst, off, off + size);
}
// stride * count covers every result (stride >= one result's size)
static void hook_CmdCopyQueryPoolResults(MDev* md, VkCommandBuffer cb, VkQueryPool, uint32_t, uint32_t count, VkBuffer dst, VkDeviceSize off,
                                         VkDeviceSize stride, VkQueryResultFlags flags) {
    VkDeviceSize one = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
    wb_note(md, cb, dst, off, stride && count ? off + stride * (count - 1) + std::max(stride, one) : kToEnd);
}
static void hook_CmdExecuteCommands(MDev* md, VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* sec) {
    std::lock_guard<std::mutex> l(md->lock);
    auto& to = md->cbw[cb];
    for (uint32_t i = 0; i < n; ++i) {
        auto it = md->cbw.find(sec[i]);
        if (it != md->cbw.end()) to.insert(to.end(), it->second.begin(), it->second.end());
    }
}
static void hook_BindBufferMemory(MDev* md, VkDevice, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
    std::lock_guard<std::mutex> l(md->lock);
    auto& b = md->bufs[h2u(buf)];
    b.mem = h2u(mem), b.off = off;
}
static void hook_BindBufferMemory2(MDev* md, VkDevice dev, uint32_t n, const VkBindBufferMemoryInfo* bi) {
    for (uint32_t i = 0; i < n; ++i) hook_BindBufferMemory(md, dev, bi[i].buffer, bi[i].memory, bi[i].memoryOffset);
}
static void hook_DestroyBuffer(MDev* md, VkDevice, VkBuffer buf, const VkAllocationCallbacks*) {
    std::lock_guard<std::mutex> l(md->lock);
    md->bufs.erase(h2u(buf));
}

// The ranges a submit's command buffers write (md->lock held by the caller).
static void wb_collect(MDev* md, VkCommandBuffer cb, Writeback& w) {
    auto it = md->cbw.find(cb);
    if (it != md->cbw.end()) w.r.insert(w.r.end(), it->second.begin(), it->second.end());
}

static VkFence wb_fence(MDev* md) {
    std::lock_guard<std::mutex> l(md->lock);
    if (!md->fence_pool.empty()) {
        VkFence f = md->fence_pool.back();
        md->fence_pool.pop_back();
        return f;
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    md->a.CreateFence(md->dd->device, &fi, nullptr, &f);
    return f;
}

// Copies one finished range from A into the shadow and into `last` (so the
// next push does not send A's GPU result to B as a CPU change: B's GPU wrote
// its own). md->lock held.
static void wb_apply(MDev* md, const MemRange& r) {
    auto it = md->mems.find(r.mem);
    if (it == md->mems.end()) return;
    Mem& m = it->second;
    if (!m.mapped || !m.shadow) return;  // unmapped: copied at the next map (readback types); cached: A's own mapping
    VkDeviceSize b = std::max(r.off, m.off), e = std::min(r.end, m.off + m.len);
    if (b >= e) return;
    VkDevice dev = md->dd->device;
    char* a = m.amap;
    bool tmp = !a;
    if (tmp && md->a.MapMemory(dev, u2h<VkDeviceMemory>(r.mem), m.off, m.len, 0, (void**)&a) < 0) return;
    if (!m.coherent) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(r.mem), m.off, VK_WHOLE_SIZE};
        md->a.InvalidateMappedMemoryRanges(dev, 1, &mr);
    }
    size_t o = size_t(b - m.off), n = size_t(e - b);
    memcpy(m.shadow + o, a + o, n);
    memcpy(m.last + o, a + o, n);
    md->wb_bytes += n;
    if (tmp) md->a.UnmapMemory(dev, u2h<VkDeviceMemory>(r.mem));
}

// Copies back every finished submit. sem/val: the app saw that timeline
// value, so submits signalling it are done and their fence signals with them
// (waited for briefly).
static void wb_poll(MDev* md, VkSemaphore sem = VK_NULL_HANDLE, uint64_t val = 0) {
    VkDevice dev = md->dd->device;
    std::lock_guard<std::mutex> l(md->lock);
    for (size_t i = 0; i < md->wbs.size();) {
        Writeback& w = md->wbs[i];
        VkResult s = md->a.GetFenceStatus(dev, w.fence);
        if (s == VK_NOT_READY && sem)
            for (auto& p : w.sig)
                if (p.first == sem && p.second <= val) {
                    s = md->a.WaitForFences(dev, 1, &w.fence, VK_TRUE, 1000000000ull);
                    break;
                }
        if (s == VK_NOT_READY || s == VK_TIMEOUT) {
            ++i;
            continue;
        }
        if (s == VK_SUCCESS)
            for (auto& r : w.r) wb_apply(md, r);
        if (w.ours) {
            md->a.ResetFences(dev, 1, &w.fence);
            md->fence_pool.push_back(w.fence);
        }
        md->wbs[i] = std::move(md->wbs.back());
        md->wbs.pop_back();
    }
}

static void wb_semaphores(MDev* md, const VkSemaphoreWaitInfo* wi) {
    for (uint32_t i = 0; i < wi->semaphoreCount; ++i) wb_poll(md, wi->pSemaphores[i], wi->pValues[i]);
}
static void hook_after_QueueWaitIdle(MDev* md, VkQueue, VkResult r) {
    if (r == VK_SUCCESS) wb_poll(md);
}
static void hook_after_DeviceWaitIdle(MDev* md, VkDevice, VkResult r) {
    if (r == VK_SUCCESS) wb_poll(md);
}
// An app fence going away (reset, destroyed): copy back what it covered first.
static void wb_drop_fence(MDev* md, VkFence f) {
    if (!f) return;
    wb_poll(md);
    std::lock_guard<std::mutex> l(md->lock);
    for (size_t i = 0; i < md->wbs.size();)
        if (md->wbs[i].fence == f && !md->wbs[i].ours) {
            md->wbs[i] = std::move(md->wbs.back());
            md->wbs.pop_back();
        } else {
            ++i;
        }
}
static void hook_DestroyFence(MDev* md, VkDevice, VkFence f, const VkAllocationCallbacks*) { wb_drop_fence(md, f); }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "mirror_gen.hpp"
#pragma GCC diagnostic pop

// ------------------------------------------------ hand-written translators --
static bool is_image_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static bool is_buffer_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
static bool is_texel_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}
static bool has_sampler(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }

static VkDescriptorImageInfo xl_image_info(Ctx& c, VkDescriptorType t, VkDescriptorImageInfo ii) {
    ii.sampler = has_sampler(t) ? c.xl(ii.sampler) : VK_NULL_HANDLE;
    ii.imageView = t != VK_DESCRIPTOR_TYPE_SAMPLER ? c.xl(ii.imageView) : VK_NULL_HANDLE;
    return ii;
}

// Only the array matching descriptorType is valid; the others may be garbage.
static void tr_VkWriteDescriptorSet(Ctx& c, const VkWriteDescriptorSet* in, VkWriteDescriptorSet* out) {
    *out = *in;
    out->pNext = tr_chain(c, in->pNext);
    out->dstSet = c.xl(in->dstSet);
    out->pImageInfo = nullptr;
    out->pBufferInfo = nullptr;
    out->pTexelBufferView = nullptr;
    uint32_t n = in->descriptorCount;
    if (is_image_desc(in->descriptorType) && in->pImageInfo) {
        auto* t = c.alloc<VkDescriptorImageInfo>(n);
        for (uint32_t i = 0; i < n; ++i) t[i] = xl_image_info(c, in->descriptorType, in->pImageInfo[i]);
        out->pImageInfo = t;
    } else if (is_buffer_desc(in->descriptorType) && in->pBufferInfo) {
        auto* t = c.alloc<VkDescriptorBufferInfo>(n);
        for (uint32_t i = 0; i < n; ++i) { t[i] = in->pBufferInfo[i]; t[i].buffer = c.xl(t[i].buffer); }
        out->pBufferInfo = t;
    } else if (is_texel_desc(in->descriptorType) && in->pTexelBufferView) {
        auto* t = c.alloc<VkBufferView>(n);
        for (uint32_t i = 0; i < n; ++i) t[i] = c.xl(in->pTexelBufferView[i]);
        out->pTexelBufferView = t;
    }
}

static void tr_VkFramebufferCreateInfo(Ctx& c, const VkFramebufferCreateInfo* in, VkFramebufferCreateInfo* out) {
    *out = *in;
    out->pNext = tr_chain(c, in->pNext);
    out->renderPass = c.xl(in->renderPass);
    if (!(in->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && in->pAttachments) {
        auto* t = c.alloc<VkImageView>(in->attachmentCount);
        for (uint32_t i = 0; i < in->attachmentCount; ++i) t[i] = c.xl(in->pAttachments[i]);
        out->pAttachments = t;
    }
}

static void tr_VkDescriptorSetLayoutBinding(Ctx& c, const VkDescriptorSetLayoutBinding* in, VkDescriptorSetLayoutBinding* out) {
    *out = *in;
    if (has_sampler(in->descriptorType) && in->pImmutableSamplers) {
        auto* t = c.alloc<VkSampler>(in->descriptorCount);
        for (uint32_t i = 0; i < in->descriptorCount; ++i) t[i] = c.xl(in->pImmutableSamplers[i]);
        out->pImmutableSamplers = t;
    } else {
        out->pImmutableSamplers = nullptr;
    }
}

// Inline module (pipeline stage pNext): patched for the right eye on B.
static void tr_VkShaderModuleCreateInfo(Ctx& c, const VkShaderModuleCreateInfo* in, VkShaderModuleCreateInfo* out) {
    *out = *in;
    out->pNext = tr_chain(c, in->pNext);
    std::vector<uint32_t> code;
    if (spv_patch_for(c.md->bep, in, code)) {
        uint32_t* w = c.alloc<uint32_t>(code.size());
        memcpy(w, code.data(), code.size() * 4);
        out->pCode = w;
        out->codeSize = code.size() * 4;
    }
}

static void tr_gpci(Ctx& c, const VkGraphicsPipelineCreateInfo* in, VkGraphicsPipelineCreateInfo* out) {
    *out = *in;
    // A may have hit its pipeline cache; B has to compile instead of failing.
    out->flags &= ~(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT | VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    out->pNext = tr_chain(c, in->pNext);
    if (in->pStages && in->stageCount) {
        auto* t = c.alloc<VkPipelineShaderStageCreateInfo>(in->stageCount);
        for (uint32_t i = 0; i < in->stageCount; ++i) tr_VkPipelineShaderStageCreateInfo(c, &in->pStages[i], &t[i]);
        out->pStages = t;
    }
    out->layout = c.xl(in->layout);
    out->renderPass = c.xl(in->renderPass);
    out->basePipelineHandle = c.xl(in->basePipelineHandle);
}

// ------------------------------------------------------------- memory -------
static bool word_diff_copy(const char* src, char* last, size_t len, char* d0, char* d1) {
    bool any = false;
    for (size_t p = 0; p < len; p += 4096) {
        size_t n = std::min<size_t>(4096, len - p);
        if (!memcmp(src + p, last + p, n)) continue;
        any = true;
        size_t i = p, end = p + n;
        for (; i + 8 <= end; i += 8) {
            uint64_t v, l;
            memcpy(&v, src + i, 8);  // read once: app threads may be writing
            memcpy(&l, last + i, 8);
            if (v == l) continue;
            memcpy(last + i, &v, 8);
            if (d0) memcpy(d0 + i, &v, 8);
            if (d1) memcpy(d1 + i, &v, 8);
        }
        for (; i < end; ++i) {
            char v = src[i];
            if (v == last[i]) continue;
            last[i] = v;
            if (d0) d0[i] = v;
            if (d1) d1[i] = v;
        }
    }
    return any;
}

// Offset of the first 4 KB page that differs, or SIZE_MAX.
static size_t first_change(const char* src, const char* last, size_t len) {
    for (size_t p = 0; p < len; p += 4096)
        if (memcmp(src + p, last + p, std::min<size_t>(4096, len - p))) return p;
    return SIZE_MAX;
}

// Dirty tracking for shadows: userfaultfd async write-protect + PAGEMAP_SCAN
// (Linux 6.7). One scan returns the pages written since the previous one and
// write-protects them again, so a push diffs only those instead of every
// mapped byte (DXVK games: ~26 ms/submit over 336 MB). The kernel resolves the
// write faults itself: no signal handler, no thread. Off (full diff) when the
// kernel or a sandbox refuses; VRDUAL_DIRTY=0 forces it off.
struct DirtyTrack { int uffd = -1, pagemap = -1; };
static const DirtyTrack& dirty_track() {
    static const DirtyTrack d = [] {
        DirtyTrack t;
        if (!strcmp(env("VRDUAL_DIRTY", "1"), "0")) { logf("mirror: dirty tracking off (VRDUAL_DIRTY=0)"); return t; }
        int u = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        uffdio_api api{UFFD_API, UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED, 0};
        int pm = u < 0 ? -1 : open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        const char* why = u < 0 ? "userfaultfd" : ioctl(u, UFFDIO_API, &api) < 0 ? "UFFDIO_API" : pm < 0 ? "/proc/self/pagemap" : nullptr;
        if (why) {
            logf("mirror: dirty tracking off (%s: %s), full memory diff", why, strerror(errno));
            if (u >= 0) close(u);
            if (pm >= 0) close(pm);
            return t;
        }
        t.uffd = u, t.pagemap = pm;
        logf("mirror: dirty tracking on (userfaultfd write-protect)");
        return t;
    }();
    return d;
}

static bool dirty_register(char* p, size_t n) {
    const DirtyTrack& d = dirty_track();
    if (d.uffd < 0) return false;
    uffdio_register r{{uint64_t(uintptr_t(p)), n}, UFFDIO_REGISTER_MODE_WP, 0};
    if (ioctl(d.uffd, UFFDIO_REGISTER, &r) == 0) return true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) logf("mirror: UFFDIO_REGISTER failed (%s), those shadows use the full diff", strerror(errno));
    return false;
}

// Appends the written ranges of [p, p + n) as (offset, length) and
// write-protects them again. False: scan failed, the caller diffs everything.
static bool dirty_scan(char* p, size_t n, std::vector<std::pair<size_t, size_t>>& out) {
    page_region v[64];
    uint64_t base = uintptr_t(p), start = base, end = base + n;
    while (start < end) {
        pm_scan_arg a{};
        a.size = sizeof a;
        a.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        a.start = start, a.end = end;
        a.vec = uintptr_t(v), a.vec_len = 64;
        a.category_mask = a.return_mask = PAGE_IS_WRITTEN;
        long k = ioctl(dirty_track().pagemap, PAGEMAP_SCAN, &a);
        if (k < 0) return false;
        for (long i = 0; i < k; ++i) out.push_back({size_t(v[i].start - base), size_t(v[i].end - v[i].start)});
        if (a.walk_end <= start) return false;
        start = a.walk_end;
    }
    return true;
}

// Write tracking for cached mappings (config cached_wp, off by default). They
// are the driver's own (VM_PFNMAP: no userfaultfd), and DXVK d3d9 keeps
// hundreds of MB there (CoJ Gunslinger: 288 MB, 23 ms/submit of memcmp for
// ~0.1 MB of changes). The mapping is made read-only per block; the first
// write faults, our SIGSEGV handler makes the block writable again and then
// sets its bit. A push swaps the bits to 0, re-protects those blocks, then
// diffs them: a write after the swap either lands before the re-protect (read
// by the diff) or faults again (next push). Faults that are not ours go to the
// previous handler (Wine's). The handler uses no libc and no TLS: in Wine the
// segment registers may be the Windows ones. The kernel writing into a
// protected block (read(2) straight into the buffer) gets EFAULT, not a fault,
// and the data never arrive: Alice: Madness Returns (UE3 texture streaming)
// shows black flickering objects in both eyes. Hence opt-in per game.
namespace wp {
constexpr int kSlots = 1024, kWords = 64;  // up to 4096 blocks per mapping
struct Slot {
    std::atomic<uintptr_t> base{0};  // page-aligned, 0 = free
    std::atomic<size_t> len{0}, blk{0};
    std::atomic<uint64_t> bits[kWords];
};
static Slot* g_slots = new Slot[kSlots];
static std::mutex& g_lock = *new std::mutex;  // slot (de)allocation, not used by the handler
static std::atomic<uint64_t> g_faults{0};
static struct sigaction g_prev;
static std::atomic<bool> g_installed{false};

static long raw_mprotect(uintptr_t p, size_t n, int prot) {
#if defined(__x86_64__)
    long r;
    asm volatile("syscall" : "=a"(r) : "a"(10L), "D"(p), "S"(n), "d"(long(prot)) : "rcx", "r11", "memory");
    return r;
#elif defined(__i386__)
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(125L), "b"(p), "c"(n), "d"(prot) : "memory");
    return r;
#else
    return mprotect((void*)p, n, prot);
#endif
}

static void on_segv(int sig, siginfo_t* si, void* uc) {
    uintptr_t a = uintptr_t(si->si_addr);
    if (si->si_code == SEGV_ACCERR)
        for (int i = 0; i < kSlots; ++i) {
            uintptr_t b = g_slots[i].base.load(std::memory_order_acquire);
            size_t n = g_slots[i].len.load(std::memory_order_relaxed), k = g_slots[i].blk.load(std::memory_order_relaxed);
            if (!b || a < b || a >= b + n || !k) continue;
            size_t j = (a - b) / k;
            // Unprotect first, then set the bit. The other order loses writes:
            // a push taking the bit and re-protecting in between would be undone
            // by our unprotect, leaving a writable block with no bit.
            if (raw_mprotect(b + j * k, std::min(k, n - j * k), PROT_READ | PROT_WRITE) != 0) break;
            g_slots[i].bits[j / 64].fetch_or(uint64_t(1) << (j % 64), std::memory_order_acq_rel);
            g_faults.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    if (g_prev.sa_flags & SA_SIGINFO) {
        g_prev.sa_sigaction(sig, si, uc);
    } else if (g_prev.sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);  // re-raised by the faulting instruction
    } else if (g_prev.sa_handler != SIG_IGN) {
        g_prev.sa_handler(sig);
    }
}

static bool enabled() {
    static const bool e = [] {
        if (!g_cfg.cached_wp) return false;  // default: full diff (correct, slower)
        struct sigaction sa{};
        sigaction(SIGSEGV, nullptr, &g_prev);
        sa.sa_sigaction = on_segv;
        sa.sa_flags = (g_prev.sa_flags | SA_SIGINFO) & ~SA_RESETHAND;
        sa.sa_mask = g_prev.sa_mask;
        if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
            logf("mirror: cached write tracking off (sigaction: %s)", strerror(errno));
            return false;
        }
        g_installed = true;
        logf("mirror: cached write tracking on (cached_wp = 1: fast; games reading files straight into GPU memory break)");
        return true;
    }();
    return e;
}

// Still the first handler? (a library may have replaced it; then faults in
// protected blocks would reach the app as crashes)
static bool still_ours() {
    struct sigaction cur{};
    sigaction(SIGSEGV, nullptr, &cur);
    return (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_segv;
}

// Protects [p, p + n) (whole pages inside it) and returns its slot, or -1.
static int add(char* p, size_t n) {
    if (!enabled()) return -1;
    uintptr_t b = (uintptr_t(p) + 4095) & ~uintptr_t(4095), e = (uintptr_t(p) + n) & ~uintptr_t(4095);
    if (e <= b) return -1;
    size_t len = e - b, blk = std::max<size_t>(64 << 10, ((len + kWords * 64 - 1) / (kWords * 64) + 4095) & ~size_t(4095));
    std::lock_guard<std::mutex> l(g_lock);
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].base.load()) continue;
        for (auto& w : g_slots[i].bits) w.store(0);
        g_slots[i].len.store(len);
        g_slots[i].blk.store(blk);
        if (raw_mprotect(b, len, PROT_READ) != 0) return -1;
        g_slots[i].base.store(b, std::memory_order_release);
        return i;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) logf("mirror: cached write tracking: all %d slots in use, more mappings get the full diff", kSlots);
    return -1;
}

// Unprotects and frees the slot (the driver may keep its mapping alive).
static void remove(int i) {
    if (i < 0) return;
    std::lock_guard<std::mutex> l(g_lock);
    uintptr_t b = g_slots[i].base.exchange(0);
    raw_mprotect(b, g_slots[i].len.load(), PROT_READ | PROT_WRITE);
}

// Written blocks of slot i as mapping offsets (p = the mapping), re-protected.
static void take(int i, const char* p, size_t n, std::vector<std::pair<size_t, size_t>>& out) {
    Slot& s = g_slots[i];
    uintptr_t b = s.base.load();
    size_t len = s.len.load(), blk = s.blk.load(), head = b - uintptr_t(p);
    for (int w = 0; w < kWords; ++w) {
        uint64_t m = s.bits[w].exchange(0, std::memory_order_acq_rel);
        for (; m; m &= m - 1) {
            size_t j = size_t(w) * 64 + size_t(__builtin_ctzll(m)), o = j * blk, k = std::min(blk, len - o);
            raw_mprotect(b + o, k, PROT_READ);
            out.push_back({head + o, k});
        }
    }
    // unprotected head/tail partial pages of the mapping: always diffed
    if (head) out.push_back({0, std::min(head, n)});
    if (head + len < n) out.push_back({head + len, n - head - len});
}
}  // namespace wp

// 32-bit processes run out of address space with four mappings per mapped
// allocation (A, B, shadow, last): there A (under a shadow) and B are mapped
// only while a push copies changed pages.
static bool transient_maps() {
    static const bool t = sizeof(void*) == 4 || !strcmp(env("VRDUAL_TRANSIENT_MAPS", "0"), "1");
    return t;
}

// md->lock held. Copies what the app changed since the last push to A (when
// the app writes a shadow; also after the mirror stopped, or the game would
// render stale data) and to B (while the mirror is live).
static void push_one(MDev* md, uint64_t amem, Mem& m) {
    bool to_a = !m.cached, to_b = md->live;
    const char* src = m.cached ? m.amap : m.shadow;
    if (!m.mapped || !src || (!to_a && !to_b)) return;
    // Ranges to diff: the written pages of a tracked shadow, else everything
    // from the first changed page. A scan that fails falls back to a full diff.
    auto& rs = md->ranges;
    rs.clear();
    uint64_t t0 = now_ns();
    size_t len = size_t(m.len);
    if (m.wp >= 0 && !wp::still_ours()) {
        logf("mirror: SIGSEGV handler replaced by someone else, cached write tracking off");
        wp::remove(m.wp);
        m.wp = -1, m.synced = false;
    }
    if (m.wp >= 0 && m.synced) wp::take(m.wp, m.amap, len, rs);
    bool scanned = m.tracked && dirty_scan(m.shadow, (len + 4095) & ~size_t(4095), rs);
    for (auto& r : rs) r.second = std::min(r.second, len - std::min(r.first, len));  // last page
    if (m.tracked && !scanned) m.tracked = m.synced = false;
    uint64_t t1 = now_ns();
    md->scan_ns += t1 - t0;
    if (!m.synced) {
        rs.clear();
        size_t first = first_change(src, m.last, m.len);
        if (first != SIZE_MAX) rs.push_back({first, m.len - first});
        m.synced = m.tracked || m.wp >= 0;  // protected before this diff: later writes show up in scans
        md->full_bytes += m.len;
        if (m.cached) md->full_cached_bytes += m.len;
        md->full_ns += now_ns() - t1;
    }
    if (rs.empty()) return;
    VkDevice adev = md->dd->device;
    char *a = to_a ? m.amap : nullptr, *b = to_b ? m.bmap : nullptr;
    bool a_tmp = to_a && !a, b_tmp = to_b && !b;
    uint64_t t2 = now_ns();
    if (a_tmp && md->a.MapMemory(adev, u2h<VkDeviceMemory>(amem), m.off, m.len, 0, (void**)&a) < 0) {
        a = nullptr;
        logf("mirror: cannot map app memory for a shadow push (%llu MB) — data lost", (unsigned long long)(m.len >> 20));
    }
    if (b_tmp && md->b.MapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem), m.off, m.len, 0, (void**)&b) < 0) {
        b = nullptr;
        mirror_fail(md, "vkMapMemory failed on B");
    }
    uint64_t t3 = now_ns();
    for (auto& r : rs) {
        word_diff_copy(src + r.first, m.last + r.first, r.second, a ? a + r.first : nullptr, b ? b + r.first : nullptr);
        md->diffed_bytes += r.second;
    }
    if (!m.coherent) {
        VkMappedMemoryRange ra{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(amem), m.off, VK_WHOLE_SIZE};
        VkMappedMemoryRange rb{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(m.bmem), m.off, VK_WHOLE_SIZE};
        if (a) md->a.FlushMappedMemoryRanges(adev, 1, &ra);
        if (b) md->b.FlushMappedMemoryRanges(md->bdev, 1, &rb);
    }
    uint64_t t4 = now_ns();
    if (a_tmp && a) md->a.UnmapMemory(adev, u2h<VkDeviceMemory>(amem));
    if (b_tmp && b) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
    md->map_ns += (t3 - t2) + (now_ns() - t4);
}

static void push_memory(MDev* md) {
    uint64_t t0 = now_ns();
    std::lock_guard<std::mutex> l(md->lock);
    static const uint64_t fail_at = strtoull(env("VRDUAL_MIRROR_FAIL_AT", "0"), nullptr, 10);  // selftest: stop on purpose
    if (fail_at && md->pushes == fail_at && md->live) mirror_fail(md, "VRDUAL_MIRROR_FAIL_AT reached");
    uint64_t bytes = 0;
    for (auto& kv : md->mems)
        if (kv.second.mapped) { push_one(md, kv.first, kv.second); bytes += kv.second.len; }
    md->pushed_bytes += bytes;
    md->push_ns += now_ns() - t0;
    if (++md->pushes % 600 == 0) {
        const double k = 600 * 1048576.0, n = 600 * 1e6;
        logf("mirror: memory sync %.2f ms/submit avg over %.1f MB mapped, %.1f MB diffed; full diff %.1f MB (%.1f MB cached) %.2f ms, "
             "dirty scan %.2f ms, transient maps %.2f ms, %llu cached write faults, %.2f MB written back",
             md->push_ns / n, bytes / 1048576.0, md->diffed_bytes / k, md->full_bytes / k, md->full_cached_bytes / k, md->full_ns / n,
             md->scan_ns / n, md->map_ns / n, (unsigned long long)wp::g_faults.exchange(0), md->wb_bytes / k);
        md->push_ns = md->diffed_bytes = md->full_bytes = md->full_cached_bytes = md->full_ns = md->scan_ns = md->map_ns = md->wb_bytes = 0;
    }
}

static void free_shadow(Mem& m) {
    if (m.shadow) munmap(m.shadow, m.cap);
    if (m.last) munmap(m.last, m.cap);
    m.shadow = m.last = nullptr;
    m.cap = 0;
    m.tracked = m.synced = false;  // munmap unregistered it
}

static char* anon(size_t n) {
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : (char*)p;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* alloc, VkDeviceMemory* out) {
    MDev* md = mdev_of(key(dev));
    // Device-address memory gets the same GPU VA on B: allocated replayable on
    // A, then replayed on B at A's opaque address. Flags are the app's, restored.
    VkMemoryAllocateFlagsInfo* fi = nullptr;
    for (auto* s = (VkBaseOutStructure*)ai->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) fi = (VkMemoryAllocateFlagsInfo*)s;
    bool rp = md->replay && fi && (fi->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    VkMemoryAllocateFlags saved = fi ? fi->flags : 0;
    if (rp) fi->flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
    VkResult r = md->a.AllocateMemory(dev, ai, alloc, out);
    if (r < 0 || !md->live) { if (fi) fi->flags = saved; return r; }
    Ctx& c = ctx_begin(md);
    VkMemoryAllocateInfo bai;
    tr_VkMemoryAllocateInfo(c, ai, &bai);
    if (fi) fi->flags = saved;
    VkMemoryOpaqueCaptureAddressAllocateInfo oc{VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO, bai.pNext};
    if (rp) {
        VkDeviceMemoryOpaqueCaptureAddressInfo q{VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO, nullptr, *out};
        oc.opaqueCaptureAddress = md->a.GetDeviceMemoryOpaqueCaptureAddress(dev, &q);
        bai.pNext = &oc;
    }
    VkDeviceMemory bm = VK_NULL_HANDLE;
    VkResult rb = c.ok() ? md->b.AllocateMemory(md->bdev, &bai, nullptr, &bm) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) { mirror_fail(md, "vkAllocateMemory failed on B"); return r; }
    record_handle(*out, bm);
    VkMemoryPropertyFlags f = md->amp.memoryTypes[ai->memoryTypeIndex].propertyFlags;
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        Mem m;
        m.bmem = h2u(bm);
        m.size = ai->allocationSize;
        // HOST_CACHED (readback) memory gets a shadow too, unless cached_wp = 1
        // (A's own mapping + write tracking) or VRDUAL_CACHED_SHADOW=0 (A's mapping, full diff)
        m.readback = f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        m.cached = (m.readback && (g_cfg.cached_wp || !strcmp(env("VRDUAL_CACHED_SHADOW", "1"), "0"))) ||
                   !strcmp(env("VRDUAL_SHADOW", "1"), "0");
        m.coherent = f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::lock_guard<std::mutex> l(md->lock);
        md->mems[h2u(*out)] = m;
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateBuffer(VkDevice dev, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* alloc, VkBuffer* out) {
    if (g_trace) trace_call("vkCreateBuffer");
    MDev* md = mdev_of(key(dev));
    VkBufferUsageFlags2 usage = ci->usage;
    for (auto* s = (const VkBaseInStructure*)ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO) usage = ((const VkBufferUsageFlags2CreateInfo*)s)->usage;
    bool rp = md->replay && (usage & VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT);
    VkBufferCreateInfo c2 = *ci;
    if (rp) c2.flags |= VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
    VkResult r = md->a.CreateBuffer(dev, &c2, alloc, out);
    if (r >= 0) {
        std::lock_guard<std::mutex> l(md->lock);
        md->bufs[h2u(*out)].size = ci->size;
    }
    if (r < 0 || !md->live) return r;
    VkBufferOpaqueCaptureAddressCreateInfo oc{VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO, c2.pNext};
    if (rp) {
        VkBufferDeviceAddressInfo q{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, *out};
        oc.opaqueCaptureAddress = md->a.GetBufferOpaqueCaptureAddress(dev, &q);
        c2.pNext = &oc;
    }
    VkBuffer bb = VK_NULL_HANDLE;
    if (md->b.CreateBuffer(md->bdev, &c2, nullptr, &bb) < 0) { mirror_fail(md, "vkCreateBuffer failed on B"); return r; }
    record_handle(*out, bb);
    return r;
}

// The app only ever sees A's addresses; B must agree or its shaders read garbage.
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL m_GetBufferDeviceAddress(VkDevice dev, const VkBufferDeviceAddressInfo* info) {
    MDev* md = mdev_of(key(dev));
    VkDeviceAddress a = md->a.GetBufferDeviceAddress(dev, info);
    if (md->live) {
        VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, xl_handle(info->buffer)};
        VkDeviceAddress b = md->b.GetBufferDeviceAddress(md->bdev, &bi);
        if (a != b) {
            logf("mirror: buffer address 0x%llx on A, 0x%llx on B", (unsigned long long)a, (unsigned long long)b);
            mirror_fail(md, "device addresses differ between the GPUs");
        }
    }
    return a;
}

static VKAPI_ATTR void VKAPI_CALL m_FreeMemory(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks* alloc) {
    MDev* md = mdev_of(key(dev));
    if (mem) {
        std::lock_guard<std::mutex> l(md->lock);
        auto it = md->mems.find(h2u(mem));
        if (it != md->mems.end()) {
            free_shadow(it->second);
            wp::remove(it->second.wp);
            md->mems.erase(it);
        }
        // a new allocation may get the same handle: forget pending writes into this one
        auto gone = [&](const MemRange& r) { return r.mem == h2u(mem); };
        for (auto& w : md->wbs) w.r.erase(std::remove_if(w.r.begin(), w.r.end(), gone), w.r.end());
        for (auto& kv : md->cbw) kv.second.erase(std::remove_if(kv.second.begin(), kv.second.end(), gone), kv.second.end());
    }
    md->a.FreeMemory(dev, mem, alloc);
    if (mem && md->live) md->b.FreeMemory(md->bdev, xl_handle(mem), nullptr);
    forget_handle(mem);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_MapMemory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags flags, void** pp) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.MapMemory(dev, mem, off, size, flags, pp);
    if (r < 0 || !md->live) return r;
    std::lock_guard<std::mutex> l(md->lock);
    auto it = md->mems.find(h2u(mem));
    if (it == md->mems.end()) return r;
    Mem& m = it->second;
    VkDeviceSize len = size == VK_WHOLE_SIZE ? m.size - off : size;
    bool tmp = transient_maps();
    void* bp = nullptr;
    if (!tmp && md->b.MapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem), off, len, 0, &bp) < 0) { mirror_fail(md, "vkMapMemory failed on B"); return r; }
    size_t cap = (size_t(len) + 4095) & ~size_t(4095);
    if (m.cap < cap || m.cap_off != off) {
        free_shadow(m);
        m.last = anon(cap);
        m.shadow = m.cached ? nullptr : anon(cap);
        if (!m.last || (!m.cached && !m.shadow)) {
            free_shadow(m);
            if (bp) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
            mirror_fail(md, "out of memory for shadow mapping");
            return r;  // the app gets A's own pointer; other shadows keep reaching A
        }
        m.cap = cap;
        m.cap_off = off;
        m.tracked = m.shadow && dirty_register(m.shadow, cap);
    }
    m.synced = false;  // a new mapping starts with a full diff
    m.amap = (char*)*pp;
    if (!m.cached && m.readback) {  // the app may read what the GPU wrote before this mapping
        if (!m.coherent) {
            VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem, off, VK_WHOLE_SIZE};
            md->a.InvalidateMappedMemoryRanges(dev, 1, &mr);
        }
        memcpy(m.shadow, m.amap, size_t(len));
        memcpy(m.last, m.amap, size_t(len));
    }
    m.a_released = tmp && !m.cached;
    if (m.a_released) {  // the app writes the shadow; A is mapped per push
        md->a.UnmapMemory(dev, mem);
        m.amap = nullptr;
    }
    m.bmap = (char*)bp;
    m.off = off;
    m.len = len;
    m.mapped = true;
    if (m.cached) {
        memcpy(m.last, m.amap, len);
        if (m.wp < 0 && md->live) m.wp = wp::add(m.amap, len);
    } else *pp = m.shadow;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL m_UnmapMemory(VkDevice dev, VkDeviceMemory mem) {
    MDev* md = mdev_of(key(dev));
    bool a_released = false;
    {
        std::lock_guard<std::mutex> l(md->lock);  // also after a stop: the shadow's last writes must reach A
        auto it = md->mems.find(h2u(mem));
        if (it != md->mems.end() && it->second.mapped) {
            Mem& m = it->second;
            push_one(md, it->first, m);
            wp::remove(m.wp);
            m.wp = -1;
            if (m.bmap) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
            a_released = m.a_released;
            m.mapped = m.a_released = false;
            m.amap = m.bmap = nullptr;
        }
    }
    if (!a_released) md->a.UnmapMemory(dev, mem);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_FlushMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange* r) {
    MDev* md = mdev_of(key(dev));
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i) {
            auto it = md->mems.find(h2u(r[i].memory));
            if (it != md->mems.end()) push_one(md, it->first, it->second);
        }
    }
    VkResult res = md->a.FlushMappedMemoryRanges(dev, n, r);
    if (md->live) {
        Ctx& c = ctx_begin(md);
        auto* br = c.alloc<VkMappedMemoryRange>(n);
        for (uint32_t i = 0; i < n; ++i) tr_VkMappedMemoryRange(c, &r[i], &br[i]);
        md->b.FlushMappedMemoryRanges(md->bdev, n, br);
        ctx_end(c);
    }
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_InvalidateMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange* r) {
    return mdev_of(key(dev))->a.InvalidateMappedMemoryRanges(dev, n, r);  // reads go to A's (cached) mapping
}

// Core in Vulkan 1.4 (DXVK uses it). Plain maps only: placed maps
// (VK_EXT_map_memory_placed) are hidden from the app.
static VKAPI_ATTR VkResult VKAPI_CALL m_MapMemory2(VkDevice dev, const VkMemoryMapInfo* info, void** pp) {
    MDev* md = mdev_of(key(dev));
    if (info->pNext) {
        mirror_fail(md, "vkMapMemory2 with a pNext chain (placed map?) not supported");
        return md->a.MapMemory2(dev, info, pp);
    }
    return m_MapMemory(dev, info->memory, info->offset, info->size, info->flags, pp);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_UnmapMemory2(VkDevice dev, const VkMemoryUnmapInfo* info) {
    MDev* md = mdev_of(key(dev));
    if (info->pNext || info->flags) {
        mirror_fail(md, "vkUnmapMemory2 with flags/pNext not supported");
        return md->a.UnmapMemory2(dev, info);
    }
    m_UnmapMemory(dev, info->memory);
    return VK_SUCCESS;
}

// --------------------------------------------------------------- sync -------
static const uint64_t kBWaitNs = 5000000000ull;  // B that far behind = broken

static void pending_add(MDev* md, VkFence f) {
    if (!f) return;
    std::lock_guard<std::mutex> l(md->lock);
    md->pending.insert(h2u(f));
}

// Waits on B for those of the app's fences that have a B submit in flight.
static void b_wait_fences(MDev* md, uint32_t n, const VkFence* f) {
    std::vector<VkFence> bf;
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i)
            if (f[i] && md->pending.erase(h2u(f[i]))) bf.push_back(xl_handle(f[i]));
    }
    if (bf.empty() || !md->live) return;
    if (md->b.WaitForFences(md->bdev, uint32_t(bf.size()), bf.data(), VK_TRUE, kBWaitNs) != VK_SUCCESS)
        mirror_fail(md, "right GPU fell >5 s behind (fence)");
}

static VKAPI_ATTR VkResult VKAPI_CALL m_WaitForFences(VkDevice dev, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.WaitForFences(dev, n, f, all, timeout);
    if (r == VK_SUCCESS) wb_poll(md);  // before the app reads what finished
    if (r != VK_SUCCESS || !md->live) return r;
    if (all) {
        b_wait_fences(md, n, f);
    } else {
        for (uint32_t i = 0; i < n; ++i)
            if (md->a.GetFenceStatus(dev, f[i]) == VK_SUCCESS) b_wait_fences(md, 1, &f[i]);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_GetFenceStatus(VkDevice dev, VkFence f) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.GetFenceStatus(dev, f);
    if (r == VK_SUCCESS) wb_poll(md);
    if (r == VK_SUCCESS && md->live) b_wait_fences(md, 1, &f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_ResetFences(VkDevice dev, uint32_t n, const VkFence* f) {
    MDev* md = mdev_of(key(dev));
    for (uint32_t i = 0; i < n; ++i) wb_drop_fence(md, f[i]);
    if (md->live) {
        b_wait_fences(md, n, f);
        std::vector<VkFence> bf;
        for (uint32_t i = 0; i < n; ++i) bf.push_back(xl_handle(f[i]));
        md->b.ResetFences(md->bdev, n, bf.data());
    }
    return md->a.ResetFences(dev, n, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_WaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo* wi, uint64_t timeout) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.WaitSemaphores(dev, wi, timeout);
    if (r == VK_SUCCESS) wb_semaphores(md, wi);
    if (r != VK_SUCCESS || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkSemaphoreWaitInfo bwi;
    tr_VkSemaphoreWaitInfo(c, wi, &bwi);
    if (md->b.WaitSemaphores(md->bdev, &bwi, kBWaitNs) != VK_SUCCESS) mirror_fail(md, "right GPU fell >5 s behind (timeline)");
    ctx_end(c);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_GetSemaphoreCounterValue(VkDevice dev, VkSemaphore s, uint64_t* v) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.GetSemaphoreCounterValue(dev, s, v);
    if (r == VK_SUCCESS) wb_poll(md, s, *v);
    if (r != VK_SUCCESS || !md->live) return r;
    VkSemaphore bs = xl_handle(s);
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &bs, v};
    if (md->b.WaitSemaphores(md->bdev, &wi, kBWaitNs) != VK_SUCCESS) mirror_fail(md, "right GPU fell >5 s behind (timeline)");
    return r;
}

// --------------------------------------------------------------- queues -----
static VKAPI_ATTR void VKAPI_CALL m_GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    vrd_GetDeviceQueue(dev, fam, idx, q);
    MDev* md = mdev_of(key(dev));
    VkQueue bq = VK_NULL_HANDLE;
    md->b.GetDeviceQueue(md->bdev, fam, idx, &bq);
    record_handle(*q, bq);
    if (!md->bqueue_any) md->bqueue_any = bq;
}

static VKAPI_ATTR void VKAPI_CALL m_GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* q) {
    vrd_GetDeviceQueue2(dev, info, q);
    MDev* md = mdev_of(key(dev));
    VkDeviceQueueInfo2 bi = *info;
    bi.pNext = nullptr;
    VkQueue bq = VK_NULL_HANDLE;
    md->b.GetDeviceQueue2(md->bdev, &bi, &bq);
    record_handle(*q, bq);
    if (!md->bqueue_any) md->bqueue_any = bq;
}

// After A accepted a submit: its host-memory writes wait for the app's sync.
static void wb_add(MDev* md, Writeback& w) {
    if (w.r.empty()) return;
    std::lock_guard<std::mutex> l(md->lock);
    md->wbs.push_back(std::move(w));
}

static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    MDev* md = mdev_of(key(q));
    wb_poll(md);
    push_memory(md);  // after a stop too: shadows must keep reaching A
    Writeback w;
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t j = 0; j < si[i].commandBufferCount; ++j) wb_collect(md, si[i].pCommandBuffers[j], w);
            for (auto* s = (const VkBaseInStructure*)si[i].pNext; s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    auto* t = (const VkTimelineSemaphoreSubmitInfo*)s;
                    for (uint32_t k = 0; k < t->signalSemaphoreValueCount && k < si[i].signalSemaphoreCount; ++k)
                        w.sig.push_back({si[i].pSignalSemaphores[k], t->pSignalSemaphoreValues[k]});
                }
        }
    }
    VkFence fa = f;
    if (!w.r.empty() && !f) fa = w.fence = wb_fence(md), w.ours = true;
    else w.fence = f;
    VkResult r = md->a.QueueSubmit(q, n, si, fa);
    if (r >= 0) wb_add(md, w);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    auto* bsi = c.alloc<VkSubmitInfo>(n);
    for (uint32_t i = 0; i < n; ++i) tr_VkSubmitInfo(c, &si[i], &bsi[i]);
    VkResult rb = VK_ERROR_UNKNOWN;
    if (c.ok()) {
        std::lock_guard<std::mutex> l(md->qlock);
        rb = md->b.QueueSubmit(c.xl(q), n, bsi, c.xl(f));
    }
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkQueueSubmit failed on B");
    else pending_add(md, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) {
    MDev* md = mdev_of(key(q));
    wb_poll(md);
    push_memory(md);  // after a stop too: shadows must keep reaching A
    Writeback w;
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t j = 0; j < si[i].commandBufferInfoCount; ++j) wb_collect(md, si[i].pCommandBufferInfos[j].commandBuffer, w);
            for (uint32_t k = 0; k < si[i].signalSemaphoreInfoCount; ++k)
                w.sig.push_back({si[i].pSignalSemaphoreInfos[k].semaphore, si[i].pSignalSemaphoreInfos[k].value});
        }
    }
    VkFence fa = f;
    if (!w.r.empty() && !f) fa = w.fence = wb_fence(md), w.ours = true;
    else w.fence = f;
    VkResult r = md->a.QueueSubmit2(q, n, si, fa);
    if (r >= 0) wb_add(md, w);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    auto* bsi = c.alloc<VkSubmitInfo2>(n);
    for (uint32_t i = 0; i < n; ++i) tr_VkSubmitInfo2(c, &si[i], &bsi[i]);
    VkResult rb = VK_ERROR_UNKNOWN;
    if (c.ok()) {
        std::lock_guard<std::mutex> l(md->qlock);
        rb = md->b.QueueSubmit2(c.xl(q), n, bsi, c.xl(f));
    }
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkQueueSubmit2 failed on B");
    else pending_add(md, f);
    return r;
}

// ------------------------------------------------------ command buffers -----
static VKAPI_ATTR VkResult VKAPI_CALL m_AllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* cbs) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.AllocateCommandBuffers(dev, ai, cbs);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkCommandBufferAllocateInfo bai;
    tr_VkCommandBufferAllocateInfo(c, ai, &bai);
    auto* b = c.alloc<VkCommandBuffer>(ai->commandBufferCount);
    VkResult rb = c.ok() ? md->b.AllocateCommandBuffers(md->bdev, &bai, b) : VK_ERROR_UNKNOWN;
    if (rb < 0) {
        mirror_fail(md, "vkAllocateCommandBuffers failed on B");
    } else {
        for (uint32_t i = 0; i < ai->commandBufferCount; ++i) record_handle(cbs[i], b[i]);
        if (ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
            std::lock_guard<std::mutex> l(md->lock);
            for (uint32_t i = 0; i < ai->commandBufferCount; ++i) md->secondary.insert(cbs[i]);
        }
    }
    ctx_end(c);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL m_FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
    MDev* md = mdev_of(key(dev));
    md->a.FreeCommandBuffers(dev, pool, n, cbs);
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i) md->cbw.erase(cbs[i]);
    }
    if (!md->live) return;
    std::vector<VkCommandBuffer> b;
    for (uint32_t i = 0; i < n; ++i)
        if (cbs[i]) b.push_back(xl_handle(cbs[i]));
    if (!b.empty()) md->b.FreeCommandBuffers(md->bdev, xl_handle(pool), uint32_t(b.size()), b.data());
    std::lock_guard<std::mutex> l(md->lock);
    for (uint32_t i = 0; i < n; ++i) md->secondary.erase(cbs[i]);
}

// pInheritanceInfo is ignored (may be garbage) for primary command buffers.
static VKAPI_ATTR VkResult VKAPI_CALL m_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    MDev* md = mdev_of(key(cb));
    VkResult r = md->a.BeginCommandBuffer(cb, bi);
    if (r >= 0) {
        std::lock_guard<std::mutex> l(md->lock);
        md->cbw[cb].clear();  // recording starts over
    }
    if (r < 0 || !md->live) return r;
    bool sec;
    {
        std::lock_guard<std::mutex> l(md->lock);
        sec = md->secondary.count(cb) != 0;
    }
    Ctx& c = ctx_begin(md);
    VkCommandBufferBeginInfo b = *bi;
    b.pNext = tr_chain(c, bi->pNext);
    b.pInheritanceInfo = nullptr;
    if (sec && bi->pInheritanceInfo) {
        auto* t = c.alloc<VkCommandBufferInheritanceInfo>(1);
        tr_VkCommandBufferInheritanceInfo(c, bi->pInheritanceInfo, t);
        b.pInheritanceInfo = t;
    }
    VkResult rb = c.ok() ? md->b.BeginCommandBuffer(c.xl(cb), &b) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkBeginCommandBuffer failed on B");
    return r;
}

// ------------------------------------------------- descriptor templates -----
static size_t desc_elem_size(VkDescriptorType t) {
    if (is_image_desc(t)) return sizeof(VkDescriptorImageInfo);
    if (is_buffer_desc(t)) return sizeof(VkDescriptorBufferInfo);
    if (is_texel_desc(t)) return sizeof(VkBufferView);
    if (t == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) return sizeof(VkAccelerationStructureKHR);
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateDescriptorUpdateTemplate(VkDevice dev, const VkDescriptorUpdateTemplateCreateInfo* ci,
                                                                     const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.CreateDescriptorUpdateTemplate(dev, ci, alloc, out);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkDescriptorUpdateTemplateCreateInfo bci;
    tr_VkDescriptorUpdateTemplateCreateInfo(c, ci, &bci);
    VkDescriptorUpdateTemplate bt = VK_NULL_HANDLE;
    VkResult rb = c.ok() ? md->b.CreateDescriptorUpdateTemplate(md->bdev, &bci, nullptr, &bt) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) { mirror_fail(md, "vkCreateDescriptorUpdateTemplate failed on B"); return r; }
    record_handle(*out, bt);
    Template t;
    t.entries.assign(ci->pDescriptorUpdateEntries, ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount);
    for (auto& e : t.entries) {
        if (!e.descriptorCount) continue;
        size_t end = e.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK
                         ? e.offset + e.descriptorCount
                         : e.offset + (e.descriptorCount - 1) * e.stride + desc_elem_size(e.descriptorType);
        t.size = std::max(t.size, end);
    }
    std::lock_guard<std::mutex> l(md->lock);
    md->templates[h2u(*out)] = std::move(t);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL m_DestroyDescriptorUpdateTemplate(VkDevice dev, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* alloc) {
    MDev* md = mdev_of(key(dev));
    md->a.DestroyDescriptorUpdateTemplate(dev, t, alloc);
    if (md->live && t) md->b.DestroyDescriptorUpdateTemplate(md->bdev, xl_handle(t), nullptr);
    std::lock_guard<std::mutex> l(md->lock);
    md->templates.erase(h2u(t));
    forget_handle(t);
}

// Copy of a template update's raw data with every handle in it translated.
static const void* tr_template_data(Ctx& c, VkDescriptorUpdateTemplate tmpl, const void* data) {
    Template t;
    {
        std::lock_guard<std::mutex> l(c.md->lock);
        auto it = c.md->templates.find(h2u(tmpl));
        if (it == c.md->templates.end()) { c.fail("unknown descriptor update template"); return data; }
        t = it->second;
    }
    char* blob = c.alloc<char>(t.size);
    memcpy(blob, data, t.size);
    for (auto& e : t.entries) {
        for (uint32_t i = 0; i < e.descriptorCount && e.descriptorType != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK; ++i) {
            char* p = blob + e.offset + i * e.stride;
            if (is_image_desc(e.descriptorType)) {
                VkDescriptorImageInfo ii;
                memcpy(&ii, p, sizeof ii);
                ii = xl_image_info(c, e.descriptorType, ii);
                memcpy(p, &ii, sizeof ii);
            } else if (is_buffer_desc(e.descriptorType)) {
                VkDescriptorBufferInfo bi;
                memcpy(&bi, p, sizeof bi);
                bi.buffer = c.xl(bi.buffer);
                memcpy(p, &bi, sizeof bi);
            } else if (is_texel_desc(e.descriptorType)) {
                VkBufferView v;
                memcpy(&v, p, sizeof v);
                v = c.xl(v);
                memcpy(p, &v, sizeof v);
            } else if (e.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                VkAccelerationStructureKHR v;
                memcpy(&v, p, sizeof v);
                v = c.xl(v);
                memcpy(p, &v, sizeof v);
            }
        }
    }
    return blob;
}

static VKAPI_ATTR void VKAPI_CALL m_UpdateDescriptorSetWithTemplate(VkDevice dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void* data) {
    MDev* md = mdev_of(key(dev));
    md->a.UpdateDescriptorSetWithTemplate(dev, set, t, data);
    if (!md->live) return;
    Ctx& c = ctx_begin(md);
    const void* bd = tr_template_data(c, t, data);
    if (c.ok()) md->b.UpdateDescriptorSetWithTemplate(md->bdev, c.xl(set), c.xl(t), bd);
    else mirror_fail(md, "vkUpdateDescriptorSetWithTemplate: translation failed");
    ctx_end(c);
}

static VKAPI_ATTR void VKAPI_CALL m_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate t, VkPipelineLayout layout,
                                                                    uint32_t set, const void* data) {
    MDev* md = mdev_of(key(cb));
    md->a.CmdPushDescriptorSetWithTemplate(cb, t, layout, set, data);
    if (!md->live) return;
    Ctx& c = ctx_begin(md);
    const void* bd = tr_template_data(c, t, data);
    if (c.ok()) md->b.CmdPushDescriptorSetWithTemplate(c.xl(cb), c.xl(t), c.xl(layout), set, bd);
    else mirror_fail(md, "vkCmdPushDescriptorSetWithTemplate: translation failed");
    ctx_end(c);
}

static VKAPI_ATTR void VKAPI_CALL m_CmdPushDescriptorSetWithTemplate2(VkCommandBuffer cb, const VkPushDescriptorSetWithTemplateInfo* info) {
    MDev* md = mdev_of(key(cb));
    md->a.CmdPushDescriptorSetWithTemplate2(cb, info);
    if (!md->live) return;
    Ctx& c = ctx_begin(md);
    VkPushDescriptorSetWithTemplateInfo b = *info;
    b.pNext = tr_chain(c, info->pNext);
    b.descriptorUpdateTemplate = c.xl(info->descriptorUpdateTemplate);
    b.layout = c.xl(info->layout);
    b.pData = tr_template_data(c, info->descriptorUpdateTemplate, info->pData);
    if (c.ok()) md->b.CmdPushDescriptorSetWithTemplate2(c.xl(cb), &b);
    else mirror_fail(md, "vkCmdPushDescriptorSetWithTemplate2: translation failed");
    ctx_end(c);
}

// --------------------------------------------------------------- shaders ----
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    MDev* md = mdev_of(key(dev));
    VkResult r = create_patched_module(md->dd->ep, md->a.CreateShaderModule, dev, ci, alloc, out);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkShaderModuleCreateInfo bci = *ci;
    bci.pNext = tr_chain(c, ci->pNext);
    VkShaderModule bm = VK_NULL_HANDLE;
    VkResult rb = create_patched_module(md->bep, md->b.CreateShaderModule, md->bdev, &bci, nullptr, &bm);
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkCreateShaderModule failed on B");
    else record_handle(*out, bm);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* cis,
                                                              const VkAllocationCallbacks* alloc, VkPipeline* out) {
    MDev* md = mdev_of(key(dev));
    // Inline modules need the left-eye patch on A as well (B's: in the pNext translation).
    InlinePatch ip;
    const VkGraphicsPipelineCreateInfo* acis = patch_inline(md->dd->ep, n, cis, ip);
    VkResult r = md->a.CreateGraphicsPipelines(dev, cache, n, acis, alloc, out);
    if (g_trace) {
        trace_call("vkCreateGraphicsPipelines (manual)");
        for (uint32_t i = 0; i < n; ++i)
            logf("  r=%d inline=%d flags=0x%llx stages=%u pipe=0x%llx", r, acis != cis, (unsigned long long)cis[i].flags, cis[i].stageCount,
                 (unsigned long long)h2u(out[i]));
    }
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    auto* bcis = c.alloc<VkGraphicsPipelineCreateInfo>(n);
    for (uint32_t i = 0; i < n; ++i) tr_gpci(c, &cis[i], &bcis[i]);
    auto* bp = c.alloc<VkPipeline>(n);
    VkResult rb = c.ok() ? md->b.CreateGraphicsPipelines(md->bdev, c.xl(cache), n, bcis, nullptr, bp) : VK_ERROR_UNKNOWN;
    if (g_trace)
        for (uint32_t i = 0; i < n; ++i) logf("  B rb=%d ok=%d pipe=0x%llx layout=0x%llx", rb, c.ok(), (unsigned long long)h2u(bp[i]), (unsigned long long)h2u(bcis[i].layout));
    if (rb < 0) mirror_fail(md, "vkCreateGraphicsPipelines failed on B");
    else
        for (uint32_t i = 0; i < n; ++i) {
            if (out[i] && !bp[i]) { mirror_fail(md, "vkCreateGraphicsPipelines: no pipeline on B"); break; }
            record_handle(out[i], bp[i]);
        }
    ctx_end(c);
    return r;
}

// ------------------------------------------------------------------ WSI -----
// Signals B's twins of what an acquire on A signalled.
static void b_signal(MDev* md, VkSemaphore s, VkFence f) {
    if (!s && !f) return;
    VkSemaphore bs = xl_handle(s);
    VkFence bf = xl_handle(f);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.signalSemaphoreCount = bs ? 1 : 0;
    si.pSignalSemaphores = &bs;
    if (!md->bqueue_any) { mirror_fail(md, "acquire before any queue was fetched"); return; }
    VkResult r;
    {
        std::lock_guard<std::mutex> l(md->qlock);
        r = md->b.QueueSubmit(md->bqueue_any, 1, &si, bf);
    }
    if (r < 0) mirror_fail(md, "acquire signal failed on B");
    else pending_add(md, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore s, VkFence f, uint32_t* idx) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.AcquireNextImageKHR(dev, sc, timeout, s, f, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && md->live) b_signal(md, s, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR* info, uint32_t* idx) {
    MDev* md = mdev_of(key(dev));
    VkResult r = md->a.AcquireNextImage2KHR(dev, info, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && md->live) b_signal(md, info->semaphore, info->fence);
    return r;
}

// Plain B images standing in for the swapchain's, plus the eye-1 frame file.
static void create_swapchain_twin(DeviceData* dd, const VkSwapchainCreateInfoKHR* ci, SwapchainData& s) {
    MDev* md = dd->md;
    if (!md->live) return;
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    for (auto* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) { fl = *(const VkImageFormatListCreateInfo*)p; fl.pNext = nullptr; }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (fl.viewFormatCount) ici.pNext = &fl;
    }
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = ci->imageFormat;
    ici.extent = {ci->imageExtent.width, ci->imageExtent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = ci->imageArrayLayers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = ci->imageSharingMode;
    ici.queueFamilyIndexCount = ci->queueFamilyIndexCount;
    ici.pQueueFamilyIndices = ci->pQueueFamilyIndices;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (VkImage ai : s.images) {
        VkImage bi = VK_NULL_HANDLE;
        VkDeviceMemory bm = VK_NULL_HANDLE;
        VkMemoryRequirements mr;
        if (md->b.CreateImage(md->bdev, &ici, nullptr, &bi) != VK_SUCCESS) { mirror_fail(md, "swapchain twin image"); return; }
        md->b.GetImageMemoryRequirements(md->bdev, bi, &mr);
        int t = pick_memtype(md->bmp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, uint32_t(t)};
        if (t < 0 || md->b.AllocateMemory(md->bdev, &mai, nullptr, &bm) != VK_SUCCESS || md->b.BindImageMemory(md->bdev, bi, bm, 0) != VK_SUCCESS) {
            mirror_fail(md, "swapchain twin memory");
            return;
        }
        s.b_images.push_back(bi);
        s.b_mem.push_back(bm);
        record_handle(ai, bi);
    }
    if (s.pixfmt && !create_readback(md->b, md->bdev, md->bmp, s.extent, s.b_rb, conv_format_of(s.format)))
        logf("mirror: readback alloc failed on B — right eye not exported");
    logf("mirror: swapchain %ux%u, %zu twin images on B", s.extent.width, s.extent.height, s.b_images.size());
}

static void destroy_swapchain_twin(DeviceData* dd, SwapchainData& s) {
    MDev* md = dd->md;
    if (md->bcr.fence) md->b.WaitForFences(md->bdev, 1, &md->bcr.fence, VK_TRUE, kBWaitNs);
    md->b.DeviceWaitIdle(md->bdev);
    for (size_t i = 0; i < s.b_images.size(); ++i) {
        forget_handle(s.images[i]);
        md->b.DestroyImage(md->bdev, s.b_images[i], nullptr);
        md->b.FreeMemory(md->bdev, s.b_mem[i], nullptr);
    }
    s.b_images.clear();
    s.b_mem.clear();
    free_readback(md->b, md->bdev, s.b_rb);
}

// Present on A = "frame done" on B: consume B's wait semaphores, copy the B
// image to the right-eye frame file with the same frame index as the left.
// `own`: shm_claim result for this swapchain (g_shm_lock held by the caller).
static void present_twin(DeviceData* dd, VkQueue queue, uint32_t family, const VkPresentInfoKHR* pi, SwapchainData* s, uint64_t frame, int own) {
    MDev* md = dd->md;
    if (!md->live) return;
    push_params(md->bep);
    VkQueue bq = xl_handle(queue);
    std::vector<VkSemaphore> waits;
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; ++i) waits.push_back(xl_handle(pi->pWaitSemaphores[i]));
    std::lock_guard<std::mutex> l(md->qlock);
    bool done = false;
    if (own == kNewOwner && s->b_rb.buf) shm_open_out(1, s->extent.width, s->extent.height, s->pixfmt, md->bname.c_str(), md->bpci.c_str());
    if (own && s->b_rb.buf && pi->pImageIndices[0] < s->b_images.size() && ensure_copy_res(md->b, md->bdev, nullptr, family, md->bcr)) {
        done = copy_to_readback(md->b, md->bdev, bq, md->bcr, s->b_images[pi->pImageIndices[0]], s->extent, s->b_rb,
                                uint32_t(waits.size()), waits.data(), false);
        if (done) shm_publish(1, frame, s->b_rb.map, size_t(s->extent.width) * s->extent.height * 4);
    }
    if (!done && !waits.empty()) {
        std::vector<VkPipelineStageFlags> st(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = uint32_t(waits.size());
        si.pWaitSemaphores = waits.data();
        si.pWaitDstStageMask = st.data();
        md->b.QueueSubmit(bq, 1, &si, VK_NULL_HANDLE);
    }
}

// ------------------------------------------------------------- lookup -------
static PFN_vkVoidFunction lookup(const char* name) {
    if (const char* m = manual_canonical(name)) {
        std::string n = m;
#define M(f) if (n == "vk" #f) return (PFN_vkVoidFunction)m_##f
        M(AllocateMemory); M(FreeMemory); M(MapMemory); M(UnmapMemory); M(MapMemory2); M(UnmapMemory2);
        M(FlushMappedMemoryRanges); M(InvalidateMappedMemoryRanges); M(WaitForFences); M(GetFenceStatus);
        M(ResetFences); M(WaitSemaphores); M(GetSemaphoreCounterValue); M(GetDeviceQueue); M(GetDeviceQueue2);
        M(QueueSubmit); M(QueueSubmit2); M(AllocateCommandBuffers); M(FreeCommandBuffers); M(BeginCommandBuffer);
        M(CreateDescriptorUpdateTemplate); M(DestroyDescriptorUpdateTemplate); M(UpdateDescriptorSetWithTemplate);
        M(CmdPushDescriptorSetWithTemplate); M(CmdPushDescriptorSetWithTemplate2); M(CreateShaderModule);
        M(CreateGraphicsPipelines); M(AcquireNextImageKHR); M(AcquireNextImage2KHR); M(CreateBuffer);
        M(GetBufferDeviceAddress);
#undef M
        if (n == "vkDestroyDevice") return (PFN_vkVoidFunction)vrd_DestroyDevice;
        if (n == "vkCreateSwapchainKHR") return (PFN_vkVoidFunction)vrd_CreateSwapchainKHR;
        if (n == "vkDestroySwapchainKHR") return (PFN_vkVoidFunction)vrd_DestroySwapchainKHR;
        if (n == "vkQueuePresentKHR") return (PFN_vkVoidFunction)vrd_QueuePresentKHR;
        return nullptr;  // A-only (vkGetSwapchainImagesKHR, ...)
    }
    return gen_lookup(name);
}

// ------------------------------------------------------- twin creation ------
static int phdr_cb(dl_phdr_info* info, size_t, void* data) {
    if (info->dlpi_name && *info->dlpi_name) ((std::vector<std::string>*)data)->push_back(info->dlpi_name);
    return 0;
}

// Copy of a pNext chain without the loader's link structs.
static const void* copy_chain_for_icd(const void* p, std::vector<std::unique_ptr<char[]>>& keep) {
    VkBaseOutStructure* head = nullptr;
    VkBaseOutStructure** tail = &head;
    for (auto* s = (const VkBaseInStructure*)p; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
        size_t n = stype_size(s->sType);
        if (!n) { logf("mirror: device pNext sType %d unknown, dropped", int(s->sType)); continue; }
        keep.emplace_back(new char[n]);
        memcpy(keep.back().get(), s, n);
        auto* o = (VkBaseOutStructure*)keep.back().get();
        o->pNext = nullptr;
        *tail = o;
        tail = &o->pNext;
    }
    return head;
}

static MDev* create_twin(DeviceData* dd, const VkDeviceCreateInfo* ci, bool bda, bool replay) {
    InstanceData* id = dd->inst;
    std::string apci = pci_string(id, dd->phys);
    std::vector<std::string> libs;
    dl_iterate_phdr(phdr_cb, &libs);
    std::stable_partition(libs.begin(), libs.end(), [](const std::string& s) { return s.find("radeon") != std::string::npos; });
    auto md = std::make_unique<MDev>();
    md->dd = dd;
    md->a = dd->d;
    id->GetPhysicalDeviceMemoryProperties(dd->phys, &md->amp);
    std::string icd;
    for (auto& lib : libs) {
        void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_NOLOAD);
        auto gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr") : nullptr;
        if (!gipa) { if (h) dlclose(h); continue; }
        auto enum_ext = (PFN_vkEnumerateInstanceExtensionProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
        auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
        if (!enum_ext || !create) continue;
        uint32_t ne = 0;
        enum_ext(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ie(ne);
        enum_ext(nullptr, &ne, ie.data());
        std::vector<const char*> iexts;
        for (auto& e : ie)
            if (!strcmp(e.extensionName, "VK_KHR_surface")) iexts.push_back("VK_KHR_surface");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "vrdual mirror";
        app.apiVersion = std::max<uint32_t>(id->api_version, VK_API_VERSION_1_1);
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(iexts.size());
        ici.ppEnabledExtensionNames = iexts.data();
        VkInstance inst = VK_NULL_HANDLE;
        if (create(&ici, nullptr, &inst) != VK_SUCCESS) continue;
        auto enum_pd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
        auto gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
        auto gpp2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
        uint32_t n = 0;
        enum_pd(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n);
        enum_pd(inst, &n, pds.data());
        // Another GPU matching right_gpu. The app's own GPU never gets a twin:
        // a second device there cannot replay A's device addresses (same VA).
        for (uint32_t i = 0; i < n && !md->bphys; ++i) {
            VkPhysicalDeviceProperties p;
            gpp(pds[i], &p);
            std::string pci = pci_string(gpp2, pds[i]);
            if (pci == apci) continue;
            if (gpu_matches(g_cfg.mirror_gpu, i, p.deviceName, pci)) {
                md->bphys = pds[i];
                md->bname = p.deviceName;
                md->bpci = pci;
            }
        }
        if (md->bphys) {
            md->binst = inst;
            md->icd_gipa = gipa;
            icd = lib;
            break;
        }
        ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, nullptr);
    }
    if (!md->bphys) {
        // Single GPU: alternate-eye rendering on A (needs live params to switch eyes).
        dd->aer = dd->ep.params != nullptr;
        if (dd->aer)
            logf("mirror: single-GPU mode — no other GPU matches '%s', alternate-eye rendering on the app's GPU (pci %s), half the frame rate per eye",
                 g_cfg.mirror_gpu.c_str(), apci.c_str());
        else
            logf("mirror: no GPU matching '%s' and no live params for alternate-eye rendering — left eye only", g_cfg.mirror_gpu.c_str());
        return nullptr;
    }
    auto gipa = md->icd_gipa;
    auto destroy_inst = (PFN_vkDestroyInstance)gipa(md->binst, "vkDestroyInstance");
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(md->binst, "vkGetPhysicalDeviceMemoryProperties"))(md->bphys, &md->bmp);
    bool same = md->amp.memoryTypeCount == md->bmp.memoryTypeCount;
    for (uint32_t i = 0; same && i < md->amp.memoryTypeCount; ++i)
        same = md->amp.memoryTypes[i].propertyFlags == md->bmp.memoryTypes[i].propertyFlags &&
               md->amp.memoryTypes[i].heapIndex == md->bmp.memoryTypes[i].heapIndex;
    if (!same) {
        logf("mirror: %s has different memory types than the app's GPU — mirror needs identical GPUs, off", md->bname.c_str());
        destroy_inst(md->binst, nullptr);
        return nullptr;
    }

    // Same device create info as the app's (+ device address for our params).
    std::vector<std::unique_ptr<char[]>> keep;
    VkDeviceCreateInfo bci = *ci;
    bci.pNext = copy_chain_for_icd(ci->pNext, keep);
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    VkPhysicalDeviceBufferDeviceAddressFeatures bf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    if (bda) {
        bool flagged = false;
        for (auto* s = (VkBaseOutStructure*)bci.pNext; s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                auto* f = (VkPhysicalDeviceVulkan12Features*)s;
                f->bufferDeviceAddress = VK_TRUE;
                if (replay) f->bufferDeviceAddressCaptureReplay = VK_TRUE;
                flagged = true;
            }
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
                auto* f = (VkPhysicalDeviceBufferDeviceAddressFeatures*)s;
                f->bufferDeviceAddress = VK_TRUE;
                if (replay) f->bufferDeviceAddressCaptureReplay = VK_TRUE;
                flagged = true;
            }
        }
        if (!flagged) { bf.bufferDeviceAddress = VK_TRUE; bf.bufferDeviceAddressCaptureReplay = replay; bf.pNext = (void*)bci.pNext; bci.pNext = &bf; }
        if (std::none_of(exts.begin(), exts.end(), [](const char* e) { return !strcmp(e, "VK_KHR_buffer_device_address"); }))
            exts.push_back("VK_KHR_buffer_device_address");
    }
    bci.enabledExtensionCount = uint32_t(exts.size());
    bci.ppEnabledExtensionNames = exts.data();
    bci.enabledLayerCount = 0;
    bci.ppEnabledLayerNames = nullptr;
    auto create_dev = (PFN_vkCreateDevice)gipa(md->binst, "vkCreateDevice");
    VkResult r = create_dev(md->bphys, &bci, nullptr, &md->bdev);
    if (r != VK_SUCCESS) {
        logf("mirror: vkCreateDevice on %s failed (%d) — mirror off", md->bname.c_str(), r);
        destroy_inst(md->binst, nullptr);
        return nullptr;
    }
    auto bgdpa = (PFN_vkGetDeviceProcAddr)gipa(md->binst, "vkGetDeviceProcAddr");
    load_dispatch(md->b, bgdpa, md->bdev);
    md->bep.eye = 1;
    md->replay = replay;
    if (bda && md->b.GetBufferDeviceAddress) create_param_buffer(md->b, md->bdev, md->bmp, md->bep);
    logf("mirror: twin device on %s (pci %s) via %s — hidden from the app%s", md->bname.c_str(), md->bpci.c_str(), icd.c_str(),
         replay ? ", device addresses replayed" : "");
    return md.release();
}

static void destroy_twin(DeviceData* dd) {
    MDev* md = dd->md;
    md->b.DeviceWaitIdle(md->bdev);
    for (auto& kv : dd->swapchains) destroy_swapchain_twin(dd, kv.second);
    for (auto& kv : md->mems) free_shadow(kv.second);
    free_param_buffer(md->b, md->bdev, md->bep);
    free_copy_res(md->b, md->bdev, md->bcr);
    logf("mirror: %s, %llu memory syncs", md->live ? "ran to the end" : "had stopped", (unsigned long long)md->pushes);
    md->b.DestroyDevice(md->bdev, nullptr);
    ((PFN_vkDestroyInstance)md->icd_gipa(md->binst, "vkDestroyInstance"))(md->binst, nullptr);
    delete md;
    dd->md = nullptr;
}

}  // namespace mirror
