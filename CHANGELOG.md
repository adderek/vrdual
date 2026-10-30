# Changelog

Versions are git tags `v0.x.y`; 0.x releases are pre-releases. Every binary
carries its build identity (`./vrdual version`). Release asset:
`vrdual-<version>.tar.gz` from `tools/release.sh` (sources with the
Vulkan-Headers submodule; GitHub's "Source code" archives lack it).

## 0.1.0 (unreleased)

First public pre-release. Linux only; tested on one machine (2x RX 7900 XTX,
Mesa RADV, Flatpak Steam, Meta Quest 3S via WiVRn).

- App-unaware stereo: a Vulkan layer patches vertex shaders to shift each
  eye; no game changes. Vulkan games, OpenGL through Zink, Proton/DXVK/vkd3d.
- Two identical GPUs: mirror mode, the second GPU renders the other eye in
  parallel. One GPU: alternate-eye rendering (half the frame rate per eye).
- Headset output through OpenXR (`vrdual xr`, WiVRn): game FOV or flat /
  curved screen, move and scale it with the controllers, system cursor drawn
  at the scene's depth, game audio to the headset while it is connected.
- Controllers as gamepad, mouse (stick / motion / aim) or tune (live
  world_scale / hud_depth / hfov); menu + a / b switches the mode in the
  headset.
- Live IPD, anchor, world scale and HUD depth (`vrdual live|set|save`),
  per-game config sections, calibration scene.
- Mirror memory sync: all mapped memory goes through a shadow with dirty
  tracking (userfaultfd), cached (readback) memory included, and what the GPU
  writes into host memory is copied back at the app's fence/semaphore sync.
  DXVK d3d9 games went from 16–24 ms per submit (full compare of cached
  memory) towards the ~0.1 ms of the other memory.
- Exported frames are opaque (games leaving alpha 0 showed black).
- Heroic (Epic/GOG through umu): wrapper command, per-game Proton logs,
  launchers (Ubisoft Connect, EA app) passive with `stereo = 0`.
  Assassin's Creed Origins runs smoothly in the headset.
- Mirror: the twin instance carries the app's name and engine, so the
  driver's per-app options match on both GPUs (RADV drirc gives vkd3d
  `VK_NV_cooperative_matrix2`; without it the twin failed with -7). A failed
  twin logs the missing extensions. `sparse = 1` keeps sparse visible
  (D3D12 feature level 12_0; Witcher 3 starts, not stable yet).
- Mirror: `VK_KHR_external_memory_fd` visible (D3D11 shared resources in
  Unity games through DXVK + winevulkan crashed on a NULL call). An import of
  memory the app exported shares B's twin of the exporter, also when another
  VkDevice of the process exported it (Unity's video player: the mirror
  stopped in Beyond Blue's videos); an fd from elsewhere gets plain memory on
  B. A failed allocation on B logs its result and pNext chain.
- Cursor in 64-bit games under gamescope (its WSI layer hides the X window
  from ours): taken from the root of gamescope's X screen.
- `vrdual xr` output log: every line with date and time in
  `~/.local/state/vrdual/xr.log`, rotated (5 MB at most); the terminal shows
  only state lines and errors, with the time.
- Layer: a present that is not exported logs why, once per reason.
- `input = map` (after tune in the menu + a / b cycle): edit button maps in
  the headset; a 14-line panel lists the buttons, a button picks one, the
  right stick steps through common codes, the left stick switches gamepad /
  mouse maps; each change goes into `[app:NAME]` at once.
- Button maps per mode and per game: `map_gamepad_<button>` /
  `map_mouse_<button>` (`map_<button>` for both), `[app:NAME]` overrides
  `[global]`; `vrdual xr` reloads them when the config changes. `_ map`
  edits them (game, mode, button, key), `vrdual map` shows the effective maps
  with their source; `vrdual config init` lists the built-in ones commented.
  A map no longer applies in `off` / `tune`.
- `vrdual-xr` watchdog: a frame loop stuck 10 s in a runtime call
  (`VRDUAL_XR_HANG_S`; WiVRn after the headset's network dropped and came
  back) reconnects in a new process instead of showing black for good.
- `vrdual-xr`: `screen = curved` is the default. Tune: in flat / curved the
  right stick x changes `screen_fov` (screen width; `hfov` there only scales
  the depth), saved with a. Screen grab: both grips only move and scale;
  turning (hands' line, left stick orbit) needs the left trigger held as well
  (accidental yaw / pivot). Notice panel wider (86 characters: the values
  line was cut, e.g. `hfov 9`); shows the screen width.
- `vrdual-xr` staging in cached memory (the edge mask read write-combined
  memory: 89 ms per frame, 10 Hz in the headset); stats split a headset frame
  into xrWaitFrame and our phases. `wivrn_gpu` puts the server on 0b.
- `vrdual xr` re-creates the headset swapchain when the frame format changes
  (an RGBA game after a BGRA one at the same size showed red and blue swapped).
- `mouse_mode = move`: moving the right controller across the screen moves
  the pointer like a desk mouse (left grip = clutch); for strategy games where
  the stick is too coarse. `mouse_speed` (per game) reloads live.
- Mirror: `vkQueueBindSparse` replay on the second GPU takes the queue lock
  and tracks its fence like a submit (Witcher 3 DX12 with `sparse = 1`).
- `vrdual xr` warns at each Flatpak game start when steam-install's config
  copy differs from the host config (headset notice: `OLD CONFIG`).
- Games: see docs/GAMES.md.

Known limits: mirror mode needs identical GPUs; Wayland-native GL apps
through Zink fail (X11/XWayland work); games swapping the swapchain (FSR 3 /
DLSS frame generation) are not supported; HDR / 10-bit swapchains are not
exported.
