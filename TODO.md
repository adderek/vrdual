# TODO

Ordered by what to do next. Each item links to a self-contained task file.
Before you start: `./vrdual selftest` must pass (see [AGENTS.md](AGENTS.md)).

1. Quest 3S works (2026-10-18). Input + head motion implemented 2026-10-18 ([input](docs/tasks/input-emulation.md), [M4](docs/tasks/M4-head-pose.md) steps 1–3): test in the Quest after a reboot (uinput module), then auto aim/motion switch, rotation via clip transform, the rest of [M3](docs/tasks/M3-openxr-output.md) (overlay in the HMD)
2. [M1](docs/tasks/M1-single-process-mirroring.md) follow-ups: dirty tracking for shadows done 2026-10-21 (userfaultfd + PAGEMAP_SCAN; measure in a game: CoJ Gunslinger had 26 ms/submit over 336 MB), cached mappings: write tracking (mprotect + SIGSEGV) opt-in per game (`cached_wp = 1`; CoJ Gunslinger 24 → 0.13 ms/submit) because kernel writes into the buffers fail with EFAULT (Alice: black flickering textures); proper fix: item 3, async B waits, Alice Madness Returns: CTD at the intro video fixed (32-bit address space + stop handling), recheck in game, Wayland-native GL via Zink
3. [Cached memory: shadow + GPU write-back](docs/tasks/cached-shadow.md): implemented 2026-10-21 (default; selftest: pread into the mapping, GPU fill read back); left: measure and check in games (Alice: no flicker and fast? Gunslinger), then drop `cached_wp`
4. [M2 — faster frame transport + measurements](docs/tasks/M2-frame-transport.md): async double-buffered readback done 2026-10-21 (1080p mirror 6.6 → 4.0 ms/frame); left: dma-buf (no CPU copy) if needed
5. [M4 — head pose](docs/tasks/M4-head-pose.md): needs M3
6. [M5 — real applications](docs/tasks/M5-real-apps.md): after M1 for games, demos earlier
7. [M6 — single-GPU mode](docs/tasks/M6-single-gpu.md): alternate-eye rendering (2026-10-18); same-frame eyes optional
8. [Guided IPD calibration](docs/tasks/guided-calibration.md): small, independent
9. [Borrowed from vorpX/FlatVR](docs/tasks/borrowed-features.md): HUD depth, cinema/curved screen, screen grab (done), HUD scale (needs HUD vs post-process distinction), edge peek; depth reprojection (Z3D) = M7 on branch `single-gpu`
10. [Backlog: small defects and hardening](docs/tasks/backlog.md)
11. Game profiles for sponsors (owner's decision 2026-10-20): curated per-game
    settings (`kAppProfiles` today: built into the binary) become a sponsor
    feature. Needs a separate data source the code loads (e.g. a profiles file
    from a private repository, which GitHub Sponsors tiers can grant access to)
    (the public code is Apache-2.0 since 2026-10-20: anything in it can be
    copied, so gated profiles must live outside this repository).
    Model since 2026-10-21: sponsorware, see `docs/PLAN.md` §J (early per
    feature and per game, public after 3 months). First step:
    `vrdual profiles sync` into a local cache, then auto-pick by exe name
    (store ids as aliases, PLAN §K).
12. Non-Steam launchers (PLAN §K): GOG native via `vrdual run`, Heroic /
    Lutris / umu wrapper command, `heroic-install` if Heroic is a Flatpak;
    store/build column in `docs/GAMES.md`.
13. `vrdual thanks` (PLAN §L): open-source projects in the user's stack that
    accept sponsors; engine name into the frame header.
14. User feedback without a server (PLAN §M): done 2026-10-21 (play log,
    `vrdual games | request | report`, no-profile hint). Left: store id and
    engine in the report (with item 13); check on GitHub that the prefilled
    Discussions body and the issue form fields arrive.
15. [3D Vision Ready games: use their own stereo fixes](docs/tasks/nvstereo.md)
    (NVSTEREO emulation; found in Alice: Madness Returns, UE3 with NVIDIA's
    integration): `nvapi.dll` stand-in, per-eye stereo parameter texture,
    NVIDIA's shift model; fixes shadows / screen-space effects, one and two
    GPUs. Research first (logging-only stand-in).
16. Skip intro videos (owner's idea 2026-10-23): `vrdual intro off|on GAME`
    renames the logo videos listed in the game's profile (`.orig` suffix,
    reversible; Steam file verification restores them). Data per game lives in
    the sponsor profiles (first: Overlord, `Video/Title_*.bik`); the command
    itself is public.
17. [Shader rules](docs/tasks/shader-rules.md) (from witcher3-vr,
    2026-10-31): per-shader `noshift | hud | hide` by SPIR-V hash in
    `shader_rules`, live bisection `_ hunt` to find a shader. Step 1 done;
    next: try it in a DXVK game (key stability), then HUD scale.

Done: M0 PoC, M0.1 config + live IPD + calibration (2026-10-09). M1 mirror mode
(one process, hidden second GPU), Steam/Flatpak/32-bit/OpenGL (Zink) support,
M3 OpenXR output tested on Monado's simulated HMD (2026-10-18). Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); options and decisions in [docs/PLAN.md](docs/PLAN.md).
