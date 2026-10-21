// vrdual-cube — a plain, stereo-UNAWARE Vulkan app used to exercise the layer.
// Cubes at several depths (parallax should shrink with distance) plus a HUD
// quad drawn with an orthographic matrix (w == 1, must NOT shift between eyes).
// Animation is driven by the frame index, so two instances stay in sync.
//
//   vrdual-cube [--frames N] [--size WxH] [--title T] [--scene cubes|calib] [--bda] [--extra-window]
//              [--clear-alpha A] [--late-verts N] [--verts-file] [--cached] [--readback]
//              [--swapchain-format a2b10g10r10|a2r10g10b10|rgba16f]
//
// --late-verts N: vertices zero until frame N, then written into the same
// mapping (the mirror must push writes long after the first sync).
// --swapchain-format: a 10-bit or FP16 swapchain (as UE4 games pick), exit 3
// if the surface does not offer it.
// --verts-file: the late vertex write is a pread(2) from a file straight into
// the mapping, i.e. done by the kernel (UE3 texture streaming does that).
// --readback: every frame vkCmdFillBuffer writes the frame number into a
// HOST_CACHED buffer, and a 4x4 image cleared to a frame colour is copied
// there with vkCmdCopyImageToBuffer; after the frame's fence the app checks
// both, the image's last texel too ("readback: N ok, M bad" every 100 frames).
// --cached: vertices in a 1 MB HOST_CACHED buffer (mirror: VRDUAL_CACHED_WP
// tracks whole pages).
// --clear-alpha A: background alpha (games leave 0; the layer exports 1).
// --extra-window: a second, small swapchain created after the main one and
// presented every frame too (like wined3d's helper windows).
//
// --bda: Vulkan 1.4 paths DXVK 3 relies on: vertices read through a buffer
// device address (no vertex input), memory mapped with vkMapMemory2, vertex
// shader passed inline in the pipeline (maintenance5, no VkShaderModule).
//
// --scene calib: static posts at 0.5, 1, 2, 5 and 20 m (eye height), for IPD
// calibration: look at one post, adjust until it sits at a believable distance
// and fuses without strain. Posts are scaled so all look equally wide.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const uint32_t kVert[] =
#include "cube.vert.inc"
    ;
static const uint32_t kVertBda[] =
#include "cube_bda.vert.inc"
    ;
static const uint32_t kFrag[] =
#include "cube.frag.inc"
    ;

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

struct M4 { float m[16]; };  // column-major
static M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            for (int k = 0; k < 4; ++k) r.m[c * 4 + rr] += a.m[k * 4 + rr] * b.m[c * 4 + k];
    return r;
}
static M4 ident() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static M4 translate(float x, float y, float z) { M4 r = ident(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static M4 scale(float x, float y, float z) { M4 r = ident(); r.m[0] = x; r.m[5] = y; r.m[10] = z; return r; }
static M4 rot_y(float a) { M4 r = ident(); r.m[0] = cosf(a); r.m[2] = -sinf(a); r.m[8] = sinf(a); r.m[10] = cosf(a); return r; }
static M4 rot_x(float a) { M4 r = ident(); r.m[5] = cosf(a); r.m[6] = sinf(a); r.m[9] = -sinf(a); r.m[10] = cosf(a); return r; }
// Vulkan clip space (y down, z 0..1), right-handed view looking down -Z.
// Horizontal FOV fixed at 90 deg => P[0][0] = 1 (matches the layer's default VRDUAL_HFOV).
static M4 perspective(float aspect, float n, float f) {
    M4 r{};
    r.m[0] = 1.0f;
    r.m[5] = -aspect;
    r.m[10] = f / (n - f);
    r.m[11] = -1;
    r.m[14] = n * f / (n - f);
    return r;
}

struct PC { M4 mvp; float color[4]; VkDeviceAddress verts; };

int main(int argc, char** argv) {
    int frames = 0, W = 960, H = 720;
    std::string title = "vrdual-cube", scene = "cubes";
    bool bda = false, extra = false;
    float clear_alpha = 1;
    int late_verts = 0;
    bool cached = false, verts_file = false, readback = false;
    std::string want_fmt;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--bda")) bda = true;
        else if (!strcmp(argv[i], "--extra-window")) extra = true;
        else if (!strcmp(argv[i], "--cached")) cached = true;
        else if (!strcmp(argv[i], "--verts-file")) verts_file = true;
        else if (!strcmp(argv[i], "--readback")) readback = true;
        else if (!strcmp(argv[i], "--swapchain-format") && i + 1 < argc) want_fmt = argv[++i];
        else if (!strcmp(argv[i], "--late-verts") && i + 1 < argc) late_verts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--clear-alpha") && i + 1 < argc) clear_alpha = (float)atof(argv[++i]);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow(title.c_str(), W, H, SDL_WINDOW_VULKAN);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    // --- instance / device -------------------------------------------------
    Uint32 nexts = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&nexts);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vrdual-cube";
    app.apiVersion = bda ? VK_API_VERSION_1_4 : VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = nexts;
    ici.ppEnabledExtensionNames = exts;
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    VkSurfaceKHR surf;
    if (!SDL_Vulkan_CreateSurface(win, inst, nullptr, &surf)) { fprintf(stderr, "surface: %s\n", SDL_GetError()); return 1; }

    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, nullptr);
    std::vector<VkPhysicalDevice> pds(npd);
    vkEnumeratePhysicalDevices(inst, &npd, pds.data());
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    fprintf(stderr, "vrdual-cube: app sees %u GPU(s), using %s\n", npd, props.deviceName);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
    uint32_t fam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { fam = i; break; }
    }
    if (fam == UINT32_MAX) { fprintf(stderr, "no graphics+present queue\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = fam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    f14.maintenance5 = VK_TRUE;
    f12.pNext = &f14;
    if (bda) {
        if (props.apiVersion < VK_API_VERSION_1_4) { fprintf(stderr, "--bda needs a Vulkan 1.4 device\n"); return 1; }
        dci.pNext = &f12;
    }
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, fam, 0, &queue);

    // --- swapchain ---------------------------------------------------------
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_SRGB) fmt = f;
    if (!want_fmt.empty()) {
        VkFormat w = want_fmt == "a2b10g10r10" ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : want_fmt == "a2r10g10b10" ? VK_FORMAT_A2R10G10B10_UNORM_PACK32
                   : want_fmt == "rgba16f" ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_UNDEFINED;
        bool found = false;
        for (auto& f : fmts) if (f.format == w && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f, found = true;
        if (!found) for (auto& f : fmts) if (f.format == w) fmt = f, found = true;
        if (!found) { fprintf(stderr, "vrdual-cube: swapchain format %s not offered by the surface\n", want_fmt.c_str()); return 3; }
        fprintf(stderr, "vrdual-cube: swapchain format %s (%d), colour space %d\n", want_fmt.c_str(), fmt.format, fmt.colorSpace);
    }
    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, nullptr);
    std::vector<VkPresentModeKHR> pms(npm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, pms.data());
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : pms) if (m == VK_PRESENT_MODE_MAILBOX_KHR) pm = m;  // don't stall when occluded
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{uint32_t(W), uint32_t(H)};
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surf;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = pm;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR sc;
    CK(vkCreateSwapchainKHR(dev, &sci, nullptr, &sc));
    uint32_t nimg = 0;
    vkGetSwapchainImagesKHR(dev, sc, &nimg, nullptr);
    std::vector<VkImage> imgs(nimg);
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs.data());

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    auto memtype = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        fprintf(stderr, "no memory type\n");
        exit(1);
    };

    // depth
    VkImageCreateInfo dic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dic.imageType = VK_IMAGE_TYPE_2D;
    dic.format = VK_FORMAT_D32_SFLOAT;
    dic.extent = {ext.width, ext.height, 1};
    dic.mipLevels = dic.arrayLayers = 1;
    dic.samples = VK_SAMPLE_COUNT_1_BIT;
    dic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage depth;
    CK(vkCreateImage(dev, &dic, nullptr, &depth));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, depth, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory dmem;
    CK(vkAllocateMemory(dev, &mai, nullptr, &dmem));
    vkBindImageMemory(dev, depth, dmem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.image = depth;
    vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depth_view;
    CK(vkCreateImageView(dev, &vci, nullptr, &depth_view));

    // render pass
    VkAttachmentDescription att[2]{};
    att[0].format = fmt.format;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[1] = att[0];
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0};
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VkRenderPass rp;
    CK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));

    std::vector<VkImageView> views(nimg);
    std::vector<VkFramebuffer> fbs(nimg);
    for (uint32_t i = 0; i < nimg; ++i) {
        vci.image = imgs[i];
        vci.format = fmt.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        CK(vkCreateImageView(dev, &vci, nullptr, &views[i]));
        VkImageView a[2] = {views[i], depth_view};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp;
        fci.attachmentCount = 2;
        fci.pAttachments = a;
        fci.width = ext.width;
        fci.height = ext.height;
        fci.layers = 1;
        CK(vkCreateFramebuffer(dev, &fci, nullptr, &fbs[i]));
    }

    // pipeline
    auto mkmod = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo c{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, bytes, code};
        VkShaderModule m;
        CK(vkCreateShaderModule(dev, &c, nullptr, &m));
        return m;
    };
    VkShaderModule vs = bda ? VK_NULL_HANDLE : mkmod(kVert, sizeof kVert), fs = mkmod(kFrag, sizeof kFrag);
    VkShaderModuleCreateInfo vsi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof kVertBda, kVertBda};
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, bda ? &vsi : nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkVertexInputBindingDescription vb{0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &vb, 2, va};
    if (bda) vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp{0, 0, float(ext.width), float(ext.height), 0, 1};
    VkRect2D scis{{0, 0}, ext};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &scis};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PC)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = st;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.layout = pl;
    gpci.renderPass = rp;
    VkPipeline pipe;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe));

    // cube mesh: 6 faces x 2 tris, pos + normal
    std::vector<float> verts;
    const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float C[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const int IDX[6] = {0, 1, 2, 0, 2, 3};
    for (auto& n : N) {
        // two tangents perpendicular to the face normal
        int ax = n[0] != 0 ? 0 : n[1] != 0 ? 1 : 2;
        float t1[3] = {0, 0, 0}, t2[3] = {0, 0, 0};
        t1[(ax + 1) % 3] = 1;
        t2[(ax + 2) % 3] = 1;
        for (int k : IDX) {
            for (int j = 0; j < 3; ++j) verts.push_back(0.5f * (n[j] + C[k][0] * t1[j] + C[k][1] * t2[j]));
            verts.insert(verts.end(), n, n + 3);
        }
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = cached ? std::max<size_t>(verts.size() * 4, 1u << 20) : verts.size() * 4;
    bci.usage = bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vbuf;
    CK(vkCreateBuffer(dev, &bci, nullptr, &vbuf));
    vkGetBufferMemoryRequirements(dev, vbuf, &mr);
    VkMemoryAllocateFlagsInfo mafi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, bda ? &mafi : nullptr, mr.size,
           memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                          (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0))};
    VkDeviceMemory vmem;
    CK(vkAllocateMemory(dev, &mai, nullptr, &vmem));
    vkBindBufferMemory(dev, vbuf, vmem, 0);
    void* p;
    VkDeviceAddress vaddr = 0;
    if (bda) {
        auto map2 = (PFN_vkMapMemory2)vkGetDeviceProcAddr(dev, "vkMapMemory2");
        VkMemoryMapInfo mmi{VK_STRUCTURE_TYPE_MEMORY_MAP_INFO, nullptr, 0, vmem, 0, VK_WHOLE_SIZE};
        CK(map2(dev, &mmi, &p));
        VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, vbuf};
        vaddr = vkGetBufferDeviceAddress(dev, &bdai);
        fprintf(stderr, "vrdual-cube: vertices at device address 0x%llx\n", (unsigned long long)vaddr);
    } else {
        vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, &p);
    }
    if (late_verts) memset(p, 0, verts.size() * 4);
    else memcpy(p, verts.data(), verts.size() * 4);
    int vfd = -1;  // --verts-file: unlinked temp file holding the vertices
    if (verts_file) {
        char tmpl[] = "/tmp/vrdual-cube-verts-XXXXXX";
        vfd = mkstemp(tmpl);
        if (vfd < 0 || write(vfd, verts.data(), verts.size() * 4) != ssize_t(verts.size() * 4)) { perror("verts file"); return 1; }
        unlink(tmpl);
    }
    // --readback: GPU fills, the app reads (one 256-byte slot per frame in flight)
    VkBuffer rbuf = VK_NULL_HANDLE;
    VkImage rimg = VK_NULL_HANDLE;
    volatile uint32_t* rbp = nullptr;
    uint64_t rb_ok = 0, rb_bad = 0;
    if (readback) {
        VkBufferCreateInfo rci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        rci.size = 4096;
        rci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        CK(vkCreateBuffer(dev, &rci, nullptr, &rbuf));
        VkMemoryRequirements rmr;
        vkGetBufferMemoryRequirements(dev, rbuf, &rmr);
        VkMemoryAllocateInfo rai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, rmr.size,
                                 memtype(rmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
        VkDeviceMemory rmem;
        CK(vkAllocateMemory(dev, &rai, nullptr, &rmem));
        vkBindBufferMemory(dev, rbuf, rmem, 0);
        void* rp;
        CK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &rp));
        rbp = (volatile uint32_t*)rp;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {4, 4, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CK(vkCreateImage(dev, &ici, nullptr, &rimg));
        VkMemoryRequirements imr;
        vkGetImageMemoryRequirements(dev, rimg, &imr);
        VkMemoryAllocateInfo iai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, imr.size, memtype(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory imem;
        CK(vkAllocateMemory(dev, &iai, nullptr, &imem));
        vkBindImageMemory(dev, rimg, imem, 0);
    }
    uint32_t nverts = uint32_t(verts.size() / 6);

    // --- frame loop --------------------------------------------------------
    const int FIF = 2;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam};
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    VkCommandBuffer cmds[FIF];
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, FIF};
    CK(vkAllocateCommandBuffers(dev, &cbai, cmds));
    VkSemaphore acquired[FIF];
    VkFence inflight[FIF];
    std::vector<VkSemaphore> rendered(nimg);
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < FIF; ++i) { CK(vkCreateSemaphore(dev, &semci, nullptr, &acquired[i])); CK(vkCreateFence(dev, &fenci, nullptr, &inflight[i])); }
    for (auto& s : rendered) CK(vkCreateSemaphore(dev, &semci, nullptr, &s));

    // Extra window: cleared by a layout transition only, presented every frame.
    VkSwapchainKHR xsc = VK_NULL_HANDLE;
    std::vector<VkImage> ximgs;
    std::vector<VkSemaphore> xrend;
    VkSemaphore xacq = VK_NULL_HANDLE;
    VkFence xfence = VK_NULL_HANDLE;
    VkCommandBuffer xcb = VK_NULL_HANDLE;
    if (extra) {
        SDL_Window* xwin = SDL_CreateWindow("vrdual-cube extra", 64, 48, SDL_WINDOW_VULKAN);
        VkSurfaceKHR xsurf;
        if (!xwin || !SDL_Vulkan_CreateSurface(xwin, inst, nullptr, &xsurf)) { fprintf(stderr, "extra window: %s\n", SDL_GetError()); return 1; }
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, fam, xsurf, &ok);
        VkSurfaceCapabilitiesKHR xcaps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, xsurf, &xcaps);
        VkSwapchainCreateInfoKHR x = sci;
        x.surface = xsurf;
        x.imageExtent = xcaps.currentExtent.width != UINT32_MAX ? xcaps.currentExtent : VkExtent2D{64, 48};
        x.minImageCount = std::max(xcaps.minImageCount, 2u);
        x.preTransform = xcaps.currentTransform;
        if (!ok) { fprintf(stderr, "extra window: queue cannot present\n"); return 1; }
        CK(vkCreateSwapchainKHR(dev, &x, nullptr, &xsc));
        uint32_t n = 0;
        vkGetSwapchainImagesKHR(dev, xsc, &n, nullptr);
        ximgs.resize(n);
        vkGetSwapchainImagesKHR(dev, xsc, &n, ximgs.data());
        xrend.resize(n);
        for (auto& s : xrend) CK(vkCreateSemaphore(dev, &semci, nullptr, &s));
        CK(vkCreateSemaphore(dev, &semci, nullptr, &xacq));
        CK(vkCreateFence(dev, &fenci, nullptr, &xfence));
        VkCommandBufferAllocateInfo xai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        CK(vkAllocateCommandBuffers(dev, &xai, &xcb));
        fprintf(stderr, "vrdual-cube: extra swapchain %ux%u\n", x.imageExtent.width, x.imageExtent.height);
    }
    auto present_extra = [&]() {
        vkWaitForFences(dev, 1, &xfence, VK_TRUE, UINT64_MAX);
        uint32_t xi;
        VkResult r = vkAcquireNextImageKHR(dev, xsc, UINT64_MAX, xacq, VK_NULL_HANDLE, &xi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
        vkResetFences(dev, 1, &xfence);
        vkResetCommandBuffer(xcb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(xcb, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = ximgs[xi];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(xcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(xcb);
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &xacq, &st, 1, &xcb, 1, &xrend[xi]};
        CK(vkQueueSubmit(queue, 1, &si, xfence));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &xrend[xi], 1, &xsc, &xi, nullptr};
        vkQueuePresentKHR(queue, &pi);
    };

    float aspect = float(ext.width) / float(ext.height);
    M4 proj = perspective(aspect, 0.1f, 100.0f);
    // Ortho for the HUD: x,y in [-1,1] directly, w stays 1.
    struct Obj { float x, y, z, sx, sy, r, g, b; };
    std::vector<Obj> objs;
    bool spin = scene != "calib";
    if (spin) {
        objs = {
            {-0.9f, -0.2f, -1.5f, 0.5f, 0.5f, 1.0f, 0.35f, 0.3f},   // near: big parallax
            {0.6f, 0.3f, -3.0f, 0.8f, 0.8f, 1.0f, 0.8f, 0.25f},
            {-1.5f, 0.8f, -6.0f, 1.2f, 1.2f, 0.3f, 0.85f, 0.4f},
            {2.5f, -0.5f, -12.0f, 2.0f, 2.0f, 0.3f, 0.6f, 1.0f},
            {-6.0f, 2.0f, -40.0f, 6.0f, 6.0f, 0.75f, 0.45f, 1.0f},  // far: ~no parallax
        };
    } else {
        // Posts spread across the view at the same angular width (~2.3 deg).
        const float dist[] = {0.5f, 1.0f, 2.0f, 5.0f, 20.0f};
        const float col[][3] = {{1, 0.35f, 0.3f}, {1, 0.75f, 0.25f}, {0.35f, 0.85f, 0.4f}, {0.3f, 0.6f, 1}, {0.75f, 0.45f, 1}};
        for (int k = 0; k < 5; ++k) {
            float d = dist[k], ang = -0.5f + 0.25f * k;  // x position as a fraction of the view
            objs.push_back({ang * d, -0.25f * d, -d, 0.04f * d, 0.5f * d, col[k][0], col[k][1], col[k][2]});
        }
    }
    bool running = true;
    for (uint64_t frame = 0; running && (frames == 0 || frame < uint64_t(frames)); ++frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
        int f = int(frame % FIF);
        vkWaitForFences(dev, 1, &inflight[f], VK_TRUE, UINT64_MAX);
        if (late_verts && frame == uint64_t(late_verts)) {
            if (vfd < 0) memcpy(p, verts.data(), verts.size() * 4);
            else if (pread(vfd, p, verts.size() * 4, 0) != ssize_t(verts.size() * 4)) fprintf(stderr, "vrdual-cube: pread into the mapping: %s\n", strerror(errno));
        }
        if (readback && frame >= uint64_t(FIF)) {  // this slot was filled FIF frames ago, its fence just waited
            uint32_t v = uint32_t(frame - FIF + 1), px = 0xff000000u | (v & 0xff) | (v & 0xff) << 8;  // RGBA8 clear colour
            if (rbp[f * 64] == v && rbp[512 + f * 16] == px && rbp[512 + f * 16 + 15] == px) ++rb_ok;
            else ++rb_bad;
            if (frame % 100 == 0) fprintf(stderr, "readback: %llu ok, %llu bad\n", (unsigned long long)rb_ok, (unsigned long long)rb_bad);
        }
        uint32_t ii;
        VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &ii);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "acquire: %d\n", ar); break; }
        vkResetFences(dev, 1, &inflight[f]);
        VkCommandBuffer cb = cmds[f];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        if (readback) {
            vkCmdFillBuffer(cb, rbuf, VkDeviceSize(f) * 256, 256, uint32_t(frame + 1));
            float c = float((frame + 1) & 0xff) / 255.0f;
            VkClearColorValue ccv{{c, c, 0, 1}};
            VkImageSubresourceRange sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = rimg;
            ib.subresourceRange = sr;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
            vkCmdClearColorImage(cb, rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &sr);
            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
            VkBufferImageCopy bic{2048 + VkDeviceSize(f) * 64, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
            vkCmdCopyImageToBuffer(cb, rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bic);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
        VkClearValue cv[2];
        cv[0].color = {{0.05f, 0.06f, 0.09f, clear_alpha}};
        cv[1].depthStencil = {1, 0};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, rp, fbs[ii], {{0, 0}, ext}, 2, cv};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize off = 0;
        if (!bda) vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
        float t = float(frame) / 60.0f;
        for (auto& o : objs) {
            M4 rot = spin ? mul(rot_y(t + o.x), rot_x(0.5f * t + o.z)) : ident();
            M4 model = mul(translate(o.x, o.y, o.z), mul(rot, scale(o.sx, o.sy, o.sx)));
            PC pc{mul(proj, model), {o.r, o.g, o.b, 1}, vaddr};
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
            vkCmdDraw(cb, nverts, 1, 0, 0);
        }
        // HUD bar (w == 1): z scaled into [0,1] near the front.
        M4 hud = mul(translate(0.0f, 0.85f, 0.05f), scale(1.6f, 0.08f, 0.01f));
        PC pc{hud, {0.9f, 0.9f, 0.9f, 1}, vaddr};
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cb, nverts, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags wst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acquired[f], &wst, 1, &cb, 1, &rendered[ii]};
        CK(vkQueueSubmit(queue, 1, &si, inflight[f]));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &rendered[ii], 1, &sc, &ii, nullptr};
        VkResult prr = vkQueuePresentKHR(queue, &pi);
        if (prr != VK_SUCCESS && prr != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "present: %d\n", prr); break; }
        if (xsc) present_extra();
    }
    vkDeviceWaitIdle(dev);
    // Teardown kept minimal (PoC); destroying the device lets the layer log stats.
    if (xsc) vkDestroySwapchainKHR(dev, xsc, nullptr);
    vkDestroySwapchainKHR(dev, sc, nullptr);
    vkDestroyDevice(dev, nullptr);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
