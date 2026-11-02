// vrdual_depth.hpp — depth export (depth_export = 1): find the game's main
// depth image and copy it with each exported frame (docs/tasks/framegen-effects.md).
// Included by vrdual_layer.cpp; its hooks are installed only on devices with
// a tracker (one process per eye or one GPU, not the mirror).
//
// Which image: single-sample 2D depth attachments are tracked from creation
// (TRANSFER_SRC added for the copy). Each render pass or dynamic rendering
// begin that uses one counts a use, at record time. At present the image with
// the swapchain's extent and the most uses wins. Its layout at present is the
// last one recorded: a render pass's final layout, dynamic rendering's
// attachment layout, or a barrier's new layout. Record order is not submit
// order, so this is a heuristic; games repeat the same pattern every frame.
#pragma once

namespace depth {

struct Img {
    VkExtent2D ext;
    VkFormat fmt;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t uses = 0;
};
struct Track {
    std::mutex m;
    std::unordered_map<VkImage, Img> img;
    std::unordered_map<VkImageView, VkImage> view;
    struct Rp { uint32_t att; VkImageLayout final; };
    std::unordered_map<VkRenderPass, Rp> rp;
    struct Fb { VkRenderPass rp; VkImage img; bool imageless; };
    std::unordered_map<VkFramebuffer, Fb> fb;
    VkImage chosen = VK_NULL_HANDLE;  // last pick (log changes only)
    bool told_none = false;
};

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

static Track* of(void* k, DeviceData** out) {
    DeviceData* dd = dev_of(k);
    *out = dd;
    return dd ? dd->dt : nullptr;
}

static void use(Track& t, VkImage img, VkImageLayout layout) {
    auto it = t.img.find(img);
    if (it == t.img.end()) return;
    it->second.uses++;
    it->second.layout = layout;
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateImage(VkDevice dev, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
    DeviceData* dd;
    Track* t = of(key(dev), &dd);
    bool want = t && is_depth(ci->format) && (ci->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                ci->imageType == VK_IMAGE_TYPE_2D && ci->samples == VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo c2 = *ci;
    if (want) c2.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = dd->d.CreateImage(dev, &c2, a, out);
    if (r == VK_SUCCESS && want) {
        std::lock_guard<std::mutex> l(t->m);
        t->img[*out] = Img{{ci->extent.width, ci->extent.height}, ci->format};
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyImage(VkDevice dev, VkImage img, const VkAllocationCallbacks* a) {
    DeviceData* dd;
    if (Track* t = of(key(dev), &dd)) {
        std::lock_guard<std::mutex> l(t->m);
        t->img.erase(img);
        if (t->chosen == img) t->chosen = VK_NULL_HANDLE;
    }
    dd->d.DestroyImage(dev, img, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateImageView(VkDevice dev, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    DeviceData* dd;
    Track* t = of(key(dev), &dd);
    VkResult r = dd->d.CreateImageView(dev, ci, a, out);
    if (r == VK_SUCCESS && t) {
        std::lock_guard<std::mutex> l(t->m);
        if (t->img.count(ci->image)) t->view[*out] = ci->image;
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks* a) {
    DeviceData* dd;
    if (Track* t = of(key(dev), &dd)) {
        std::lock_guard<std::mutex> l(t->m);
        t->view.erase(v);
    }
    dd->d.DestroyImageView(dev, v, a);
}

// The first subpass with a depth attachment names the pass's depth image.
template <class CI>
static void note_rp(Track* t, const CI* ci, VkRenderPass rp) {
    for (uint32_t i = 0; i < ci->subpassCount; ++i) {
        auto* ds = ci->pSubpasses[i].pDepthStencilAttachment;
        if (!ds || ds->attachment == VK_ATTACHMENT_UNUSED || ds->attachment >= ci->attachmentCount) continue;
        std::lock_guard<std::mutex> l(t->m);
        t->rp[rp] = {ds->attachment, ci->pAttachments[ds->attachment].finalLayout};
        return;
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    DeviceData* dd;
    Track* t = of(key(dev), &dd);
    VkResult r = dd->d.CreateRenderPass(dev, ci, a, out);
    if (r == VK_SUCCESS && t) note_rp(t, ci, *out);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    DeviceData* dd;
    Track* t = of(key(dev), &dd);
    VkResult r = dd->d.CreateRenderPass2(dev, ci, a, out);
    if (r == VK_SUCCESS && t) note_rp(t, ci, *out);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL vrd_DestroyRenderPass(VkDevice dev, VkRenderPass rp, const VkAllocationCallbacks* a) {
    DeviceData* dd;
    if (Track* t = of(key(dev), &dd)) {
        std::lock_guard<std::mutex> l(t->m);
        t->rp.erase(rp);
    }
    dd->d.DestroyRenderPass(dev, rp, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* out) {
    DeviceData* dd;
    Track* t = of(key(dev), &dd);
    VkResult r = dd->d.CreateFramebuffer(dev, ci, a, out);
    if (r != VK_SUCCESS || !t) return r;
    std::lock_guard<std::mutex> l(t->m);
    auto rp = t->rp.find(ci->renderPass);
    if (rp == t->rp.end()) return r;
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
    DeviceData* dd;
    if (Track* t = of(key(dev), &dd)) {
        std::lock_guard<std::mutex> l(t->m);
        t->fb.erase(fb);
    }
    dd->d.DestroyFramebuffer(dev, fb, a);
}

static void begin_rp(Track* t, const VkRenderPassBeginInfo* bi) {
    std::lock_guard<std::mutex> l(t->m);
    auto fb = t->fb.find(bi->framebuffer);
    if (fb == t->fb.end()) return;
    auto rp = t->rp.find(fb->second.rp);
    if (rp == t->rp.end()) return;
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
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    DeviceData* dd;
    if (Track* t = of(key(cb), &dd)) begin_rp(t, bi);
    dd->d.CmdBeginRenderPass(cb, bi, c);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* sbi) {
    DeviceData* dd;
    if (Track* t = of(key(cb), &dd)) begin_rp(t, bi);
    dd->d.CmdBeginRenderPass2(cb, bi, sbi);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    DeviceData* dd;
    Track* t = of(key(cb), &dd);
    if (t && ri->pDepthAttachment && ri->pDepthAttachment->imageView) {
        std::lock_guard<std::mutex> l(t->m);
        auto v = t->view.find(ri->pDepthAttachment->imageView);
        if (v != t->view.end()) use(*t, v->second, ri->pDepthAttachment->imageLayout);
    }
    dd->d.CmdBeginRendering(cb, ri);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                                        uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b,
                                                        uint32_t ni, const VkImageMemoryBarrier* im) {
    DeviceData* dd;
    Track* t = of(key(cb), &dd);
    if (t && ni) {
        std::lock_guard<std::mutex> l(t->m);
        for (uint32_t i = 0; i < ni; ++i) {
            auto it = t->img.find(im[i].image);
            if (it != t->img.end()) it->second.layout = im[i].newLayout;
        }
    }
    dd->d.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im);
}
static VKAPI_ATTR void VKAPI_CALL vrd_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    DeviceData* dd;
    Track* t = of(key(cb), &dd);
    if (t && di->imageMemoryBarrierCount) {
        std::lock_guard<std::mutex> l(t->m);
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) {
            auto it = t->img.find(di->pImageMemoryBarriers[i].image);
            if (it != t->img.end()) it->second.layout = di->pImageMemoryBarriers[i].newLayout;
        }
    }
    dd->d.CmdPipelineBarrier2(cb, di);
}

static PFN_vkVoidFunction intercept(const char* name) {
#define I(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)vrd_##n
    I(CreateImage); I(DestroyImage); I(CreateImageView); I(DestroyImageView); I(CreateRenderPass); I(CreateRenderPass2);
    I(DestroyRenderPass); I(CreateFramebuffer); I(DestroyFramebuffer); I(CmdBeginRenderPass); I(CmdBeginRenderPass2);
    I(CmdBeginRendering); I(CmdPipelineBarrier); I(CmdPipelineBarrier2);
#undef I
    if (!strcmp(name, "vkCreateRenderPass2KHR")) return (PFN_vkVoidFunction)vrd_CreateRenderPass2;
    if (!strcmp(name, "vkCmdBeginRenderPass2KHR")) return (PFN_vkVoidFunction)vrd_CmdBeginRenderPass2;
    if (!strcmp(name, "vkCmdBeginRenderingKHR")) return (PFN_vkVoidFunction)vrd_CmdBeginRendering;
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) return (PFN_vkVoidFunction)vrd_CmdPipelineBarrier2;
    return nullptr;
}

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

}  // namespace depth
