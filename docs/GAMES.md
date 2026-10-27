# Games

Status per game, dual GPU (2x RX 7900 XTX, host "fractal") unless noted.
Launch option for all: `/home/adderek/vrdual/run %command%` (from
`vrdual steam-install`); `vrdual xr` runs once for all games. Last update
2026-10-26. Most games here are Steam; Epic/GOG ones go through Heroic
(setup below), Assassin's Creed Origins through Heroic + Ubisoft Connect.

Status: **works** = stereo in the HMD confirmed; **runs** = renders, stereo
not confirmed or weak; **broken** = crash or no picture.

| game (Steam appid) | path | status | settings | notes |
|---|---|---|---|---|
| Zen Bound 2 (61600) | native GL → Zink | works | | first game tested in the Quest |
| Alice: Madness Returns (19680) | Proton, D3D9 (UE3, DXVK, 32-bit) | works | | also single GPU. Two-GPU mirror: fast and without the black flickering shadows/objects since cached memory is shadowed with write-back (2026-10-22; maybe slightly slower than `cached_wp = 1`, which was fast but flickered). `head_look` mouse and edge both work (2026-10-21) |
| Portal 2 (620) | native GL → Zink | works | `-windowed -w 1920 -h 1080` | fullscreen picks the portrait 2880x5120 |
| LEGO Harry Potter 1-4 (21130) | Proton | works | turn off video effects | a "ghost" with effects on (likely DoF) |
| Tomb Raider III (225320) | Proton | works | windowed / 1920x1080 | |
| Psychonauts (3830) | native GL → Zink | works | `skip_w1 = 0` (built-in) | draws the scene with `w == 1`; one SIGSEGV seen, not analysed |
| Mirror's Edge (17410) | Proton, D3D9 (UE3) | works | `world_scale = 50`, `hud_depth = 1` (built-in); launch in a 1920x1080 gamescope window (below) | confirmed 2026-10-20; stereo works but less impressive than expected |
| Hogwarts Legacy (990080) | Proton, D3D12 (UE4) | works | special configuration (sponsor profile) | impressive stereo; fast since the cached-memory write tracking (2026-10-22; before: mirror memory sync ~12 ms/submit); early screenshots showed only the GUI (game image black), the 2026-10-23 ones are fine; no native VR (a UEVR mod exists for Windows); confirmed 2026-10-21 |
| Call of Juarez: Gunslinger (204450) | Proton, DXVK d3d9 (32-bit) | works | special configuration (sponsor profile) | game leaves alpha 0 (black in the HMD before the layer forced alpha 1); weak depth at default settings; mirror memory sync 24 ms → 0.13 ms/submit with cached write tracking; confirmed 2026-10-21, fast 2026-10-22 |
| Aliens: Colonial Marines (49540) | Proton, D3D9 (UE3, DXVK, 32-bit `ACM.exe`) | works | none saved | runs OK; old game, so modest graphics; confirmed 2026-10-23 |
| Duke Nukem Forever (57900) | Proton, D3D9 (DXVK, 32-bit `DukeForever.exe`) | runs | none: ran at `world_scale = 1` (Unreal-derived engine, try ~50) | medium effect; some levels are shader-heavy, problems expected there. The layer sees `app=''` (wine-preloader), so `vrdual save` wrote `[app:wine-preloader]`, which then matches every 32-bit Proton game without its own profile (seen in Deus Ex: HR); 1680x1050 under gamescope, 2026-10-23 |
| Deus Ex: Human Revolution (28050) | Proton, D3D9 (DXVK, 32-bit `dxhr.exe`) | works | | confirmed 2026-10-23 (a first attempt showed nothing in the HMD, cause unknown, then it worked). Layer sees `exe='wine-preloader'` and picked the `[app:wine-preloader]` profile (saved during Duke Nukem Forever) |
| Fable Anniversary (288470) | Proton, D3D9 (UE3, DXVK, 32-bit) | works | `world_scale` ~10–13 (tuned live), `hfov` ~91 | confirmed 2026-10-23. `vrdual save` wrote into `[app:wine-preloader]` (the profile it had matched by exe), not `[app:Fable Anniversary.exe]`, although the layer knew the app name |
| Tomb Raider GOTY (2013, 203160) | Proton (forced: Proton Experimental), D3D11 (DXVK, 32-bit `TombRaider.exe`) | works | `world_scale` < 10, best ~2 | looks better in the HMD than in screenshots. At larger scales objects shift too far into the depth. The native Linux build (Feral) does not start: `libicui18n.so.51` missing (game.log), so force Proton. Picked the `[app:wine-preloader]` profile; confirmed 2026-10-23 |
| Firewatch (383870) | Unity | runs | landscape resolution | the portrait desktop cuts the menu |
| Skyrim (72850) | Proton, D3D9 | runs | `world_scale = 70` (built-in, not tuned) | weak depth at scale 1; long loading |
| Fallout: New Vegas (22380) | Proton, D3D9 | runs | `world_scale = 70` (built-in, not tuned) | HUD shown; `hud_depth` not confirmed yet |
| Tomb Raider II (225300) | Proton | runs | `input = mouse` (a = Enter, b = Esc) | the menu wants keys, pad buttons do nothing |
| Overlord (11450) | native GL → Zink | runs | `hud_depth` ~2 (subtitles); Mesa ≤ 26.2: `ZINK_DEBUG=noreorder` | Zink bugs, also without vrdual: flashing squares on the loading screen (fixed in Mesa 26.2.5), green glow after the new-game video (gone in Mesa main since MR 45178, 0786a9511, which only flushes less often: with the old flush interval main still glows, only at some camera angles); subtitles sit at infinity with `hud_depth = 0`, doubled against the near scene; default 800x600 window, camera odd with the mouse; crash on exit with every driver (game bug), 2026-10-23 |
| LEGO Marvel Super Heroes (249130) | Proton, D3D9 (DXVK) | works | `world_scale` ~0.1, `hud_depth` ~75 (user config, `[app:LEGOMARVEL.exe]`) | gamescope 1920x1080; confirmed 2026-10-23 |
| LEGO Batman 3 (313690) | Proton, D3D9 (DXVK) | works | `world_scale` ~0.1 | gamescope 1920x1080; the launcher can start `LEGOBatman3.exe` (D3D9) or `LEGOBatman3_DX11.EXE`: a profile saved for one does not apply to the other; confirmed 2026-10-23 |
| Magicka (42910) | Proton, XNA on Wine Mono | broken | | reaches the menu, black screen when a game loads (the layer log shows submits continuing, no error). Log full of Mono TLS/HTTP errors (dead Paradox servers: 308/403), likely harmless. Not yet checked without vrdual, 2026-10-23 |
| Civilization V (8930) | native GL → Zink (`Civ5XP`) | runs | user profile `[app:Civ5XP]`, `world_scale` ~0.1 | stereo not impressive; crash to desktop after a while (game.log ends in normal mirror stats, no error; not analysed, not checked without vrdual), 2026-10-23 |
| Unravel (1225560) | Proton, via EA app (`steam2ea://`) | broken | | CTD at start. Not anti-cheat: the game's 2nd `vkCreateDevice` (the 1st device was created and destroyed, 0 frames) faults right after the mirror's twin device is created: `err:vulkan:vkCreateDevice Exception 0xc0000005 in Unix call`, ip = a heap address (0x5555…, a call through a garbage pointer, likely in the layer). The EA app (`EADesktop.exe`) loads the layer too. Log `steam-1225560.log`; not checked with `VRDUAL_EYE=left` or without vrdual, 2026-10-23 |
| Assassin's Creed Origins (Epic, Heroic + Ubisoft Connect) | Proton (umu, Proton-CachyOS), D3D11 (AnvilNext, DXVK, 64-bit `ACOrigins.exe`) | works | gamescope 1920x1080 (Heroic); `[app:ACOrigins.exe]` `world_scale` ~0.1 (larger values: strong nausea, user still getting used to VR), `hud_depth = 0`, `hfov = 90` | great: smooth in the HMD, the 3D stereo is impressive. Fixed on the way: a double free at exit (mirror Destroy/Free wrappers translated handles after A's call, racing handle reuse) and 10 Hz in the HMD (`vrdual xr` staging in write-combined memory, the edge mask read it back: 89 ms/frame). Open: the character seems to float above the ground (screen-space effects? AO / contact shadows off not tried); fixed 2026-10-26: the start crash in ~2 of 3 launches (`vkCreateDevice` `Exception 0xc0000005 in Unix call`: the layer's per-thread device lookup cache returned a destroyed device's data when the third device reused its dispatch key; 3 of 3 starts fine after the fix; likely also UplayWebCore's jump to NULL from the loader); Ubisoft Connect's helpers (upc, UplayWebCore, explorer, xalia) run with `stereo = 0` (built-in), 2026-10-26 |
| Fell in love with coser 5 (Epic, Heroic) | Proton (umu, Proton-CachyOS), D3D11 (Unity, DXVK 3.1.1 from Heroic, 64-bit) | works | `VRDUAL_KEEP_EXT=VK_KHR_external_memory_fd,VK_KHR_external_semaphore_fd` (Heroic env) | starts and runs (user: not really a game, videos; marked working, not tested further). Without the env var: CTD at start, with or without gamescope: Unity asks for a D3D11 shared resource, DXVK logs `Failed to create shared resource: VK_KHR_EXTERNAL_MEMORY_WIN32 not supported` and still calls `vkGetMemoryWin32HandleKHR`, which is NULL (the layer hides `external_memory_fd`; winevulkan lists the win32 extension anyway). With the env var the mirror stops on the 2nd device (`vkAllocateMemory failed on B`: B cannot import the shared memory), so the right eye freezes. Fix idea (same as Crysis 3): keep `external_memory_fd` visible, give B plain memory without the import/export structs. `-screen-width 1920 -screen-height 1080 -screen-fullscreen 0` was ignored (2880x5120), 2026-10-26 |
| Alien: Isolation (Epic, Heroic) | Proton (umu, Proton-CachyOS), D3D11 (DXVK, 32-bit `AI.exe`) | broken | gamescope 1920x1080 (Heroic) | hangs when gameplay starts after the intro videos were skipped; artifacts in areas the game does not draw (game at 1920x1200 in a 1920x1080 gamescope); not analysed yet (logs were overwritten by the next game), 2026-10-25 |
| Tomb Raider I (224960) | DOSBox | broken | | flat 2D image, no stereo; exits in windowed mode / on alt+tab |
| Portal (400) | native GL → Zink | broken | | crash on level load (NULL GL buffer map), also with Zink alone |
| Shadow Warrior (233130) | native GL → Zink | retest | | SIGBUS in the 2026-10-19 frame export (reverted 55c0b81); window scaling issues |
| Hospital Tycoon (11590) | Proton | works | gamescope 1920x1080 (below) | crashes (NULL read in the game) on the portrait desktop; system cursor drawn by `vrdual xr` at the scene's depth (`cursor_depth = auto`), confirmed 2026-10-20 |
| Little Nightmares (424840) | Proton, D3D11 (UE4, DXVK) | runs | gamescope 1920x1080 (below); `world_scale = 1` | dark, no wow: at 1 the eye shift is ~mono for UE units (cm); higher values make the eyes render differently (screen-space effects?), not checked which; micro stutter in the HMD, smooth on the monitor (gamescope nested refresh is 60 Hz); a first start showed a black screen once; segfault at exit in `vkDestroySwapchainKHR` (call into an unloaded library), 2026-10-22 |
| Planet Zoo (703080) | Proton, D3D11 (DXVK) | works | no gamescope; in-game window 1920x1080 (fullscreen picks the portrait 2880x5120) | without gamescope: menus and a zoo, no crash, confirmed 2026-10-22. With gamescope: once glibc "double free or corruption (!prev)" ~10 s in on DXVK's `dxvk-queue` thread (not seen with `VRDUAL_CACHED_SHADOW=0`; write-back copied 0 MB, so the readback shadow is the suspect), once gamescope itself died entering a zoo; gamescope without vrdual fine. Keep `VRDUAL_BACKTRACE=1` set: an abort then logs its native frames |
| X: Beyond the Frontier (2840) | Proton, old D3D via wined3d → GL → Zink (32-bit) | works | `[app:wine-preloader]` profile (shared by 32-bit Proton games) | very old, poor textures, but the depth is a "wow" effect: in VR the game becomes likeable; 2026-10-25 |
| Grim Fandango Remastered (GOG, Heroic) | Proton (umu, Proton-CachyOS), OpenGL → Zink (32-bit `GrimFandango.exe`) | works | gamescope 1920x1080 (Heroic); `[app:wine-preloader]` profile (`world_scale` ~0.1) | confirmed 2026-10-27 |
| Quantum Conundrum (200010) | Proton, D3D9 (UE3, DXVK, 32-bit `TryGame-Win32-Shipping.exe`) | works | `[app:wine-preloader]` profile | looks nice, nothing special; 2026-10-25 |
| Control (Epic, Heroic) | Proton (umu, Proton-CachyOS), D3D11 (Northlight, DXVK, 64-bit `Control_DX11.exe`) | works | Heroic "Alternative EXE" = `Control_DX11.exe`; gamescope 1920x1080 (Heroic) | DX11 runs (mirror on, ~100k submits in the first session). `Control.exe` starts `Control_DX12.exe`, which crashes at start: vkd3d's first `vkCreateSemaphore` (swapchain setup) jumps to NULL in winevulkan (`SIGSEGV ip (nil)`, `err:vulkan:vkCreateSemaphore Exception 0xc0000005 in Unix call`); likely the D3D12 shared fence path on `VK_KHR_external_semaphore_fd`, which the layer hides (same class as Fell in love with coser 5), 2026-10-27 |
| The Witcher 3 GOTY (GOG, Heroic; next-gen DX12 build) | Proton (umu, Proton-CachyOS), D3D12 (vkd3d-proton 3.1, 64-bit `bin/x64_dx12/witcher3.exe`) | runs | gamescope (Heroic); `[app:witcher3.exe]` `sparse = 1` (without it: "DirectX 12 is required") | works, but not stable yet. Fixed on the way: the mirror's twin device failed with -7 (vkd3d enables `VK_NV_cooperative_matrix2`, which RADV offers only by drirc to engine `vkd3d`; the twin instance now carries the app's name and engine). vkd3d gives D3D12 feature level 12_0 only with sparse residency (Tiled Resources Tier 2), which the layer hides by default, hence the new `sparse` key. Stability: one start hung ~1 s after the swapchain (black screen, render thread spinning without syscalls; no stack taken); one session ran ~8 min (~140 submits/s, mirror sync 0.6 ms over 483 MB), and 21 s after the game exited **06:00.0 hung** (`ring sdma0 test failed`, SMU no response, reset failed), machine reboot. Suspects, not checked: the `vkQueueBindSparse` replay on B (generated wrapper, no `qlock`, no fence tracking), vkd3d's copy queue (SDMA) on 06. Next: the game with `sparse = 1` and `VRDUAL_EYE` unset (is 06 stable without the layer?), 2026-10-27 |
| Crysis 3 Remastered (2096610) | Proton, D3D11 (DXVK, 64-bit) | broken | | does not start. The game opens a second Vulkan device in the process (likely the ray tracing interop) and wants a D3D11 shared resource: DXVK `Failed to create shared resource: VK_KHR_EXTERNAL_MEMORY_WIN32 not supported`, then an access violation at `rip=0`. winevulkan builds that extension on `VK_KHR_external_memory_fd`, which the layer hides (the mirror cannot replay external memory on B). Next: turn ray tracing off in the game's config, or check with `VRDUAL_KEEP_EXT=VK_KHR_external_memory_fd,VK_KHR_external_semaphore_fd` whether that alone is the blocker; a real fix = mirror support for memory shared between two devices of one process (larger work). 2026-10-25 |
| Portal with RTX (2012840) | Proton, RTX Remix (DXVK-Remix, path tracing) | broken | | not an AMD problem: Remix logs `Required Vulkan extension VK_KHR_ray_query / VK_KHR_ray_tracing_pipeline not supported` because the layer hides ray tracing (RADV has it). Unhiding is not enough: the image is path traced from the camera in raygen shaders, there is no rasterized vertex output to shift, and the mirror would have to replay acceleration structures on B. Stereo needs a per-eye camera in Remix's ray generation: big work, not planned. 2026-10-25 |
| Portal: Revolution (601360) | Windows-only Source mod build (`bin/win64/revolution.exe`) | retest | Properties → Compatibility → force Proton | Steam launched the `.exe` without Proton (through the Steam Linux Runtime, scout-on-soldier); it exits after ~2 s, no Proton log, the layer never loads. Not a vrdual problem: force Proton and retest. 2026-10-25 |

Single GPU (RTX 5070 Ti, host "sharkoon", alternate-eye): Alice: Madness
Returns, Zen Bound 2, Overlord (same Zink glow).

Gamescope keeps a game at 1920x1080 whatever the (portrait) desktop is, e.g.
Mirror's Edge launch option. `gamescope-pv-display` is a local helper
(`~/.var/app/com.valvesoftware.Steam/bin/`, the Flatpak's `~/bin`): Flatpak
Steam's container start resets `DISPLAY` to the desktop's, the helper puts
gamescope's `DISPLAY` back inside the container.

```
gamescope -w 1920 -h 1080 -W 1920 -H 1080 --force-grab-cursor -- /home/adderek/vrdual/run /home/adderek/bin/gamescope-pv-display %command%
```

Heroic (Flatpak, Epic/GOG games): game settings → Advanced → Wrapper command
`/home/adderek/.var/app/com.valvesoftware.Steam/vrdual/run` (no args; the
Steam copy of the layer is visible to Heroic), Other → Gamescope 1920x1080.
`gamescope-pv-display` does not apply (umu builds the container command
itself); the same DISPLAY reset happens there (game on the desktop, gamescope
WSI: "creating swapchain for non-gamescope swapchain hooking has failed"), so
`vrdual steam-install` patches `~/.local/share/umu/umu-shim` to restore
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

Found values for everyone go into `kAppProfiles` in
`common/vrdual_config.hpp` (and this table).
