// vrdual-devext-test: a device with the extensions an OpenXR runtime asks for
// (WiVRn's list, which xrizer's temporary device uses too) must be creatable
// through the layer. The loader checks requests against the layer's filtered
// enumeration, so a hidden extension here fails with ERROR_EXTENSION_NOT_PRESENT.
// Then a sync fd round trip (semaphore and fence, SYNC_FD): export after a
// submit signals them, import into others, wait. In mirror mode the layer
// replays these calls on device B; its failure shows as "mirror STOPPED" on
// stderr (the selftest check fails on that line). Headless: no window, one device.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int fail(const char* what, VkResult r) {
    fprintf(stderr, "%s: %d\n", what, int(r));
    return 1;
}

// Sync fd round trip on the app's device (A). Returns 0 when all waits succeed.
static int sync_fd_roundtrip(VkDevice dev, VkQueue q) {
    auto gsfd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
    auto isfd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
    auto gffd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(dev, "vkGetFenceFdKHR");
    auto iffd = (PFN_vkImportFenceFdKHR)vkGetDeviceProcAddr(dev, "vkImportFenceFdKHR");
    if (!gsfd || !isfd || !gffd || !iffd) { fprintf(stderr, "sync fd entry points missing\n"); return 1; }
    const uint64_t timeout = 2000000000ull;  // 2 s: a stuck import is a failure, not a hang

    // exportable binary semaphore and fence (source), plain ones (targets)
    VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &esci;
    VkSemaphore s_src, s_dst, s_wait_dst;
    VkResult r = vkCreateSemaphore(dev, &sci, nullptr, &s_src);
    if (r != VK_SUCCESS) return int(fail("create exportable semaphore", r));
    VkSemaphoreCreateInfo plain{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if ((r = vkCreateSemaphore(dev, &plain, nullptr, &s_dst)) != VK_SUCCESS) return int(fail("create semaphore", r));
    if ((r = vkCreateSemaphore(dev, &plain, nullptr, &s_wait_dst)) != VK_SUCCESS) return int(fail("create semaphore", r));

    VkExportFenceCreateInfo efci{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
    efci.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.pNext = &efci;
    VkFence f_src, f_dst, f_done;
    if ((r = vkCreateFence(dev, &fci, nullptr, &f_src)) != VK_SUCCESS) return int(fail("create exportable fence", r));
    VkFenceCreateInfo fplain{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = vkCreateFence(dev, &fplain, nullptr, &f_dst)) != VK_SUCCESS) return int(fail("create fence", r));
    if ((r = vkCreateFence(dev, &fplain, nullptr, &f_done)) != VK_SUCCESS) return int(fail("create fence", r));

    int rc = 0;
    // 1. signal the source semaphore and fence with an empty submit
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s_src;
    if ((r = vkQueueSubmit(q, 1, &si, f_src)) != VK_SUCCESS) { rc = int(fail("submit signal", r)); goto out; }
    if ((r = vkWaitForFences(dev, 1, &f_src, VK_TRUE, timeout)) != VK_SUCCESS) { rc = int(fail("source fence", r)); goto out; }

    // 2. export both as SYNC_FD (the payloads move into the fds)
    {
        VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        gi.semaphore = s_src;
        gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int sfd = -1;
        if ((r = gsfd(dev, &gi, &sfd)) != VK_SUCCESS || sfd < 0) { rc = int(fail("export semaphore sync fd", r)); goto out; }

        VkFenceGetFdInfoKHR fgi{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
        fgi.fence = f_src;
        fgi.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        int ffd = -1;
        if ((r = gffd(dev, &fgi, &ffd)) != VK_SUCCESS || ffd < 0) { rc = int(fail("export fence sync fd", r)); goto out; }

        // 3. import the fds into the targets (temporary: the targets go back afterwards)
        VkImportSemaphoreFdInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        ii.semaphore = s_dst;
        ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        ii.fd = sfd;  // the driver owns the fd after a successful import
        if ((r = isfd(dev, &ii)) != VK_SUCCESS) { rc = int(fail("import semaphore sync fd", r)); goto out; }

        VkImportFenceFdInfoKHR fii{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
        fii.fence = f_dst;
        fii.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
        fii.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        fii.fd = ffd;
        if ((r = iffd(dev, &fii)) != VK_SUCCESS) { rc = int(fail("import fence sync fd", r)); goto out; }
    }

    // 4. the imported fence and semaphore must both be signalled: wait on each
    if ((r = vkWaitForFences(dev, 1, &f_dst, VK_TRUE, timeout)) != VK_SUCCESS) { rc = int(fail("imported fence", r)); goto out; }
    {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo wi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        wi.waitSemaphoreCount = 1;
        wi.pWaitSemaphores = &s_dst;
        wi.pWaitDstStageMask = &stage;
        if ((r = vkQueueSubmit(q, 1, &wi, f_done)) != VK_SUCCESS) { rc = int(fail("submit wait on imported semaphore", r)); goto out; }
        if ((r = vkWaitForFences(dev, 1, &f_done, VK_TRUE, timeout)) != VK_SUCCESS) { rc = int(fail("imported semaphore", r)); goto out; }
    }

out:
    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, f_done, nullptr);
    vkDestroyFence(dev, f_dst, nullptr);
    vkDestroyFence(dev, f_src, nullptr);
    vkDestroySemaphore(dev, s_wait_dst, nullptr);
    vkDestroySemaphore(dev, s_dst, nullptr);
    vkDestroySemaphore(dev, s_src, nullptr);
    return rc;
}

int main() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vrdual-devext-test";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { fprintf(stderr, "no instance\n"); return 1; }

    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(inst, &n, nullptr) != VK_SUCCESS || !n) { fprintf(stderr, "no physical device\n"); return 1; }
    std::vector<VkPhysicalDevice> pds(n);
    if (vkEnumeratePhysicalDevices(inst, &n, pds.data()) != VK_SUCCESS) { fprintf(stderr, "enumerate physical devices\n"); return 1; }
    VkPhysicalDevice pd = pds[0];  // the layer's selected GPU is first

    static const char* const want[] = {
        "VK_KHR_dedicated_allocation", "VK_KHR_external_fence", "VK_KHR_external_memory",
        "VK_KHR_external_semaphore", "VK_KHR_get_memory_requirements2", "VK_KHR_external_memory_fd",
        "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd", "VK_KHR_image_format_list",
        "VK_KHR_timeline_semaphore",
    };

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp.data());
    uint32_t qf = 0;
    for (uint32_t i = 0; i < qn; ++i)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = uint32_t(sizeof(want) / sizeof(want[0]));
    dci.ppEnabledExtensionNames = want;

    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(pd, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) return fail("vkCreateDevice with the OpenXR device extensions", r);

    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qf, 0, &q);
    int rc = sync_fd_roundtrip(dev, q);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    if (rc) return rc;
    printf("device with %u OpenXR device extensions, sync fd round trip: ok\n", dci.enabledExtensionCount);
    return 0;
}
