# M7 — depth reprojection (Z3D), single GPU

**Branch:** `single-gpu` (developed on "sharkoon", one RTX 5070 Ti). Merge to
`master` when selftest passes on both hosts.

**Goal:** both eyes from the same frame at the game's full frame rate on one
GPU, as an alternative to alternate-eye rendering (M6: half rate per eye, one
frame between the eyes). Borrowed from vorpX "Z3D"/"Z-Normal" and FlatVR
(depth-buffer provider); see the comparison in the 2026-10-19 session notes.

**Why single GPU:** on two GPUs geometry stereo already gives both eyes at
full rate with correct occlusion. Z3D trades correctness (disocclusion holes,
wrong depth for transparents/particles, HUD needs masking) for speed, which
only pays off when there is one GPU. On two GPUs it stays an opt-in fallback
for games whose shaders the patch breaks.

## Approach

1. **Find the scene depth:** in the layer, track depth attachments of render
   passes / dynamic rendering whose extent matches the swapchain; the last one
   written before the final full-screen pass (tonemap/UI) is the scene depth.
   Per-game override (`z3d_depth = <index>`) when the heuristic picks wrong.
2. **Export depth:** at present, copy that image (D32/D24 → R32F) next to the
   colour readback into the frame file (new slot plane, bump `VRDUAL_VERSION`).
   Linearise with near/far (config or projection detection, PLAN §A2).
3. **Reproject:** in `vrdual-xr` (it owns the output), render a grid mesh per
   eye displaced by depth (vorpX style; holes stretch instead of tearing), or
   a compute forward-warp + hole fill. Parallax from `ipd`, `hfov`,
   `world_scale` as in the shader patch, so calib scene numbers stay
   comparable.
4. **HUD:** draws with `w == 1` are already known to the layer; render them
   into a mask (or record the colour before the first `w == 1` draw) so the
   HUD is not smeared by reprojection and can be placed at `hud_depth`.

## Done when

- calib scene with `VRDUAL_STEREO=z3d` (single GPU) passes `measure_posts.py`
  (±1 px / 4 %), selftest check added;
- at least one DXVK game in the Quest at full frame rate.
