# M3 — HMD output via OpenXR (no headset needed to start)

**Goal:** the compositor submits L/R frames to an OpenXR runtime instead of (or
in addition to) the SBS window. Develop against Monado's simulated HMD; later
switch the runtime to WiVRn → Meta Quest 3S. Nothing in the code should be
Quest-specific.

**Why first:** without HMD output, IPD calibration has no absolute meaning
(ARCHITECTURE "Calibration UI"), and head pose (M4) has no source.

**Context:** `compositor/main.cpp` (pairing logic: `pair_newest`, `eye_read`;
keep it), PLAN §F (decision: OpenXR + WiVRn; ALVR/Link rejected and why).

**Approach sketch**
1. Check what is available on Arch: `monado` (AUR?), `openxr` loader, `wivrn-server`.
   Ask the user before installing anything system-wide (no sudo for the agent).
2. New compositor mode `--openxr`: `xrCreateInstance` (Vulkan enable2),
   session on **GPU `0b:00.0`** (CPU-direct, will also encode for WiVRn),
   one swapchain per eye (or one array swapchain), frame loop
   `xrWaitFrame/BeginFrame/EndFrame`, `XrCompositionLayerProjection` with the
   two eye views; pose = `xrLocateViews` (log it now, it feeds M4).
3. Upload: memcpy from shm into a host-visible staging buffer → copy to the
   swapchain image. Simple first; zero-copy is M2.
4. Keep the SBS window path working; the overlay should later be drawn into
   the XR images too (in-HMD calibration UI, PLAN §H).

**Done when:** `vrdual-compositor --openxr` runs against Monado's simulated
HMD and shows both eyes in Monado's preview window, and a selftest check
(skipped with a clear message when no runtime is installed) asserts that the
session reaches `XR_SESSION_STATE_FOCUSED` and submits ≥ 60 frames.

**Pitfalls:** frame pacing now comes from `xrWaitFrame`; lockstep in the
layers still decides which F is pairable, so the compositor just takes the
newest pair. The Quest's refresh (72/90/120 Hz) is set by the runtime.
