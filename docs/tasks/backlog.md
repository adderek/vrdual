# Backlog — small known defects and hardening

- Live IPD change lands mid-frame: draws already queued read the new value. Fix:
  double-buffer the params (two vec4 slots, index flipped at present), or accept.
- Compositor keys are only tested through `vrdual-ctl`; add an SDL event
  injection path (e.g. `--keys "Up Up W"`) so selftest can cover them.
- Readback: 10-bit and FP16 swapchains go through a blit to 8-bit since
  2026-10-21; HDR colour spaces (PQ / scRGB above 1) are not tone-mapped.
- `vrdual gpus` "behind-switch" heuristic counts bridge hops (>4); the Navi31
  card has its own internal switch. Verify on other boards.
- Patched shaders: geometry/tess/mesh stages, shader objects, inline modules,
  pipeline binaries (PLAN §G).
- Shadow-map passes: detect depth-only passes with non-swapchain extent and skip.
- Layer logs every live change on both eyes; consider rate limiting.
- Not a git repo yet — ask the user.
