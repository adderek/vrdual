# Game-side stereo fixes of "3D Vision Ready" games (NVSTEREO emulation)

**Status 2026-10-21:** idea, nothing built. Found in Alice: Madness Returns.

## What the games have

NVIDIA's 3D Vision Automatic did in the driver what vrdual does in the layer:
every draw twice, vertex shaders shifted per eye; one GPU. Effects that
rebuild positions from depth (shadows, deferred lights, SSAO, post) then come
out wrong, the known "shadows slightly off" limitation. "3D Vision Ready"
games fixed that themselves: shaders read a small stereo parameter texture
(separation, convergence, eye) and correct those effects.

Alice: Madness Returns (UE3, D3D9) has NVIDIA's own engine integration:
`Engine/Config/BaseEngine.ini` carries `; NVCHANGE_BEGIN: Jiayuan -- Allow
Stereo 3D by default` / `AllowNvidiaStereo3d=True`; the exe names `nvapi.dll`,
`nvapi_QueryInterface`, `NvStereoEnabled`, `NvStereoFixTexture`, `StereoFix`,
`EnableStereo3D`. Other UE3 games of that era may have the same; check an exe
with `strings -a GAME.exe | grep -i -E 'nvstereo|stereofix|nvapi'`.

How the texture reached each eye (NVIDIA's `nvstereo.h` approach, from memory:
verify): the app writes both eyes' values side by side into a staging surface
whose last row is a header with the signature "NV3D", then StretchRects it into
a stereo texture; the driver, seeing the signature, gives each eye its half.

## What vrdual would need

1. **`nvapi.dll` stand-in** (Windows DLL, mingw), opt-in per game, next to the
   exe: `nvapi_QueryInterface` answering the stereo calls (enabled, activated,
   eye separation, separation, convergence) from vrdual's config. On AMD the
   game finds no NVAPI and never takes its stereo path. Puts a file into the
   game folder: an owner decision (PLAN §H, app-unaware).
2. **Layer: route the halves.** Recognise the upload (signature "NV3D" in the
   staged data / the small blit that follows): mirror mode gives GPU A the
   left half and B the right half (B's copy of the data is ours to change);
   single GPU (alternate eye) gives each frame its eye's half.
3. **NVIDIA's shift model.** Their shaders correct for
   `x' = x + sep·(w − conv)` (convergence at the screen plane), vrdual shifts
   `x' = x + const` (parallel cameras). Render with NVIDIA's model for such
   games and let `vrdual-xr` shift each eye's FOV by `sep` so that infinity
   lines up: same geometry as today.

## Steps

1. Probe: a logging-only `nvapi.dll` for Alice (which interface ids it asks
   for, in what order, with what values; whether it creates the stereo texture
   at all).
2. Stand-in answering "stereo on"; capture a frame: does the game upload the
   "NV3D" surface? which Vulkan commands does DXVK turn it into?
3. Layer routing + shift model + xr FOV shift; compare shadows/SSAO with and
   without, both GPU modes.

**Done when** Alice's shadows and screen-space effects line up in both eyes
(calib-style measurement or captured frames), one and two GPUs. Fits PLAN §J
"legacy-game tweaks" as an early (sponsor) feature.
