// vrdual_passes.hpp — render pass tracking for two features (docs/tasks/framegen-effects.md,
// backlog "Shadow-map passes"). Included by vrdual_layer.cpp; the hooks exist
// only on devices with a tracker (no cost otherwise). On a mirror device they
// sit in front of the mirror's wrappers (Next).
//
// depth_export: find the game's main depth image and copy it with each
// exported frame (one process per eye or one GPU; not on a mirror device yet).
// Single-sample 2D depth attachments are tracked from creation (TRANSFER_SRC
// added for the copy). Each render pass or dynamic rendering begin that uses
// one counts a use, at record time. At present the image with the swapchain's
// extent and the most uses wins. Its layout at present is the last one
// recorded: a render pass's final layout, dynamic rendering's attachment
// layout, or a barrier's new layout. Record order is not submit order, so
// this is a heuristic; games repeat the same pattern every frame.
//
// auto_noshift: only the camera's view should be shifted per eye. Off-screen
// passes (shadow maps, baked terrain / overlay textures, reflections at
// another size) are drawn from elsewhere; shifting them (or giving their
// w == 1 vertices the hud_depth shift) makes each eye look up a texture
// offset from its own geometry (Civilization V: shadows cast from the camera
// side, seams between baked terrain tiles). A pass is off-screen if its
// extent is neither the swapchain's nor the main view's (the extent with
// colour that has the most shader keys; a game may render its 3D view at
// another size than the swapchain, Civ V: 1368x768 into 1920x1080). Shader
// keys (pipeline -> last pre-raster stage) seen bound in passes are recorded
// with the pass extent; keys seen only in off-screen passes go into the live
// key table of the param buffer (addr+96, 32 entries), which patched shaders
// check: not shifted. Re-evaluated every 30 presents, so a key later seen in
// the main view leaves the table again.
#pragma once

namespace passes {

struct Img {
    VkExtent2D ext;
    VkFormat fmt;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t uses = 0;
};
struct Next {  // the next layer, or the mirror's wrapper on a mirror device
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdEndRenderPass2 CmdEndRenderPass2;
    PFN_vkCmdBeginRendering CmdBeginRendering;
    PFN_vkCmdEndRendering CmdEndRendering;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
};
struct Track {
    bool depth = false, autons = false;  // depth_export, auto_noshift
    Next next{};
    std::mutex m;
    // depth_export
    std::unordered_map<VkImage, Img> img;
    std::unordered_map<VkImageView, VkImage> view;
    struct Rp { uint32_t att; VkImageLayout final; bool depth_only; };  // att: depth attachment, UINT32_MAX = none
    std::unordered_map<VkRenderPass, Rp> rp;
    struct Fb { VkRenderPass rp; VkImage img; bool imageless; };
    std::unordered_map<VkFramebuffer, Fb> fb;
    VkImage chosen = VK_NULL_HANDLE;  // last pick (log changes only)
    bool told_none = false;
    // auto_noshift
    struct Cb { VkPipeline pipe = VK_NULL_HANDLE; int pass = 0; uint64_t ext = 0; };  // pass: 0 none, 1 with colour, 2 depth only
    std::unordered_map<VkCommandBuffer, Cb> cbs;
    std::unordered_map<VkShaderModule, uint32_t> mod_key;
    std::unordered_map<VkPipeline, uint32_t> pipe_key;
    std::unordered_map<uint32_t, std::vector<uint64_t>> key_use;  // key -> (extent << 1 | depth only) seen
    uint64_t presents = 0;
    std::string last_passes;  // log_passes: last logged
    uint64_t main_ext = 0;    // main view extent (log changes)
};

static uint64_t ext_key(VkExtent2D e) { return uint64_t(e.width) << 32 | e.height; }

static bool is_depth(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
    default: return false;
    }
}
static bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// Buffer texels of a depth-aspect copy -> float NDC z (n pixels).
static void to_float(float* d, const void* src, VkFormat f, size_t n) {
    if (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT) {
        auto* s = (const uint16_t*)src;
        for (size_t i = 0; i < n; ++i) d[i] = s[i] * (1.0f / 65535.0f);
    } else if (f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D24_UNORM_S8_UINT) {
        auto* s = (const uint32_t*)src;
        for (size_t i = 0; i < n; ++i) d[i] = (s[i] & 0xffffffu) * (1.0f / 16777215.0f);
    } else {
        memcpy(d, src, n * 4);
    }
}

static Track* of(void* k) {
    DeviceData* dd = dev_of(k);
    return dd ? dd->pt : nullptr;
}

// ------------------------------------------------------ resources ---------
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateImage(VkDevice dev, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
    Track* t = of(key(dev));
    bool want = t->depth && is_depth(ci->format) && (ci->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                ci->imageType == VK_IMAGE_TYPE_2D && ci->samples == VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo c2 = *ci;
    if (want) c2.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = t->next.CreateImage(dev, &c2, a, out);
    if (r == VK_SUCCESS && want) {
        std::lock_guard<std::mutex> l(t->m);
        t->img[*out] = Img{{ci->extent.width, ci->extent.height}, ci->format};
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyImage(VkDevice dev, VkImage img, const VkAllocationCallbacks* a) {
    Track* t = of(key(dev));
    {
        std::lock_guard<std::mutex> l(t->m);
        t->img.erase(img);
        if (t->chosen == img) t->chosen = VK_NULL_HANDLE;
    }
    t->next.DestroyImage(dev, img, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateImageView(VkDevice dev, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    Track* t = of(key(dev));
    VkResult r = t->next.CreateImageView(dev, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(t->m);
        if (t->img.count(ci->image)) t->view[*out] = ci->image;
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks* a) {
    Track* t = of(key(dev));
    {
        std::lock_guard<std::mutex> l(t->m);
        t->view.erase(v);
    }
    t->next.DestroyImageView(dev, v, a);
}

// Depth attachment: of the first subpass that has one. Depth only: no subpass has colour.
template <class CI>
static void note_rp(Track* t, const CI* ci, VkRenderPass rp) {
    Track::Rp r{UINT32_MAX, VK_IMAGE_LAYOUT_UNDEFINED, true};
    for (uint32_t i = 0; i < ci->subpassCount; ++i) {
        auto& sp = ci->pSubpasses[i];
        if (sp.colorAttachmentCount) r.depth_only = false;
        auto* ds = sp.pDepthStencilAttachment;
        if (r.att == UINT32_MAX && ds && ds->attachment != VK_ATTACHMENT_UNUSED && ds->attachment < ci->attachmentCount)
            r.att = ds->attachment, r.final = ci->pAttachments[ds->attachment].finalLayout;
    }
    if (r.att == UINT32_MAX) r.depth_only = false;
    std::lock_guard<std::mutex> l(t->m);
    t->rp[rp] = r;
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    Track* t = of(key(dev));
    VkResult r = t->next.CreateRenderPass(dev, ci, a, out);
    if (r == VK_SUCCESS) note_rp(t, ci, *out);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    Track* t = of(key(dev));
    VkResult r = t->next.CreateRenderPass2(dev, ci, a, out);
    if (r == VK_SUCCESS) note_rp(t, ci, *out);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyRenderPass(VkDevice dev, VkRenderPass rp, const VkAllocationCallbacks* a) {
    Track* t = of(key(dev));
    {
        std::lock_guard<std::mutex> l(t->m);
        t->rp.erase(rp);
    }
    t->next.DestroyRenderPass(dev, rp, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* out) {
    Track* t = of(key(dev));
    VkResult r = t->next.CreateFramebuffer(dev, ci, a, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::mutex> l(t->m);
    auto rp = t->rp.find(ci->renderPass);
    if (rp == t->rp.end() || rp->second.att == UINT32_MAX) return r;
    bool imageless = ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
    VkImage img = VK_NULL_HANDLE;
    if (!imageless && rp->second.att < ci->attachmentCount) {
        auto v = t->view.find(ci->pAttachments[rp->second.att]);
        if (v != t->view.end()) img = v->second;
    }
    if (img || imageless) t->fb[*out] = {ci->renderPass, img, imageless};
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks* a) {
    Track* t = of(key(dev));
    {
        std::lock_guard<std::mutex> l(t->m);
        t->fb.erase(fb);
    }
    t->next.DestroyFramebuffer(dev, fb, a);
}

// Shader key of the last pre-raster stage the layer patches (GS > TES > VS):
// from the module, an inline module, or a linked library.
static uint32_t stage_key(Track* t, const VkPipelineShaderStageCreateInfo& s) {
    if (s.module) {
        auto it = t->mod_key.find(s.module);
        return it == t->mod_key.end() ? 0 : it->second;
    }
    for (auto* p = (const VkBaseInStructure*)s.pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            auto* mi = (const VkShaderModuleCreateInfo*)p;
            return uint32_t(spv::shader_hash(mi->pCode, mi->codeSize / 4) >> 32);
        }
    return 0;
}
static void note_module(DeviceData* dd, const VkShaderModuleCreateInfo* ci, VkShaderModule m) {
    Track* t = dd->pt;
    if (!t || !t->autons) return;
    uint32_t k = uint32_t(spv::shader_hash(ci->pCode, ci->codeSize / 4) >> 32);
    std::lock_guard<std::mutex> l(t->m);
    t->mod_key[m] = k;
}
static void note_pipelines(DeviceData* dd, uint32_t n, const VkGraphicsPipelineCreateInfo* cis, const VkPipeline* out) {
    Track* t = dd->pt;
    if (!t || !t->autons) return;
    std::lock_guard<std::mutex> l(t->m);
    for (uint32_t i = 0; i < n; ++i) {
        if (!out[i]) continue;
        uint32_t k = 0;
        int rank = 0;
        for (uint32_t j = 0; j < cis[i].stageCount; ++j) {
            auto st = cis[i].pStages[j].stage;
            int r = st == VK_SHADER_STAGE_GEOMETRY_BIT ? 3 : st == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ? 2 : st == VK_SHADER_STAGE_VERTEX_BIT ? 1 : 0;
            if (r > rank) rank = r, k = stage_key(t, cis[i].pStages[j]);
        }
        if (!rank)
            for (auto* p = (const VkBaseInStructure*)cis[i].pNext; p && !k; p = p->pNext)
                if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) {
                    auto* li = (const VkPipelineLibraryCreateInfoKHR*)p;
                    for (uint32_t j = 0; j < li->libraryCount && !k; ++j) {
                        auto it = t->pipe_key.find(li->pLibraries[j]);
                        if (it != t->pipe_key.end()) k = it->second;
                    }
                }
        if (k) t->pipe_key[out[i]] = k;
    }
}

// ------------------------------------------------- command buffers ---------
// The bound pipeline's key in the current pass, recorded with the pass kind (t->m held).
static void note_draw(Track* t, const Track::Cb& c) {
    if (!t->autons || !c.pass || !c.pipe) return;
    auto it = t->pipe_key.find(c.pipe);
    if (it == t->pipe_key.end()) return;
    uint64_t u = c.ext << 1 | (c.pass == 2);
    auto& v = t->key_use[it->second];
    if (std::find(v.begin(), v.end(), u) == v.end() && v.size() < 16) v.push_back(u);
}
static void begin_pass(Track* t, VkCommandBuffer cb, bool depth_only, VkExtent2D ext) {
    if (!t->autons) return;
    auto& c = t->cbs[cb];
    c.pass = depth_only ? 2 : 1;
    c.ext = ext_key(ext);
    note_draw(t, c);
}
static void use(Track& t, VkImage img, VkImageLayout layout) {
    auto it = t.img.find(img);
    if (it == t.img.end()) return;
    it->second.uses++;
    it->second.layout = layout;
}
static void begin_rp(Track* t, VkCommandBuffer cb, const VkRenderPassBeginInfo* bi) {
    std::lock_guard<std::mutex> l(t->m);
    auto rp = t->rp.find(bi->renderPass);
    if (rp == t->rp.end()) return;
    begin_pass(t, cb, rp->second.depth_only, bi->renderArea.extent);
    if (!t->depth) return;
    auto fb = t->fb.find(bi->framebuffer);
    if (fb == t->fb.end()) return;
    VkImage img = fb->second.img;
    if (fb->second.imageless)
        for (auto* p = (const VkBaseInStructure*)bi->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                auto* ab = (const VkRenderPassAttachmentBeginInfo*)p;
                if (rp->second.att < ab->attachmentCount) {
                    auto v = t->view.find(ab->pAttachments[rp->second.att]);
                    if (v != t->view.end()) img = v->second;
                }
            }
    if (img) use(*t, img, rp->second.final);
}
static void end_pass(Track* t, VkCommandBuffer cb) {
    if (!t->autons) return;
    std::lock_guard<std::mutex> l(t->m);
    auto& c = t->cbs[cb];
    c.pass = 0;
    c.pipe = VK_NULL_HANDLE;  // counted at the next begin only if bound in between (dynamic rendering binds before)
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    Track* t = of(key(cb));
    begin_rp(t, cb, bi);
    t->next.CmdBeginRenderPass(cb, bi, c);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* sbi) {
    Track* t = of(key(cb));
    begin_rp(t, cb, bi);
    t->next.CmdBeginRenderPass2(cb, bi, sbi);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdEndRenderPass(VkCommandBuffer cb) {
    Track* t = of(key(cb));
    end_pass(t, cb);
    t->next.CmdEndRenderPass(cb);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo* sei) {
    Track* t = of(key(cb));
    end_pass(t, cb);
    t->next.CmdEndRenderPass2(cb, sei);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    Track* t = of(key(cb));
    {
        std::lock_guard<std::mutex> l(t->m);
        bool colour = false;
        for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) colour |= ri->pColorAttachments[i].imageView != VK_NULL_HANDLE;
        bool depth = ri->pDepthAttachment && ri->pDepthAttachment->imageView;
        if (colour || depth) begin_pass(t, cb, !colour, ri->renderArea.extent);
        if (t->depth && depth) {
            auto v = t->view.find(ri->pDepthAttachment->imageView);
            if (v != t->view.end()) use(*t, v->second, ri->pDepthAttachment->imageLayout);
        }
    }
    t->next.CmdBeginRendering(cb, ri);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdEndRendering(VkCommandBuffer cb) {
    Track* t = of(key(cb));
    end_pass(t, cb);
    t->next.CmdEndRendering(cb);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    Track* t = of(key(cb));
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> l(t->m);
        auto& c = t->cbs[cb];
        c.pipe = p;
        note_draw(t, c);
    }
    t->next.CmdBindPipeline(cb, bp, p);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                                        uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b,
                                                        uint32_t ni, const VkImageMemoryBarrier* im) {
    Track* t = of(key(cb));
    if (ni) {
        std::lock_guard<std::mutex> l(t->m);
        for (uint32_t i = 0; i < ni; ++i) {
            auto it = t->img.find(im[i].image);
            if (it != t->img.end()) it->second.layout = im[i].newLayout;
        }
    }
    t->next.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    Track* t = of(key(cb));
    if (di->imageMemoryBarrierCount) {
        std::lock_guard<std::mutex> l(t->m);
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) {
            auto it = t->img.find(di->pImageMemoryBarriers[i].image);
            if (it != t->img.end()) it->second.layout = di->pImageMemoryBarriers[i].newLayout;
        }
    }
    t->next.CmdPipelineBarrier2(cb, di);
}

// Hooks for this tracker's features; nullptr = not hooked.
static PFN_vkVoidFunction intercept(const char* name, const Track* t) {
    std::string n = name;
    for (const char* k : {"vkCreateRenderPass2KHR", "vkCmdBeginRenderPass2KHR", "vkCmdEndRenderPass2KHR", "vkCmdBeginRenderingKHR",
                          "vkCmdEndRenderingKHR", "vkCmdPipelineBarrier2KHR"})
        if (n == k) n.resize(n.size() - 3);
#define I(f) if (n == "vk" #f) return (PFN_vkVoidFunction)vrd_##f
    I(CreateRenderPass); I(CreateRenderPass2); I(DestroyRenderPass);
    I(CmdBeginRenderPass); I(CmdBeginRenderPass2); I(CmdEndRenderPass); I(CmdEndRenderPass2);
    I(CmdBeginRendering); I(CmdEndRendering);
    if (t->autons) { I(CmdBindPipeline); }
    if (t->depth) {
        I(CreateImage); I(DestroyImage); I(CreateImageView); I(DestroyImageView); I(CreateFramebuffer); I(DestroyFramebuffer);
        I(CmdPipelineBarrier); I(CmdPipelineBarrier2);
    }
#undef I
    return nullptr;
}

static Track* create(DeviceData* dd, bool depth, bool autons) {
    auto* t = new Track;
    t->depth = depth, t->autons = autons;
    Next& x = t->next;
    const Dispatch& d = dd->d;
#define N(f) x.f = d.f; \
    if (dd->md) if (auto p = mirror::lookup("vk" #f)) x.f = (PFN_vk##f)p
    N(CreateImage); N(DestroyImage); N(CreateImageView); N(DestroyImageView); N(CreateRenderPass); N(CreateRenderPass2);
    N(DestroyRenderPass); N(CreateFramebuffer); N(DestroyFramebuffer); N(CmdBeginRenderPass); N(CmdBeginRenderPass2);
    N(CmdEndRenderPass); N(CmdEndRenderPass2); N(CmdBeginRendering); N(CmdEndRendering); N(CmdBindPipeline);
    N(CmdPipelineBarrier); N(CmdPipelineBarrier2);
#undef N
    return t;
}

// auto_noshift, at present: keys seen only in off-screen passes -> the live table.
// Every 3000 presents, when it changed: which passes the game draws (a shadow
// pass the heuristic misses shows up here: its extent, colour or not, its key
// count) and how many pipelines / modules have a key (t.m held).
static void log_passes(Track& t) {
    std::map<uint64_t, std::pair<int, int>> ext;  // extent<<1|depth only -> keys, keys only there
    for (auto& kv : t.key_use)
        for (uint64_t u : kv.second) {
            auto& e = ext[u];
            e.first++;
            if (kv.second.size() == 1) e.second++;
        }
    std::string s;
    for (auto& kv : ext) {
        char b[96];
        snprintf(b, sizeof b, " %ux%u %s: %d key(s), %d only there;", uint32_t(kv.first >> 33), uint32_t(kv.first >> 1),
                 kv.first & 1 ? "depth only" : "colour", kv.second.first, kv.second.second);
        s += b;
    }
    char k[96];
    snprintf(k, sizeof k, " (%zu pipelines, %zu modules with a key)", t.pipe_key.size(), t.mod_key.size());
    if (s == t.last_passes) return;
    t.last_passes = s;
    logf("auto_noshift: passes seen (extent, kind, shader keys):%s%s", s.empty() ? " none" : s.c_str(), k);
}

static void refresh_auto(Track& t, VkExtent2D sc) {
    if (!t.autons || ++t.presents % 30) return;
    if (t.presents % 3000 == 600) {
        std::lock_guard<std::mutex> l(t.m);
        log_passes(t);
    }
    std::vector<std::pair<uint32_t, uint64_t>> found;  // key, an off-screen extent (log)
    {
        std::lock_guard<std::mutex> l(t.m);
        uint64_t sce = ext_key(sc), main = sce;
        std::map<uint64_t, int> n;  // colour extent -> keys drawn there
        for (auto& kv : t.key_use)
            for (uint64_t u : kv.second)
                if (!(u & 1)) n[u >> 1]++;
        int best = n.count(sce) ? n[sce] : 0;
        for (auto& e : n)
            if (e.second > best) best = e.second, main = e.first;
        for (auto& kv : t.key_use) {
            bool off = !kv.second.empty();
            for (uint64_t u : kv.second)
                if ((u >> 1) == sce || (u >> 1) == main) { off = false; break; }
            if (off) found.push_back({kv.first, kv.second[0] >> 1});
        }
        if (main != t.main_ext) {
            t.main_ext = main;
            logf("auto_noshift: main view %ux%u (swapchain %ux%u)", uint32_t(main >> 32), uint32_t(main), sc.width, sc.height);
        }
    }
    std::sort(found.begin(), found.end());
    if (found.size() > 32) found.resize(32);
    AutoKeys& a = auto_keys();
    uint32_t k[32] = {};
    for (size_t i = 0; i < found.size(); ++i) k[i] = found[i].first;
    std::lock_guard<std::mutex> l(a.m);
    if (!memcmp(k, a.keys, sizeof k)) return;
    memcpy(a.keys, k, sizeof k);
    a.seq.fetch_add(1);
    std::string s;
    for (auto& f : found)
        s += " " + [&] { char b[48]; snprintf(b, sizeof b, "%08x@%ux%u", f.first, uint32_t(f.second >> 32), uint32_t(f.second)); return std::string(b); }();
    logf("auto_noshift: %zu shader(s) drawn only in off-screen passes, not shifted:%s", found.size(), s.empty() ? " none" : s.c_str());
}

// ------------------------------------------------------ depth export ------
// What to copy at present: the most used depth image of the swapchain's size
// in a layout it can be copied from. Logs the pick when it changes.
struct Pick { VkImage img = VK_NULL_HANDLE; VkFormat fmt = VK_FORMAT_UNDEFINED; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
static Pick pick(Track& t, VkExtent2D ext) {
    std::lock_guard<std::mutex> l(t.m);
    Pick p;
    uint64_t best = 0;
    for (auto& kv : t.img) {
        const Img& i = kv.second;
        if (i.ext.width != ext.width || i.ext.height != ext.height || !i.uses || i.uses <= best) continue;
        if (i.layout == VK_IMAGE_LAYOUT_UNDEFINED || i.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) continue;
        best = i.uses, p = {kv.first, i.fmt, i.layout};
    }
    if (p.img && p.img != t.chosen)
        logf("depth_export: image 0x%llx %ux%u format %d (%llu render pass uses), layout %d", (unsigned long long)(uintptr_t)p.img,
             ext.width, ext.height, p.fmt, (unsigned long long)best, p.layout);
    if (!p.img && !t.told_none && !t.img.empty()) {
        std::string c;
        for (auto& kv : t.img)
            c += " " + std::to_string(kv.second.ext.width) + "x" + std::to_string(kv.second.ext.height) + "/" + std::to_string(kv.second.uses);
        logf("depth_export: no used depth image of the swapchain's size %ux%u yet (tracked WxH/uses:%s)", ext.width, ext.height, c.c_str());
        t.told_none = true;
    }
    if (p.img) t.chosen = p.img;
    return p;
}

// Records the copy of the picked image's depth aspect into rb (after the
// colour copy, same command buffer): its layout -> TRANSFER_SRC and back.
static void record_copy(const Dispatch& d, VkCommandBuffer cb, const Pick& p, VkExtent2D ext, Readback& rb) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = p.layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = p.img;
    b.subresourceRange = {VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(p.fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    reg.imageExtent = {ext.width, ext.height, 1};
    d.CmdCopyImageToBuffer(cb, p.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &reg);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = p.layout;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace passes
