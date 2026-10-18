# AGENTS.md — vrdual handoff for AI agents

Read order: this file → [TODO.md](TODO.md) → the task file you pick →
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (how it works, config reference) →
[docs/PLAN.md](docs/PLAN.md) (option space, decisions, why).

## What this is

App-unaware stereo VR on two GPUs. One process per eye: a Vulkan layer hides
all but one GPU, patches every vertex shader to shift clip-space X by the eye
offset, and exports presented frames to `/dev/shm`. A compositor pairs L[F] and
R[F] side by side. IPD/anchor/HFOV change live through a shared control block.
Status: M0 (PoC) + M0.1 (config, live IPD, calibration) done 2026-10-09.
2026-10-18: mirror mode (one process, `VRDUAL_EYE=both`, hidden 2nd GPU), Steam
(Flatpak) + 32-bit + OpenGL via Zink (Zen Bound 2 runs), OpenXR output
(`vrdual-xr`, tested on Monado simulated HMD and in the Quest 3S via WiVRn:
Zen Bound 2, per-eye parallax confirmed by the user).

## Golden rule

`./vrdual selftest` must pass before you start and after every change (exit
0). It takes ~1 min and opens a few small windows (needs `make lib32` deps and,
for the OpenXR check, a Monado build in `build/monado/`, else SKIP). When you add behaviour, add a
check to `vrdual_selftest` in `_`. "Done" means selftest passes plus a new
check for the new behaviour, or a stated reason why it cannot be checked
automatically.

## File map

| path | role |
|---|---|
| `layer/vrdual_layer.cpp` | the Vulkan layer: GPU filter, capability filter, BDA enable + param buffer, shader patch hook, swapchain readback, lockstep |
| `layer/vrdual_mirror.hpp` | mirror mode: hidden twin device via the ICD, handle map, memory shadow/diff, sync, WSI twins |
| `tools/gen_mirror.py` | generates `build/mirror_{dispatch,gen}.hpp` (dispatch table + ~400 replay wrappers) from vk.xml |
| `compositor/xr.cpp` | `vrdual-xr`: OpenXR output (projection layer, game FOV), runtime opened without a loader |
| `common/vrdual_frames.hpp` | frame file reading + L/R pairing (compositor and xr) |
| `common/vrdual_input.hpp` | controllers → virtual pad / mouse / keys (Mapper, pure) + uinput devices |
| `tools/input_test.cpp` | `vrdual-input-test`: Mapper scenarios + uinput pad round trip |
| `testapp/gltest.cpp` | `vrdual-gltest`: GL 2.1-style Xlib/GLX app for Zink tests (also built 32-bit) |
| `layer/spirv_patch.hpp` | SPIR-V rewriter (baked or live params, near/far blend, `w==1` skip) |
| `layer/VkLayer_vrdual.json.in` | layer manifest (`VK_LAYER_VRDUAL_stereo`, explicit) |
| `common/vrdual_shm.h` | shm frame ring + control block structs (C ABI shared by all processes) |
| `common/vrdual_config.hpp` | `.conf` parser, precedence, `save_key` |
| `common/vrdual_ctl.hpp` | control block open/read/write, eye param math, `ctl_save` |
| `compositor/main.cpp` | SBS window, frame pairing, calibration overlay + keys, `--snapshot` |
| `testapp/main.cpp` | stereo-unaware test app; `--scene calib` = static posts at 0.5/1/2/5/20 m |
| `tools/vrdual_ctl.cpp` | `vrdual-ctl` CLI (config show/init, live/set/save) |
| `tools/spvpatch.cpp` | offline patcher driver for `spirv-val` checks |
| `tools/measure_posts.py` | measures disparity/anchor in a calib snapshot vs theory (±1 px / 4 %) |
| `_` / `vrdual` | implementation + human menu / machine CLI (sources `_`) |

## Commands

```bash
./vrdual build                # make; outputs in build/
./vrdual selftest             # THE check
./vrdual run --session S -- APP ARGS   # any Vulkan app, one copy per eye
./vrdual-ctl / ./vrdual live|set|save  # live control of session S (VRDUAL_SESSION or --session)
./vrdual clean                # stale /dev/shm/vrdual-* from manual runs
```

Manual layer use: `VK_ADD_LAYER_PATH=$PWD/build VK_INSTANCE_LAYERS=VK_LAYER_VRDUAL_stereo VRDUAL_EYE=left VRDUAL_SESSION=x APP`.

## Machine facts (host "fractal") — load-bearing

- Two RX 7900 XTX (RADV, Mesa 26). **Always address GPUs by PCI address**:
  DRM cardN and ROCm indices are swapped relative to each other here.
  - `0000:06:00.0`: behind the B550 chipset, **PCIe Gen3 x4**, no PCIe atomics,
    hosts the GNOME desktop. Its gfx ring has hung the whole desktop under
    Firefox before, so keep load on it modest.
  - `0000:0b:00.0`: CPU-direct **Gen4 x16**, headless. It may be running the
    user's production llama.cpp router. Check VRAM and processes before heavy
    work; never kill it.
- Spawning many short-lived GPU processes in a tight loop once wedged
  `06:00.0` (runtime-PM resume failure); only a reboot recovers it. Don't
  write such loops. selftest starts 2 processes, which is fine.
- No sudo. Not installed: `vulkan-headers` (vendored in `third_party/`),
  `vulkan-tools`/`vkcube`, numpy (use plain python or `uv run --with`).
  Installed: `glslc`, `glslangValidator`, `spirv-val`/`spirv-dis`, SDL3,
  ImageMagick, ffmpeg.
- Wayland session; the test app picks MAILBOX so occluded windows don't stall FIFO.

## Invariants / decisions (don't relitigate without new evidence)

- The layer stays **passive**: no UI, network, threads or prompts inside the
  app process. UIs edit the control block; layers only read it. See PLAN §H.
- App-unaware is the point. No changes to apps or engines.
- Config precedence: defaults < `[global]` < `[app:NAME]` < `VRDUAL_<KEY>` env.
  `VRDUAL_EYE`, `VRDUAL_GPU` and `VRDUAL_SESSION` are per process only.
- Changing a struct in `vrdual_shm.h` → bump `VRDUAL_VERSION` or the ctl `version`.
- HMD path = OpenXR app with WiVRn as the runtime (Quest 3S). Quest Link and
  Virtual Desktop are Windows-only; ALVR needs SteamVR. See PLAN §F.
- One fixed (far) IPD is geometrically correct without eye tracking; `ipd_near`
  is an opt-in perceptual tweak (ARCHITECTURE "Near/far IPD").

## Gotchas already paid for

- Zink (Mesa 26) refuses a Vulkan 1.4 driver whose reported apiVersion is
  clamped to 1.3 ("failed to load driver: zink") — hide features, not versions.
- GPL link pipelines come with `FAIL_ON_PIPELINE_COMPILE_REQUIRED`: A hits its
  cache, B returns `VK_PIPELINE_COMPILE_REQUIRED` + null → strip the flag for B.
- Layer globals must never be destroyed: driver threads call in during exit.
- DXVK 3 (Proton) refuses a device without `bufferDeviceAddress`,
  `maintenance5`, `maintenance6` ("No adapters found" in the Proton log). The
  mirror replays device addresses (capture/replay) instead of hiding them.
- A mirror stop must leave the app intact: shadow mappings keep being copied
  to A afterwards (otherwise the game renders stale data and crashes).
  32-bit games: address space, see "Host memory" in ARCHITECTURE.
- One process can have several live swapchains (wined3d). Never size the frame
  file from one swapchain and publish from another (`shm_claim`).
- `/dev/uinput` fails with ENODEV after a kernel package update until reboot
  (the running kernel's modules are gone). The input test then skips.
- Proton logs: Steam writes none by default. Add `export PROTON_LOG=1` to the
  installed wrapper (`vrdual steam-install` rewrites it), log lands in
  `~/.var/app/com.valvesoftware.Steam/steam-<appid>.log`. Launch with
  `flatpak run com.valvesoftware.Steam steam://rungameid/<appid>`. gdb cannot
  attach across pressure-vessel's PID namespace; bisect via the wrapper.

- `glslc` segfaults on `GL_EXT_buffer_reference_uvec2` with constant operands;
  `glslangValidator -V` works.
- SPIR-V inserts must respect section order: capabilities → extensions →
  ext-inst imports → memory model → … → globals before the first `OpFunction`.
  Live mode switches the addressing model to `PhysicalStorageBuffer64`.
- Layer-allocated dispatchable objects (command buffers) need
  `pfnSetDeviceLoaderData`.
- If the app chains `VkPhysicalDeviceVulkan12Features`, adding a separate BDA
  features struct is invalid. The layer flips the app's flag and restores it.
  If the app uses `VK_EXT_buffer_device_address`, live mode is off (baked fallback).
- The swapchain gets `TRANSFER_SRC` usage added for readback. Present waits on
  the layer's semaphore, which is signalled after the copy.
- Measuring parallax: use `--scene calib` (static, known distances). The
  rotating cubes scene is only good for eyeballing.

## Conventions

- `_` = humans (menu); `vrdual` = machines (no prompts, stdout data). New
  operations become subcommands in `_`, not new top-level scripts.
- Code comments and docs in English; match the existing terse comment style.
  The user writes in Polish, is a senior engineer and prefers terse answers.
- Git repo since 2026-10-18 (`master`, no remote). Vulkan-Headers is a
  submodule (`git submodule update --init`). Commit only when the user asks.
- Steam here is the **Flatpak** (`~/.var/app/com.valvesoftware.Steam`): its
  `/dev/shm` is private, `$HOME` inside = that dir. Test games by hand with
  `flatpak run --command=bash com.valvesoftware.Steam -c '...'` (see ARCHITECTURE
  "Steam"); never `pkill -f` a pattern that matches your own shell command.
- The desktop monitor here is portrait (X reports `DP-2 2880x5120`). Fullscreen
  games pick 2880x5120: the HMD shows a tall screen, and Portal 2 (Source)
  renders only part of it correctly. Use `-windowed -w 1920 -h 1080` or the
  game's resolution setting.
- Debug crashes inside the Flatpak: `coredumpctl dump` + gdb with a sysroot of
  symlinks to `/var/lib/flatpak/{app,runtime}/…/files`.
