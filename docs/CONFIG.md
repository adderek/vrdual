# vrdual configuration reference

Every setting can be given four ways; later ones win:

**default < `[global]` < built-in game profile < `[app:NAME]` < environment `VRDUAL_<KEY>`**

Built-in game profiles (`kAppProfiles` in `common/vrdual_config.hpp`): values
found for specific games, e.g. Psychonauts `skip_w1 = 0`, Mirror's Edge
`world_scale = 50`, `hud_depth = 1`. `./vrdual-ctl show APP` marks them `built-in`.

- The environment variable is the key in upper case: `screen` → `VRDUAL_SCREEN`,
  `hud_depth` → `VRDUAL_HUD_DEPTH`, `map_a` → `VRDUAL_MAP_A`.
- `NAME` is the game's `VkApplicationInfo` name or its exe basename
  (case-insensitive). `./vrdual live` prints it as `app` while a game runs.
- Keys marked **live** can also be changed while the game runs:
  `./vrdual set KEY VALUE`, read back with `./vrdual live`, kept with
  `./vrdual save` (`--global` for `[global]`; `ipd`, `ipd_near`, `near_dist`,
  `far_dist` are personal and only saved with `--global`).

## Where the file is

| who reads it | path |
|---|---|
| host tools (`vrdual xr`, `view`, `run`, `vrdual-ctl`) | `$VRDUAL_CONFIG`, else `$XDG_CONFIG_HOME/vrdual/vrdual.conf`, else `~/.config/vrdual/vrdual.conf` |
| Steam games (the layer inside the game) | the copy `vrdual steam-install` makes: `~/.var/app/com.valvesoftware.Steam/vrdual/vrdual.conf` (Flatpak) or `~/.local/share/vrdual/vrdual.conf` |

Edit the host file, then run `./vrdual steam-install` so games see the change
(`vrdual xr` warns when the copy is out of date: at its start, and for a
Flatpak game at each game start, in the headset notice as `OLD CONFIG`). `./vrdual-ctl path` prints
the host path, `./vrdual-ctl init` writes a commented default file there,
`./vrdual-ctl show [APP]` shows each effective value and where it came from.

```ini
[global]
ipd = 0.064
screen = curved

[app:FalloutNV.exe]
hfov = 100
hud_depth = 2
input = mouse
map_x = KEY_E
```

## Stereo (the layer, in the game)

| key | default | values | meaning |
|---|---|---|---|
| `ipd` | `0.064` | meters (`64mm` ok for `set`) | eye distance for far objects; **live** |
| `ipd_near` | `0` | meters, 0 = same as `ipd` | optional perceptual IPD for near objects; **live** |
| `near_dist` | `0.5` | meters | where `ipd_near` applies fully; **live** |
| `far_dist` | `5` | meters | where `ipd` applies fully; **live** |
| `anchor` | `center` | `center` `left` `right` | which eye the game's own camera is; **live** |
| `world_scale` | `1` | game units per meter: Unreal 100 (some UE3 games 50), Gamebryo/Creation (Skyrim, Fallout 3/NV) ~70, Source ~39. Too small = flat image; tune live | **live** |
| `hfov` | `90` | degrees | the game's horizontal FOV (sets the eye shift and the HMD picture size); **live** |
| `stereo` | `1` | `0` `1` | `0` = the layer stays passive in this app (no mirror, shader patch, export), as without `VRDUAL_EYE`; for launchers started by the same wrapper as the game. Built in for Ubisoft Connect (`upc.exe`, `UplayWebCore.exe`) and the EA app (`EADesktop.exe`), Wine's `explorer.exe` and Proton's `xalia.exe`. Wine processes also match `[app:NAME]` by the .exe in their command line (`upc.exe`), not only by `pApplicationName` |
| `skip_w1` | `1` | `0` `1` | leave `w == 1` draws (HUD, post-process, 2D) unshifted |
| `sparse` | `0` | `0` `1` | `1` = sparse binding / residency stay visible (hidden by default with the other features the mirror may not replay); vkd3d-proton needs them for D3D12 feature level 12_0 (Tiled Resources Tier 2). The mirror replays `vkQueueBindSparse` on the 2nd GPU (Witcher 3 with `sparse = 1` ended in a 06:00.0 hang before the replay got queue locking and fence tracking, 2026-10-30; GAMES.md) |
| `shader_rules` | (empty) | comma list `KEY:MODE`, MODE `noshift` `hud` `hide` | per vertex shader: `noshift` = not patched (same in both eyes), `hud` = every vertex treated like `w == 1` (at `hud_depth`), `hide` = draws nothing. KEY = 8-16 hex digits, a prefix of the shader's FNV-1a 64 hash of its SPIR-V (`vrdual hunt shaders` lists them, `_ hunt find` finds one). Keys hold for one game build and one DXVK / vkd3d / Zink version; stale keys match nothing (logged at exit). Applies at app start. See `docs/tasks/shader-rules.md` |
| `hud_depth_scope` | `all` | `all` `rules` | `all`: `hud_depth` moves every `w == 1` vertex (HUD, but also full-screen post passes: seams, stale strips at one edge); `rules`: only shaders with a `hud` rule in `shader_rules`, everything else with `w == 1` stays unshifted. Find the HUD shader with `_ hunt find hud`. Applies at app start |
| `hud_depth` | `0` | meters, 0 = infinity | show `w == 1` draws (HUD) at this distance; live mode only; **live** |
| `live` | `1` | `0` `1` | live params (shader reads a GPU buffer); falls back to baked automatically |
| `export` | `1` | `0` `1` | publish frames to `/dev/shm` |
| `cursor` | `1` | `0` `1` | export the X11 cursor (position over the game window + image) with the frames; `vrdual xr` draws it (games using the system / hardware cursor, which the swapchain never contains) |
| `cursor_depth` | `auto` | `auto` `hud` | auto = at the depth of the scene under the pointer (both eye images matched there; kept on flat areas); hud = at `hud_depth` |
| `lockstep` | `1` | `0` `1` | hold each frame until the other eye has it |
| `lockstep_timeout_ms` | `250` | ms | give up waiting after this |
| `edge_mask` | `outer` | `outer` `both` `off` | `vrdual xr`: black strip, then a fade, at each eye's outer edge (left eye: left, right eye: right), the "floating window" of 3D cinema. Each eye's shifted view shows a strip there the game never drew for its own camera (culled objects, pop-in, odd colours) and near objects seen by one eye only (flicker). `both` = also the inner edge (vignette) |
| `edge_mask_dist` | `1` | meters | strip width = how far the eye shift moves things this near (hfov 90°, IPD 68 mm: 1 m → 33 px of 1920, 0.5 m → 65 px); follows IPD, anchor, hfov and head lean live |
| `cached_wp` | `0` | `0` `1` | mirror mode, superseded: since 2026-10-21 cached (driver-mapped) GPU memory gets a shadow with dirty tracking and write-back by default, fast and correct. `1` = the older write tracking on the driver's own mapping (mprotect): about as fast, but **breaks games that read files straight into that memory** (the kernel's writes fail; Alice: Madness Returns showed black flickering objects). Keep `0` |
| `left_gpu` | `pci:0000:06:00.0` | `pci:…`, index, name substring | GPU for the left eye (and the game in mirror mode) |
| `right_gpu` | `pci:0000:0b:00.0` | same | GPU for the right eye |
| `wivrn_gpu` | `pci:0000:0b:00.0` | `pci:…` `default` | GPU for `wivrn-server` (`vrdual wivrn`, via `DRI_PRIME`); `vrdual-xr` follows it. Keeps the game's GPU (06: game, desktop, gamescope) free. `default` = Vulkan's choice. Read when the server starts ([global] only) |
| `log` | `1` | `0` `1` | layer log to stderr (Proton log for Steam games) |
| `head_lean` | `off` | `off` `on` | head position moves the rendered viewpoint (needs `vrdual xr`) |
| `lean_max` | `0.1` | meters | soft limit of the lean offset |
| `lean_y_sign` | `1` | `1` `-1` | `1` GL/D3D games, `-1` Vulkan apps that flip y |

## HMD picture (`vrdual xr`)

| key | default | values | meaning |
|---|---|---|---|
| `screen` | `curved` | `curved` `flat` `full` | full = game FOV around you; flat / curved = cinema screen |
| `screen_dist` | `2.5` | meters | flat/curved screen distance |
| `screen_fov` | `60` | degrees | flat/curved screen width seen from the centre |
| `screen_scale_x` | `1` | 0.1..10 | picture width scale (grab changes it; the release log prints the value) |
| `screen_scale_y` | `1` | 0.1..10 | picture height scale |
| `overlay_time` | `4` | seconds, 0 = off | notice panel 1.5 m in front of the eyes on events (headset connected, game started / stopped, screen recentred or moved, tune changes): the event, the game, screen and input mode, ipd, world_scale, hud_depth, hfov |

Command line: `vrdual xr [--session S] [--lock world|head] [--no-start-wivrn] [--restart-wivrn] [--no-audio] [--frames N] [--swap]`.
`vrdual wivrn-restart` restarts the server alone (a running `vrdual xr` reconnects).
Start it once, before or after the game; it keeps running across games and
reloads each new game's settings.
With WiVRn as the runtime it needs `wivrn-server`: when none runs it starts
one in the background (log `$XDG_RUNTIME_DIR/vrdual-wivrn.log`);
`--no-start-wivrn` refuses instead. `vrdual wivrn` (foreground) and the
background start share a lock, so vrdual never runs two servers.
Audio: when the headset connects (WiVRn's `wivrn.sink` appears) it becomes the
default output, so games play in the headset; the previous default comes back
when `vrdual xr` exits. Only on connect: an output chosen by hand later stays.
`--no-audio` leaves the outputs alone.
`--lock world` (default) keeps the picture in the room; `--lock head` keeps it
in front of the face (films). Controls (menu: every mode; grab: `tune` only,
elsewhere the grips are the game's):

| control | world lock | head lock |
|---|---|---|
| menu (≡, left) held 1 s | picture in front of the current gaze | freeze in place / follow again |
| menu held + a / b | next / previous control mode: off → gamepad → mouse: stick → motion → aim → move → tune → map (notice in the headset) | same |
| both grips held (`tune`) | move with the hands, hand distance scales; no turning | same |
| … + left trigger held | also turn with the hands' line (15° snap; press the right trigger during the grab for free turning) and orbit with the left stick | same |
| … + right stick | stretch width (x) / height (y) | same |
| … + left trigger + left stick | orbit the picture around you | same |

## Controllers → game (`vrdual xr`, needs `/dev/uinput`)

| key | default | values | meaning |
|---|---|---|---|
| `input` | `gamepad` | `gamepad` `mouse` `tune` `map` `off` | gamepad = virtual Xbox 360 pad; mouse = mouse + keyboard; tune = the controllers tune stereo settings and move the screen (below), the game gets nothing. The mode a game starts in; menu + a / b switches it in the headset. Each switch is remembered per game (`~/.local/state/vrdual/input_last.tsv`, with `mouse_mode`) and the next start uses it instead of `[global]` / built-in; `input` in `[app:NAME]` (or `VRDUAL_INPUT`) wins over the remembered mode. The `vrdual xr` log says which one applied (`input: …`) |
| `mouse_mode` | `stick` | `stick` `motion` `aim` `move` | right stick moves the pointer / right controller rotation / controller ray = absolute pointer / right controller moved across the screen, like a desk mouse (5000 counts per metre × `mouse_speed`). motion and move: left grip held = clutch (re-centre the hand), right stick y = wheel. A short menu press cycles them |
| `mouse_speed` | `1` | factor | pointer speed (stick, motion, move), per game in `[app:NAME]`; `vrdual xr` picks up a changed value within 1 s (no restart) |
| `mouse_curve` | `2.5` | exponent | stick response (stick mode) |
| `deadzone` | `0.15` | 0..1 | stick dead zone |
| `lstick` | `wasd` | `wasd` `arrows` `none` | left stick in mouse mode |
| `map_<mode>_<button>` | built-in per mode | `KEY_*` `BTN_*` `none` | mode `gamepad` or `mouse`, buttons `a b x y l3 r3 lt rt lg rg menu`, e.g. `map_mouse_x = KEY_E`. `map_<button>` (no mode) applies to both. Most specific wins: `[app:NAME]` `map_<mode>_<button>` > `[app:NAME]` `map_<button>` > `[global]` `map_<mode>_<button>` > `[global]` `map_<button>` > built-in (listed commented by `vrdual config init`). `vrdual xr` reloads them when the file changes (no restart). Edit: `_ map` (menu: game → mode → button → key), `vrdual map APP\|--global MODE BUTTON CODE\|none\|default`, `vrdual map APP` shows the effective maps and where each comes from |
| `head_look` | `off` | `off` `mouse` `edge` | head turns the game camera: `mouse` = 1:1 (natural for games where you point with the mouse; in first-person games it takes getting used to, and pairs well with a controller: the stick turns slowly and imprecisely, the head adds quick precise turns); `edge` = turn while looking past the dead zone (easier start in first-person games) |
| `head_deadzone` | `15` | degrees | edge mode dead zone |
| `head_rate` | `120` | deg/s | edge mode turn rate at 45° past the dead zone |
| `head_counts_per_deg` | `10` | mouse counts | per degree of game camera turn; calibrate per game |

`input = tune` (sticks: no grip or trigger held; values print in the `vrdual xr` log;
both grips: the screen grab above):

| control | changes |
|---|---|
| left stick up / down | `world_scale` ×2 / ÷2 per second at full deflection |
| right stick up / down | `hud_depth` farther / nearer, ×2 per second (from infinity: down starts at 100 m; past 100 m back to infinity) |
| right stick left / right | `screen = full`: `hfov` ±20°/s; flat / curved: `screen_fov` ±20°/s (screen width; there `hfov` only scales the depth like `world_scale`, set it with `vrdual set` if needed) |
| a | save them into `[app:NAME]` (and the Steam copy of the config); flat / curved also `screen_fov` |
| b | undo: back to the last saved values; again: to the values the game started with; again: to the defaults (`world_scale` 1, `hud_depth` 0, `hfov` 90, `screen_fov` 60) |

`input = map` (edit button maps in the headset; the game gets nothing, `map` is
not remembered as the game's start mode): a panel lists the 11 buttons with
their code and where it comes from (built-in, `[global]`, this game).

| control | does |
|---|---|
| a b x y l3 r3 lt rt lg rg | pick that button (`>` in the list) |
| right stick up / down | its code: next / previous of `default` (remove this game's setting), `none`, common keys (E F Q R I J M Tab Space Enter Esc Backspace Shift Ctrl Alt 1-4 F1 F5 F9), mouse buttons, in gamepad maps also the pad buttons; repeats while held |
| left stick left / right | edit the gamepad / mouse maps |
| menu + a / b | leave (next / previous control mode) |

Each change is written at once as `map_<mode>_<button>` into `[app:NAME]` and
applies immediately. Other codes: `_ map` or the config file.

Default buttons in mouse mode: a = Enter, b = Esc, x = Space, y = Tab,
l3 = Alt, r3 = middle click, lt = Shift, rt = left click, lg = Ctrl,
rg = right click, menu = mode cycle (held 1 s: recentre). Gamepad mode: Xbox
layout (a b x y, sticks, triggers, grips = bumpers), menu = Start. Menu's own
code (Start, or a `map_menu`) goes out as a tap when menu is released, unless
it was held 1 s or used for menu + a / b.

## Per-process environment only

| variable | values | meaning |
|---|---|---|
| `VRDUAL_SESSION` | name, default `default` | session name (frame files, control block); `--session` on the tools |
| `VRDUAL_EYE` | `left` `right` `both` | which eye this process renders; `both` = mirror mode (Steam wrapper) |
| `VRDUAL_GPU` | like `left_gpu` | overrides the GPU choice for this process |
| `VRDUAL_CONFIG` | path | config file |
| `VRDUAL_SHM_DIR` | path (`vrdual xr`: `a:b` list) | where frame files / control block live (set automatically for Flatpak Steam; `vrdual xr` watches host and Flatpak and follows the game) |
| `XR_RUNTIME_JSON` | path | OpenXR runtime for `vrdual xr` (default: WiVRn Flatpak) |

Debugging only: `VRDUAL_FILTER=0` (no GPU / capability filter),
`VRDUAL_KEEP_EXT=VK_EXT_a,…` (leave those extensions visible),
`VRDUAL_HIDE_FEATURES=0`, `VRDUAL_CLAMP_API=1`, `VRDUAL_TRACE_EXTS=1` (log
enabled device extensions), `VRDUAL_MIRROR_TRACE=N` (log the first N mirrored
calls), `VRDUAL_SHADOW=0`, `VRDUAL_TRANSIENT_MAPS=1`, `VRDUAL_DIRTY=0` (full memory
diff, no userfaultfd), `VRDUAL_CACHED_SHADOW=0` (mirror: cached memory
mapped directly, full diff every submit), `VRDUAL_MIRROR_FAIL_AT=N`
(selftest), `VRDUAL_XR_TEST_RUNTIME` (selftest), `VRDUAL_XR_STATS=S` (`vrdual xr`
stats line every S seconds, default 5, 0 = off), `VRDUAL_XR_LOG=PATH`
(`vrdual xr` output log, default `~/.local/state/vrdual/xr.log`, `0` = off;
every line with date and time; a terminal shows only the state lines, with
the time), `VRDUAL_XR_LOG_BYTES=N` (rotation size, default 1 MB; 4 old files
kept, 5 MB at most), `VRDUAL_BACKTRACE=1` (on
SIGABRT, e.g. glibc "double free", log the aborting thread's native frames as
module + offset for `addr2line`, then abort as before; also the first 8
SIGSEGVs in native code with the faulting ip, then Wine's handler, which
reports a crash inside a Unix call only as `Exception 0xc0000005 in Unix call`).
