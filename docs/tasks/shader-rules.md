# Shader rules: per-game, per-shader behaviour as data

**Context:** comparison with [witcher3-vr](https://github.com/tig3rmast3r/witcher3-vr)
and its fork [joschi127/witcher3-vr](https://github.com/joschi127/witcher3-vr)
(2026-10-31, MIT). They identify the game's shaders by a hash of the
bytecode per game build (`src/shader_fingerprints.h`: FNV-1a 64, a 0 hash =
"not mapped on this build", the feature then stays off) and give single
shaders special treatment (HUD composite, smoke, terrain, distant trees).
Their engine hooks (camera, first person, HUD composite replacement) break
our invariants (app-unaware, passive layer) and are not taken over.

**Why:** the clip-space shift is one rule for every vertex shader; a few
shaders in a game need another one. The `w == 1` heuristic catches the HUD
and full-screen passes alike, so `hud_depth` shifts post-process passes too
(seam at one edge) and HUD scale (borrowed-features #2) is blocked. Rules
keyed by shader hash keep the code generic and put the game knowledge into
data: `[app:NAME]` sections and later the sponsor profiles (PLAN §J).

## Design

- Key: FNV-1a 64 over the SPIR-V the layer receives, printed as 16 hex
  digits. That is DXVK's / vkd3d's / Zink's translation, so a key holds for
  one game build *and* one translator version. A game patch or a new Proton
  changes the keys; the rule then matches nothing and the shader gets the
  default treatment (fail-closed, no build guard needed). The layer logs how
  many rules matched, so a stale profile shows.
- Rules: config key `shader_rules`, a comma list `KEY:MODE`. KEY is a hex
  prefix of the key (at least 8 digits), MODE:
  - `noshift`: shader not patched (both eyes identical, at infinity),
  - `hud`: every vertex gets the HUD treatment (`hud_depth` disparity, no
    head offset), whatever its `w`,
  - `hide`: the shader draws nothing (unwanted overlays, debugging).
- Finding keys (*hunt*): every patched shader carries its key's high 32 bits
  as a constant and, in live mode, reads `(lo, hi, mode)` from the param
  buffer (`addr + 32`). Shaders with `lo <= key32 <= hi` get `mode`
  (`hide | noshift | hud`) without a pipeline rebuild. The layer appends
  every new key to `/dev/shm/vrdual-SESSION-shaders`; `_ hunt` bisects over
  that list (hide half, "did it disappear?"), ~log2(N) questions for N
  shaders, and offers the found key as a rule. `?` (can't tell, e.g. hide
  blacked out the picture) tries the other half instead. First real use
  (Mirror's Edge, 2026-10-31): `noshift` found the right-edge seam of
  `hud_depth = 1` in 7 steps over 123 shaders.

## Steps

1. Hash, list file, `shader_rules` (noshift / hud / hide), live hunt range,
   `vrdual-ctl hunt`, `_ hunt` bisection; selftest: patched variants
   validate, list contains the test shader's key, hunt hide empties the
   picture and restores it, a `noshift` rule makes both eyes equal.
   Done 2026-10-31.
2. In a real game (DXVK): is the key stable across runs of the same game and
   Proton? Yes for Mirror's Edge (D3D9, 2026-10-31: same keys across
   restarts and a resolution change; GAMES.md). Added on the way:
   `hud_depth_scope = rules` (only `hud` rule shaders get `hud_depth`; one
   hunt for the HUD instead of one per post pass), `?` answer, settings copy
   for Flatpak Steam refreshed on `hunt rule` / `save`. Left: Find a HUD shader with `_ hunt`, put it into `shader_rules`.
   Then: is there a better key that survives a Proton update (DXVK keeps
   the DXBC's SHA-1 in its shader key; not visible to a Vulkan layer as far
   as known)?
3. HUD scale (borrowed-features #2) for `hud` rule shaders only.
4. Rules in the sponsor profiles (`vrdual profiles sync`, TODO 11).

## Not taken over (and why)

- Engine hooks at per-build addresses, head-tracked game camera, first
  person (their scripts wait for their DLL), HUD composite PSO replacement,
  in-game HUD editor: game-specific code in the app process.
- PureDark AFW (closed binary), OFXR, OptiScaler, DLSS 5, ReShade: Windows /
  D3D / NVIDIA. Frame generation is our M7.
- Their per-eye `imageRect` crop of a symmetric image to the HMD's
  asymmetric FOV: only pays when the game's `hfov` exceeds the headset's;
  noted for `vrdual-xr`, not done.
- `linux/w3vr-linux.py` (Steam native / Flatpak / Deck, Heroic native /
  Flatpak, Proton prefixes, `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1`):
  reference for TODO 12 (non-Steam launchers), not copied yet.
