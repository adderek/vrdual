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
  Result 2026-11-02 (owner, headset): the rest of Civ V is fine now (HUD,
  selection circle, terrain), unit shadows still differ per eye although
  every off-screen pass incl. 2048² / 1024² is unshifted. So the shadow map
  is right and the lookup is wrong: the main view resolves shadows from
  depth with the game's unshifted matrices (effect kind 2,
  framegen-effects.md). Fix only per game: patch that pixel shader's
  unprojection (find it with a hunt on pixel shaders, not supported yet),
  or turn shadows down in the game.
  Not covered: a shadow atlas the size of the main view.
- Civ V, right eye only (mirror B): thin dark lines along terrain tile
  edges, also at IPD 0, gone at times with hud_depth 0 (2026-11-02). Partly
  the off-screen shift above (grooves); thin lines remain. Mirror replay
  difference on B, cause unknown (no mirror warnings in the log).
- Civ V: the selection circle under a unit differs a lot between the eyes
  (screenshot 2026-11-02); likely a decal projected on the terrain from
  depth (effect kind 2 in framegen-effects.md). Candidate for a per-game
  fix (setting or shader rule).
- Civ V crashed 2026-11-02 14:19, again at 17:15 and at the end of the next
  session, same stack in the game's own code
  (`cvLandmarkVisSystem::LandmarkRenderJob`, TBB worker, SEGV_MAPERR), mirror
  mode, 32-bit. Next: the same session without vrdual (or one eye, no
  mirror) to tell a game bug from the mirror's memory sync.
  2026-11-03: twice more within minutes (09:23, 11:29), same pc
  (Civ5XP+0x813d5f: `mov (%eax),%eax` gives 0, then `0x14(%eax)`, a null
  object in a TBB job). Mirror maps only ~37 MB, so not address space.
  Known on Linux without vrdual too: an Arch forum thread (bbs.archlinux.org
  viewtopic.php?id=308562) fixed random Civ5XP null segfaults by raising
  `MaxSimultaneousThreads` (Aspyr config.ini, default 8; this box: 24
  threads). Owner to try that first.
  Bisect 2026-11-03: Zink alone (no wrapper) ran 30 min clean; vrdual,
  vrdual one eye (no mirror), one eye with `VRDUAL_FILTER=0`, and the layer
  passive (`VRDUAL_STEREO=0`) all crashed, the passive one at Civ5XP+0xc90534
  (GrannyDag2Resolver in cvUnitVisSystem), the very address of the Arch
  thread. 9 cores today, all in the vis systems' TBB render jobs (Landmark,
  City, Unit). `MaxSimultaneousThreads = 4`: crash; `= 1` plus affinity 0-7
  set live (`taskset -a -cp`): crash after ~23 min. Verdict: the game's own
  race, timing-sensitive; not pursued further. Side finding: `taskset -c
  0-7` in the Steam launch options (after gamescope's `--`) does not reach
  the game, which had `Cpus_allowed_list: 0-23`.
  Root cause 2026-11-03 (disassembly + both cores): `Civ5App::SetupJobManager`
  sets n = `MaxSimultaneousThreads` clamped to 3..16 (so `= 1` gives 3) and
  calls `JobManager_Initialize(n)` (`task_scheduler_init(n)`). The scheduler
  observer gives each thread entering TBB `g_ThreadIDCount++` (never bounded)
  as its index; the per-thread render data (`sm_pThreadRenderer` of the
  Landmark/City/Unit vis systems, Granny pose caches, command streams) has
  `JobManager_GetNumberThreads()` = n slots. Two threads more than n enter
  (core 1: n 8, counter 10, crash at index 9; core 2: n 16, counter 18), and a
  job on such a thread reads a null slot. The landmark renderer holds at most
  16 slots inline, so slots cannot go above 16. Fix: `GetNumberThreads`
  (vaddr 0x8dc282b, 19 bytes) returns 16 once initialized
  (`a1 08 14 0a 10 85 c0 74 05 b8 10 00 00 00 c3` + 4 nops), with
  `MaxSimultaneousThreads = 8` (indices up to 10). Applied by hand to the
  owner's copy (`Civ5XP.orig` kept); first session over 30 min, no crash
  (owner, 2026-11-03). Next: `vrdual fix
  on|off GAME` like `intro` (bytes from the profile, checked before
  writing, `.orig` backup), selftest check on a fake file.
- Civ V (and other top-down strategy games): look around by turning the head
  (owner 2026-11-03). Today `head_look = clip` turns the rendered view, but the
  game culls everything outside its own frustum, so past the edge nothing is
  drawn. Plan:
  1. `head_look = scroll`: `clip` inside `head_rot_max`; past it vrdual-xr holds
     the arrow keys (uinput), at a rate set by the angle past the limit. Generic
     (most 4X/RTS games scroll with the arrows), Mapper scenario in selftest.
  2. Wider culling only (game patch): the clip rotation happens in the vertex
     shader before clipping, so anything the game submits shows up when the
     head turns. Widening only Civ V's culling frustum (FCamera / vis systems;
     the binary exports class symbols) keeps its projection, mouse picking and
     UI as they are. Risks: the cached terrain texture
     (DiffuseCachedTexture) and the shadow map may only cover the visible
     area (blurry / unshadowed edges); more draws cost CPU.
- Fragment shader hunt + shader replacement (owner 2026-11-02, Civ V unit
  shadows): today's hunt and rules only cover the stages we patch (VS/TES/GS),
  so hiding the shader that resolves shadows would hide the terrain too.
  1. Done 2026-11-02: `hunt_fragment = 1` lists FS keys, hunt `hide` on an
     FS discards (selftest 3c'f). Civ V 2026-11-02: found
     `a27db010721f21a6` three times in a row. The half holding the final
     full-screen pass blacks the whole picture (the owner bisected by hand;
     `x` = can't tell applies the other half). Idea: leave the shaders drawn
     into the swapchain out of the first steps, or hide them last.
     The key is Civ V's terrain fragment shader (Zink keeps the names:
     DiffuseCachedTexture, TerrainOverlay, DynamicShadowTexture, FOW…): it
     samples the shadow texture with a projective lookup from its varyings
     (world based, so the same for both eyes if the texture is). Hide
     removed the terrain, rule taken out. Open: does the shadow texture
     differ per eye? Suspect B's twin pass flag (selftest checks only A's
     shadow edge in the mirror); next: a snapshot of both eyes in the game.
     Snapshots 2026-11-02: at IPD 0 both eyes are pixel-identical (memory
     sync fine); with anchor right (right eye unshifted) the shadows still
     differ, so A shifts the shadow texture too: the pass that draws it is
     not flagged off-screen (all flagged extents logged; maybe it renders
     at the main view's 1368x768). Next: `pass_trace` in Civ V.
     `pass_trace` 2026-11-03 (frame 6000): every non-main pass is flagged
     (320x190, 1024², 2048² ×2 with the one shadow caster key 7a6cdc31,
     512²), so that theory is out. Terrain FS shadow lookup reads world
     position varyings (VAR1.zw, VAR2.x, also used for fog distance): eye
     independent. Snapshot: after aligning the terrain (11 px), the left
     eye's (A) unit shadows sit at the feet, the right eye's (B) ~35 px to
     the left, i.e. B's shadow map looks shifted (or rotated by head_look).
     But the mechanism works on B: new selftest checks show B's own shadow
     map (cube: shadow row copied into the image; gltest --shadow: FBO
     drawn as an unshifted quad) in the mirror, Zink 64 and 32 bit.
     Found 2026-11-03 (live hunt per key, snapshots): hiding 7a6cdc31
     (2048²) or 25ef3baf (1024²) changes nothing; f6228eab (512²) is the
     terrain. The casters are 8d5184d74a754d9a (people; `noshift` makes
     both eyes' shadows agree, `hide` removes them) and probably
     a8403c1ec875af08 (other models), both in the frame's first pass at the
     main view's size 1368x768, which the extent rule cannot tell from the
     main view. Fix options: per-game `noshift` rules (Zink keys change with
     Mesa), or auto_noshift by target image instead of extent (pass_trace
     now logs the target to check the main view uses one image).
     Trace with targets: the main view is 3 passes into one target, the
     shadow pass (8d5184d7, 537b334a, a8403c1e) another. auto_noshift now
     also flags passes of the main view's size into another target when
     the main view is not the swapchain's size (selftest: gltest --view).
     Confirmed in the game 2026-11-03 without shader rules: main view
     target found, the two other 1368x768 targets flagged, a snapshot of
     both eyes shows no shadow only one eye has (people, deer). Done.
     Earlier plan: live with vrdual-ctl: `hunt noshift 7a6cdc31
     7a6cdc31` (does B fix?), `hunt hide` it (is it the caster?), head_look
     off (rotation?).
  2. Replacement by key: `shader_dir/<key>.spv` (or `.glsl` via glslang
     offline) loaded instead of the game's module, 3Dmigoto ShaderFixes
     style (MODDING.md "Shaders"). Zink hashes depend on the Mesa version:
     store it next to the file and warn on a mismatch.
  3. For the shadow fix itself the shader needs the eye offset: give
     replacements the live param block (addr from the same BDA buffer).
- Civ V 2026-11-02 (owner, headset): `head_look = clip` with
  `vrdual xr --lock head` works well (the world turns, the HUD stays).
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
