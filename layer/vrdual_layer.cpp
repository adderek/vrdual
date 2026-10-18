// VK_LAYER_VRDUAL_stereo — app-unaware per-eye rendering layer (PoC).
//
// Two modes:
//   * per-eye (VRDUAL_EYE=left|right): one process renders one eye on one GPU,
//     two copies of the app run side by side (optionally frame-locked);
//   * mirror (VRDUAL_EYE=both): ONE process. The app sees only the left GPU;
//     the layer creates a hidden twin device on the right GPU straight through
//     the driver (ICD) and replays every device call there with the other
//     eye's parameters (vrdual_mirror.hpp).
// In both modes the layer
//   * hides every GPU but the app's one,
//   * patches vertex shaders so the scene is rendered from the eye position,
//   * copies each presented frame into /dev/shm for the compositor.
//
// Config: ~/.config/vrdual/vrdual.conf + [app:NAME] profile + VRDUAL_* env,
// resolved at vkCreateInstance (needs the app name). Per-process: VRDUAL_EYE,
// VRDUAL_SESSION, VRDUAL_GPU. Live values (IPD, anchor, ...) come from the
// session control block and can change every frame — see README.md.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common/vrdual_ctl.hpp"
#include "../common/vrdual_shm.h"
#include "spirv_patch.hpp"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace mirror {
#include "mirror_dispatch.hpp"
}
using mirror::Dispatch;

// ---------------------------------------------------------------- config ----
struct Config {
    int eye = -1;              // -1 = stereo off, 0 left, 1 right (mirror: 0 = the app's device)
    bool mirror = false;       // VRDUAL_EYE=both: one process, twin device on mirror_gpu
    bool skip_w1 = true;
    bool export_frames = true;
    bool lockstep = true;
    bool live = true;          // wanted; per device it also needs bufferDeviceAddress
    int lockstep_timeout_ms = 250;
    std::string gpu;           // "" | index | pci:BBBB:BB:DD.F | substring of name
    std::string mirror_gpu;    // mirror mode: right eye GPU
    std::string session = "default";
    bool log = true;
    vrdual_ctl* ctl = nullptr; // live session values
};
static Config g_cfg;

static void logf(const char* fmt, ...) {
    if (!g_cfg.log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[vrdual%s] ", g_cfg.mirror ? ":LR" : g_cfg.eye == 0 ? ":L" : g_cfg.eye == 1 ? ":R" : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char* env(const char* k, const char* def) {
    const char* v = getenv(k);
    return v && *v ? v : def;
}

static std::string exe_name() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    const char* s = strrchr(buf, '/');
    return s ? s + 1 : buf;
}

static void load_config(const VkApplicationInfo* app) {
    Config& c = g_cfg;
    std::string eye = env("VRDUAL_EYE", "");
    c.mirror = eye == "both" || eye == "mirror" || eye == "LR";
    c.eye = c.mirror || eye == "left" || eye == "L" || eye == "0" ? 0 : eye == "right" || eye == "R" || eye == "1" ? 1 : -1;
    c.session = env("VRDUAL_SESSION", "default");
    std::string app_name = app && app->pApplicationName ? app->pApplicationName : "";
    vrdual::Settings st = vrdual::load_settings({app_name, exe_name()});
    c.log = st.i("log") != 0;
    c.skip_w1 = st.i("skip_w1") != 0;
    c.export_frames = st.i("export") != 0;
    c.lockstep = st.i("lockstep") != 0 && !c.mirror;
    c.lockstep_timeout_ms = st.i("lockstep_timeout_ms");
    c.live = st.i("live") != 0;
    c.gpu = env("VRDUAL_GPU", c.eye == 0 ? st.s("left_gpu").c_str() : c.eye == 1 ? st.s("right_gpu").c_str() : "");
    if (c.mirror) c.mirror_gpu = st.s("right_gpu");
    if (c.eye < 0) {
        logf("VRDUAL_EYE unset: stereo/export off, GPU filter only (gpu='%s')", c.gpu.c_str());
        return;
    }
    bool created = false;
    c.ctl = vrdual::ctl_open(c.session, &st, &created);
    if (!c.ctl) { logf("cannot open control block %s", vrdual::ctl_path(c.session).c_str()); return; }
    vrdual_ctl v = vrdual::ctl_read(c.ctl);
    logf("app='%s' exe='%s' profile='%s' session=%s (%s control block)", app_name.c_str(), exe_name().c_str(),
         st.profile.c_str(), c.session.c_str(), created ? "seeded" : "joined");
    logf("eye=%s anchor=%s ipd=%.1fmm ipd_near=%.1fmm world_scale=%g hfov=%g skip_w1=%d gpu='%s'%s%s",
         c.mirror ? "both" : c.eye ? "right" : "left", vrdual_anchor_name(v.anchor), v.ipd * 1e3, v.ipd_near * 1e3,
         v.world_scale, v.hfov, c.skip_w1, c.gpu.c_str(), c.mirror ? " mirror_gpu=" : "", c.mirror_gpu.c_str());
}

// ------------------------------------------------------------ dispatch ------
template <typename T> static void* key(T h) { return *(void**)h; }

struct InstanceData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkEnumeratePhysicalDeviceGroups EnumeratePhysicalDeviceGroups;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    uint32_t api_version = VK_API_VERSION_1_0;
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    bool chosen_resolved = false;
};

// Host-visible readback buffer for one swapchain image.
struct Readback {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    bool coherent = true;
};

// Command buffer + sync for the readback copy, on the presenting queue family.
struct CopyRes {
    uint32_t family = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore sem = VK_NULL_HANDLE;
};

// Live eye params: vec4 in host-visible memory, address baked into shaders.
struct EyeBuf {
    int eye = -1;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    float* params = nullptr;
    uint64_t addr = 0;
    uint64_t ctl_seq = ~0ull;
    int patched = 0, skipped = 0;
};

struct SwapchainData {
    VkFormat format;
    VkExtent2D extent;
    std::vector<VkImage> images;
    uint32_t pixfmt = 0;  // 0 = not exportable
    Readback rb;
    // mirror: device B stand-ins for the swapchain images
    std::vector<VkImage> b_images;
    std::vector<VkDeviceMemory> b_mem;
    Readback b_rb;
};

namespace mirror { struct MDev; }
struct DeviceData;
namespace mirror {
static MDev* create_twin(DeviceData* dd, const VkDeviceCreateInfo* ci, bool bda, bool replay);
static void destroy_twin(DeviceData* dd);
static void create_swapchain_twin(DeviceData* dd, const VkSwapchainCreateInfoKHR* ci, struct ::SwapchainData& s);
static void destroy_swapchain_twin(DeviceData* dd, struct ::SwapchainData& s);
static void present_twin(DeviceData* dd, VkQueue queue, uint32_t family, const VkPresentInfoKHR* pi, struct ::SwapchainData* s, uint64_t frame, int own);
static PFN_vkVoidFunction lookup(const char* name);
}

struct DeviceData {
    VkDevice device;
    VkPhysicalDevice phys;
    InstanceData* inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
    Dispatch d;  // next layer / driver
    EyeBuf ep;
    std::unordered_map<VkQueue, uint32_t> queue_family;
    std::unordered_map<VkSwapchainKHR, SwapchainData> swapchains;
    CopyRes cr;
    uint64_t frame = 0;
    mirror::MDev* md = nullptr;  // mirror twin, if any
};

// Never destroyed: driver threads can still call in while static destructors run at exit.
static std::mutex& g_lock = *new std::mutex;
static std::unordered_map<void*, InstanceData*>& g_instances = *new std::unordered_map<void*, InstanceData*>;
static std::unordered_map<void*, DeviceData*>& g_devices = *new std::unordered_map<void*, DeviceData*>;

static InstanceData* inst_of(void* k) { std::lock_guard<std::mutex> l(g_lock); return g_instances[k]; }
static DeviceData* dev_of(void* k) {
    static thread_local void* last_k = nullptr;
    static thread_local DeviceData* last_d = nullptr;
    if (k == last_k && last_d) return last_d;
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_devices.find(k);
    last_k = k;
    last_d = it == g_devices.end() ? nullptr : it->second;
    return last_d;
}

// ------------------------------------------------------------- shm ---------
struct Shm {
    int fd = -1;
    vrdual_shm_header* hdr = nullptr;
    size_t bytes = 0;
};
static Shm g_out[2], g_peer;

static std::string shm_path(int eye) { return vrdual::shm_dir() + "/vrdual-" + g_cfg.session + "-" + std::to_string(eye); }

static bool shm_open_out(int eye, uint32_t w, uint32_t h, uint32_t pixfmt, const char* gpu_name, const char* pci) {
    Shm& o = g_out[eye];
    uint64_t slot = uint64_t(w) * h * 4;
    size_t bytes = vrdual_file_bytes(slot);
    if (o.hdr) { munmap(o.hdr, o.bytes); o.hdr = nullptr; }
    if (o.fd < 0) o.fd = open(shm_path(eye).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (o.fd < 0 || ftruncate(o.fd, bytes) != 0) { logf("shm open/ftruncate %s failed", shm_path(eye).c_str()); return false; }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, o.fd, 0);
    if (p == MAP_FAILED) { logf("mmap failed"); return false; }
    o.hdr = (vrdual_shm_header*)p;
    o.bytes = bytes;
    auto* hd = o.hdr;
    hd->magic = 0;  // invalid while re-initialising
    __sync_synchronize();
    hd->version = VRDUAL_VERSION;
    hd->width = w; hd->height = h; hd->stride = w * 4; hd->pixfmt = pixfmt; hd->eye = eye;
    hd->slot_bytes = slot;
    hd->last_frame = 0;
    for (auto& s : hd->slots) { s.seq = 0; s.frame = ~0ull; }
    snprintf(hd->gpu_name, sizeof hd->gpu_name, "%.63s", gpu_name);
    snprintf(hd->pci, sizeof hd->pci, "%.31s", pci);
    __sync_synchronize();
    hd->magic = VRDUAL_MAGIC;
    logf("exporting %ux%u fmt=%u to %s", w, h, pixfmt, shm_path(eye).c_str());
    return true;
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

static void shm_publish(int eye, uint64_t frame, const void* pixels, size_t n) {
    auto* hd = g_out[eye].hdr;
    if (!hd) return;
    auto& s = hd->slots[frame % VRDUAL_SLOTS];
    s.seq++;  // odd: writing
    __sync_synchronize();
    s.frame = frame;
    memcpy((char*)hd + VRDUAL_HDR_BYTES + (frame % VRDUAL_SLOTS) * hd->slot_bytes, pixels, n);
    s.present_ns = now_ns();
    __sync_synchronize();
    s.seq++;
    hd->last_frame = frame + 1;
}

// All swapchains of the process share one frame file per eye (wined3d keeps
// several: helper windows, an old fullscreen one, ...). The file belongs to the
// biggest swapchain that presented within the last second; the others skip
// export. Hold g_shm_lock from the claim to the publish so a re-map on one
// thread never races a copy on another.
static std::mutex& g_shm_lock = *new std::mutex;
static struct { uint64_t sc, area, last_ns; } g_owner;

static uint64_t sc_id(VkSwapchainKHR h) {
    if constexpr (std::is_pointer_v<VkSwapchainKHR>) return (uint64_t)(uintptr_t)h;
    else return (uint64_t)h;
}

enum { kNotOwner = 0, kOwner = 1, kNewOwner = 2 };  // kNewOwner: (re)open the frame files
static int shm_claim(uint64_t sc, VkExtent2D e) {
    uint64_t now = now_ns(), area = uint64_t(e.width) * e.height;
    if (g_owner.sc != sc && g_owner.sc && now - g_owner.last_ns < 1000000000ull && area <= g_owner.area) return kNotOwner;
    int r = g_owner.sc == sc ? kOwner : kNewOwner;
    g_owner = {sc, area, now};
    return r;
}

// Block until the other eye has published `frame` (or timeout). Keeps the two
// processes frame-locked so the compositor can always pair L[F] with R[F].
static void lockstep_wait(uint64_t frame) {
    if (!g_cfg.lockstep) return;
    static bool warned = false;
    uint64_t deadline = now_ns() + uint64_t(g_cfg.lockstep_timeout_ms) * 1000000ull;
    for (;;) {
        if (!g_peer.hdr) {
            int fd = open(shm_path(1 - g_cfg.eye).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size >= (off_t)VRDUAL_HDR_BYTES) {
                    void* p = mmap(nullptr, VRDUAL_HDR_BYTES, PROT_READ, MAP_SHARED, fd, 0);
                    if (p != MAP_FAILED) { g_peer.hdr = (vrdual_shm_header*)p; g_peer.bytes = VRDUAL_HDR_BYTES; }
                }
                close(fd);
            }
        }
        if (g_peer.hdr && g_peer.hdr->magic == VRDUAL_MAGIC && g_peer.hdr->last_frame >= frame + 1) {
            warned = false;
            return;
        }
        if (now_ns() > deadline) {
            if (!warned) logf("lockstep: peer eye not at frame %llu after %d ms, running free", (unsigned long long)frame, g_cfg.lockstep_timeout_ms);
            warned = true;
            return;
        }
        usleep(warned ? 0 : 200);
        if (warned) return;  // peer gone: don't stall every frame
    }
}

// ------------------------------------------------------------ instance -----
static std::string pci_string(PFN_vkGetPhysicalDeviceProperties2 gpp2, VkPhysicalDevice pd) {
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pci};
    if (!gpp2) return "";
    gpp2(pd, &p2);
    char b[32];
    snprintf(b, sizeof b, "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
    return b;
}
static std::string pci_string(InstanceData* id, VkPhysicalDevice pd) { return pci_string(id->GetPhysicalDeviceProperties2, pd); }

// "pci:BBBB:BB:DD.F" | "pci:BB:DD.F" | index | name substring
static bool gpu_matches(const std::string& want, uint32_t index, const char* name, const std::string& pci) {
    if (want.rfind("pci:", 0) == 0) {
        std::string w = want.substr(4);
        return pci == w || (pci.size() > 5 && pci.substr(5) == w);
    }
    if (!want.empty() && want.find_first_not_of("0123456789") == std::string::npos) return (uint32_t)atoi(want.c_str()) == index;
    return !want.empty() && strstr(name, want.c_str()) != nullptr;
}

// Picks the one physical device the application will be allowed to see.
static void resolve_gpu(InstanceData* id) {
    if (id->chosen_resolved) return;
    id->chosen_resolved = true;
    if (g_cfg.gpu.empty()) return;
    uint32_t n = 0;
    id->EnumeratePhysicalDevices(id->instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    id->EnumeratePhysicalDevices(id->instance, &n, all.data());
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        id->GetPhysicalDeviceProperties(all[i], &p);
        std::string pci = pci_string(id, all[i]);
        bool match = gpu_matches(g_cfg.gpu, i, p.deviceName, pci);
        logf("  physdev[%u] %s pci=%s%s", i, p.deviceName, pci.c_str(), match && !id->chosen ? "  <- selected" : "");
        if (match && !id->chosen) id->chosen = all[i];
    }
    if (!id->chosen) logf("VRDUAL_GPU='%s' matched nothing — exposing all GPUs", g_cfg.gpu.c_str());
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkInstance* out) {
    auto* lci = (VkLayerInstanceCreateInfo*)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerInstanceCreateInfo*)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    static std::once_flag once;
    std::call_once(once, load_config, ci->pApplicationInfo);

    auto* id = new InstanceData{};
    id->instance = *out;
    id->gipa = gipa;
    if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) id->api_version = ci->pApplicationInfo->apiVersion;
#define IFN(n) id->n = (PFN_vk##n)gipa(*out, "vk" #n)
    IFN(DestroyInstance); IFN(EnumeratePhysicalDevices); IFN(EnumeratePhysicalDeviceGroups);
    IFN(GetPhysicalDeviceProperties); IFN(GetPhysicalDeviceProperties2); IFN(GetPhysicalDeviceMemoryProperties);
    IFN(GetPhysicalDeviceFeatures); IFN(GetPhysicalDeviceFeatures2); IFN(EnumerateDeviceExtensionProperties);
#undef IFN
    if (!id->GetPhysicalDeviceProperties2) id->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
    if (!id->GetPhysicalDeviceFeatures2) id->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
    std::lock_guard<std::mutex> l(g_lock);
    g_instances[key(*out)] = id;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vrd_DestroyInstance(VkInstance inst, const VkAllocationCallbacks* alloc) {
    InstanceData* id = inst_of(key(inst));
    id->DestroyInstance(inst, alloc);
    std::lock_guard<std::mutex> l(g_lock);
    g_instances.erase(key(inst));
    delete id;
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_EnumeratePhysicalDevices(VkInstance inst, uint32_t* count, VkPhysicalDevice* devs) {
    InstanceData* id = inst_of(key(inst));
    resolve_gpu(id);
    if (!id->chosen) return id->EnumeratePhysicalDevices(inst, count, devs);
    if (!devs) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    devs[0] = id->chosen;
    *count = 1;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_EnumeratePhysicalDeviceGroups(VkInstance inst, uint32_t* count, VkPhysicalDeviceGroupProperties* groups) {
    InstanceData* id = inst_of(key(inst));
    resolve_gpu(id);
    if (!id->chosen) return id->EnumeratePhysicalDeviceGroups(inst, count, groups);
    if (!groups) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    groups[0].physicalDeviceCount = 1;
    groups[0].physicalDevices[0] = id->chosen;
    groups[0].subsetAllocation = VK_FALSE;
    *count = 1;
    return VK_SUCCESS;
}

// ------------------------------------------------- capability filtering ----
// With stereo on, the app must not use paths that bypass the vertex shader
// patch (shader objects, inline modules) or that the mirror cannot replay on a
// second device (device addresses, external memory, sparse, ...). Hiding them
// makes engines like Zink/DXVK pick their plain fallbacks.
static bool filtering() { return g_cfg.eye >= 0 && strcmp(env("VRDUAL_FILTER", "1"), "0"); }

static bool ext_hidden(const char* e) {
    // debugging aid: VRDUAL_KEEP_EXT=VK_EXT_a,VK_KHR_b leaves those visible
    if (const char* keep = getenv("VRDUAL_KEEP_EXT"); keep && strstr(keep, e)) return false;
    // maintenance5 inline shader modules: the mirror patches them on both
    // devices; the one-process-per-eye path only patches vkCreateShaderModule.
    // maintenance6 (*2 command variants) is plain replay. DXVK 3 needs both.
    if (!strcmp(e, "VK_KHR_maintenance5") || !strcmp(e, "VK_KHR_maintenance6")) return !g_cfg.mirror;
    static const char* const kDeny[] = {
        "VK_EXT_shader_object", "VK_EXT_descriptor_buffer", "VK_EXT_descriptor_heap",
        "VK_KHR_maintenance7", "VK_KHR_maintenance8",
        "VK_KHR_maintenance9", "VK_KHR_maintenance10", "VK_KHR_maintenance11",
        "VK_EXT_buffer_device_address", "VK_KHR_device_address_commands",
        "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd",
        "VK_EXT_external_memory_dma_buf", "VK_EXT_external_memory_host", "VK_EXT_image_drm_format_modifier",
        "VK_EXT_queue_family_foreign", "VK_EXT_external_memory_acquire_unmodified",
        "VK_EXT_mesh_shader", "VK_NV_mesh_shader", "VK_KHR_acceleration_structure", "VK_KHR_ray_tracing_pipeline",
        "VK_KHR_ray_query", "VK_KHR_ray_tracing_maintenance1", "VK_KHR_ray_tracing_position_fetch",
        "VK_KHR_deferred_host_operations", "VK_EXT_opacity_micromap", "VK_EXT_host_image_copy",
        "VK_EXT_map_memory_placed", "VK_KHR_map_memory2", "VK_EXT_shader_module_identifier", "VK_KHR_pipeline_binary",
        "VK_KHR_present_wait", "VK_KHR_present_wait2", "VK_KHR_present_id", "VK_KHR_present_id2",
        "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1", "VK_EXT_device_generated_commands",
        "VK_NV_device_generated_commands", "VK_NV_device_generated_commands_compute", "VK_AMD_anti_lag",
        "VK_EXT_pipeline_properties",
    };
    for (auto d : kDeny)
        if (!strcmp(e, d)) return true;
    return !strncmp(e, "VK_KHR_video", 12) || !strncmp(e, "VK_NV_ray", 9);
}

static void hide_core_features(VkPhysicalDeviceFeatures* f) {
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyImage3D = VK_FALSE;
    f->sparseResidency2Samples = f->sparseResidency4Samples = f->sparseResidency8Samples = VK_FALSE;
    f->sparseResidency16Samples = f->sparseResidencyAliased = VK_FALSE;
}

// Device addresses: fine with one GPU per process; the mirror needs them
// captured on A and replayed on B (and live params, which set that up).
static void hide_bda(VkBool32& bda, VkBool32& replay, VkBool32& multi) {
    if (g_cfg.mirror && !(g_cfg.live && replay)) bda = VK_FALSE;
    replay = multi = VK_FALSE;
}

static void hide_features(void* chain) {
    if (!strcmp(env("VRDUAL_HIDE_FEATURES", "1"), "0")) return;
    for (auto* s = (VkBaseOutStructure*)chain; s; s = s->pNext) {
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: hide_core_features(&((VkPhysicalDeviceFeatures2*)s)->features); break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            auto* f = (VkPhysicalDeviceVulkan12Features*)s;
            hide_bda(f->bufferDeviceAddress, f->bufferDeviceAddressCaptureReplay, f->bufferDeviceAddressMultiDevice);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
            auto* f = (VkPhysicalDeviceBufferDeviceAddressFeatures*)s;
            hide_bda(f->bufferDeviceAddress, f->bufferDeviceAddressCaptureReplay, f->bufferDeviceAddressMultiDevice);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT: {
            auto* f = (VkPhysicalDeviceBufferDeviceAddressFeaturesEXT*)s;
            f->bufferDeviceAddress = f->bufferDeviceAddressCaptureReplay = f->bufferDeviceAddressMultiDevice = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES: {
            auto* f = (VkPhysicalDeviceVulkan14Features*)s;
            if (!g_cfg.mirror) f->maintenance5 = f->maintenance6 = VK_FALSE;
            f->hostImageCopy = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT: ((VkPhysicalDeviceShaderObjectFeaturesEXT*)s)->shaderObject = VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT: {
            auto* f = (VkPhysicalDeviceDescriptorBufferFeaturesEXT*)s;
            f->descriptorBuffer = f->descriptorBufferCaptureReplay = f->descriptorBufferImageLayoutIgnored = f->descriptorBufferPushDescriptors = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES:
            if (!g_cfg.mirror) ((VkPhysicalDeviceMaintenance5Features*)s)->maintenance5 = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES: ((VkPhysicalDeviceHostImageCopyFeatures*)s)->hostImageCopy = VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT: {
            auto* f = (VkPhysicalDeviceMeshShaderFeaturesEXT*)s;
            f->taskShader = f->meshShader = f->multiviewMeshShader = f->primitiveFragmentShadingRateMeshShader = f->meshShaderQueries = VK_FALSE;
            break;
        }
        default: break;
        }
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* layer, uint32_t* count, VkExtensionProperties* props) {
    InstanceData* id = inst_of(key(pd));
    if (!filtering() || layer) return id->EnumerateDeviceExtensionProperties(pd, layer, count, props);
    uint32_t n = 0;
    VkResult r = id->EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> all(n);
    r = id->EnumerateDeviceExtensionProperties(pd, nullptr, &n, all.data());
    if (r < 0) return r;
    all.resize(n);
    all.erase(std::remove_if(all.begin(), all.end(), [](const VkExtensionProperties& e) { return ext_hidden(e.extensionName); }), all.end());
    if (!props) { *count = uint32_t(all.size()); return VK_SUCCESS; }
    uint32_t k = std::min<uint32_t>(*count, uint32_t(all.size()));
    std::copy(all.begin(), all.begin() + k, props);
    *count = k;
    return k < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static uint32_t clamp_api(uint32_t v) {
    if (strcmp(env("VRDUAL_CLAMP_API", "0"), "1")) return v;  // off: Zink refuses a 1.4 driver reporting 1.3
    return v > VK_API_VERSION_1_3 ? VK_MAKE_API_VERSION(0, 1, 3, VK_API_VERSION_PATCH(v)) : v;
}

static VKAPI_ATTR void VKAPI_CALL vrd_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
    inst_of(key(pd))->GetPhysicalDeviceProperties(pd, p);
    if (filtering()) p->apiVersion = clamp_api(p->apiVersion);
}

static VKAPI_ATTR void VKAPI_CALL vrd_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    inst_of(key(pd))->GetPhysicalDeviceProperties2(pd, p);
    if (filtering()) p->properties.apiVersion = clamp_api(p->properties.apiVersion);
}

static VKAPI_ATTR void VKAPI_CALL vrd_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    inst_of(key(pd))->GetPhysicalDeviceFeatures(pd, f);
    if (filtering()) hide_core_features(f);
}

static VKAPI_ATTR void VKAPI_CALL vrd_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f) {
    inst_of(key(pd))->GetPhysicalDeviceFeatures2(pd, f);
    if (filtering()) hide_features(f);
}

// -------------------------------------------------------------- device -----
// Can this device read eye params from a GPU buffer (live IPD)? Fills in what
// has to be added to the app's VkDeviceCreateInfo to make that work.
struct BdaPlan {
    bool ok = false;
    bool add_ext = false;    // append VK_KHR_buffer_device_address
    VkBool32* flag = nullptr;  // app's own feature struct field to flip (restored after)
    VkBool32* replay_flag = nullptr;  // same struct's bufferDeviceAddressCaptureReplay
    bool add_struct = false;   // otherwise chain our own feature struct
    bool replay = false;       // mirror: also enable capture/replay
};

static BdaPlan plan_bda(InstanceData* id, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci) {
    BdaPlan p;
    if (!g_cfg.live || g_cfg.eye < 0 || !id->GetPhysicalDeviceFeatures2) return p;
    VkPhysicalDeviceBufferDeviceAddressFeatures q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q};
    id->GetPhysicalDeviceFeatures2(pd, &f2);
    if (!q.bufferDeviceAddress) { logf("live params off: no bufferDeviceAddress"); return p; }
    p.replay = g_cfg.mirror && q.bufferDeviceAddressCaptureReplay;
    bool has_khr = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        if (!strcmp(ci->ppEnabledExtensionNames[i], "VK_EXT_buffer_device_address")) {
            logf("live params off: app uses VK_EXT_buffer_device_address");
            return p;
        }
        if (!strcmp(ci->ppEnabledExtensionNames[i], "VK_KHR_buffer_device_address")) has_khr = true;
    }
    p.add_ext = !has_khr;
    for (auto* s = (VkBaseOutStructure*)ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            p.flag = &((VkPhysicalDeviceVulkan12Features*)s)->bufferDeviceAddress;
            p.replay_flag = &((VkPhysicalDeviceVulkan12Features*)s)->bufferDeviceAddressCaptureReplay;
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            p.flag = &((VkPhysicalDeviceBufferDeviceAddressFeatures*)s)->bufferDeviceAddress;
            p.replay_flag = &((VkPhysicalDeviceBufferDeviceAddressFeatures*)s)->bufferDeviceAddressCaptureReplay;
        }
    }
    p.add_struct = !p.flag;
    p.ok = true;
    return p;
}

// Writes this eye's params from the control block into the GPU-visible vec4.
static void push_params(EyeBuf& ep) {
    if (!g_cfg.ctl || !ep.params) return;
    uint64_t seq = g_cfg.ctl->seq;
    if (seq == ep.ctl_seq) return;
    vrdual_ctl c = vrdual::ctl_read(g_cfg.ctl);
    vrdual::EyeParams e = vrdual::eye_params(c, ep.eye);
    memcpy(ep.params, e.v, sizeof e.v);
    if (ep.ctl_seq != ~0ull)
        logf("live[%s]: anchor=%s ipd=%.1fmm ipd_near=%.1fmm hfov=%g scale=%g -> clip_dx far %+.5f near %+.5f", ep.eye ? "R" : "L",
             vrdual_anchor_name(c.anchor), c.ipd * 1e3, c.ipd_near * 1e3, c.hfov, c.world_scale, e.v[0], e.v[1]);
    ep.ctl_seq = seq;
}

// First memory type allowed by `bits` having all of `want` (then `fallback`).
static int pick_memtype(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) {
    for (VkMemoryPropertyFlags w : {want, fallback})
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & w) == w) return int(i);
    return -1;
}

static void create_param_buffer(const Dispatch& d, VkDevice dev, const VkPhysicalDeviceMemoryProperties& mp, EyeBuf& ep) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (d.CreateBuffer(dev, &bci, nullptr, &ep.buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    d.GetBufferMemoryRequirements(dev, ep.buf, &mr);
    // Prefer VRAM the CPU can write through the BAR (ReBAR), else system memory.
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int best = pick_memtype(mp, mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size, uint32_t(best)};
    void* map = nullptr;
    if (best < 0 || d.AllocateMemory(dev, &ai, nullptr, &ep.mem) != VK_SUCCESS ||
        d.BindBufferMemory(dev, ep.buf, ep.mem, 0) != VK_SUCCESS ||
        d.MapMemory(dev, ep.mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        logf("live params off: param buffer allocation failed");
        return;
    }
    VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, ep.buf};
    ep.addr = d.GetBufferDeviceAddress(dev, &bai);
    ep.params = (float*)map;
    push_params(ep);
    logf("live params at device address 0x%llx (%s eye, memory type %d)", (unsigned long long)ep.addr, ep.eye ? "right" : "left", best);
}

static void free_param_buffer(const Dispatch& d, VkDevice dev, EyeBuf& ep) {
    if (ep.buf) d.DestroyBuffer(dev, ep.buf, nullptr);
    if (ep.mem) d.FreeMemory(dev, ep.mem, nullptr);
    ep.buf = VK_NULL_HANDLE; ep.mem = VK_NULL_HANDLE; ep.params = nullptr;
}

// ------------------------------------------------------------- shaders -----
// Patched copy of a vertex shader for ep's eye; false = use the original.
static bool spv_patch_for(EyeBuf& ep, const VkShaderModuleCreateInfo* ci, std::vector<uint32_t>& code) {
    if (ep.eye < 0 || !g_cfg.ctl) return false;
    spv::PatchOptions o;
    o.skip_w1 = g_cfg.skip_w1;
    if (ep.params) {
        o.addr = ep.addr;  // live: shader reads params every draw
    } else {
        vrdual::EyeParams e = vrdual::eye_params(vrdual::ctl_read(g_cfg.ctl), ep.eye);
        memcpy(o.params, e.v, sizeof e.v);
        if (o.params[0] == 0 && o.params[1] == 0) return false;  // anchor eye
    }
    code.assign(ci->pCode, ci->pCode + ci->codeSize / 4);
    spv::PatchResult pr = spv::patch_vertex(code, o);
    if (!pr.patched) {
        ep.skipped++;
        if (pr.why != "no vertex entry point") logf("shader module not patched: %s", pr.why.c_str());
        return false;
    }
    ep.patched++;
    return true;
}

// Patches a vertex shader for ep's eye and creates it with `create`.
static VkResult create_patched_module(EyeBuf& ep, PFN_vkCreateShaderModule create, VkDevice dev, const VkShaderModuleCreateInfo* ci,
                                      const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    std::vector<uint32_t> code;
    if (spv_patch_for(ep, ci, code)) {
        VkShaderModuleCreateInfo ci2 = *ci;
        ci2.codeSize = code.size() * 4;
        ci2.pCode = code.data();
        return create(dev, &ci2, alloc, out);
    }
    return create(dev, ci, alloc, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkDevice* out) {
    InstanceData* id = inst_of(key(pd));
    auto* lci = (VkLayerDeviceCreateInfo*)ci->pNext;
    PFN_vkSetDeviceLoaderData set_data = nullptr;
    VkLayerDeviceCreateInfo* link = nullptr;
    for (auto* p = lci; p; p = (VkLayerDeviceCreateInfo*)p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
        if (p->function == VK_LAYER_LINK_INFO && !link) link = p;
        if (p->function == VK_LOADER_DATA_CALLBACK) set_data = p->u.pfnSetDeviceLoaderData;
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateDevice)gipa(id->instance, "vkCreateDevice");

    // Enable bufferDeviceAddress for the live eye params, without disturbing the app.
    BdaPlan bda = plan_bda(id, pd, ci);
    VkDeviceCreateInfo ci2 = *ci;
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    VkPhysicalDeviceBufferDeviceAddressFeatures ours{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkBool32 saved = VK_FALSE, saved_replay = VK_FALSE;
    if (bda.ok) {
        if (bda.add_ext) { exts.push_back("VK_KHR_buffer_device_address"); ci2.enabledExtensionCount = uint32_t(exts.size()); ci2.ppEnabledExtensionNames = exts.data(); }
        if (bda.flag) { saved = *bda.flag; *bda.flag = VK_TRUE; }
        if (bda.replay_flag) { saved_replay = *bda.replay_flag; *bda.replay_flag |= bda.replay; }
        if (bda.add_struct) { ours.bufferDeviceAddress = VK_TRUE; ours.bufferDeviceAddressCaptureReplay = bda.replay; ours.pNext = (void*)ci->pNext; ci2.pNext = &ours; }
    }
    VkResult r = create(pd, &ci2, alloc, out);
    if (bda.flag) *bda.flag = saved;
    if (bda.replay_flag) *bda.replay_flag = saved_replay;
    if (r != VK_SUCCESS && bda.ok) {
        logf("vkCreateDevice with bufferDeviceAddress failed (%d), retrying without live params", r);
        bda.ok = bda.replay = false;
        r = create(pd, ci, alloc, out);
    }
    if (r != VK_SUCCESS) return r;

    auto* dd = new DeviceData{};
    dd->device = *out;
    dd->phys = pd;
    dd->inst = id;
    dd->gdpa = gdpa;
    dd->set_loader_data = set_data;
    dd->ep.eye = g_cfg.eye;
    mirror::load_dispatch(dd->d, gdpa, *out);
    VkPhysicalDeviceProperties p;
    id->GetPhysicalDeviceProperties(pd, &p);
    logf("device created on %s (pci %s)", p.deviceName, pci_string(id, pd).c_str());
    if (getenv("VRDUAL_TRACE_EXTS")) for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) logf("  ext %s", ci->ppEnabledExtensionNames[i]);
    VkPhysicalDeviceMemoryProperties mp;
    id->GetPhysicalDeviceMemoryProperties(pd, &mp);
    if (bda.ok && dd->d.GetBufferDeviceAddress) create_param_buffer(dd->d, dd->device, mp, dd->ep);
    else if (g_cfg.eye >= 0) {
        logf("baked mode: IPD changes need an app restart");
        if (g_cfg.ctl) vrdual::ctl_write(g_cfg.ctl, [](vrdual_ctl& c) { c.live = 0; });
    }
    if (g_cfg.mirror) dd->md = mirror::create_twin(dd, ci, bda.ok, bda.replay);
    std::lock_guard<std::mutex> l(g_lock);
    g_devices[key(*out)] = dd;
    return VK_SUCCESS;
}

static void free_readback(const Dispatch& d, VkDevice dev, Readback& rb) {
    if (rb.buf) d.DestroyBuffer(dev, rb.buf, nullptr);
    if (rb.mem) d.FreeMemory(dev, rb.mem, nullptr);
    rb = Readback{};
}

static void free_copy_res(const Dispatch& d, VkDevice dev, CopyRes& cr) {
    if (cr.fence) d.DestroyFence(dev, cr.fence, nullptr);
    if (cr.sem) d.DestroySemaphore(dev, cr.sem, nullptr);
    if (cr.pool) d.DestroyCommandPool(dev, cr.pool, nullptr);
    cr = CopyRes{};
}

static VKAPI_ATTR void VKAPI_CALL vrd_DestroyDevice(VkDevice dev, const VkAllocationCallbacks* alloc) {
    DeviceData* dd = dev_of(key(dev));
    logf("device destroyed: %d vertex shaders patched, %d modules untouched, %llu frames", dd->ep.patched, dd->ep.skipped, (unsigned long long)dd->frame);
    if (dd->md) mirror::destroy_twin(dd);
    for (auto& kv : dd->swapchains) free_readback(dd->d, dev, kv.second.rb);
    free_param_buffer(dd->d, dev, dd->ep);
    free_copy_res(dd->d, dev, dd->cr);
    dd->d.DestroyDevice(dev, alloc);
    std::lock_guard<std::mutex> l(g_lock);
    g_devices.erase(key(dev));
    delete dd;
}

static VKAPI_ATTR void VKAPI_CALL vrd_GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    DeviceData* dd = dev_of(key(dev));
    dd->d.GetDeviceQueue(dev, fam, idx, q);
    std::lock_guard<std::mutex> l(g_lock);
    dd->queue_family[*q] = fam;
}

static VKAPI_ATTR void VKAPI_CALL vrd_GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* q) {
    DeviceData* dd = dev_of(key(dev));
    dd->d.GetDeviceQueue2(dev, info, q);
    std::lock_guard<std::mutex> l(g_lock);
    dd->queue_family[*q] = info->queueFamilyIndex;
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    DeviceData* dd = dev_of(key(dev));
    return create_patched_module(dd->ep, dd->d.CreateShaderModule, dev, ci, alloc, out);
}

// ----------------------------------------------------------- swapchain -----
static uint32_t pixfmt_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VRDUAL_FMT_BGRA8;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VRDUAL_FMT_RGBA8;
    default: return 0;
    }
}

// Host-visible buffer the size of one 4-byte-per-pixel frame; HOST_CACHED
// preferred since the CPU reads it every frame.
static bool create_readback(const Dispatch& d, VkDevice dev, const VkPhysicalDeviceMemoryProperties& mp, VkExtent2D e, Readback& rb) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(e.width) * e.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (d.CreateBuffer(dev, &bci, nullptr, &rb.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    d.GetBufferMemoryRequirements(dev, rb.buf, &mr);
    int best = pick_memtype(mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, uint32_t(best)};
    if (best < 0 || d.AllocateMemory(dev, &ai, nullptr, &rb.mem) != VK_SUCCESS) {
        d.DestroyBuffer(dev, rb.buf, nullptr);
        rb = Readback{};
        return false;
    }
    rb.coherent = mp.memoryTypes[best].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    d.BindBufferMemory(dev, rb.buf, rb.mem, 0);
    d.MapMemory(dev, rb.mem, 0, VK_WHOLE_SIZE, 0, &rb.map);
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceData* dd = dev_of(key(dev));
    bool exp = g_cfg.eye >= 0 && g_cfg.export_frames;
    VkSwapchainCreateInfoKHR ci2 = *ci;
    if (exp) ci2.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = dd->d.CreateSwapchainKHR(dev, &ci2, alloc, out);
    if (r != VK_SUCCESS || (!exp && !dd->md)) return r;

    SwapchainData s{};
    s.format = ci->imageFormat;
    s.extent = ci->imageExtent;
    s.pixfmt = exp ? pixfmt_of(ci->imageFormat) : 0;
    uint32_t n = 0;
    dd->d.GetSwapchainImagesKHR(dev, *out, &n, nullptr);
    s.images.resize(n);
    dd->d.GetSwapchainImagesKHR(dev, *out, &n, s.images.data());
    if (exp && !s.pixfmt) logf("swapchain format %d not exportable (PoC handles 8-bit RGBA/BGRA only)", ci->imageFormat);
    if (s.pixfmt) {
        VkPhysicalDeviceMemoryProperties mp;
        dd->inst->GetPhysicalDeviceMemoryProperties(dd->phys, &mp);
        if (!create_readback(dd->d, dev, mp, s.extent, s.rb)) {
            logf("readback alloc failed — export disabled");
            s.pixfmt = 0;
        }  // the frame file is opened by the first present (shm_claim)
    }
    if (dd->md) mirror::create_swapchain_twin(dd, ci, s);
    std::lock_guard<std::mutex> l(g_lock);
    dd->swapchains[*out] = std::move(s);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vrd_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    DeviceData* dd = dev_of(key(dev));
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = dd->swapchains.find(sc);
        if (it != dd->swapchains.end()) {
            if (dd->cr.fence) dd->d.WaitForFences(dev, 1, &dd->cr.fence, VK_TRUE, UINT64_MAX);
            free_readback(dd->d, dev, it->second.rb);
            if (dd->md) mirror::destroy_swapchain_twin(dd, it->second);
            dd->swapchains.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> l(g_shm_lock);
        if (g_owner.sc == sc_id(sc)) g_owner.sc = 0;
    }
    dd->d.DestroySwapchainKHR(dev, sc, alloc);
}

static bool ensure_copy_res(const Dispatch& d, VkDevice dev, PFN_vkSetDeviceLoaderData sld, uint32_t family, CopyRes& cr) {
    if (cr.pool && cr.family == family) return true;
    if (cr.pool) return false;  // PoC: one presenting queue family
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (d.CreateCommandPool(dev, &pci, nullptr, &cr.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cr.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    d.AllocateCommandBuffers(dev, &ai, &cr.cmd);
    // Layer-allocated dispatchable object: must carry the loader's dispatch
    // pointer (not needed on the mirror device, which bypasses the loader).
    if (sld) sld(dev, cr.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    d.CreateFence(dev, &fci, nullptr, &cr.fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    d.CreateSemaphore(dev, &sci, nullptr, &cr.sem);
    cr.family = family;
    return true;
}

// Copies a presentable image into rb, waiting on `waits` first; optionally
// signals cr.sem for the real present. Synchronous: returns after the copy.
static bool copy_to_readback(const Dispatch& d, VkDevice dev, VkQueue queue, CopyRes& cr, VkImage img, VkExtent2D ext, Readback& rb,
                             uint32_t nwait, const VkSemaphore* waits, bool signal) {
    VkCommandBuffer cb = cr.cmd;
    d.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {ext.width, ext.height, 1};
    d.CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &reg);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    d.EndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stages(nwait, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = nwait;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = &cr.sem;
    d.ResetFences(dev, 1, &cr.fence);
    VkResult r = d.QueueSubmit(queue, 1, &si, cr.fence);
    if (r != VK_SUCCESS) { logf("copy submit failed: %d", r); return false; }
    // PoC: synchronous readback. Costs latency; a real build double-buffers.
    d.WaitForFences(dev, 1, &cr.fence, VK_TRUE, UINT64_MAX);
    if (!rb.coherent) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, rb.mem, 0, VK_WHOLE_SIZE};
        d.InvalidateMappedMemoryRanges(dev, 1, &mr);
    }
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL vrd_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    DeviceData* dd = dev_of(key(queue));
    push_params(dd->ep);  // live IPD: picked up by the next frame's draws
    SwapchainData* s = nullptr;
    uint32_t family;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = pi->swapchainCount ? dd->swapchains.find(pi->pSwapchains[0]) : dd->swapchains.end();
        if (it != dd->swapchains.end()) s = &it->second;
        family = dd->queue_family.count(queue) ? dd->queue_family[queue] : 0;
    }
    uint64_t frame = dd->frame;
    std::unique_lock<std::mutex> el(g_shm_lock);
    int own = g_cfg.eye >= 0 && g_cfg.export_frames && s && s->pixfmt ? shm_claim(sc_id(pi->pSwapchains[0]), s->extent) : kNotOwner;
    if (own == kNewOwner) {
        VkPhysicalDeviceProperties p;
        dd->inst->GetPhysicalDeviceProperties(dd->phys, &p);
        shm_open_out(g_cfg.eye, s->extent.width, s->extent.height, s->pixfmt, p.deviceName, pci_string(dd->inst, dd->phys).c_str());
    }
    if (dd->md) mirror::present_twin(dd, queue, family, pi, s, frame, own);
    if (!own || !ensure_copy_res(dd->d, dd->device, dd->set_loader_data, family, dd->cr)) {
        el.unlock();
        return dd->d.QueuePresentKHR(queue, pi);
    }

    // The copy consumes the app's "render finished" semaphores; present then
    // waits on ours instead.
    if (!copy_to_readback(dd->d, dd->device, queue, dd->cr, s->images[pi->pImageIndices[0]], s->extent, s->rb,
                          pi->waitSemaphoreCount, pi->pWaitSemaphores, true)) {
        el.unlock();
        return dd->d.QueuePresentKHR(queue, pi);
    }
    dd->frame++;
    shm_publish(g_cfg.eye, frame, s->rb.map, size_t(s->extent.width) * s->extent.height * 4);
    el.unlock();
    lockstep_wait(frame);

    VkPresentInfoKHR pi2 = *pi;
    pi2.waitSemaphoreCount = 1;
    pi2.pWaitSemaphores = &dd->cr.sem;
    return dd->d.QueuePresentKHR(queue, &pi2);
}

#include "vrdual_mirror.hpp"

// ---------------------------------------------------------- proc addrs -----
static PFN_vkVoidFunction intercept_instance(const char* name) {
#define I(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)vrd_##n
    I(CreateInstance); I(DestroyInstance); I(EnumeratePhysicalDevices); I(EnumeratePhysicalDeviceGroups);
    I(CreateDevice); I(EnumerateDeviceExtensionProperties); I(GetPhysicalDeviceProperties);
    I(GetPhysicalDeviceProperties2); I(GetPhysicalDeviceFeatures); I(GetPhysicalDeviceFeatures2);
#undef I
    if (!strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR")) return (PFN_vkVoidFunction)vrd_EnumeratePhysicalDeviceGroups;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)vrd_GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)vrd_GetPhysicalDeviceFeatures2;
    return nullptr;
}

// Device-level functions of the per-eye mode (also used by mirror for A).
static PFN_vkVoidFunction intercept_device(const char* name) {
#define I(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)vrd_##n
    I(DestroyDevice); I(GetDeviceQueue); I(GetDeviceQueue2); I(CreateShaderModule);
    I(CreateSwapchainKHR); I(DestroySwapchainKHR); I(QueuePresentKHR);
#undef I
    return nullptr;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vrdual_GetDeviceProcAddr(VkDevice dev, const char* name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vrdual_GetInstanceProcAddr(VkInstance inst, const char* name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vrdual_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vrdual_GetDeviceProcAddr;
    if (auto f = intercept_instance(name)) return f;
    if (auto f = intercept_device(name)) return f;
    if (!inst) return nullptr;
    InstanceData* id = inst_of(key(inst));
    return id ? id->gipa(inst, name) : nullptr;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vrdual_GetDeviceProcAddr(VkDevice dev, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vrdual_GetDeviceProcAddr;
    DeviceData* dd = dev ? dev_of(key(dev)) : nullptr;
    if (dd && dd->md)
        if (auto f = mirror::lookup(name)) return f;
    if (auto f = intercept_device(name)) return f;
    return dd ? dd->gdpa(dev, name) : nullptr;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vrdual_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = vrdual_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = vrdual_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
