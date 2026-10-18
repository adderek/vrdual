# M5 — real applications

**Goal:** run something that is not our test app. Order: a Vulkan demo
(`vkcube` once vulkan-tools is installed; Sascha Willems samples), a native
Vulkan game, then a DXVK/VKD3D-Proton title.

**Context:** ARCHITECTURE "Known PoC limitations". Before M1, only
deterministic apps line up across the two processes, so start with demos that
animate by frame count or can be paused.

**Expected failures to triage:** geometry/tess/mesh shaders (patch the last
pre-raster stage, PLAN §G), `VK_EXT_shader_object`, maintenance5 inline modules
(`VkShaderModuleCreateInfo` in pipeline stage `pNext`), perspective shadow maps
being shifted, post effects using view matrices from UBOs, non-8-bit
swapchains (HDR, 10-bit), several swapchains.

**Done when:** a short per-app compatibility table (works / partly / no, why)
in docs/ARCHITECTURE.md, with a profile per working app in the example config.
