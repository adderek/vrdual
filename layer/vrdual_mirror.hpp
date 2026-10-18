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
    bool cached = false, coherent = true;
    bool mapped = false;
    char* amap = nullptr;    // null while A is mapped only during pushes (transient)
    char* bmap = nullptr;    // same for B
    bool a_released = false; // transient: A unmapped right after the app's vkMapMemory
    VkDeviceSize off = 0, len = 0;
    char* shadow = nullptr;  // non-cached only
    char* last = nullptr;    // what A and B hold, as last pushed
    size_t cap = 0;          // bytes mmap'd for shadow/last
    VkDeviceSize cap_off = 0;
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
    uint64_t pushed_bytes = 0, push_ns = 0, pushes = 0;
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
    size_t first = first_change(src, m.last, m.len);
    if (first == SIZE_MAX) return;
    VkDevice adev = md->dd->device;
    char *a = to_a ? m.amap : nullptr, *b = to_b ? m.bmap : nullptr;
    bool a_tmp = to_a && !a, b_tmp = to_b && !b;
    if (a_tmp && md->a.MapMemory(adev, u2h<VkDeviceMemory>(amem), m.off, m.len, 0, (void**)&a) < 0) {
        a = nullptr;
        logf("mirror: cannot map app memory for a shadow push (%llu MB) — data lost", (unsigned long long)(m.len >> 20));
    }
    if (b_tmp && md->b.MapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem), m.off, m.len, 0, (void**)&b) < 0) {
        b = nullptr;
        mirror_fail(md, "vkMapMemory failed on B");
    }
    word_diff_copy(src + first, m.last + first, m.len - first, a ? a + first : nullptr, b ? b + first : nullptr);
    if (!m.coherent) {
        VkMappedMemoryRange ra{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(amem), m.off, VK_WHOLE_SIZE};
        VkMappedMemoryRange rb{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(m.bmem), m.off, VK_WHOLE_SIZE};
        if (a) md->a.FlushMappedMemoryRanges(adev, 1, &ra);
        if (b) md->b.FlushMappedMemoryRanges(md->bdev, 1, &rb);
    }
    if (a_tmp && a) md->a.UnmapMemory(adev, u2h<VkDeviceMemory>(amem));
    if (b_tmp && b) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
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
    if (++md->pushes % 600 == 0)
        logf("mirror: memory sync %.2f ms/submit avg over %.1f MB mapped", md->push_ns / 1e6 / 600, bytes / 1048576.0), md->push_ns = 0;
}

static void free_shadow(Mem& m) {
    if (m.shadow) munmap(m.shadow, m.cap);
    if (m.last) munmap(m.last, m.cap);
    m.shadow = m.last = nullptr;
    m.cap = 0;
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
        m.cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || !strcmp(env("VRDUAL_SHADOW", "1"), "0");
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
            md->mems.erase(it);
        }
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
    }
    m.amap = (char*)*pp;
    m.a_released = tmp && !m.cached;
    if (m.a_released) {  // the app writes the shadow; A is mapped per push
        md->a.UnmapMemory(dev, mem);
        m.amap = nullptr;
    }
    m.bmap = (char*)bp;
    m.off = off;
    m.len = len;
    m.mapped = true;
    if (m.cached) memcpy(m.last, m.amap, len);
    else *pp = m.shadow;
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
    if (r == VK_SUCCESS && md->live) b_wait_fences(md, 1, &f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_ResetFences(VkDevice dev, uint32_t n, const VkFence* f) {
    MDev* md = mdev_of(key(dev));
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

static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    MDev* md = mdev_of(key(q));
    push_memory(md);  // after a stop too: shadows must keep reaching A
    VkResult r = md->a.QueueSubmit(q, n, si, f);
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
    push_memory(md);  // after a stop too: shadows must keep reaching A
    VkResult r = md->a.QueueSubmit2(q, n, si, f);
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
    // Inline modules (VkShaderModuleCreateInfo first in a stage's pNext) need the
    // left-eye patch on A as well.
    std::vector<VkGraphicsPipelineCreateInfo> acis;
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> astages;
    std::vector<std::vector<uint32_t>> codes;
    std::vector<VkShaderModuleCreateInfo> smcis;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t s = 0; s < cis[i].stageCount && cis[i].pStages; ++s) {
            auto* sm = (const VkShaderModuleCreateInfo*)cis[i].pStages[s].pNext;
            if (cis[i].pStages[s].module || !sm || sm->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            if (acis.empty()) {
                acis.assign(cis, cis + n);
                astages.resize(n);
                codes.reserve(64);
                smcis.reserve(64);
            }
            if (astages[i].empty()) astages[i].assign(cis[i].pStages, cis[i].pStages + cis[i].stageCount);
            std::vector<uint32_t> code;
            if (smcis.size() < smcis.capacity() && spv_patch_for(md->dd->ep, sm, code)) {
                codes.push_back(std::move(code));
                VkShaderModuleCreateInfo m = *sm;
                m.codeSize = codes.back().size() * 4;
                m.pCode = codes.back().data();
                smcis.push_back(m);
                astages[i][s].pNext = &smcis.back();
            }
            acis[i].pStages = astages[i].data();
        }
    VkResult r = md->a.CreateGraphicsPipelines(dev, cache, n, acis.empty() ? cis : acis.data(), alloc, out);
    if (g_trace) {
        trace_call("vkCreateGraphicsPipelines (manual)");
        for (uint32_t i = 0; i < n; ++i)
            logf("  r=%d inline=%d flags=0x%llx stages=%u pipe=0x%llx", r, !acis.empty(), (unsigned long long)cis[i].flags, cis[i].stageCount,
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
    if (s.pixfmt && !create_readback(md->b, md->bdev, md->bmp, s.extent, s.b_rb))
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
        for (uint32_t i = 0; i < n && !md->bphys; ++i) {
            VkPhysicalDeviceProperties p;
            gpp(pds[i], &p);
            std::string pci = pci_string(gpp2, pds[i]);
            if (pci != apci && gpu_matches(g_cfg.mirror_gpu, i, p.deviceName, pci)) {
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
        logf("mirror: no GPU matching '%s' in any loaded driver — mirror off, left eye only", g_cfg.mirror_gpu.c_str());
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
