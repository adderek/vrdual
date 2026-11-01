# Backlog — small known defects and hardening

- Live params are written by the present's copy submit (2026-10-31), ordered
  after this frame's draws on the present queue. Draws on other queues (async
  compute, a second graphics queue) are not ordered against it (would need a
  semaphore); without export (no copy submit) the CPU writes them immediately.
- Compositor keys are only tested through `vrdual-ctl`; add an SDL event
  injection path (e.g. `--keys "Up Up W"`) so selftest can cover them.
- Readback: the CPU copy into /dev/shm costs per eye on the game's present
  thread 1.14 ms at 1080p, 4.67 ms at 4K (2026-11-01, after non-temporal
  stores; before 1.58 / 6.6 ms). The GPU cannot write into the frame file:
  probe 2026-11-01 (RADV, Navi31, headless GPU), `VK_EXT_external_memory_host`
  import of a /dev/shm MAP_SHARED mapping fails with
  `VK_ERROR_INVALID_EXTERNAL_HANDLE`, anonymous private memory imports fine
  (1080p copy 0.39 ms). Likely cause: libdrm creates the userptr BO with
  `AMDGPU_GEM_USERPTR_ANONONLY`, so file-backed (shmem, memfd) pages are
  refused. Anonymous memory is not shareable with the reader, and
  process_vm_readv / pidfd_getfd need ptrace rights (Yama, Flatpak PID
  namespace). Left: export the readback memory as a dma-buf and hand the fd
  to vrdual-xr over a unix socket (the reader then imports it, no CPU copy
  on either side). That changes the passive-layer rule (a socket in the app
  process): maintainer's decision first.
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
  0.6–1.3 ms, pairing 0.6–1.4 ms): `vrdual-xr`'s fence wait every frame,
  the mirror's `g_hmap` lock as a `shared_mutex` (measure contention
  first). Done 2026-11-01: pairing copies straight into the staging buffers
  (one CPU copy per eye fewer), the R/B swizzle runs in place.
- Review 2026-11-01 (fixed then: lockstep never resuming after a timeout,
  destroy vs present race on the pending readbacks, non-atomic config saves,
  the xr pair copy). Left:
  - Measured 2026-11-01 and dropped (`vrdual-cube` on the headless GPU, lockstep off):
    the wait for the previous frame's copy is 0.00-0.13 ms/frame, so
    publishing without waiting gains nothing; the cursor image fetch costs
    ~4 us per frame (pointer + geometry 12.7 us, + 48x48 image 16.5 us), not
    worth an XFixes event path. The layer now logs `export: ... ms/frame`
    every 600 frames for checks in games.
  - Mirror reviewed 2026-11-01: memory (map / shadow / push / write-back),
    fence and semaphore sync, submits, present_twin. Fixed: a re-map with
    another range got a zeroed shadow (the app read zeros, zero writes never
    reached A or B; shadows now cover the whole allocation), WAIT_ANY waited
    up to 1 s for the other semaphore's write-back. Open, not reproduced:
    two app threads submitting to different queues with a binary semaphore
    between them reach B in A's order only by luck (A and B submits are not
    one atomic step); whole-allocation shadows cost more address space for
    32-bit apps that map small windows of big allocations.
  - Not reviewed yet: the generated replay wrappers (`tools/gen_mirror.py`),
    twin device / WSI creation, the mirror stop path, `_`,
    `common/vrdual_input.hpp`, `site/`.
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
  catalog with descriptions.
- Site / showcase media (owner's ranking 2026-11-01, best first): HospitalTycoon-3,
  HogwartsLegacy_1, DukeNukem4ever_1, Mafia, HogwartsLegacy_2,
  AssasinsCreedOrigins. `HospitalTycoon-3.gif`: the subtitle line at the
  bottom spoils it, re-record or crop. Landing page "how it works" picture:
  done 2026-11-01 (`site/img/promo.webp`, generated, no third-party game
  characters); the top-view eye diagram was removed (too technical for most visitors). Side-by-side PNGs are not in git (2-10 MB): compressed JPEGs could
  go on the per-game pages.
- Site translations (landing page in 9 languages done 2026-11-01, machine
  translated): games.html / game.html UI is English only (strings in
  games.js / game.html, not `data-i18n` yet); a native-speaker review of
  `site/i18n.json`, Polish first (owner). New landing pictures: the owner
  generates them with an image model (no recognizable third-party game
  characters, only titles we tested, no vendor-coloured GPUs).
- LEGO games: per-eye shading errors visible in the LEGO Marvel GIFs (a
  screen-space effect computed for one eye); find and fix the shader.
- Witcher 3 and Cyberpunk 2077: well-known titles, worth fixing shader
  issues, but say on the site how vrdual compares with the dedicated VR
  projects for them (witcher3-vr and its Linux fork for Witcher 3; Cyberpunk:
  research), which will be the better choice for those games.
- Frame generation (DLSS FG / FSR FG) with two GPUs: untested (does each
  GPU generate its own frames, does pairing L[F]/R[F] hold?).
