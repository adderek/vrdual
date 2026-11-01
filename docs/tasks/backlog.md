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
  Seen 2026-11-01 in Civilization V (README screenshot `CivV_1.gif`): shadows
  look cast from the camera side. Likely cause: the shadow-map pass (light
  space) is shifted per eye like the camera view, so each eye looks up a
  shadow map offset from its own geometry. Check: `_ hunt find` the shadow
  VS, `noshift` it, compare. Fix candidates: a `noshift` rule per game, or
  auto-noshift for depth-only render passes (no color attachment, extent not
  the swapchain's). Same risk in other top-down games (RTS, 4X) and any game
  with shadow maps.
- Layer logs every live change on both eyes; consider rate limiting.
- Review items 2026-10-31 left until a measurement says otherwise (xr.log:
  our work 2.5–4 ms per frame; upload fence wait 0.3–0.6 ms, staging
  0.6–1.3 ms, pairing 0.6–1.4 ms): three CPU copies per eye (shm → pair
  buffer → staging), `vrdual-xr`'s fence wait every frame, the R/B swizzle
  loop (never triggered in the logs), the mirror's `g_hmap` lock as a
  `shared_mutex` (measure contention first).
- Done 2026-11-01: Witcher 3 DX12 unpatched modules (219 log lines, 12
  distinct, `shader_dump` on sharkoon) are all harmless: empty VS (no
  outputs), VS that only pass vertex IDs or tessellation control data, the
  HairWorks TES (isolines, position as a plain varying for the GS), an empty
  GS and a stream-out GS (Xfb, points). None writes gl_Position for the
  rasterizer.
- Shader catalog, next steps (`shader_catalog` reading + log done
  2026-11-01): `_ shaders new LOG` turning the "not in catalog" lines into
  catalog lines; record the translator (DXVK / vkd3d version) in the catalog
  so an all-new run says "translator changed" rather than "mod?"; a Witcher 3
  catalog with descriptions (sponsors, `~/src/vrdual-sponsors`).
- Site / showcase media (owner's ranking 2026-11-01, best first): HospitalTycoon-3,
  HogwartsLegacy_1, DukeNukem4ever_1, Mafia, HogwartsLegacy_2,
  AssasinsCreedOrigins. `HospitalTycoon-3.gif`: the subtitle line at the
  bottom spoils it, re-record or crop. The landing page diagram is abstract
  (top view); wanted: a picture of an older game whose 3D comes out of the
  screen. Side-by-side PNGs are not in git (2-10 MB): compressed JPEGs could
  go on the per-game pages.
- LEGO games: per-eye shading errors visible in the LEGO Marvel GIFs (a
  screen-space effect computed for one eye); find and fix the shader.
- Witcher 3 and Cyberpunk 2077: well-known titles, worth fixing shader
  issues, but say on the site how vrdual compares with the dedicated VR
  projects for them (witcher3-vr and its Linux fork for Witcher 3; Cyberpunk:
  research), which will be the better choice for those games.
- Frame generation (DLSS FG / FSR FG) with two GPUs: untested (does each
  GPU generate its own frames, does pairing L[F]/R[F] hold?).
