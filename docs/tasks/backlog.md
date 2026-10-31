# Backlog — small known defects and hardening

- Live params are written by the present's copy submit (2026-10-31), ordered
  after this frame's draws on the present queue. Draws on other queues (async
  compute, a second graphics queue) are not ordered against it (would need a
  semaphore); without export (no copy submit) the CPU writes them immediately.
- Compositor keys are only tested through `vrdual-ctl`; add an SDL event
  injection path (e.g. `--keys "Up Up W"`) so selftest can cover them.
- Readback: the CPU copy into /dev/shm (~0.8 ms per frame for two 1080p eyes)
  could go if the GPU wrote into the frame file (host-pointer import of the
  slot, alpha forced by the readers). Small gain, amdgpu userptr risk: later.
- Readback: 10-bit and FP16 swapchains go through a blit to 8-bit since
  2026-10-21; HDR colour spaces (PQ / scRGB above 1) are not tone-mapped.
- `vrdual gpus` "behind-switch" heuristic counts bridge hops (>4); the Navi31
  card has its own internal switch. Verify on other boards.
- Patched shaders: geometry/tess/mesh stages, shader objects, pipeline
  binaries (PLAN §G). With TES/GS in a pipeline the VS output is not clip
  space yet: patch the last pre-raster stage instead (TES: before OpReturn;
  GS: before every OpEmitVertex/OpEmitStreamVertex). Needs the patch moved to
  pipeline creation (or the original VS code kept per module), since
  vkCreateShaderModule does not know the pipeline. DX11 terrain, HairWorks.
- Shadow-map passes: detect depth-only passes with non-swapchain extent and skip.
- Layer logs every live change on both eyes; consider rate limiting.
- Not a git repo yet — ask the user.
