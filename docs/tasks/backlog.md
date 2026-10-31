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
- Patched shaders: mesh stages, shader objects, pipeline binaries (PLAN §G).
  TES/GS done 2026-10-31 (gate spec constant per pipeline). Geometry shaders
  emitting from a function other than the entry point are patched too (any
  emit in the module).
- Shadow-map passes: detect depth-only passes with non-swapchain extent and skip.
- Layer logs every live change on both eyes; consider rate limiting.
- Review items 2026-10-31 left until a measurement says otherwise (xr.log:
  our work 2.5–4 ms per frame; upload fence wait 0.3–0.6 ms, staging
  0.6–1.3 ms, pairing 0.6–1.4 ms): three CPU copies per eye (shm → pair
  buffer → staging), `vrdual-xr`'s fence wait every frame, the R/B swizzle
  loop (never triggered in the logs), the mirror's `g_hmap` lock as a
  `shared_mutex` (measure contention first).
- Witcher 3 DX12 (vkd3d) logs 88× `shader module not patched: no float32
  type` for modules with a VS/TES/GS entry point (sharkoon, 2026-10-31). Dump
  one (no dump option yet: add one that writes the unpatched module next to the shader keys file, then spirv-dis) and check whether it
  draws visible geometry (e.g. float16-only or integer-position shaders) or
  is harmless (depth-only / stream-out).
- Frame generation (DLSS FG / FSR FG) with two GPUs: untested (does each
  GPU generate its own frames, does pairing L[F]/R[F] hold?).
