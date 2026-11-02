# Frame generation and stereo-safe effects (research)

**Status 2026-11-02:** desk research and reasoning only. Nothing in this file
has been tested in a game yet. Owner's question (TODO 22): can frame
generation (DLSS / FSR) work with our approach, on one GPU and on two? And
can we add effects, such as shadows, that break in stereo?

## Frame generation

### Two GPUs (mirror mode)

**FSR 3 FG (FidelityFX).**
- Ordinary compute work: its own optical flow runs in compute shaders, with
  no vendor extension. It also has a frame-pacing swapchain wrapper that
  presents the generated frames from its own thread and queue.
- In mirror mode, A runs it on the left eye's frames and B replays it on the
  right eye's frames. So each eye interpolates only its own images, using
  that eye's depth (the geometry is already shifted) and the game's motion
  vectors.
- The motion vectors come from the game's unshifted matrices. Per vertex,
  the eye shift is almost constant over time, so the error in a motion
  vector is only the change of the shift between two frames:
  - zero with one IPD;
  - with the near/far blend, small, and only for objects moving in depth.
- Expected: it works if the replay keeps up.
- Risks:
  - Presents from FG's pacing thread double the presents. Each present is
    exported, so export bandwidth doubles too. B replays the same presents,
    so the L[F]/R[F] frame counts should still match; backlog item "same
    frame count" applies.
  - Async compute queues and timeline semaphores in the mirror. Supported
    generically, but untested under this load.
  - The UI composition callback (HUD drawn onto both real and generated
    frames).
- First test: on two GPUs, a game with FSR 3 FG, FG on vs off. Compare game
  fps in `vrdual perf` and look in the headset.

**DLSS FG.**
- NVIDIA only: optical-flow hardware through NGX (`VK_NV_optical_flow`,
  `VK_NVX_binary_import`).
- An AMD twin cannot replay it.
- On two identical NVIDIA GPUs, the replay would have to cover NVIDIA's
  private paths: unknown. Low priority, since the reference machine is AMD.
- Workaround that exists: the `dlssg-to-fsr3` mod replaces DLSS FG with
  FSR 3 FG in some games.

**Either way:** FG doubles game frames, not headset frames, and adds about
one frame of latency.

### One GPU (alternate eyes)

The game's FG interpolates frame F (left eye) with frame F+1 (right eye),
which is useless. It must stay off; the site's one-GPU note already says so.

Options outside the game:

a. **Headset reprojection, already there.**
   - For a projection layer, the Quest re-warps the last picture to the
     current head rotation every display frame. That covers head rotation
     between game frames, not camera motion inside the game.
   - Positional timewarp and App SpaceWarp need depth, plus motion vectors
     for AppSW (`XR_KHR_composition_layer_depth`, `XR_FB_space_warp`).
   - Whether WiVRn forwards them: unknown. The strings in
     `libopenxr_wivrn.so` include those type names, but that is only the
     enum-to-string table, not proof of support.
   - `vrdual-xr` logs the runtime's extension list since 2026-11-02 (`[xr]
     runtime extensions`). The next Quest session answers this.
b. **Image-only FG per eye stream in `vrdual-xr`** (like Lossless Scaling /
   lsfg-vk).
   - A compute optical flow between two frames of the same eye,
     interpolating 30 to 60 fps per eye.
   - Costs GPU time in xr, artefacts at disocclusions, +1 frame latency.
   - lsfg-vk itself cannot be used: it is a layer on an app's swapchain and
     needs the user's copy of Lossless Scaling (proprietary shaders).
c. **Depth-image-based rendering (DIBR) of the eye not rendered this frame**,
   most promising.
   - Alternate eyes stay. In each frame, the missing eye is warped from the
     newest frame of the other eye, using its depth and the known eye
     offset (a pure x shift in clip space, so the warp is a per-pixel
     horizontal disparity `ipd · P00 / depth`).
   - Full frame rate for both eyes, with a real view every other frame.
   - Holes at disocclusions are filled from that eye's previous real frame.
   - Prior art: ReShade SuperDepth3D, 3D Vision "compatibility mode"
     (2D + depth).
   - Needs the depth export below.

## Effects that break in stereo

What goes wrong, by kind:

1. **Shadow maps.** The shadow pass's vertex shader gets patched as well, so
   shadows shift per eye. Fix: a `noshift` rule per shader. Rules exist; the
   automatic version is the blocked item 6.
2. **Screen-space passes that rebuild positions from depth** with the game's
   unshifted inverse matrices: deferred lighting, SSAO, SSR, decals,
   volumetrics.
   - The reconstructed position is off by the eye shift (± IPD / 2).
   - Diffuse lighting hardly shows it; SSR, decals and light shafts do.
   - Fixes:
     - turn the effect off in the game;
     - patch the pixel shader where it unprojects (3Dmigoto's approach). That
       needs the matrix constant per shader, so it is per game;
     - NVSTEREO (item 15).
3. **Screen-positioned effects assumed mono** (sun shafts from the
   screen-space sun position, lens flares): drawn at screen depth. Fix:
   `hud` / `noshift` rules (item 17).
4. **Temporal effects** (TAA, TAAU, motion blur): only a problem on one GPU,
   where they mix the eyes. Two GPUs are fine.

Effects we could add ourselves, per eye, after the game (in `vrdual-xr` or a
post pass in the layer):

- **SSAO from the exported depth.** Generic: P from `hfov`, aspect and an
  assumed near/far. Replaces the game's SSAO when that is switched off. Doable.
- **Real shadows: no.** Shadow maps need the scene geometry and the lights,
  which an app-unaware layer does not have. Screen-space contact shadows
  need a light direction, also unknown. Not generic.
- **Depth-aware comfort aids:** automatic `hud_depth` from the depth at the
  centre, and an edge mask scaled by depth.

**Post layers** (vkBasalt; ReShade via vkBasalt or the Linux ReShade builds):
- The order in `VK_INSTANCE_LAYERS` decides.
  - Above vrdual (app side): its commands go through the mirror and are
    replayed on B, so both eyes get the effect.
  - Below vrdual: only A's eye gets it.
- To check with vkBasalt CAS on the test app. vkBasalt is not installed on
  the reference machine.
- ReShade depth add-ons such as SuperDepth3D are the DIBR prior art above.

## Next steps, in order

1. Done 2026-11-02: `vrdual-xr` logs the runtime's extensions. Check
   `XR_KHR_composition_layer_depth` / `XR_FB_space_warp` with WiVRn in the
   next Quest session.
2. Done 2026-11-02: **depth export** (`depth_export = 1`, frame file v5,
   ARCHITECTURE "Depth export"). The most used depth image with the
   swapchain's extent (render pass begins), copied at present as float NDC z.
   Selftest: the calib posts' depth matches their distances where the left
   image shows them, one process per eye and one GPU. Left:
   - mirror mode (B's twin image, copied in `present_twin`);
   - render scale / dynamic resolution (depth extent ≠ swapchain extent);
   - check in games: which image gets picked, layout right, cost per frame
     (1080p: one more 8 MB copy; the export stats line covers it).
3. DIBR of the missing eye for one GPU in `vrdual-xr` (needs 2).
4. FSR 3 FG on two GPUs in a real game (needs the owner and a game).
5. Optional: SSAO from depth (needs 2); vkBasalt stacking test.

Sources: [lsfg-vk](https://github.com/LoveDoLove/lsfg-vk), [Unity: XR
SpaceWarp](https://docs.unity3d.com/Manual/xr-graphics-spacewarp.html),
[Meta: OS compositor, positional timewarp](https://developers.meta.com/vr/documentation/unity/os-compositor/),
[NVIDIA forum: DLSS 3 FG on Linux](https://forums.developer.nvidia.com/t/dlss-3-and-frame-generation-on-linux/239304),
[GamingOnLinux: FSR 3 FG under Proton](https://gamingonlinux.com/2024/03/the-last-of-us-part-i-added-amd-fsr-3-with-frame-generation),
[WiVRn](https://github.com/WiVRn/WiVRn).
