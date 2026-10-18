// vrdual-xr — shows the paired L/R frames in an HMD through OpenXR (WiVRn ->
// Meta Quest, Monado, ...). Same pairing as the SBS compositor.
//
// Each eye's frame is submitted as one view of an XrCompositionLayerProjection
// whose FOV is the game camera's FOV (hfov from the session, vertical from
// the aspect ratio), so the image covers exactly the angle it was rendered for.
//   --lock head   (default) the image follows the head: a stereo "screen"
//                 filling the game's FOV — no head tracking in the game yet
//   --lock world  the image stays where it was when the session started
//
// No OpenXR loader needed: the active runtime (XR_RUNTIME_JSON, else
// ~/.config/openxr/1/active_runtime.json, /etc/xdg/..., /etc/...) is opened
// directly and negotiated with like the loader would.
//
//   vrdual-xr [--session S] [--lock head|world] [--frames N] [--swap]
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
// clang-format off
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
// clang-format on

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../common/vrdual_ctl.hpp"
#include "../common/vrdual_frames.hpp"
#include "../common/vrdual_input.hpp"

static volatile sig_atomic_t g_quit = 0;
static PFN_xrGetInstanceProcAddr g_gipa;
static XrInstance g_inst = XR_NULL_HANDLE;

#define XRFN(n) static PFN_##n n
XRFN(xrCreateInstance); XRFN(xrDestroyInstance); XRFN(xrGetInstanceProperties); XRFN(xrGetSystem); XRFN(xrGetSystemProperties);
XRFN(xrPollEvent); XRFN(xrResultToString); XRFN(xrCreateSession); XRFN(xrDestroySession); XRFN(xrBeginSession); XRFN(xrEndSession);
XRFN(xrRequestExitSession); XRFN(xrEnumerateViewConfigurationViews); XRFN(xrCreateReferenceSpace); XRFN(xrEnumerateSwapchainFormats);
XRFN(xrCreateSwapchain); XRFN(xrDestroySwapchain); XRFN(xrEnumerateSwapchainImages); XRFN(xrAcquireSwapchainImage);
XRFN(xrWaitSwapchainImage); XRFN(xrReleaseSwapchainImage); XRFN(xrWaitFrame); XRFN(xrBeginFrame); XRFN(xrEndFrame);
XRFN(xrLocateViews); XRFN(xrGetVulkanGraphicsRequirements2KHR); XRFN(xrCreateVulkanInstanceKHR);
XRFN(xrGetVulkanGraphicsDevice2KHR); XRFN(xrCreateVulkanDeviceKHR);
XRFN(xrStringToPath); XRFN(xrCreateActionSet); XRFN(xrCreateAction); XRFN(xrSuggestInteractionProfileBindings);
XRFN(xrAttachSessionActionSets); XRFN(xrSyncActions); XRFN(xrGetActionStateFloat); XRFN(xrGetActionStateBoolean);
XRFN(xrGetActionStateVector2f); XRFN(xrCreateActionSpace); XRFN(xrLocateSpace);
#undef XRFN

static void load_fns() {
#define L(n) g_gipa(g_inst, #n, (PFN_xrVoidFunction*)&n)
    L(xrDestroyInstance); L(xrGetInstanceProperties); L(xrGetSystem); L(xrGetSystemProperties); L(xrPollEvent); L(xrResultToString);
    L(xrCreateSession); L(xrDestroySession); L(xrBeginSession); L(xrEndSession); L(xrRequestExitSession);
    L(xrEnumerateViewConfigurationViews); L(xrCreateReferenceSpace); L(xrEnumerateSwapchainFormats); L(xrCreateSwapchain);
    L(xrDestroySwapchain); L(xrEnumerateSwapchainImages); L(xrAcquireSwapchainImage); L(xrWaitSwapchainImage);
    L(xrReleaseSwapchainImage); L(xrWaitFrame); L(xrBeginFrame); L(xrEndFrame); L(xrLocateViews);
    L(xrGetVulkanGraphicsRequirements2KHR); L(xrCreateVulkanInstanceKHR); L(xrGetVulkanGraphicsDevice2KHR); L(xrCreateVulkanDeviceKHR);
    L(xrStringToPath); L(xrCreateActionSet); L(xrCreateAction); L(xrSuggestInteractionProfileBindings);
    L(xrAttachSessionActionSets); L(xrSyncActions); L(xrGetActionStateFloat); L(xrGetActionStateBoolean);
    L(xrGetActionStateVector2f); L(xrCreateActionSpace); L(xrLocateSpace);
#undef L
}

static const char* xr_str(XrResult r) {
    static char b[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString && g_inst) xrResultToString(g_inst, r, b);
    else snprintf(b, sizeof b, "XrResult %d", r);
    return b;
}

#define XR_CHECK(x)                                                              \
    do {                                                                         \
        XrResult r_ = (x);                                                       \
        if (XR_FAILED(r_)) {                                                     \
            fprintf(stderr, "[xr] %s failed: %s\n", #x, xr_str(r_));             \
            return 1;                                                            \
        }                                                                        \
    } while (0)

// ------------------------------------------------------------- runtime -----
static std::string json_string(const std::string& doc, const std::string& key) {
    size_t k = doc.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t a = doc.find('"', doc.find(':', k) + 1), b = a == std::string::npos ? a : doc.find('"', a + 1);
    return a == std::string::npos || b == std::string::npos ? "" : doc.substr(a + 1, b - a - 1);
}

static std::string active_runtime_json() {
    std::vector<std::string> c;
    if (const char* e = getenv("XR_RUNTIME_JSON"); e && *e) c.push_back(e);
    const char* xch = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    c.push_back((xch && *xch ? std::string(xch) : std::string(home ? home : "") + "/.config") + "/openxr/1/active_runtime.json");
    std::string dirs = getenv("XDG_CONFIG_DIRS") && *getenv("XDG_CONFIG_DIRS") ? getenv("XDG_CONFIG_DIRS") : "/etc/xdg";
    std::stringstream ss(dirs);
    for (std::string d; std::getline(ss, d, ':');) c.push_back(d + "/openxr/1/active_runtime.json");
    c.push_back("/etc/openxr/1/active_runtime.json");
    for (auto& p : c)
        if (access(p.c_str(), R_OK) == 0) return p;
    return "";
}

static bool open_runtime() {
    std::string js = active_runtime_json();
    if (js.empty()) {
        fprintf(stderr, "[xr] no active OpenXR runtime (start WiVRn/Monado, or set XR_RUNTIME_JSON)\n");
        return false;
    }
    std::ifstream f(js);
    std::stringstream doc;
    doc << f.rdbuf();
    std::string lib = json_string(doc.str(), "library_path");
    if (!lib.empty() && lib[0] != '/' && lib.find('/') != std::string::npos) {
        char real[4096];
        std::string base = realpath(js.c_str(), real) ? real : js;
        lib = base.substr(0, base.rfind('/') + 1) + lib;
    }
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "[xr] %s: cannot load %s: %s\n", js.c_str(), lib.c_str(), dlerror()); return false; }
    auto neg = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(h, "xrNegotiateLoaderRuntimeInterface");
    if (!neg) { fprintf(stderr, "[xr] %s has no xrNegotiateLoaderRuntimeInterface\n", lib.c_str()); return false; }
    XrNegotiateLoaderInfo li{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li,
                             XR_CURRENT_LOADER_RUNTIME_VERSION, XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0),
                             XR_MAKE_VERSION(1, 1, 0xffffffffu)};
    XrNegotiateRuntimeRequest rr{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof rr};
    if (XR_FAILED(neg(&li, &rr)) || !rr.getInstanceProcAddr) { fprintf(stderr, "[xr] runtime negotiation failed\n"); return false; }
    g_gipa = rr.getInstanceProcAddr;
    g_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    fprintf(stderr, "[xr] runtime %s (%s), API %u.%u\n", lib.c_str(), js.c_str(), XR_VERSION_MAJOR(rr.runtimeApiVersion),
            XR_VERSION_MINOR(rr.runtimeApiVersion));
    return xrCreateInstance != nullptr;
}

// --------------------------------------------------------------- vulkan ----
struct Gpu {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp{};
};

struct EyeOut {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> images;
    uint32_t w = 0, h = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    bool swizzle = false;  // swap R/B while copying (frame BGRA, swapchain RGBA or the other way)
    bool released = false; // has content that can be shown
};

static int find_mem(const Gpu& g, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.mp.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
}

static bool make_staging(Gpu& g, EyeOut& e) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(e.w) * e.h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(g.dev, &bci, nullptr, &e.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, e.buf, &mr);
    int t = find_mem(g, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, uint32_t(t)};
    return t >= 0 && vkAllocateMemory(g.dev, &ai, nullptr, &e.mem) == VK_SUCCESS && vkBindBufferMemory(g.dev, e.buf, e.mem, 0) == VK_SUCCESS &&
           vkMapMemory(g.dev, e.mem, 0, VK_WHOLE_SIZE, 0, (void**)&e.map) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------- input ----
// Touch controllers (WiVRn's Quest controllers) -> vrdual::Controls.
static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {  // rotate v by q
    XrVector3f u{q.x, q.y, q.z};
    auto cross = [](XrVector3f a, XrVector3f b) { return XrVector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    XrVector3f t = cross(u, v);
    t = {2 * t.x, 2 * t.y, 2 * t.z};
    XrVector3f c = cross(u, t);
    return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}
static void yaw_pitch(XrQuaternionf q, float& yaw, float& pitch) {  // degrees, yaw positive = left
    XrVector3f f = qrot(q, {0, 0, -1});
    yaw = atan2f(-f.x, -f.z) * 57.29578f;
    pitch = asinf(std::fmax(-1.f, std::fmin(1.f, f.y))) * 57.29578f;
}

struct XrInput {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction stick, trigger, grip, click, a, b, x, y, menu, aim;
    XrPath hand[2];
    XrSpace aim_space = XR_NULL_HANDLE;
    bool ok = false;

    XrAction make(const char* name, XrActionType t, bool both) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
        snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", name);
        ci.actionType = t;
        ci.countSubactionPaths = both ? 2 : 0;
        ci.subactionPaths = both ? hand : nullptr;
        XrAction act = XR_NULL_HANDLE;
        if (xrCreateAction(set, &ci, &act) != XR_SUCCESS) ok = false;
        return act;
    }
    XrPath path(const char* p) {
        XrPath r = XR_NULL_PATH;
        xrStringToPath(g_inst, p, &r);
        return r;
    }
    void init(XrSession s) {
        XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
        snprintf(si.actionSetName, sizeof si.actionSetName, "vrdual");
        snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "vrdual");
        if (xrCreateActionSet(g_inst, &si, &set) != XR_SUCCESS) return;
        hand[0] = path("/user/hand/left");
        hand[1] = path("/user/hand/right");
        ok = true;
        stick = make("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
        trigger = make("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
        grip = make("grip", XR_ACTION_TYPE_FLOAT_INPUT, true);
        click = make("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
        a = make("a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        b = make("b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        x = make("x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        y = make("y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        menu = make("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        aim = make("aim", XR_ACTION_TYPE_POSE_INPUT, false);
        if (!ok) return;
        std::vector<XrActionSuggestedBinding> sb;
        for (const char* h : {"left", "right"}) {
            std::string p = std::string("/user/hand/") + h + "/input/";
            sb.push_back({stick, path((p + "thumbstick").c_str())});
            sb.push_back({trigger, path((p + "trigger/value").c_str())});
            sb.push_back({grip, path((p + "squeeze/value").c_str())});
            sb.push_back({click, path((p + "thumbstick/click").c_str())});
        }
        sb.push_back({a, path("/user/hand/right/input/a/click")});
        sb.push_back({b, path("/user/hand/right/input/b/click")});
        sb.push_back({x, path("/user/hand/left/input/x/click")});
        sb.push_back({y, path("/user/hand/left/input/y/click")});
        sb.push_back({menu, path("/user/hand/left/input/menu/click")});
        sb.push_back({aim, path("/user/hand/right/input/aim/pose")});
        XrInteractionProfileSuggestedBinding ipb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        ipb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        ipb.countSuggestedBindings = uint32_t(sb.size());
        ipb.suggestedBindings = sb.data();
        XrResult r = xrSuggestInteractionProfileBindings(g_inst, &ipb);
        XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, aim, XR_NULL_PATH, {{0, 0, 0, 1}, {0, 0, 0}}};
        XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
        if (r != XR_SUCCESS || xrCreateActionSpace(s, &asi, &aim_space) != XR_SUCCESS || xrAttachSessionActionSets(s, &at) != XR_SUCCESS) {
            fprintf(stderr, "[xr] controller actions unavailable (%s)\n", xr_str(r));
            ok = false;
            return;
        }
        fprintf(stderr, "[xr] controller actions attached (Touch profile)\n");
    }
    float f(XrSession s, XrAction act, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, act, hand[h]};
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        return xrGetActionStateFloat(s, &gi, &st) == XR_SUCCESS && st.isActive ? st.currentState : 0;
    }
    bool btn(XrSession s, XrAction act, int h = -1) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, act, h < 0 ? XR_NULL_PATH : hand[h]};
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        return xrGetActionStateBoolean(s, &gi, &st) == XR_SUCCESS && st.isActive && st.currentState;
    }
    XrVector2f v2(XrSession s, XrAction act, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, act, hand[h]};
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        return xrGetActionStateVector2f(s, &gi, &st) == XR_SUCCESS && st.isActive ? st.currentState : XrVector2f{0, 0};
    }
    // screen: the space the game picture is shown in, screen_q its orientation
    // there; tan_h/tan_v: half-FOV tangents of the picture.
    vrdual::Controls read(XrSession s, XrTime t, XrSpace local, XrSpace view, XrSpace screen, XrQuaternionf screen_q, float tan_h, float tan_v) {
        vrdual::Controls c;
        XrActiveActionSet aas{set, XR_NULL_PATH};
        XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &aas};
        if (xrSyncActions(s, &sy) != XR_SUCCESS) return c;
        XrVector2f l = v2(s, stick, 0), r = v2(s, stick, 1);
        c.lx = l.x; c.ly = l.y; c.rx = r.x; c.ry = r.y;
        c.lt = f(s, trigger, 0); c.rt = f(s, trigger, 1);
        c.lg = f(s, grip, 0); c.rg = f(s, grip, 1);
        c.l3 = btn(s, click, 0); c.r3 = btn(s, click, 1);
        c.a = btn(s, a); c.b = btn(s, b); c.x = btn(s, x); c.y = btn(s, y); c.menu = btn(s, menu);
        const XrSpaceLocationFlags rot = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(aim_space, local, t, &loc) == XR_SUCCESS && (loc.locationFlags & rot)) {
            c.hand_ok = true;
            yaw_pitch(loc.pose.orientation, c.hand_yaw, c.hand_pitch);
        }
        loc = {XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(aim_space, screen, t, &loc) == XR_SUCCESS && (loc.locationFlags & rot)) {
            XrQuaternionf inv{-screen_q.x, -screen_q.y, -screen_q.z, screen_q.w};
            XrVector3f d = qrot(inv, qrot(loc.pose.orientation, {0, 0, -1}));  // picture at infinity: direction only
            if (d.z < -1e-3f) {
                c.aim_u = 0.5f + 0.5f * (d.x / -d.z) / tan_h;
                c.aim_v = 0.5f - 0.5f * (d.y / -d.z) / tan_v;
                c.aim_ok = c.aim_u >= 0 && c.aim_u <= 1 && c.aim_v >= 0 && c.aim_v <= 1;
            }
        }
        loc = {XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(view, local, t, &loc) == XR_SUCCESS && (loc.locationFlags & rot)) {
            c.head_ok = true;
            yaw_pitch(loc.pose.orientation, c.head_yaw, c.head_pitch);
            c.head_px = loc.pose.position.x;
            c.head_py = loc.pose.position.y;
            c.head_pz = loc.pose.position.z;
        }
        return c;
    }
};

// ----------------------------------------------------------------- main ----
int main(int argc, char** argv) {
    std::string session = getenv("VRDUAL_SESSION") ? getenv("VRDUAL_SESSION") : "default";
    bool world = false, swap = false;
    long max_frames = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--session" && i + 1 < argc) session = argv[++i];
        else if (a == "--lock" && i + 1 < argc) world = std::string(argv[++i]) == "world";
        else if (a == "--frames" && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (a == "--swap") swap = true;
        else { fprintf(stderr, "usage: %s [--session S] [--lock head|world] [--frames N] [--swap]\n", argv[0]); return 2; }
    }
    if (!open_runtime()) return 1;

    // instance + system
    const char* exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "vrdual");
    snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "vrdual");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    fprintf(stderr, "[xr] connecting to the runtime (WiVRn answers once a headset is connected)...\n");
    XR_CHECK(xrCreateInstance(&ici, &g_inst));
    load_fns();
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    fprintf(stderr, "[xr] runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
            XR_VERSION_PATCH(ip.runtimeVersion));
    // Ctrl-C ends the session cleanly from here on (before, the default action
    // just exits). Runtime calls can block in IPC indefinitely (WiVRn with the
    // headset gone), so a second signal or 3 s without a clean exit force it.
    static volatile sig_atomic_t signals = 0;
    auto on_sig = [](int) {
        if (signals++) _exit(130);
        g_quit = 1;
        alarm(3);
    };
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGALRM, [](int) {
        static const char m[] = "[xr] runtime did not end the session in 3 s, exiting\n";
        if (write(2, m, sizeof m - 1)) {}
        _exit(130);
    });
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    for (int tries = 0;; ++tries) {
        XrResult r = xrGetSystem(g_inst, &sgi, &sys);
        if (r == XR_SUCCESS) break;
        if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE || g_quit) { fprintf(stderr, "[xr] xrGetSystem: %s\n", xr_str(r)); return 1; }
        if (tries == 0) fprintf(stderr, "[xr] no headset yet — put it on / start the client app; waiting...\n");
        usleep(500000);
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_inst, sys, &sp);
    fprintf(stderr, "[xr] system: %s\n", sp.systemName);

    // Vulkan on the GPU the runtime wants (WiVRn: the one it encodes on)
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(xrGetVulkanGraphicsRequirements2KHR(g_inst, sys, &req));
    Gpu g;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vrdual-xr";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xvi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvi.systemId = sys;
    xvi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xvi.vulkanCreateInfo = &vici;
    VkResult vr;
    XR_CHECK(xrCreateVulkanInstanceKHR(g_inst, &xvi, &g.inst, &vr));
    if (vr != VK_SUCCESS) { fprintf(stderr, "[xr] vkCreateInstance: %d\n", vr); return 1; }
    XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, nullptr, sys, g.inst};
    XR_CHECK(xrGetVulkanGraphicsDevice2KHR(g_inst, &gdi, &g.pd));
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(g.pd, &pp);
    vkGetPhysicalDeviceMemoryProperties(g.pd, &g.mp);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &nq, qf.data());
    for (uint32_t i = 0; i < nq; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.family = i; break; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, g.family, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    XrVulkanDeviceCreateInfoKHR xdi{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdi.systemId = sys;
    xdi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdi.vulkanPhysicalDevice = g.pd;
    xdi.vulkanCreateInfo = &dci;
    XR_CHECK(xrCreateVulkanDeviceKHR(g_inst, &xdi, &g.dev, &vr));
    if (vr != VK_SUCCESS) { fprintf(stderr, "[xr] vkCreateDevice: %d\n", vr); return 1; }
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g.family};
    vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(g.dev, &cai, &g.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g.dev, &fci, nullptr, &g.fence);
    fprintf(stderr, "[xr] Vulkan device: %s\n", pp.deviceName);

    // session + spaces
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, nullptr, g.inst, g.pd, g.dev, g.family, 0};
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO, &gb, 0, sys};
    XrSession s = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(g_inst, &sci, &s));
    XrSpace view_space, local_space;
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_VIEW, {{0, 0, 0, 1}, {0, 0, 0}}};
    XR_CHECK(xrCreateReferenceSpace(s, &rci, &view_space));
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(s, &rci, &local_space));
    XrInput xin;
    xin.init(s);
    uint32_t nvc = 0;
    xrEnumerateViewConfigurationViews(g_inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nvc, nullptr);
    std::vector<XrViewConfigurationView> vcv(nvc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nvc, &nvc, vcv.data());
    if (nvc == 2) fprintf(stderr, "[xr] HMD recommends %ux%u per eye\n", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(s, 0, &nf, nullptr);
    std::vector<int64_t> formats(nf);
    xrEnumerateSwapchainFormats(s, nf, &nf, formats.data());

    vrdual::Eye L{0, vrdual::shm_dir() + "/vrdual-" + session + "-0"}, R{1, vrdual::shm_dir() + "/vrdual-" + session + "-1"};
    EyeOut out[2];
    vrdual_ctl* ctl = nullptr;
    XrPosef world_pose[2];
    bool have_world = false;
    bool running = false;
    long submitted = 0, uploads = 0;
    uint64_t shown = 0;
    int live = -1;  // game frames fresh? -1 = not reported yet
    bool blanked = false;
    // input: profile follows the game in the frame file (hdr->app)
    std::string input_app = "\x01";
    std::unique_ptr<vrdual::Mapper> mapper;
    vrdual::Uinput uinput;
    double t_input = vrdual::now_s();
    // head_lean: head position relative to a reference, in the frame of the
    // reference yaw (x right, y up, z forward), soft-clamped -> control block
    vrdual::InputCfg icfg;
    bool lean_ref = false;
    float ref_p[3] = {}, ref_yaw = 0;
    int recentres = 0;
    float sent[3] = {NAN, NAN, NAN};  // first call always writes (clears an offset a killed run left)
    auto send_head = [&](float x, float y, float z) {
        if (fabsf(x - sent[0]) < 5e-4f && fabsf(y - sent[1]) < 5e-4f && fabsf(z - sent[2]) < 5e-4f) return;
        if (!ctl) ctl = vrdual::ctl_open(session, nullptr);
        if (!ctl) return;
        vrdual::ctl_write(ctl, [&](vrdual_ctl& k) { k.head_x = x; k.head_y = y; k.head_z = z; });
        sent[0] = x; sent[1] = y; sent[2] = z;
    };
    double t_report = vrdual::now_s();
    fprintf(stderr, "[xr] reading %s and %s\n", L.name.c_str(), R.name.c_str());

    while (true) {
        if (g_quit && running) { xrRequestExitSession(s); g_quit = 0; }
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        bool exit_loop = false;
        while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = (XrEventDataSessionStateChanged*)&ev;
                fprintf(stderr, "[xr] session state %d\n", sc->state);
                if (sc->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                    XR_CHECK(xrBeginSession(s, &bi));
                    running = true;
                } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(s);
                    running = false;
                } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                    exit_loop = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                exit_loop = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (exit_loop) break;
        if (!running) {
            if (g_quit) break;
            usleep(10000);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(s, nullptr, &fs));
        XR_CHECK(xrBeginFrame(s, nullptr));
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        // No new game frame for 1 s (game exited or hung): show black. Submitting
        // no layer is not enough, WiVRn's headset keeps the last streamed image.
        bool fresh = vrdual::eye_map(L) && vrdual::eye_map(R) && std::max(vrdual::eye_age_s(L), vrdual::eye_age_s(R)) < 1.0;
        if (int(fresh) != live) {
            fprintf(stderr, fresh ? "[xr] game frames arriving\n" : "[xr] no game frames: showing nothing\n");
            live = fresh;
        }
        if (fresh && L.hdr->app != input_app) {
            input_app.assign(L.hdr->app, strnlen(L.hdr->app, sizeof L.hdr->app));
            vrdual::InputCfg ic = vrdual::load_input_cfg(input_app);
            mapper.reset(ic.input != "off" || ic.head_look != "off" || ic.lean ? new vrdual::Mapper(ic) : nullptr);
            fprintf(stderr, "[xr] app '%s': input=%s mouse_mode=%s head_look=%s head_lean=%s\n", input_app.c_str(), ic.input.c_str(),
                    ic.mouse_mode.c_str(), ic.head_look.c_str(), ic.lean ? "on" : "off");
            icfg = ic;
            lean_ref = false;
            recentres = 0;
        }
        double t_now = vrdual::now_s();
        if (mapper && xin.ok && fresh) {  // only while a game shows frames: never drive the bare desktop
            if (!ctl) ctl = vrdual::ctl_open(session, nullptr);
            float th = tanf((ctl ? vrdual::ctl_read(ctl).hfov : 90.0f) * float(M_PI) / 360.0f);
            float tv = th * float(L.h) / float(L.w ? L.w : 1);
            XrQuaternionf sq = world && have_world ? world_pose[0].orientation : XrQuaternionf{0, 0, 0, 1};
            vrdual::Controls c = xin.read(s, fs.predictedDisplayTime, local_space, view_space, world ? local_space : view_space, sq, th, tv);
            uinput.emit(mapper->step(c, t_now - t_input));
            if (icfg.lean && c.head_ok) {
                if (!lean_ref || mapper->recentres != recentres) {
                    ref_p[0] = c.head_px; ref_p[1] = c.head_py; ref_p[2] = c.head_pz;
                    ref_yaw = c.head_yaw * float(M_PI) / 180.0f;
                    lean_ref = true;
                    recentres = mapper->recentres;
                }
                float d[3] = {c.head_px - ref_p[0], c.head_py - ref_p[1], c.head_pz - ref_p[2]};
                // reference frame: forward (-sin, 0, -cos)(yaw), right (cos, 0, -sin)(yaw)
                float rx = d[0] * cosf(ref_yaw) - d[2] * sinf(ref_yaw);
                float fz = -d[0] * sinf(ref_yaw) - d[2] * cosf(ref_yaw);
                float L = icfg.lean_max > 0 ? icfg.lean_max : 0.1f;
                send_head(L * tanhf(rx / L), L * tanhf(d[1] / L), L * tanhf(fz / L));
            }
        }
        if (!fresh || !mapper || !icfg.lean) send_head(0, 0, 0);  // nothing drives it: no stale offset
        t_input = t_now;
        if (fs.shouldRender && (fresh || (out[0].released && out[1].released))) {
            // newest paired frame (or black once the game is gone) -> swapchain images
            uint64_t F;
            bool blank = !fresh && !blanked;
            bool got = fresh && vrdual::pair_newest(L, R, shown, &F);
            if (blank) {
                for (EyeOut& o : out) memset(o.map, 0, size_t(o.w) * o.h * 4);
                blanked = true;
                fprintf(stderr, "[xr] blank frame submitted\n");
            }
            if (got) {
                shown = F + 1;
                blanked = false;
                vrdual::Eye* src[2] = {swap ? &R : &L, swap ? &L : &R};
                for (int e = 0; e < 2; ++e) {
                    EyeOut& o = out[e];
                    if (o.sc && (o.w != src[e]->w || o.h != src[e]->h)) {  // game resized: new swapchain
                        vkDeviceWaitIdle(g.dev);
                        xrDestroySwapchain(o.sc);
                        vkDestroyBuffer(g.dev, o.buf, nullptr);
                        vkFreeMemory(g.dev, o.mem, nullptr);
                        o = EyeOut{};
                    }
                    if (!o.sc) {
                        bool bgra = src[e]->fmt == VRDUAL_FMT_BGRA8;
                        int64_t want = bgra ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
                        int64_t other = bgra ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
                        int64_t fmt = 0;
                        for (int64_t f : formats) if (f == want) fmt = f;
                        if (!fmt) for (int64_t f : formats) if (f == other) fmt = f;
                        if (!fmt) { fprintf(stderr, "[xr] runtime offers no 8-bit sRGB swapchain format\n"); return 1; }
                        o.swizzle = fmt != want;
                        o.w = src[e]->w;
                        o.h = src[e]->h;
                        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                        ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                        ci.format = fmt;
                        ci.sampleCount = 1;
                        ci.width = o.w;
                        ci.height = o.h;
                        ci.faceCount = ci.arraySize = ci.mipCount = 1;
                        XR_CHECK(xrCreateSwapchain(s, &ci, &o.sc));
                        uint32_t ni = 0;
                        xrEnumerateSwapchainImages(o.sc, 0, &ni, nullptr);
                        o.images.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
                        xrEnumerateSwapchainImages(o.sc, ni, &ni, (XrSwapchainImageBaseHeader*)o.images.data());
                        if (!make_staging(g, o)) { fprintf(stderr, "[xr] staging buffer failed\n"); return 1; }
                        fprintf(stderr, "[xr] eye %d: %ux%u swapchain, %u images%s\n", e, o.w, o.h, ni, o.swizzle ? " (R/B swizzle)" : "");
                    }
                    size_t n = size_t(o.w) * o.h * 4;
                    if (!o.swizzle) {
                        memcpy(o.map, src[e]->px.data(), n);
                    } else {
                        const uint8_t* p = src[e]->px.data();
                        for (size_t i = 0; i < n; i += 4) { o.map[i] = p[i + 2]; o.map[i + 1] = p[i + 1]; o.map[i + 2] = p[i]; o.map[i + 3] = p[i + 3]; }
                    }
                }
            }
            if (got || blank) {
                uint32_t idx[2];
                for (int e = 0; e < 2; ++e) {
                    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XR_CHECK(xrAcquireSwapchainImage(out[e].sc, &ai, &idx[e]));
                    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
                    XR_CHECK(xrWaitSwapchainImage(out[e].sc, &wi));
                }
                vkResetCommandBuffer(g.cmd, 0);
                VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                vkBeginCommandBuffer(g.cmd, &cbi);
                for (int e = 0; e < 2; ++e) {
                    VkImage img = out[e].images[idx[e]].image;
                    barrier(g.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy reg{};
                    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    reg.imageExtent = {out[e].w, out[e].h, 1};
                    vkCmdCopyBufferToImage(g.cmd, out[e].buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
                    barrier(g.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                }
                vkEndCommandBuffer(g.cmd);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1;
                si.pCommandBuffers = &g.cmd;
                vkResetFences(g.dev, 1, &g.fence);
                vkQueueSubmit(g.queue, 1, &si, g.fence);
                vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
                for (int e = 0; e < 2; ++e) {
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    XR_CHECK(xrReleaseSwapchainImage(out[e].sc, &ri));
                    out[e].released = true;
                }
                uploads += got;
            }

            if (out[0].released && out[1].released) {
                XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, fs.predictedDisplayTime,
                                     world ? local_space : view_space};
                XrViewState vs{XR_TYPE_VIEW_STATE};
                XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t nv = 0;
                xrLocateViews(s, &vli, &vs, 2, &nv, views);
                bool valid = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
                // Headset off / tracking lost: runtimes may return zero quaternions,
                // which xrEndFrame rejects. Submit nothing this frame.
                if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) goto end_frame;
                if (world && valid && !have_world) {
                    world_pose[0] = views[0].pose;
                    world_pose[1] = views[1].pose;
                    have_world = true;
                }
                if (!ctl) ctl = vrdual::ctl_open(session, nullptr);
                float hfov = ctl ? vrdual::ctl_read(ctl).hfov : 90.0f;
                float h = hfov * float(M_PI) / 360.0f;
                float v = atanf(tanf(h) * float(out[0].h) / float(out[0].w));
                for (int e = 0; e < 2; ++e) {
                    pv[e].pose = world ? (have_world ? world_pose[e] : views[e].pose) : views[e].pose;
                    pv[e].fov = {-h, h, v, -v};
                    pv[e].subImage.swapchain = out[e].sc;
                    pv[e].subImage.imageRect = {{0, 0}, {int32_t(out[e].w), int32_t(out[e].h)}};
                }
                proj.space = world ? local_space : view_space;
                proj.viewCount = 2;
                proj.views = pv;
                layers.push_back((XrCompositionLayerBaseHeader*)&proj);
                if (submitted == 0)
                    fprintf(stderr, "[xr] first frame: eye offset L %+.4f R %+.4f m, game hfov %g deg\n", views[0].pose.position.x,
                            views[1].pose.position.x, hfov);
            }
        }
    end_frame:
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = uint32_t(layers.size());
        fe.layers = layers.data();
        XR_CHECK(xrEndFrame(s, &fe));
        if (!layers.empty()) ++submitted;
        double t = vrdual::now_s();
        if (t - t_report > 5) {
            fprintf(stderr, "[xr] %ld frames submitted, %ld new game frames (%.1f/s)\n", submitted, uploads, uploads / (t - t_report));
            uploads = 0;
            t_report = t;
        }
        if (max_frames >= 0 && submitted >= max_frames) { fprintf(stderr, "[xr] %ld frames submitted\n", submitted); g_quit = 1; }
    }
    vkDeviceWaitIdle(g.dev);
    for (auto& o : out)
        if (o.sc) xrDestroySwapchain(o.sc);
    send_head(0, 0, 0);
    xrDestroySession(s);
    xrDestroyInstance(g_inst);
    return 0;
}
