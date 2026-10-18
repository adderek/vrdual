# M4 — head pose for apps that don't know about the HMD

**Goal:** head rotation and translation (6DoF) change what the game shows,
although the game's camera knows nothing about the HMD. Pose source: OpenXR
`xrLocateViews` in `vrdual-xr`, written to the control block every frame; the
layer reads it at present (like live IPD), so it lands one frame later.

**Context:** PLAN §E. The clip-space trick generalises: with projection P
known, `clip' = P · H · P⁻¹ · clip` applies a head transform H. It needs P
(HFOV, aspect, near/far): HFOV is configured, aspect comes from the swapchain,
near/far from config or projection detection (PLAN §A2). The eye offset we
already apply is the x-translation special case of this.

## Translation (lean sideways / up-down / forward-back)

Render-only, the game's camera and physics stay where they are, so the player
capsule never enters a wall; only the rendered viewpoint moves.

- x/y: same formula as the eye offset (`clip.x += −P00·dx·scale`, same for y
  with P11) — exact for a perspective P, cheap, fits the live param buffer
  (add dy, later dz).
- z: needs the full `P·T·P⁻¹` (near/far matter). Second step.
- **Soft clamp:** `d_game = L·tanh(d_head / L)`, L ≈ 10 cm in game units
  (`world_scale`). A typical FPS capsule radius is 30–40 cm, so a ≤10 cm render
  offset almost never shows the inside of a wall; full 1:1 near the centre,
  saturating when leaning far.
- Fishtank feel with a world-locked screen: set the convergence (`anchor`
  plane) at the virtual screen distance, then leaning shows "around the window
  edge" correctly.
- Later, if clipping shows up in practice: fade/vignette when the centre depth
  is closer than the near plane + offset (needs a depth readback), or scale the
  offset down near geometry. Not before there is evidence.
- Per-game option `head_lean = strafe`: lean beyond a threshold → lean keys
  (Q/E) or a stick nudge; the game's own physics moves the character.

## Rotation (yaw / pitch / roll)

Three modes, per game (`head_look`), user choice:

| mode | screen | what head rotation does | risk |
|---|---|---|---|
| `off` | world-locked (`--lock world`) | look around the virtual screen, game untouched | none, today's behaviour |
| `mouse` | head-locked | head yaw/pitch deltas → relative mouse via uinput (E3), per-game counts-per-degree calibration (turn 360° in game, count) | game smoothing/accel/low fps desync the world from the vestibular sense → sickness; needs constant fps |
| `edge` | world-locked | inside ±N° (≈15°) of the screen centre nothing happens; beyond it, angle → turn rate (like a stick), so 45° of head turn can spin 360° | comfortable, but not 1:1 |

Latency hiding for `mouse`: apply the not-yet-acknowledged part of the
rotation with the clip transform (`P·R·P⁻¹`, no lag, edge popping because the
game culls for its own camera), fading out as the game camera catches up.
Roll: clip-space rotation only (games rarely have a roll axis).

## Steps

1. Control block: head pose (position, orientation, timestamp) written by
   `vrdual-xr`; bump the ctl version.
2. Translation x/y with soft clamp in the live params (vec4 → 2×vec4).
3. `head_look = edge` and `mouse` via the uinput sink from
   [input emulation](input-emulation.md) (same device, same per-game config).
4. Rotation/z via `P·H·P⁻¹` (param buffer → mat4), latency hiding.

**Done when:** in the calib scene a simulated head offset/yaw moves the posts
by the predicted pixel amount (extend `measure_posts.py`; Monado's simulated
HMD can script poses), selftest passes; manual check in the Quest.
