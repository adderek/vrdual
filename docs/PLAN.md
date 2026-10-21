# vrdual — option space and roadmap

Goal: unmodified ("stereo-unaware") 3D apps in VR, using both GPUs: one eye per
GPU. Single-GPU operation stays an option. AMD/RADV first, 2× RX 7900 XTX.

Each axis below lists the options; **[PoC]** marks what the MVP uses.

## A. How the second eye is produced

| option | quality | cost | notes |
|---|---|---|---|
| **A1. Clip-space vertex patch** [PoC] | correct geometry parallax | ~0 | 3D Vision-style. Breaks screen-space effects that read view/proj from UBOs; shifts perspective shadow maps |
| A2. View/projection matrix detection in UBOs / push constants | can also fix lighting, specular, SSR, TAA | heuristic per engine | needed for correct post-processing and for head pose (E2) |
| A3. Depth-based reprojection (DIBR) of one rendered view | holes at disocclusions, wrong speculars | very cheap, single GPU | good fallback for the single-GPU mode; does not use GPU 2 |
| A4. Engine-specific injection (UEVR-like) | best | per engine | out of scope, worth knowing it exists |

A1 → A2 is the natural path: keep A1 as the default, use A2 only where it is
found reliably.

## B. How work is split across GPUs

| option | app instances | sync | notes |
|---|---|---|---|
| **B1. Two app processes, one per GPU** [PoC] | 2 | lockstep at present | only deterministic apps line up; game state diverges |
| B2. **Single process, API mirroring layer** | 1 | inherent | app talks to GPU A; layer creates a shadow `VkDevice` on GPU B and replays every call with handle translation; mapped-memory writes mirrored at submit. This is the real architecture |
| B3. Single process, eyes alternate between GPUs per frame (AFR) | 1 | — | simpler than B2 but doubles latency and halves per-eye rate; bad for VR |
| B4. Single GPU, both eyes | 1 | — | replay each render pass twice with different `clip_dx` (spec constant), or A3. Fallback for 1-GPU users |
| B5. Split-frame inside one eye | 1 | — | no benefit over B2 for VR |

B2 cost drivers: every `vkCmd*` and object call needs a twin (hundreds of entry
points; start with what `vkcube`/`vrdual-cube` use), dispatchable handle
bookkeeping, descriptor/memory aliasing, and detecting host writes to mapped
memory (copy dirty ranges at `vkQueueSubmit`; whole-allocation copy is enough
for a first cut).

## C. Which GPU the app sees (B2 only)

- **Fast GPU (CPU-direct)**: uploads, readbacks and texture streaming go over
  the wide link. HMD on the same GPU → the local eye never crosses PCIe; only the
  remote eye's finished frame travels (one-way, ~1.7 GB/s at 2160²@90 Hz).
- **Slow GPU (chipset)**: no obvious upside on this box; would also put app-side
  readbacks on the narrow link.
- Mirrored uploads reach the shadow GPU over its own link either way, so the
  narrow link carries asset bytes regardless; the question is only latency of
  the primary path. Plan: default to fast GPU, make it a switch, measure frame
  time and upload stalls both ways.
- Generalize: `vrdual gpus` already ranks GPUs by narrowest link on the path.

## D. Frame transport between GPUs

| option | notes |
|---|---|
| **D1. Sync readback + memcpy into /dev/shm** [PoC] | simple, adds a full GPU→CPU wait per frame |
| D2. Async double-buffered readback | removes the stall, +1 frame latency on that eye |
| D3. dma-buf export (`VK_EXT_external_memory_dma_buf`) of a GTT buffer, imported on the other GPU | zero CPU copies; both GPUs DMA the same system-memory pages |
| D4. PCIe P2P into the peer's BAR (ReBAR is on here) | fastest in theory; unclear through the B550 chipset switch, which already fails AtomicOp routing; test before relying on it |

## E. Head tracking (apps are unaware of the HMD)

| option | notes |
|---|---|
| E1. Late reprojection only (OpenXR runtime ATW) | gives rotation comfort, not real camera motion |
| E2. Inject full head pose in clip space: `clip' = P·H·P⁻¹·clip` | needs P (from A2 or user-supplied FOV/aspect/near/far); one more patch in the same place as A1 |
| E3. Head rotation → mouse look emulation (VorpX-style) | works with any game, laggy and coupled to game sensitivity |

## F. Output

| option | notes |
|---|---|
| **F1. Side-by-side window** [PoC] | for checking alignment; usable with SBS viewers |
| F2. OpenXR app talking to Monado | compositor submits the two eye images as an `XrCompositionLayerProjection`; Monado does distortion, ATW and direct mode |
| **F2-stream. Same OpenXR app, runtime = WiVRn** (target HMD: Meta Quest 3S) | WiVRn is a Monado-based OpenXR runtime that encodes (VAAPI on AMD) and streams to the Quest over Wi-Fi or USB; the headset does reprojection. Head pose comes back via `xrLocateViews` (feeds E2). Develop now without a headset against Monado's simulated HMD; same code later on WiVRn |
| F2-alt. ALVR | works on Linux but goes through SteamVR: our side would be an OpenVR driver, a heavier dependency than plain OpenXR |
| not an option | Quest Link / Air Link and Virtual Desktop streamer are Windows-only |
| F3. Direct DRM lease of the HMD | only if F2 adds too much latency |

## G. Shader patch hardening

- Patch the **last pre-raster stage** (geometry / tessellation eval / mesh), not
  always vertex.
- Specialization constant instead of a baked float, so one module serves both
  eyes (required for B2 and B4).
- Cover `VK_EXT_shader_object`, maintenance5 inline modules, pipeline binaries
  and app-side pipeline caches.
- Per-pass heuristics: skip depth-only passes whose extent differs from the
  swapchain (shadow maps), skip passes rendering into small/offscreen targets.

## H. Configuration and calibration UI

| option | verdict |
|---|---|
| UI inside the game process (layer) | no: two copies, crash risk, the layer must stay passive |
| Separate desktop GUI (Qt/GTK, or a Windows .exe) | not now: invisible while wearing the HMD, a second program to keep in sync |
| Local HTTP service | not now: localhost endpoints are reachable from any browser page (CSRF / DNS rebinding) unless authenticated; useful later only as a thin front end for remote control (phone) |
| **Compositor overlay + `vrdual-ctl` CLI** [done] | the compositor's output is what goes into the HMD (F2), so its overlay is the in-headset UI later; the CLI is the scriptable twin |

All front ends edit one thing, the session control block. The layers only read it.
Next steps: in-HMD overlay driven by OpenXR controller input (with M3), and an
optional guided calibration flow that shows one post at a time, asks
"nearer / farther / right?", and bisects the IPD.

## I. Known accepted PoC defects (from the brief)

Culling for the original camera, an eye passing through a near wall, parallax
occlusion mapping and other screen-space post effects. Revisit after B2.

## J. Sponsors and early access (decided 2026-10-21)

No DRM: no keys, device limits or per-device e-mail login. The public code is
Apache-2.0, a fork removes any check, and data files get copied anyway.
Sponsors pay for time and service, not for exclusive access.

- Early access lives in a private repo attached to a GitHub Sponsors tier
  (GitHub grants and revokes access itself; auth = the user's GitHub account
  via `gh`). Builds of the early channel are release assets there.
- Two dimensions of early:
  - feature: a new capability (e.g. legacy-game tweaks: shader injection,
    resolution, textures);
  - game: per-game work (profile, fix or game-specific feature for one game).
- Everything early becomes public (Apache-2.0) **3 months after its first
  early release**. No funding goal to speed it up. The clock starts at release,
  not at "maturity" (subjective, could be postponed forever); an immature
  feature goes public behind an experimental flag, off by default.
- A dud is published too: anything that was in early goes public at the
  3-month mark even if dropped, as archived code (`attic/` or a tag; not
  built, not maintained) with a note on why it failed. No silent removal:
  the promise stays checkable without exceptions.
- Bug fixes are never early: they go public at once. VR basics (world scale,
  FOV, HUD distance, stability) are public, not early.
- No CLA: early code is written by the owner only, so it can carry a
  source-available licence (no redistribution, converts to Apache-2.0 at the
  change date). Outside PRs go to the public repo.
- Fetching is a host-side CLI job (`vrdual profiles sync` into a local cache);
  the layer stays passive and the Steam wrapper only reads the cache.
- To keep merges cheap, early features plug into small public extension points
  (shader hooks, overlay panels, config sections) from their own directories;
  going public means moving the directory.
- Store-neutral: a profile bought for one game works for that game from any
  store (Steam, GOG, Epic, ...). Sponsorship never depends on a store.

## K. Stores and launchers

The layer does not know where a game came from. Profiles are keyed by the
executable name (`kAppProfiles`: `mirrorsedge.exe`); store ids (Steam appid,
GOG product id, Epic app name, umu `GAMEID`) are optional aliases only, so
nothing locks profiles to Steam. GOG (DRM-free, legacy catalogue) is the
reference store for legacy-game tweaks: tests reproduce without a Steam account.

| source | how it starts on Linux | vrdual hook |
|---|---|---|
| GOG native Linux build | directly | `vrdual run -- ./start.sh` (works today) |
| GOG / Epic Windows build | Heroic (gogdl / legendary), Lutris, Bottles, `umu-run` (Proton outside Steam) | Heroic "wrapper command", Lutris "command prefix", umu: plain wrapper |
| Steam | Flatpak, pressure-vessel | `vrdual steam-install` (done) |

Heroic as a Flatpak likely has Steam's problems (private `/dev/shm`, own
`$HOME`) and needs a `heroic-install` twin of `steam-install` (untested:
Heroic/Lutris/umu are not installed on fractal). Epic: DRM is rarely the
problem (Denuvo usually lets Vulkan layers load, ReShade works); anti-cheat
(EAC, BattlEye) in online games is. Offline only, warning in the docs.
`docs/GAMES.md` should name the store/build per row: GOG and Steam builds of
one game can differ.

## L. "Who else to support" (idea, 2026-10-21)

Show the open-source projects the user's game session depends on that accept
sponsors: fair to the ecosystem and an answer to "vrdual is a money grab".

- What: the stack actually used, detected, not guessed: engine name from
  `VkApplicationInfo` (DXVK, vkd3d-proton, Zink), Wine/Proton, Mesa driver,
  XR runtime (WiVRn, Monado), launcher (Heroic, Lutris, umu), plus a curated
  per-game list (source ports, unofficial patches, e.g. OpenMW).
- Data: a curated public file mapping component → project + funding link;
  links come from each project's own `FUNDING.yml`. No paid placement, no
  affiliate links, vrdual listed like the others, not first.
- Where: `vrdual thanks` (stdout data) and an item in the `_` menu; maybe a
  credits panel in the `vrdual-xr` overlay, off by default. Never during
  play, never unprompted, no telemetry, no network unless asked.
- Needs: the layer to record the engine name in the frame header (it already
  sees `VkApplicationInfo`; still passive).

## M. User feedback without a server (decided 2026-10-21; first part done 2026-10-21)

Done: play log in `vrdual-xr`, `vrdual games | request | report`, no-profile
hint (ARCHITECTURE "Commands"). Open: store id and engine in the report (needs
§L's engine name in the frame header).

A server of our own is not worth it for a non-commercial project (cost, plus
GDPR duties: an IP or install id is personal data). GitHub is the backend.

- Game and feature requests: GitHub Discussions, categories "Game requests"
  and "Feature ideas", one discussion per game/feature, ranked by native
  upvotes. `vrdual request` searches for an existing one first, else opens a
  prefilled `discussions/new?category=…&title=…&body=…` URL in the browser
  (no token, no `gh`). Sponsor weight: private sponsor repo or a label, by
  hand while the volume is small.
- "No profile" moment: when a game starts without a profile, the host side
  (CLI/overlay, never the layer) offers to report it, prefilled with what the
  user would not type: exe, store id, engine (DXVK/vkd3d/Zink), Mesa, GPU,
  vrdual version.
- Stats are opt-in per send: a local log (`~/.local/state/vrdual/games.tsv`:
  exe, store, play time, works or not) useful to the user alone;
  `vrdual report` shows exactly what will be sent and opens a prefilled issue
  form (`.github/ISSUE_TEMPLATE/game-report.yml`, fields set by query string).
  Nothing is sent in the background. The reports feed `docs/GAMES.md`.
- Not: background telemetry, third-party endpoints (Cloudflare Workers,
  Plausible), anything sent from the layer.
- Unverified: Discussions prefill parameters behave like issue prefill;
  practical URL limit (~8 KB) fits a report.

## Roadmap

| milestone | content | exit criterion |
|---|---|---|
| **M0 PoC** (done 2026-10-09) | A1 + B1 + D1 + F1, anchor modes, IPD, GPU filter, lockstep | paired SBS frames from two GPUs, parallax measured |
| **M0.1 config** (done 2026-10-09) | `.conf` + profiles, live IPD via buffer device address, near/far IPD, calibration scene + overlay | live IPD change measured on both GPUs |
| M1 | B2 mirroring layer for `vrdual-cube` and `vkcube` | one process, both eyes, same frame, no lockstep needed |
| M2 | D2 then D3; measure per-eye cost on the x4 link at 2160² | numbers for 90/120 Hz in docs/ARCHITECTURE.md |
| M3 | F2 via Monado (Monado on Arch, test with a real HMD or the simulated one) | image in HMD/simulator |
| M4 | E2 head pose + A2 projection detection | head-locked world on the test app |
| M5 | a real Vulkan game, then DXVK title | playable with the accepted defects |
| M6 | B4 single-GPU mode | same app on one GPU, both eyes |
| later | C measurements, D4 P2P, G hardening, other vendors | — |
