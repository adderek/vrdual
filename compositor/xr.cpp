// vrdual-xr — shows the paired L/R frames in an HMD through OpenXR (WiVRn ->
// Meta Quest, Monado, ...). Same pairing as the SBS compositor.
//
// Each eye's frame is submitted as one view of an XrCompositionLayerProjection
// whose FOV is the game camera's FOV (hfov from the session, vertical from
// the aspect ratio), so the image covers exactly the angle it was rendered for.
//   --lock world  (default) the image stays where it was put (session start,
//                 or menu held 1 s: in front of the current gaze, level)
//   --lock head   the image follows the head (films); menu held 1 s freezes
//                 it in place, again: follows again
// Config `screen` (per game) picks the surface: full (above), or flat/curved:
// a virtual cinema screen `screen_fov` wide at `screen_dist`, one quad (or
// cylinder, XR_KHR_composition_layer_cylinder) per eye with that eye's image.
//
// No OpenXR loader needed: the active runtime (XR_RUNTIME_JSON, else
// ~/.config/openxr/1/active_runtime.json, /etc/xdg/..., /etc/...) is opened
// directly and negotiated with like the loader would.
//
// Runs across games: start it once, before or after the game. VRDUAL_SHM_DIR
// may list several dirs (a:b, `vrdual xr` passes /dev/shm and Flatpak Steam's);
// while no frames arrive it follows the dir where the session's frames are
// fresh. A new game process (frame header `producer`) reloads its settings.
//
//   vrdual-xr [--session S] [--lock world|head] [--frames N] [--swap]
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
XRFN(xrCreateInstance); XRFN(xrEnumerateInstanceExtensionProperties); XRFN(xrDestroyInstance); XRFN(xrGetInstanceProperties); XRFN(xrGetSystem); XRFN(xrGetSystemProperties);
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
    g_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
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
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    float o[4], fa[4] = {a.x, a.y, a.z, a.w}, fb[4] = {b.x, b.y, b.z, b.w};
    vrdual::qmul(fa, fb, o);
    return {o[0], o[1], o[2], o[3]};
}
static XrVector3f vadd(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static XrVector3f vsub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
// Head orientation without roll (yaw, then pitch): a screen placed with it is
// level even if the head was tilted; looking straight up/down (lying) the yaw
// comes from the head's up vector.
static XrQuaternionf level(XrQuaternionf q) {
    XrVector3f f = qrot(q, {0, 0, -1}), u = qrot(q, {0, 1, 0});
    float pitch = asinf(std::fmax(-1.f, std::fmin(1.f, f.y)));
    float yaw = cosf(pitch) > 0.1f ? atan2f(-f.x, -f.z) : pitch > 0 ? atan2f(u.x, u.z) : atan2f(-u.x, -u.z);
    return qmul({0, sinf(yaw / 2), 0, cosf(yaw / 2)}, {sinf(pitch / 2), 0, 0, cosf(pitch / 2)});
}

// Surface the picture is shown on (config `screen`).
enum { SCREEN_FULL, SCREEN_FLAT, SCREEN_CURVED };
struct Screen {
    int mode = SCREEN_FULL;
    float dist = 2.5f, fov = 60;  // meters, degrees (flat/curved)
    float sx = 1, sy = 1;         // screen_scale_x/y: start scale (grab changes it)
};
// The picture this frame, in the layer's space. frame: picture centre (flat),
// viewer (full: picture at infinity; curved: cylinder axis), oriented like
// the picture (-z = forward). Full: tan_h/tan_v half-FOV tangents; flat:
// w x h meters; curved: angle (rad) wide, h meters high.
struct ScreenGeom {
    int mode = SCREEN_FULL;
    XrPosef frame{{0, 0, 0, 1}, {0, 0, 0}};
    float dist = 1, tan_h = 1, tan_v = 1, w = 1, h = 1, angle = 1;
};
static ScreenGeom screen_geom(const Screen& sc, XrPosef base, const vrdual::ScreenGrab& g, float hfov, float aspect) {
    ScreenGeom G;
    G.mode = sc.mode;
    G.dist = sc.dist;
    float o[4];
    g.orbit_q(o);
    base.orientation = qmul(base.orientation, {o[0], o[1], o[2], o[3]});  // left stick while grabbing
    XrQuaternionf q = qmul({g.rot[0], g.rot[1], g.rot[2], g.rot[3]}, base.orientation);
    XrVector3f off{g.off[0], g.off[1], g.off[2]};
    XrVector3f centre = vadd(vadd(base.position, qrot(base.orientation, {0, 0, -sc.dist})), off);
    float t = tanf(sc.fov * float(M_PI) / 360.0f);
    if (sc.mode == SCREEN_FULL) {
        float th = tanf(hfov * float(M_PI) / 360.0f);
        G.tan_h = th * g.sx;
        G.tan_v = th / aspect * g.sy;
        G.frame = {q, base.position};
    } else if (sc.mode == SCREEN_FLAT) {
        G.w = 2 * sc.dist * t * g.sx;
        G.h = 2 * sc.dist * t / aspect * g.sy;
        G.frame = {q, centre};
    } else {
        float a = sc.fov * float(M_PI) / 180.0f;
        G.angle = std::fmin(a * g.sx, 6.2f);
        G.h = sc.dist * a / aspect * g.sy;
        G.frame = {q, vsub(centre, qrot(q, {0, 0, -sc.dist}))};
    }
    return G;
}
static int screen_parse(const std::string& s) { return s == "flat" ? SCREEN_FLAT : s == "curved" ? SCREEN_CURVED : SCREEN_FULL; }
static const char* screen_name(int m) { return m == SCREEN_FLAT ? "flat" : m == SCREEN_CURVED ? "curved" : "full"; }

static void yaw_pitch(XrQuaternionf q, float& yaw, float& pitch) {  // degrees, yaw positive = left
    XrVector3f f = qrot(q, {0, 0, -1});
    yaw = atan2f(-f.x, -f.z) * 57.29578f;
    pitch = asinf(std::fmax(-1.f, std::fmin(1.f, f.y))) * 57.29578f;
}

struct XrInput {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction stick, trigger, grip, click, a, b, x, y, menu, aim, grip_pose;
    XrPath hand[2];
    XrSpace aim_space = XR_NULL_HANDLE, grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
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
        grip_pose = make("grip_pose", XR_ACTION_TYPE_POSE_INPUT, true);
        if (!ok) return;
        std::vector<XrActionSuggestedBinding> sb;
        for (const char* h : {"left", "right"}) {
            std::string p = std::string("/user/hand/") + h + "/input/";
            sb.push_back({stick, path((p + "thumbstick").c_str())});
            sb.push_back({trigger, path((p + "trigger/value").c_str())});
            sb.push_back({grip, path((p + "squeeze/value").c_str())});
            sb.push_back({click, path((p + "thumbstick/click").c_str())});
            sb.push_back({grip_pose, path((p + "grip/pose").c_str())});
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
        bool spaces = xrCreateActionSpace(s, &asi, &aim_space) == XR_SUCCESS;
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo gsi{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, grip_pose, hand[h], {{0, 0, 0, 1}, {0, 0, 0}}};
            spaces &= xrCreateActionSpace(s, &gsi, &grip_space[h]) == XR_SUCCESS;
        }
        if (r != XR_SUCCESS || !spaces || xrAttachSessionActionSets(s, &at) != XR_SUCCESS) {
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
    // screen: the space the game picture is shown in (layer space), G where it is.
    vrdual::Controls read(XrSession s, XrTime t, XrSpace local, XrSpace view, XrSpace screen, const ScreenGeom& G) {
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
            const XrQuaternionf& fq = G.frame.orientation;
            XrQuaternionf inv{-fq.x, -fq.y, -fq.z, fq.w};
            XrVector3f d = qrot(inv, qrot(loc.pose.orientation, {0, 0, -1}));
            XrVector3f p = qrot(inv, vsub(loc.pose.position, G.frame.position));
            bool hit = false;
            if (G.mode == SCREEN_FULL && d.z < -1e-3f) {  // picture at infinity: direction only
                c.aim_u = 0.5f + 0.5f * (d.x / -d.z) / G.tan_h;
                c.aim_v = 0.5f - 0.5f * (d.y / -d.z) / G.tan_v;
                hit = true;
            } else if (G.mode == SCREEN_FLAT && d.z < -1e-3f) {  // ray ∩ plane z = 0 of the frame
                float k = -p.z / d.z;
                c.aim_u = 0.5f + (p.x + k * d.x) / G.w;
                c.aim_v = 0.5f - (p.y + k * d.y) / G.h;
                hit = k > 0;
            } else if (G.mode == SCREEN_CURVED) {  // ray ∩ cylinder x² + z² = dist² (far root: hand inside)
                float a = d.x * d.x + d.z * d.z, b = 2 * (p.x * d.x + p.z * d.z), cc = p.x * p.x + p.z * p.z - G.dist * G.dist;
                float disc = b * b - 4 * a * cc;
                if (a > 1e-6f && disc >= 0) {
                    float k = (-b + sqrtf(disc)) / (2 * a);
                    float x = p.x + k * d.x, y = p.y + k * d.y, z = p.z + k * d.z;
                    c.aim_u = 0.5f + atan2f(x, -z) / G.angle;
                    c.aim_v = 0.5f - y / G.h;
                    hit = k > 0;
                }
            }
            c.aim_ok = hit && c.aim_u >= 0 && c.aim_u <= 1 && c.aim_v >= 0 && c.aim_v <= 1;
        }
        c.hands_ok = true;
        for (int h = 0; h < 2; ++h) {
            loc = {XR_TYPE_SPACE_LOCATION};
            c.hands_ok &= xrLocateSpace(grip_space[h], screen, t, &loc) == XR_SUCCESS && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
            c.hand_p[h][0] = loc.pose.position.x; c.hand_p[h][1] = loc.pose.position.y; c.hand_p[h][2] = loc.pose.position.z;
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
    bool world = true, swap = false;
    long max_frames = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--session" && i + 1 < argc) session = argv[++i];
        else if (a == "--lock" && i + 1 < argc) world = std::string(argv[++i]) == "world";
        else if (a == "--frames" && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (a == "--swap") swap = true;
        else { fprintf(stderr, "usage: %s [--session S] [--lock world|head] [--frames N] [--swap]\n", argv[0]); return 2; }
    }
    if (!open_runtime()) return 1;

    // instance + system
    std::vector<const char*> exts{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    bool has_cyl = false;
    if (xrEnumerateInstanceExtensionProperties) {
        uint32_t ne = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &ne, nullptr);
        std::vector<XrExtensionProperties> ep(ne, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, ne, &ne, ep.data());
        for (auto& e : ep) has_cyl |= !strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    if (has_cyl) exts.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "vrdual");
    snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "vrdual");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.enabledExtensionNames = exts.data();
    fprintf(stderr, "[xr] connecting to the runtime (WiVRn answers once a headset is connected)...\n");
    if (XrResult r = xrCreateInstance(&ici, &g_inst); XR_FAILED(r)) {
        fprintf(stderr, "[xr] xrCreateInstance failed: %s — is the runtime's server running? (WiVRn: start `vrdual wivrn` first)\n", xr_str(r));
        return 1;
    }
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

    std::vector<std::string> dirs;
    {
        std::stringstream ss(vrdual::shm_dir());
        for (std::string d; std::getline(ss, d, ':');) if (!d.empty()) dirs.push_back(d);
        if (dirs.empty()) dirs.push_back("/dev/shm");
    }
    vrdual::Eye L{0, ""}, R{1, ""};
    EyeOut out[2];
    vrdual_ctl* ctl = nullptr;
    size_t dir_i = 0;
    auto use_dir = [&](size_t i) {
        dir_i = i;
        setenv("VRDUAL_SHM_DIR", dirs[i].c_str(), 1);  // ctl_path follows
        for (vrdual::Eye* e : {&L, &R}) {
            if (e->hdr) munmap(e->hdr, e->bytes);
            e->hdr = nullptr;
            e->w = e->h = 0;
            e->name = dirs[i] + "/vrdual-" + session + "-" + char('0' + e->eye);
        }
        if (ctl) munmap(ctl, sizeof *ctl);
        ctl = nullptr;
    };
    use_dir(0);
    double t_scan = 0;
    uint32_t producer = 0;  // frame header of the game whose settings are loaded
    // anchored (world lock, or head lock frozen): the picture lives in LOCAL
    // space at world_base (between the eyes, level), captured on the next
    // valid view after have_world is cleared
    XrPosef world_base{{0, 0, 0, 1}, {0, 0, 0}};
    bool have_world = false, frozen = false;
    Screen scr;
    vrdual::ScreenGrab grab;  // both grips: drag / turn / scale the picture
    int grab_recentres = 0;
    float hfov_now = 90;
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
    for (auto& d : dirs) fprintf(stderr, "[xr] watching %s/vrdual-%s-{0,1}\n", d.c_str(), session.c_str());

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
        XrCompositionLayerQuad quad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
        XrCompositionLayerCylinderKHR cyl[2] = {{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
        // No new game frame for 1 s (game exited or hung): show black. Submitting
        // no layer is not enough, WiVRn's headset keeps the last streamed image.
        bool fresh = vrdual::eye_map(L) && vrdual::eye_map(R) && std::max(vrdual::eye_age_s(L), vrdual::eye_age_s(R)) < 1.0;
        if (!fresh && vrdual::now_s() - t_scan > 1) {  // game in another dir (host vs Flatpak)?
            t_scan = vrdual::now_s();
            struct stat st;  // Steam restarted = new Flatpak /dev/shm: the old mapping is dead
            if (L.hdr && stat(L.name.c_str(), &st) == 0 && st.st_ino != L.ino) use_dir(dir_i);
            for (size_t i = 0; i < dirs.size(); ++i) {
                if (i == dir_i) continue;
                vrdual::Eye p{0, dirs[i] + "/vrdual-" + session + "-0"};
                bool there = vrdual::eye_map(p) && vrdual::eye_age_s(p) < 1.0;
                if (p.hdr) munmap(p.hdr, p.bytes);
                if (there) {
                    fprintf(stderr, "[xr] session '%s' found in %s\n", session.c_str(), dirs[i].c_str());
                    use_dir(i);
                    break;
                }
            }
        }
        if (int(fresh) != live) {
            fprintf(stderr, fresh ? "[xr] game frames arriving\n" : "[xr] no game frames: showing nothing\n");
            live = fresh;
        }
        if (fresh && (L.hdr->app != input_app || L.hdr->producer != producer)) {  // other game, or the same one restarted
            producer = L.hdr->producer;
            if (ctl) munmap(ctl, sizeof *ctl);  // the new game may have a new block
            ctl = nullptr;
            input_app.assign(L.hdr->app, strnlen(L.hdr->app, sizeof L.hdr->app));
            vrdual::InputCfg ic = vrdual::load_input_cfg(input_app);
            mapper.reset(ic.input != "off" || ic.head_look != "off" || ic.lean ? new vrdual::Mapper(ic) : nullptr);
            fprintf(stderr, "[xr] app '%s': input=%s mouse_mode=%s head_look=%s head_lean=%s\n", input_app.c_str(), ic.input.c_str(),
                    ic.mouse_mode.c_str(), ic.head_look.c_str(), ic.lean ? "on" : "off");
            icfg = ic;
            vrdual::Settings st = vrdual::load_settings({input_app});
            scr.mode = screen_parse(st.s("screen"));
            scr.dist = std::max(0.2f, st.f("screen_dist"));
            scr.fov = std::clamp(st.f("screen_fov"), 10.0f, 170.0f);
            if (scr.mode == SCREEN_CURVED && !has_cyl) {
                fprintf(stderr, "[xr] runtime has no %s: curved screen shown flat\n", XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
                scr.mode = SCREEN_FLAT;
            }
            scr.sx = std::clamp(st.f("screen_scale_x"), 0.1f, 10.0f);
            scr.sy = std::clamp(st.f("screen_scale_y"), 0.1f, 10.0f);
            grab = vrdual::ScreenGrab{};
            grab.sx = scr.sx;
            grab.sy = scr.sy;
            grab_recentres = 0;
            if (scr.mode == SCREEN_FULL) fprintf(stderr, "[xr] screen: full (game FOV), scale %g x %g\n", scr.sx, scr.sy);
            else fprintf(stderr, "[xr] screen: %s, %g deg wide at %g m, scale %g x %g\n", screen_name(scr.mode), scr.fov, scr.dist, scr.sx, scr.sy);
            lean_ref = false;
            recentres = 0;
        }
        double t_now = vrdual::now_s();
        bool anchored = world || frozen;
        float aspect = float(L.w ? L.w : 16) / float(L.h ? L.h : 9);
        XrPosef base = anchored && have_world ? world_base : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
        if (xin.ok && fresh) {  // only while a game shows frames: never drive the bare desktop
            ScreenGeom G = screen_geom(scr, base, grab, hfov_now, aspect);
            vrdual::Controls c = xin.read(s, fs.predictedDisplayTime, local_space, view_space, anchored ? local_space : view_space, G);
            bool was = grab.both;
            memcpy(grab.basis, &base.orientation, sizeof grab.basis);
            if (grab.step(c, t_now - t_input)) c.lg = c.rg = c.lt = c.rt = c.lx = c.ly = c.rx = c.ry = 0;  // grabbing: the game doesn't see them
            if (was && !grab.both)
                fprintf(stderr, "[xr] screen moved: offset %+.2f %+.2f %+.2f m, orbit %+.0f %+.0f deg, scale %.2f x %.2f (screen_scale_x/y to keep)\n",
                        grab.off[0], grab.off[1], grab.off[2], grab.orbit[0] * 57.29578f, grab.orbit[1] * 57.29578f, grab.sx, grab.sy);
            if (grab.recentres != grab_recentres) {  // menu held 1 s
                grab_recentres = grab.recentres;
                if (world) {
                    have_world = false;  // in front of the current gaze
                    grab.reset_pose();
                    fprintf(stderr, "[xr] screen recentred to the current gaze\n");
                } else {
                    frozen = !frozen;
                    have_world = false;
                    fprintf(stderr, frozen ? "[xr] screen frozen in place\n" : "[xr] screen follows the head\n");
                }
            }
            if (mapper) uinput.emit(mapper->step(c, t_now - t_input));
            if (mapper && icfg.lean && c.head_ok) {
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
                anchored = world || frozen;
                XrSpace space = anchored ? local_space : view_space;
                XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, fs.predictedDisplayTime, space};
                XrViewState vs{XR_TYPE_VIEW_STATE};
                XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t nv = 0;
                xrLocateViews(s, &vli, &vs, 2, &nv, views);
                bool valid = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
                // Headset off / tracking lost: runtimes may return zero quaternions,
                // which xrEndFrame rejects. Submit nothing this frame.
                if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) goto end_frame;
                if (anchored && valid && !have_world) {
                    const XrVector3f &a = views[0].pose.position, &b = views[1].pose.position;
                    world_base = {level(views[0].pose.orientation), {(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2}};
                    have_world = true;
                }
                if (!ctl) ctl = vrdual::ctl_open(session, nullptr);
                float hfov = hfov_now = ctl ? vrdual::ctl_read(ctl).hfov : 90.0f;
                XrPosef at = anchored && have_world ? world_base : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
                ScreenGeom G = screen_geom(scr, at, grab, hfov, float(out[0].w) / float(out[0].h));
                float h = atanf(G.tan_h), v = atanf(G.tan_v);
                XrQuaternionf r{grab.rot[0], grab.rot[1], grab.rot[2], grab.rot[3]};
                float o[4];
                grab.orbit_q(o);
                for (int e = 0; e < 2; ++e) {
                    // anchored: level picture orientation, current eye positions (picture at infinity)
                    pv[e].pose = {anchored ? G.frame.orientation : qmul(r, qmul(views[e].pose.orientation, {o[0], o[1], o[2], o[3]})), views[e].pose.position};
                    pv[e].fov = {-h, h, v, -v};
                    pv[e].subImage.swapchain = out[e].sc;
                    pv[e].subImage.imageRect = {{0, 0}, {int32_t(out[e].w), int32_t(out[e].h)}};
                }
                proj.space = space;
                proj.viewCount = 2;
                proj.views = pv;
                for (int e = 0; e < 2; ++e) {  // cinema: same surface for both eyes, each eye sees its own image
                    XrEyeVisibility vis = e ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                    // 1 px inset: the runtime's sampler would otherwise blend the edge columns with
                    // the opposite edge / border (seam at the cylinder's left and right edge)
                    XrSwapchainSubImage si{out[e].sc, {{1, 1}, {int32_t(out[e].w) - 2, int32_t(out[e].h) - 2}}, 0};
                    quad[e].space = space;
                    quad[e].eyeVisibility = vis;
                    quad[e].subImage = si;
                    quad[e].pose = G.frame;
                    quad[e].size = {G.w, G.h};
                    cyl[e].space = space;
                    cyl[e].eyeVisibility = vis;
                    cyl[e].subImage = si;
                    cyl[e].pose = G.frame;
                    cyl[e].radius = G.dist;
                    cyl[e].centralAngle = G.angle;
                    cyl[e].aspectRatio = G.dist * G.angle / G.h;
                    if (scr.mode == SCREEN_FLAT) layers.push_back((XrCompositionLayerBaseHeader*)&quad[e]);
                    if (scr.mode == SCREEN_CURVED) layers.push_back((XrCompositionLayerBaseHeader*)&cyl[e]);
                }
                if (scr.mode == SCREEN_FULL) layers.push_back((XrCompositionLayerBaseHeader*)&proj);
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
