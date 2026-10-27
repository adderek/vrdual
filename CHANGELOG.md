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
- `vrdual-xr` staging in cached memory (the edge mask read write-combined
  memory: 89 ms per frame, 10 Hz in the headset); stats split a headset frame
  into xrWaitFrame and our phases. `wivrn_gpu` puts the server on 0b.
- Games: see docs/GAMES.md.

Known limits: mirror mode needs identical GPUs; Wayland-native GL apps
through Zink fail (X11/XWayland work); games swapping the swapchain (FSR 3 /
DLSS frame generation) are not supported; HDR / 10-bit swapchains are not
exported.
