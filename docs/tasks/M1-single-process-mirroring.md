# M1 — single process, command stream mirrored to the second GPU

**Goal:** run ONE copy of the app. It sees GPU A. The layer creates a shadow
`VkDevice` on GPU B and replays everything there with the other eye's params.
This removes the two-instance limitation (input/RNG/timing divergence), which is
why real games cannot work today.

**Context:** PLAN §B (B2) and §C (which GPU the app sees: default fast GPU
`0b:00.0`, make it a switch, measure). Current layer: `layer/vrdual_layer.cpp`.

**Approach sketch**
1. Start with the API surface of `testapp/main.cpp` only (instance, device,
   memory, buffers, images, render pass, pipeline, command buffers, sync,
   swapchain). Generate the forwarding table instead of hand-writing ~100
   functions if it gets repetitive (vk.xml is in `third_party/Vulkan-Headers/registry`).
2. Handle maps: every app handle → twin on B. Dispatchable twins via the device's loader data.
3. Memory: app writes to mapped memory of A; at `vkQueueSubmit` copy the dirty
   mapped allocations to B (whole allocation first; dirty ranges later).
4. Shaders: one module, two eye values → use the live-param path (already
   there); each device has its own param buffer, so the same patched SPIR-V shape works.
5. Swapchain: only A has the real one; on B render into an offscreen image of
   the same format and export it like today's readback.
6. Synchronisation: the B submit mirrors A's submit; semaphores/fences get
   twins; cross-device waits go through the CPU (fence on B) at present.

**Done when:** `vrdual run --mirror -- build/vrdual-cube --scene calib`
(one process) passes the same `measure_posts.py` checks as selftest, and the
rotating scene stays frame-identical between eyes with lockstep disabled.

**Pitfalls:** descriptor sets with buffer device addresses (pointers differ
per device), `vkGetBufferDeviceAddress` values the app stores in memory,
queries/timestamps, host-visible coherent memory written without a flush
(must snapshot at submit), external memory, pipeline caches keyed by device UUID.
