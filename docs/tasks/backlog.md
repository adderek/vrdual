# Backlog — small known defects and hardening

- Live params are written by the present's copy submit (2026-10-31), ordered
  after this frame's draws on the present queue. Draws on other queues (async
  compute, a second graphics queue) are not ordered against it (would need a
  semaphore); without export (no copy submit) the CPU writes them immediately.
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
  with shadow maps. Done 2026-11-02 as `auto_noshift = 1` (opt-in, live key
  table, also in the mirror; selftest `--shadow`). Civ V findings
  2026-11-02 (Linux build, GL via Zink, mirror): no depth-only passes at all
  (every FBO has colour), 3D view at 1368x768 into a 1920x1080 swapchain,
  off-screen passes at 2048² (shadows), 1024², 512², 256², 128², 64²,
  320x190. Its shadow map is orthographic (w == 1): the shadows and the
  right eye's seams between terrain tiles both moved with `hud_depth`
  (correct at 0). Hence the rule is now "off-screen" (extent neither the
  swapchain's nor the main view's), colour or not. A first version kept a
  table of shader keys drawn only off-screen: HUD and the selection circle
  got better, unit shadows did not (their vertex shader is shared with the
  units: keys at 1024² / 64² also drawn elsewhere). Now the pass decides: a
  flag written before each render pass. Left: confirm unit shadows in
  Civ V; measure the cost of the two barriers per pass in a heavy game.
  Not covered: a shadow atlas the size of the main view.
- Civ V, right eye only (mirror B): thin dark lines along terrain tile
  edges, also at IPD 0, gone at times with hud_depth 0 (2026-11-02). Partly
  the off-screen shift above (grooves); thin lines remain. Mirror replay
  difference on B, cause unknown (no mirror warnings in the log).
- Civ V: the selection circle under a unit differs a lot between the eyes
  (screenshot 2026-11-02); likely a decal projected on the terrain from
  depth (effect kind 2 in framegen-effects.md). Candidate for a per-game
  fix (setting or shader rule).
- Civ V crashed once 2026-11-02 14:19 in the game's own code
  (`cvLandmarkVisSystem::LandmarkRenderJob`, TBB worker, SEGV_MAPERR), mirror
  mode, 32-bit; not seen again yet. If it repeats: compare without vrdual.
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
  - Also reviewed 2026-11-01: twin device and WSI creation, acquire, the
    generated wrapper pattern. Fixed: the generated Destroy / Free wrappers
    never dropped the handle from `g_hmap` (3000 samplers churned left 3022
    entries, now 30). Objects freed implicitly (descriptor sets by a pool
    reset or destroy, command buffers by a pool destroy) leave it too since
    2026-11-02 (per-pool child lists; `FreeCommandBuffers` never dropped its
    entries either; selftest churn: 7527 entries left before, 30 after).
  - Also reviewed 2026-11-01: the mirror stop path (selftest covers it),
    `_` (run, install, config copies, xr / audio / WiVRn), and
    `common/vrdual_input.hpp`. Fixed: the config copies games read
    (`vrdual install`, `config_sync`) were plain `cp` (a game starting
    meanwhile could read half a file), now cp + mv. Minor, left: a new
    uinput device may miss its first events before userspace opens it.
  - Struct translators in `tools/gen_mirror.py` reviewed 2026-11-02. Fixed:
    "may carry a handle" was a recursive walk that cut cycles and cached
    the wrong answer inside them (now a least fixpoint; with today's vk.xml
    the output is byte-identical, so no struct was affected). Left: an
    unknown pNext sType (returnedonly, platform / provisional, newer than
    our vk.xml) and the rest of its chain reach B untranslated (logged once
    per sType); none in 35 mirror game logs here, so no change without a
    case (dropping the node would change B's behaviour instead). Stubs that
    stop the mirror: acceleration structure builds, shader objects,
    pipeline binaries, indirect execution sets, descriptor heap writes; all
    behind features the layer hides.
  - Not reviewed yet: `site/`.
- Done 2026-11-01: Witcher 3 DX12 unpatched modules (219 log lines, 12
  distinct, `shader_dump` on sharkoon) are all harmless: empty VS (no
  outputs), VS that only pass vertex IDs or tessellation control data, the
  HairWorks TES (isolines, position as a plain varying for the GS), an empty
  GS and a stream-out GS (Xfb, points). None writes gl_Position for the
  rasterizer.
- Shader catalog, next steps (`shader_catalog` reading + log done
  2026-11-01; `vrdual shaders new LOG [CATALOG]` and the `# translator` line
  done 2026-11-02): a Witcher 3 catalog with descriptions (needs the game).
- Site / showcase media (owner's ranking 2026-11-01, best first): HospitalTycoon-3,
  HogwartsLegacy_1, DukeNukem4ever_1, Mafia, HogwartsLegacy_2,
  AssasinsCreedOrigins. `HospitalTycoon-3.gif`: the subtitle line at the
  bottom spoils it, re-record or crop. Landing page "how it works" picture:
  done 2026-11-01 (`site/img/promo.webp`, generated, no third-party game
  characters); the top-view eye diagram was removed (too technical for most visitors). Side-by-side PNGs are not in git (2-10 MB): compressed JPEGs could
  go on the per-game pages.
- Site translations (landing page in 9 languages done 2026-11-01, games.html
  / game.html UI 2026-11-02, machine translated; game data in games.json
  stays English): a native-speaker review of `site/i18n.json`, Polish first
  (owner). New landing pictures: the owner
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
