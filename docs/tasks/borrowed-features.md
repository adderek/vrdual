# Features borrowed from vorpX / FlatVR (GPU-count independent)

**Context:** comparison 2026-10-19. vorpX G3D is the closest relative (shader
patching, profiles, Windows/DX only); FlatVR is a 2D capture + AI depth
viewer. Depth reprojection (vorpX Z3D) mainly pays off on one GPU: M7 on the
`single-gpu` branch. Everything here helps two GPUs as much as one: `master`.

| # | feature | from | status |
|---|---|---|---|
| 1 | `hud_depth`: HUD (`w == 1` draws) at a chosen distance instead of infinity | vorpX HUD depth | done 2026-10-19, selftest "hud_depth 1.5m" |
| 2 | HUD scale: shrink `w == 1` draws towards the centre (HUD corners outside the comfortable FOV) | vorpX HUD scale | open |
| 3 | cinema mode in `vrdual-xr`: stereo quad pair (`XrCompositionLayerQuad`, `eyeVisibility`) at `screen_dist`/`screen_size` instead of the full-FOV projection; curved via `XR_KHR_composition_layer_cylinder` when the runtime has it | vorpX virtual cinema, FlatVR curved screen | open |
| 4 | move/resize the screen with the controllers (grab with both grips) | FlatVR | open, needs 3 |
| 5 | edge peek: render with a wider FOV than shown, head rotation looks into the margin before `head_look` turns the game | vorpX edge peek | open, M4 step 4 |

## Notes

- 1: exact for a flat HUD: the constant NDC shift `-P00·eye_dx/d` is what a
  point at distance d gets from the clip offset. Baked mode (no live params)
  keeps the HUD at infinity. Post-process passes are also `w == 1` but are
  full-screen copies of already-stereo images; shifting them by a few pixels
  shows as a thin seam at one edge. If that is visible in a game, set
  `hud_depth = 0` for it (per-game profile) until there is a HUD/post
  distinction (e.g. only shift draws with alpha blending).
- 3: the full-FOV projection stays the default (geometrically right when
  `hfov` matches). Cinema is for 3rd-person/strategy games and for comfort.
