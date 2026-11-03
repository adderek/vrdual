# Games

Status per game, dual GPU (2x RX 7900 XTX, host "fractal", see AGENTS.md "Reference test machine") unless noted.
Launch option for all: `/home/<user>/vrdual/run %command%` (from
`vrdual install`); `vrdual xr` runs once for all games. Last update
2026-11-03. Most games here are Steam; Epic/GOG ones go through Heroic
(setup below), Assassin's Creed Origins through Heroic + Ubisoft Connect.

Status: **works** = stereo in the HMD confirmed; **runs** = renders, stereo
not confirmed or weak; **broken** = crash or no picture;
**no effect** = runs, but nothing to gain (2D game, no 3D scene to shift).
**store: test** = the store whose copy was tested (Epic / GOG / Amazon through
Heroic) and whether that test was positive; other stores' copies are untested.

**Write-up pending**: some games run best with a tuned configuration (tested
settings, fixes for the problems already worked through) that is not written
up here yet; it will be. Until then everything can be configured by hand
(`docs/CONFIG.md`).

Notes list what holds now, one fact per item; **tested** is the date of the last test.
Rows in alphabetical order (a leading "The" / "A" ignored). The table is
generated: edit `site/games.json`, then run `tools/gen_games.py` (also the
data of the web page, `site/`).

<!-- games:begin (generated from site/games.json by tools/gen_games.py) -->
| game (Steam appid) | store: test | path | status | tested | settings | notes |
|---|---|---|---|---|---|---|
| ABZU | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (UE4, DXVK, 64-bit `AbzuGame-Win64-Shipping.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); no own profile (ran at `world_scale` ~0.1 from the control block) | • Works. |
| Age of Wonders (1999) | GOG (Heroic): negative (2D game) | Proton (umu, Proton-CachyOS), 32-bit `aow.exe` (launched from `AoWSetup.exe`), OpenGL → Zink (log: `pEngineName: mesa zink`) | no effect | 2026-10-29 |  | • 2D game: no 3D scene, nothing to gain in VR. |
| Alice: Madness Returns (19680) | Steam: positive | Proton, D3D9 (UE3, DXVK, 32-bit) | works | 2026-10-22 |  | • Works on a single GPU too.<br>• Two-GPU mirror: fast, no flickering black shadows or objects (cached memory shadowed with write-back; `cached_wp = 1` was slightly faster but flickered).<br>• `head_look` `mouse` and `edge` both work. |
| Alien: Isolation | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (DXVK, 32-bit `AI.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `world_scale` 0.1 or lower (`[app:wine-preloader]` profile) | • Works correctly.<br>• `world_scale` 0.2 gives a strong effect but can cause heavy nausea.<br>• Earlier hang after skipping the intro videos and artifacts outside the 1920x1200 image in a 1920x1080 gamescope are no longer seen. |
| Aliens: Colonial Marines (49540) | Steam: positive | Proton, D3D9 (UE3, DXVK, 32-bit `ACM.exe`) | works | 2026-10-23 | none saved | • Works; old game, modest graphics. |
| Assassin's Creed Origins | Epic (Heroic + Ubisoft Connect): positive | Proton (umu, Proton-CachyOS), D3D11 (AnvilNext, DXVK, 64-bit `ACOrigins.exe`) | works | 2026-10-26 | gamescope 1920x1080 (Heroic); `[app:ACOrigins.exe]` `world_scale` ~0.1 (larger values: strong nausea, user still getting used to VR), `hud_depth = 0`, `hfov = 90` | • Smooth in the HMD, impressive stereo.<br>• Open: the character seems to float above the ground (screen-space effects? AO / contact shadows off not tried).<br>• Ubisoft Connect's helpers (upc, UplayWebCore, explorer, xalia) run with `stereo = 0` (built in).<br>• Fixed in vrdual: crash in ~2 of 3 starts (device lookup cache), double free at exit, 10 Hz in the HMD. |
| Beyond Blue | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (Unity, DXVK, 64-bit `Beyond Blue.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `world_scale` ~0.1 (at 1 the depth is very strong, with artefacts), `hud_depth = 0`; `screen = curved` | • Works.<br>• Unity's video player decodes on a second D3D11 device; the mirror handles its shared frames (before: right eye froze at the first video). |
| Botany Manor | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (Unity, DXVK, 64-bit `Botany Manor.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `input = mouse`, `mouse_mode = stick` (made for keyboard + mouse); small `world_scale` (~0.01) | • Works.<br>• Unity's second D3D11 device (shared memory) is handled by the mirror.<br>• If the headset's network drops, `vrdual xr` reconnects after 10 s stuck (before: black HMD). |
| Call of Juarez: Gunslinger (204450) | Steam: positive | Proton, DXVK d3d9 (32-bit) | works | 2026-10-22 | tuned configuration, write-up pending (until then tune it yourself) | • Works, fast (mirror memory sync 0.13 ms per submit with cached write tracking).<br>• The game leaves alpha 0; vrdual forces alpha 1 (otherwise black in the HMD).<br>• Weak depth at default settings. |
| Carmageddon TDR 2000 | GOG (Heroic): positive | Proton (umu, Proton-CachyOS), old D3D via wined3d → GL → Zink (32-bit `TDR2000.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `[app:wine-preloader]` profile | • Works; old game, looks poor. |
| Chasm: The Rift | GOG (Heroic): positive | Proton (umu, Proton-CachyOS), 64-bit `Chasm.exe` (DXVK and Zink both loaded; renderer not checked) | works | 2026-10-29 | gamescope 1920x1080 (Heroic) | • Runs correctly.<br>• Red and blue were swapped in the HMD (`vrdual xr` kept the previous game's BGRA swapchain); fixed, not re-tested here. |
| Civilization V (8930) | Steam: positive | native Linux port (Aspyr, `Civ5XP`, 32-bit, GL from the Mac code) → Zink; Steam runs it, not the Windows build, unless Proton is forced | works | 2026-11-03 | user profile `[app:Civ5XP]`, `world_scale` ~0.1, `auto_noshift = 1` (shadows, baked terrain), `head_look = scroll` with `cull_widen = 1.5` (look past the edge, the map scrolls further out), `hud_follow` (HUD stays in place for a turn), `scroll_angle` / `scroll_hold_angle` (taps, then held); Aspyr config.ini `MaxSimultaneousThreads = 8` (the thread fix is applied by vrdual, `game_fixes = 1`, the default) | • Works in the headset: shadows agree in both eyes, no crash with the thread patch.<br>• Looking around works in the headset: with `cull_widen` the game draws past the screen edge, so a head turn (`head_look = scroll`) shows more of the map, and turning further scrolls it.<br>• The depth effect is modest for a top-down map.<br>• Steam runs the native Linux port (Aspyr, from the Mac code), not the Windows build; Proton must be forced for that.<br>• Port vs Windows build: DX9 renderer (no DX11-only options), not updated with Windows (version mismatch in multiplayer with Windows players), no mods that replace the game core DLL (e.g. Vox Populi); players report both better and worse performance than under Proton.<br>• Port bug, also without vrdual (Steam reports since 2018): crash after a few to ~25 minutes. Each thread entering its TBB job system gets an index from an unbounded counter, but the per-thread render data has only n = `MaxSimultaneousThreads` slots (clamped to 3..16); two threads more than n enter, and a render job on such a thread reads a null slot.<br>• Fix (applied by the vrdual layer in the game's process, `game_fixes`; the game file stays as it is): `JobManager_GetNumberThreads()` returns 16 (the port's own maximum), with `MaxSimultaneousThreads = 8`; over 30 minutes without a crash with the same patch in the file (before: 2-25 min).<br>• Shadows: the game draws its shadow map at the 3D view's size into another target; `auto_noshift = 1` leaves it unshifted, so both eyes' shadows agree.<br>• Wider culling (`cull_widen`): every culling frustum of the game comes from one function (`FGXGetClipPlanesFromMatrix`); vrdual replaces it with one that culls with a wider view while the game draws with its own.<br>• HUD in place (`hud_follow`) and the cursor drawn where its target is shown after a turn: new, not tried in the game yet. |
| Control | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (`Control_DX11.exe`, DXVK) or D3D12 (`Control_DX12.exe`, vkd3d-proton), Northlight, 64-bit | works | 2026-10-29 | DX11: Heroic "Alternative EXE" = `Control_DX11.exe`; DX12: tuned configuration, write-up pending (until then tune it yourself); gamescope 1920x1080 (Heroic) | • DX12 works correctly in the HMD; DX11 runs too.<br>• `Control.exe` starts `Control_DX12.exe`; without the right configuration DX12 crashes at start (vkd3d's first `vkCreateSemaphore` jumps to NULL in winevulkan). |
| Crysis 3 Remastered (2096610) | Steam: negative | Proton, D3D11 (DXVK, 64-bit) | broken | 2026-10-25 |  | • Does not start: a second Vulkan device in the process (likely ray tracing interop) wants a D3D11 shared resource, which needs `VK_KHR_external_memory_fd` (DXVK: `VK_KHR_EXTERNAL_MEMORY_WIN32 not supported`, then an access violation).<br>• To try: ray tracing off in the game's config; `VRDUAL_KEEP_EXT=VK_KHR_external_memory_fd,VK_KHR_external_semaphore_fd`.<br>• Real fix: mirror support for memory shared between two devices of one process. |
| Deus Ex: Human Revolution (28050) | Steam: positive | Proton, D3D9 (DXVK, 32-bit `dxhr.exe`) | works | 2026-10-23 |  | • Works (a first attempt showed nothing in the HMD, cause unknown).<br>• The layer sees `exe='wine-preloader'` and uses the `[app:wine-preloader]` profile. |
| Disney•Pixar WALL-E | Amazon (Heroic): positive | Proton (umu, Proton-CachyOS), D3D9 (DXVK, 32-bit `WALL-E.exe`) | works | 2026-10-28 | gamescope 1920x1080 (Heroic); `[app:wine-preloader]` profile | • No problems. |
| Duke Nukem Forever (57900) | Steam: positive | Proton, D3D9 (DXVK, 32-bit `DukeForever.exe`) | runs | 2026-10-23 | none: ran at `world_scale = 1` (Unreal-derived engine, try ~50) | • Medium effect; some levels are shader-heavy, problems expected there.<br>• The layer sees `app=''` (wine-preloader), so `vrdual save` writes `[app:wine-preloader]`, which then matches every 32-bit Proton game without its own profile.<br>• 1680x1050 under gamescope. |
| The Elder Scrolls III: Morrowind | GOG (Heroic): positive | Proton (umu, Proton-CachyOS), D3D8 (DXVK, 32-bit `Morrowind.exe`, 1440x1080) | works | 2026-10-29 | gamescope (Heroic); `[app:wine-preloader]` profile | • Playable in VR even without mods, though very old and looks poor.<br>• Parallax textures would help (see MODDING.md). |
| Fable Anniversary (288470) | Steam: positive | Proton, D3D9 (UE3, DXVK, 32-bit) | works | 2026-10-23 | `world_scale` ~10–13 (tuned live), `hfov` ~91 | • Works.<br>• `vrdual save` writes into `[app:wine-preloader]` (the profile it matched by exe), not `[app:Fable Anniversary.exe]`. |
| Fallout: New Vegas (22380) | Steam: positive | Proton, D3D9 | runs |  | `world_scale = 70` (built-in, not tuned) | • HUD shown; `hud_depth` not confirmed yet. |
| Fell in love with coser 5 | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (Unity, DXVK 3.1.1 from Heroic, 64-bit) | works | 2026-10-26 | `VRDUAL_KEEP_EXT=VK_KHR_external_memory_fd,VK_KHR_external_semaphore_fd` (Heroic env) | • Starts and runs (videos rather than a game; not tested further).<br>• Unity asks for a D3D11 shared resource: without `VRDUAL_KEEP_EXT` the game crashes at start; with it the mirror stops on the second device and the right eye freezes.<br>• Fix idea (as Crysis 3): give B plain memory without the import/export structs.<br>• `-screen-width 1920 -screen-height 1080 -screen-fullscreen 0` is ignored (runs at the desktop's 2880x5120). |
| Firewatch (383870) | Steam: positive | Unity | runs |  | landscape resolution | • The portrait desktop cuts the menu. |
| Galactic Civilizations III | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (DXVK, 64-bit `GalCiv3.exe`) | works | 2026-10-29 | gamescope 1920x1080 windowed (Heroic) | • Works correctly.<br>• Mouse cursor in the HMD: under gamescope (64-bit games) it is read from the root of gamescope's X screen. |
| Grim Fandango Remastered | GOG (Heroic): positive | Proton (umu, Proton-CachyOS), OpenGL → Zink (32-bit `GrimFandango.exe`) | works | 2026-10-27 | gamescope 1920x1080 (Heroic); `[app:wine-preloader]` profile (`world_scale` ~0.1) | • Works. |
| Hogwarts Legacy (990080) | Steam: positive | Proton, D3D12 (UE4) | works | 2026-10-23 | tuned configuration, write-up pending (until then tune it yourself) | • Impressive stereo.<br>• Fast since cached-memory write tracking (before: mirror memory sync ~12 ms per submit).<br>• No native VR (a UEVR mod exists for Windows). |
| Hospital Tycoon (11590) | Steam: positive | Proton | works | 2026-10-20 | gamescope 1920x1080 (below) | • Works; the system cursor is drawn by `vrdual xr` at the scene's depth (`cursor_depth = auto`).<br>• Crashes on a portrait desktop (NULL read in the game). |
| LEGO Batman 3 (313690) | Steam: positive | Proton, D3D9 (DXVK) | works | 2026-10-23 | `world_scale` ~0.1 | • Works under gamescope 1920x1080.<br>• The launcher starts `LEGOBatman3.exe` (D3D9) or `LEGOBatman3_DX11.EXE`: a profile saved for one does not apply to the other. |
| LEGO Builder's Journey | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), Unity, D3D12 (vkd3d-proton) + D3D11 (DXVK), 64-bit | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `world_scale` ≤ 2.4 | • Runs since the layer keeps `VK_KHR_external_memory_fd` visible (B gets plain memory for the shared resource).<br>• 3D effect weak; problems above `world_scale` 2.4. |
| LEGO Harry Potter 1-4 (21130) | Steam: positive | Proton | works |  | turn off video effects | • A "ghost" with effects on (likely depth of field). |
| LEGO Marvel Super Heroes (249130) | Steam: positive | Proton, D3D9 (DXVK) | works | 2026-10-23 | `world_scale` ~0.1, `hud_depth` ~75 (user config, `[app:LEGOMARVEL.exe]`) | • Works under gamescope 1920x1080. |
| LIMBO | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D9 (DXVK, 32-bit `Limbo.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `[app:wine-preloader]` profile | • Little to gain: a flat game (2D layers), weak depth. |
| Little Nightmares (424840) | Steam: positive | Proton, D3D11 (UE4, DXVK) | runs | 2026-10-22 | gamescope 1920x1080 (below); `world_scale = 1` | • Dark, no wow effect.<br>• At `world_scale` 1 the eye shift is almost mono (UE units are cm); higher values make the eyes render differently (screen-space effects?).<br>• Micro stutter in the HMD, smooth on the monitor (gamescope nested refresh 60 Hz).<br>• Segfault at exit in `vkDestroySwapchainKHR` (call into an unloaded library). |
| Magicka (42910) | Steam: negative | Proton, XNA on Wine Mono, D3D9 (DXVK, 32-bit `Magicka.exe`) | broken | 2026-10-29 |  | • Crashes at start in `D3D9DeviceEx::ResetSwapChain` (NULL read outside every loaded DLL, likely Mono JIT code), also with the layer passive (`VRDUAL_STEREO=0`): not vrdual.<br>• Earlier it reached the menu, then a black screen when a game loaded.<br>• Mono TLS/HTTP errors in the log (dead Paradox servers), likely harmless. |
| Maid of Sker | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (Unity, DXVK, 64-bit `Maid of Sker.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `world_scale` 0.01-0.02 (larger: too strong) | • Impressive. |
| Medal of Honor (47790) | Steam: positive | Proton, DXVK (32-bit `moh.exe`) | works | 2026-10-29 | `[app:wine-preloader]` profile | • Works; the log shows an 800x600 swapchain (launcher or game resolution, not checked). |
| Mirror's Edge (17410) | Steam: positive | Proton, D3D9 (UE3) | works | 2026-10-31 | `world_scale = 50`, `hud_depth = 1`, `hfov = 106.26` at 16:9; tuned configuration, write-up pending (until then tune it yourself); launch in a 1920x1080 gamescope window (below) | • Stereo works, less impressive than expected.<br>• Set up with [shader rules](tasks/shader-rules.md): with `hud_depth_scope = all`, `hud_depth` also shifts the full-screen post passes (stale strip at one edge, flicker in the letterbox bars); `hud_depth_scope = rules` plus a `hud` rule for the crosshair (`_ hunt find hud`) fixes that.<br>• One crash seen, cause unknown. |
| Never Alone (Kisima Ingitchuna) | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (Unity, DXVK, 64-bit `NeverAlone.exe`) | works | 2026-10-29 | gamescope 1920x1080 (Heroic); `world_scale` ≤ 10 | • Runs since the layer keeps `VK_KHR_external_memory_fd` visible (B gets plain memory for the game's shared resource).<br>• Stereo not impressive: a side-scrolling platformer with little depth.<br>• Artifacts above `world_scale` 10.<br>• Starts in 4K: the Heroic gamescope window type is fullscreen. |
| Oddworld: Abe's Oddysee | GOG (Heroic): negative (2D game) | Proton (umu, Proton-CachyOS), 32-bit `AbeWin.exe`, Zink | no effect | 2026-10-28 |  | • 2D game, no stereo to gain.<br>• Ignored the gamescope size, ran at 4K and very slowly. |
| OpenTTD | GOG (Heroic): negative (2D game) | Proton (umu, Proton-CachyOS), `openttd.exe` | no effect | 2026-10-28 |  | • 2D game, no stereo to gain. |
| Overlord (11450) | Steam: positive | native GL → Zink | runs | 2026-10-23 | `hud_depth` ~2 (subtitles); Mesa ≤ 26.2: `ZINK_DEBUG=noreorder` | • Zink bugs, also without vrdual: flashing squares on the loading screen (fixed in Mesa 26.2.5); green glow after the new-game video (gone in Mesa main since MR 45178, still at some camera angles).<br>• Subtitles sit at infinity with `hud_depth = 0`, doubled against the near scene.<br>• Default 800x600 window; camera odd with the mouse.<br>• Crash on exit with every driver (game bug). |
| Planet Zoo (703080) | Steam: positive | Proton, D3D11 (DXVK) | works | 2026-10-22 | no gamescope; in-game window 1920x1080 (fullscreen picks the portrait 2880x5120) | • Works without gamescope (menus and a zoo).<br>• With gamescope: once a glibc double free on DXVK's queue thread (not seen with `VRDUAL_CACHED_SHADOW=0`), once gamescope itself died; gamescope without vrdual is fine.<br>• Keep `VRDUAL_BACKTRACE=1`: an abort then logs its native frames. |
| Portal (400) | Steam: positive | native GL → Zink | runs | 2026-10-29 | gamescope 1920x1080 (`gamescope -w 1920 -h 1080 -W 1920 -H 1080 --force-grab-cursor -- /home/<user>/vrdual/run /home/<user>/bin/gamescope-pv-display %command%`); quit from the game menu, not Alt+F4 | • Menu, level load and stereo in the HMD work.<br>• **After a level loads the HMD stays black until you move in the game** (mouse): the game presents nothing until then; maybe Source pausing without focus (`-nopause` / `engine_no_focus_sleep 0` not tried).<br>• Alt+F4 closes gamescope, not the game: Portal hangs in its own exit; Steam "Stop" does not end it, kill the PID. |
| Portal 2 (620) | Steam: positive | native GL → Zink | works |  | `-windowed -w 1920 -h 1080` | • Fullscreen picks the portrait 2880x5120 desktop size. |
| Portal: Revolution (601360) | Steam: positive | Proton Experimental, Windows-only Source mod build (`bin/win64/revolution.exe`) | works | 2026-10-30 | Properties → Compatibility → force Proton (Experimental); gamescope 1920x1080 (below); pad: tuned configuration, write-up pending (until then tune it yourself) | • Runs with Proton Experimental.<br>• Without forced Proton, Steam starts the `.exe` natively in the Steam Linux Runtime and it exits after ~2 s. |
| Portal with RTX (2012840) | Steam: negative | Proton, RTX Remix (DXVK-Remix, path tracing) | broken | 2026-10-25 |  | • Path traced from the camera in raygen shaders: no rasterized vertex output to shift.<br>• Remix needs `VK_KHR_ray_query` / `VK_KHR_ray_tracing_pipeline`, which the layer hides; unhiding is not enough (the mirror would have to replay acceleration structures).<br>• Stereo needs a per-eye camera in Remix's ray generation: big work, not planned. |
| Psychonauts (3830) | Steam: positive | native GL → Zink | works |  | `skip_w1 = 0` (built-in) | • Draws the scene with `w == 1` (profile: `skip_w1 = 0`).<br>• One SIGSEGV seen, not analysed. |
| Quantum Conundrum (200010) | Steam: positive | Proton, D3D9 (UE3, DXVK, 32-bit `TryGame-Win32-Shipping.exe`) | works | 2026-10-25 | `[app:wine-preloader]` profile | • Looks nice, nothing special. |
| RollerCoaster Tycoon 3: Complete Edition | Epic (Heroic): negative | Proton (umu, Proton-CachyOS), D3D9 (DXVK, 32-bit `RCT3.exe`) | broken | 2026-10-29 | gamescope 1920x1080 (Heroic); `[app:wine-preloader]` profile | • Hangs loading a park at "Clear data (0%)": ~100 % CPU, no more presents, no error in the log.<br>• Not yet checked with `VRDUAL_STEREO=0`. |
| Shadow Warrior (233130) | Steam: not finished | native GL → Zink | retest | 2026-10-19 |  | • SIGBUS in an old frame export (reverted); window scaling issues. Needs a retest. |
| Skyrim (72850) | Steam: positive | Proton Experimental, D3D9 | works | 2026-10-30 | gamescope 1920x1080 (below); `world_scale = 70` (built-in, not tuned) | • Works; weak depth at `world_scale` 1; long loading. |
| Tomb Raider GOTY (2013, 203160) | Steam: positive | Proton (forced: Proton Experimental), D3D11 (DXVK, 32-bit `TombRaider.exe`) | works | 2026-10-29 | `world_scale` < 10, best ~2 | • Stereo correct, looks better in the HMD than in screenshots, but no wow effect.<br>• At larger scales objects shift too far into the depth.<br>• The native Linux build (Feral) does not start (`libicui18n.so.51` missing): force Proton.<br>• Uses the `[app:wine-preloader]` profile. |
| Tomb Raider I (224960) | Steam: negative | DOSBox | broken |  |  | • Flat 2D image, no stereo.<br>• Exits in windowed mode / on alt+tab. |
| Tomb Raider II (225300) | Steam: positive | Proton | runs |  | `input = mouse` (a = Enter, b = Esc) | • The menu wants keys; pad buttons do nothing. |
| Tomb Raider III (225320) | Steam: positive | Proton | works |  | windowed / 1920x1080 |  |
| A Total War Saga: TROY | Epic (Heroic): positive | Proton (umu, Proton-CachyOS), D3D11 (DXVK, 64-bit `Troy.exe`) | works | 2026-10-30 | gamescope 1920x1080 (Heroic); turn-based strategy: `input = mouse`, `mouse_mode = move` (stick mode is too coarse), `world_scale` ~0.15, `hud_depth = 0` | • Colours right.<br>• The game's swapchain is `R8G8B8A8_UNORM` (most games: BGRA); `vrdual xr` now re-creates its headset swapchain when the frame format changes (before: red and blue swapped after another game). |
| Unravel (1225560) | Steam (+ EA app): positive | Proton, via EA app (`steam2ea://`), D3D11 (DXVK, 64-bit `Unravel.exe`) | works | 2026-10-29 | gamescope 1920x1080; `world_scale` ~0.1 (no own profile, ran with the values in the control block) | • **Wow effect**, although a side-scrolling platformer: the scene is rendered in full 3D.<br>• Renders 1920x800 (letterbox).<br>• The EA app (`EADesktop.exe`) loads the layer too.<br>• Earlier crash at start (second `vkCreateDevice`) not seen since the layer's device lookup cache is invalidated on device create/destroy. |
| The Witcher 3 GOTY (next-gen DX12 build) | GOG (Heroic): positive | Proton (umu, Proton-CachyOS), D3D12 (vkd3d-proton 3.1, 64-bit `bin/x64_dx12/witcher3.exe`) | works | 2026-10-30 | gamescope (Heroic); `[app:witcher3.exe]` `sparse = 1` (without it: "DirectX 12 is required") | • Works in the HMD with `sparse = 1` (vkd3d gives D3D12 feature level 12_0 only with sparse residency, which the layer hides by default).<br>• Stability: one start crashed after 26 s (`SIGSEGV`, not analysed), the next worked; earlier sessions hung once at start and once hung the display GPU after exit (`ring sdma0 test failed`, reboot). Suspects: the `vkQueueBindSparse` replay (rewritten since), vkd3d's copy queue on the display GPU.<br>• A dedicated mod exists: [witcher3-vr](https://github.com/tig3rmast3r/witcher3-vr) (next-gen DX12 v4.0.4, Windows, OpenXR) and its fork [joschi127/witcher3-vr](https://github.com/joschi127/witcher3-vr) (also Remastered 5.01, and Linux under Proton + WiVRn via `wineopenxr`, see its `LINUX.md`); made for this game (head-tracked camera, first person, HUD on its own layer), it may give a better result. vrdual took [shader rules](tasks/shader-rules.md) from it.<br>• The mirror's twin instance carries the app's name and engine, so RADV enables `VK_NV_cooperative_matrix2` for vkd3d on both GPUs. |
| X: Beyond the Frontier (2840) | Steam: positive | Proton, old D3D via wined3d → GL → Zink (32-bit) | works | 2026-10-25 | `[app:wine-preloader]` profile (shared by 32-bit Proton games) | • Very old, poor textures, but the depth is a wow effect: in VR the game becomes likeable. |
| Zen Bound 2 (61600) | Steam: positive | native GL → Zink | works |  |  | • First game tested in the Quest. |
<!-- games:end -->

Single GPU (RTX 5070 Ti, host "sharkoon", alternate-eye): Alice: Madness
Returns, Zen Bound 2, Overlord (same Zink glow), The Witcher 3 next-gen DX12
(2026-10-31: eyes clean only with temporal AA/upscaling and motion blur off;
XeSS and FSR upscaling mix the eyes, FXAA, HairWorks, SSAO/GTAO, blur, bloom
and DoF look fine, judged by eye in `vrdual view`).

## Game settings that cause problems

| setting | why | effect | mode |
|---|---|---|---|
| temporal AA and upscalers (TAA, DLSS, DLAA, FSR 2/3, XeSS) | blend in the previous frame, which in alternate-eye rendering is the other eye's | each eye shows a ghost of the other eye's picture (Witcher 3: XeSS, FSR; FXAA is fine) | single GPU |
| motion blur | same: uses the previous frame | same ghosting, worst when moving | single GPU |
| frame generation (DLSS FG, FSR FG, XeSS FG) | interpolates between the last two frames, i.e. between the eyes | generated frames mix both eyes | single GPU (untested with two GPUs) |
| other temporal effects (temporal SSR/AO/volumetrics; same mechanism, not seen yet: Witcher 3's SSAO/GTAO look fine) | reuse history from the previous frame | weaker ghosting in those effects | single GPU |
| tessellated hair/fur (NVIDIA HairWorks) | positions come out of tessellation/geometry shaders, the layer shifted only vertex shaders | hair at screen depth, detached from the head (Witcher 3, 2026-10-31) | both; fixed the same day (TES/GS patched), fine in Witcher 3 by eye (single GPU) |
| depth of field and other blur post effects | blur an image whose depth no longer matches the shifted geometry | a "ghost" around objects (LEGO Harry Potter; Witcher 3's DoF, blur and bloom look fine) | both |
| fullscreen on the portrait desktop | the game picks the desktop's 2880x5120 | tall picture in the HMD, cut menus | both (host "fractal") |


Gamescope keeps a game at 1920x1080 whatever the (portrait) desktop is, e.g.
Mirror's Edge launch option. `gamescope-pv-display` is a local helper
(`~/.var/app/com.valvesoftware.Steam/bin/`, the Flatpak's `~/bin`): Flatpak
Steam's container start resets `DISPLAY` to the desktop's, the helper puts
gamescope's `DISPLAY` back inside the container.

```
gamescope -w 1920 -h 1080 -W 1920 -H 1080 --force-grab-cursor -- /home/<user>/vrdual/run /home/<user>/bin/gamescope-pv-display %command%
```

Heroic (Flatpak, Epic/GOG games): game settings → Advanced → Wrapper command
`/home/<user>/.var/app/com.valvesoftware.Steam/vrdual/run` (no args; the
Steam copy of the layer is visible to Heroic), Other → Gamescope 1920x1080.
The same in Settings → Game Defaults (`defaultSettings` in
`config/heroic/config.json`, edit only with Heroic closed) applies to every
game without its own value, so new games get the wrapper and gamescope.
`gamescope-pv-display` does not apply (umu builds the container command
itself); the same DISPLAY reset happens there (game on the desktop, gamescope
WSI: "creating swapchain for non-gamescope swapchain hooking has failed"), so
`vrdual install` patches `~/.local/share/umu/umu-shim` to restore
gamescope's display from `VRDUAL_DISPLAY` (set by the wrapper). Heroic has its own private `/dev/shm`
(`/run/user/UID/.flatpak/com.heroicgameslauncher.hgl/dev-shm`), watched by
`vrdual xr` since 2026-10-25. umu games: the Proton log (layer lines included) goes to
`~/.var/app/com.heroicgameslauncher.hgl/vrdual-<app name>/` (the wrapper sets
`PROTON_LOG_DIR` per game; Proton alone names it `steam-0-….log` for every
game), umu's own output to `…/vrdual/game.log`, overwritten by every launch. Launchers
started by the same wrapper (Ubisoft Connect, EA app) get `stereo = 0` built
in (their CEF UI was full of artefacts when mirrored).

## To test: showcase candidates (2026-10-21)

From [MY_GAMES.md](MY_GAMES.md) (owned): picked for looking good in stereo on a
fixed screen, a calm camera, Proton D3D9/D3D11 or native Vulkan, and no
anti-cheat. Third-person "diorama" games look like a model in front of you and
do not upset the inner ear; `world_scale` can shrink them further.

| game | why | risk | other VR route (as found 2026-10-21) |
|---|---|---|---|
| LEGO Marvel Super Heroes / LEGO Batman 3 / LEGO The Hobbit | LEGO Harry Potter already works; toy bricks on the desk; for everyone | post effects (DoF) off | none found (no VR mod, no vorpX profile confirmed) |
| Little Nightmares | dark miniature diorama, ideal scale and pace | UE4: 10-bit backbuffer, exported since 2026-10-21 (blit to 8-bit) | UEVR (generic, Windows); a separate VR game, Little Nightmares VR: Altered Echoes (2026) |
| Unravel / Unravel Two | photoreal small scale, calm | unknown engine path | Geo-11 (generic DX11 stereo, Windows) reported to work |
| Trine | fairy-tale 2.5D with many layers, strong depth | little camera movement | built-in stereoscopic 3D (side by side) in Trine 1–3 |
| Batman: Arkham Asylum / City | stereo classic of the 3D Vision era, third person, well known | UE3 D3D9 via DXVK: Alice's cached-memory trade-off until TODO 3 | none found (Arkham VR is a separate PS VR / PC VR game) |
| Tomb Raider (2013) / Rise of the Tomb Raider | spectacular vistas, third person | D3D11 / D3D12, heavier | Rise: only the Blood Ties DLC in VR; full game via generic stereo tools |
| Alien: Isolation (Epic) | the best-known flat-to-VR horror: slow, atmospheric, first person (`head_look = edge`) | D3D11 deferred renderer: screen-space effects may be off | MotherVR (Windows only): on Linux vrdual would be the only route |
| Cities: Skylines / Planet Zoo / Jurassic World Evolution / RollerCoaster Tycoon 3 | builders seen from above look like a model on a table; mouse play, the system cursor is drawn at the scene's depth (works in Hospital Tycoon) | Cities: Skylines has a native Linux (OpenGL → Zink) build | none found |
| LEGO Builder's Journey (Epic) | a pure diorama | Unity; ray tracing off | vorpX reported in the classic mode only |

First round (2026-10-22): LEGO Marvel Super Heroes or LEGO Batman 3, Alien:
Isolation, one builder (Cities: Skylines or Planet Zoo), Little Nightmares.

Second wave: Dead Space (2008), Mass Effect Legendary Edition, Unravel Two, BioShock Infinite, Dishonored, Shadow of Mordor, Metro 2033 Redux,
Stray, Ghost of a Tale, Spyro Reignited, Grow Home; first-person ones for
`head_look`. DOOM (2016) is native Vulkan (no DXVK, no cached-memory issue): a
good test of the Vulkan path, too fast for a showcase.

## Tuning a new game

1. **Resolution**: the desktop here is portrait (2880x5120). Use windowed
   1920x1080 or the game's resolution setting, otherwise the HMD shows a tall
   picture and some games cut their menu.
2. **Depth** (`world_scale`, game units per meter): flat image = too small.
   Unreal 100 (some UE3 games 50), Gamebryo/Creation ~70, Source ~39. Tune
   live, then keep it for the game:
   ```bash
   ./vrdual set world_scale 50
   ./vrdual save            # into [app:<game>] of the config
   ```
3. **Both eyes identical**: the game may draw the scene with `w == 1` (CPU
   transformed vertices). Try `VRDUAL_SKIP_W1=0` in the launch option.
4. **Crosshair / HUD doubled**: HUD draws sit at infinity by default. Put them
   at a distance: `./vrdual set hud_depth 5`.
5. **Crash**: check whether Zink alone crashes too
   (`MESA_LOADER_DRIVER_OVERRIDE=zink %command%`, OpenGL games). Logs: Proton
   `~/.var/app/com.valvesoftware.Steam/steam-<appid>.log`, native
   `~/.var/app/com.valvesoftware.Steam/vrdual/game.log`, cores `coredumpctl`.

Found values for everyone go into a profile file in `profiles/` (format:
[CONFIG.md](CONFIG.md)), and into this table.
