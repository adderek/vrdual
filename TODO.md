# TODO

Ordered by what to do next. Each item links to a self-contained task file.
Before you start: `./vrdual selftest` must pass (see [AGENTS.md](AGENTS.md)).

1. Quest 3S works (2026-10-18). Input + head motion implemented 2026-10-18 ([input](docs/tasks/input-emulation.md), [M4](docs/tasks/M4-head-pose.md) steps 1–3): test in the Quest after a reboot (uinput module), then auto aim/motion switch, rotation via clip transform, the rest of [M3](docs/tasks/M3-openxr-output.md) (overlay in the HMD)
2. [M1](docs/tasks/M1-single-process-mirroring.md) follow-ups: dirty tracking instead of full memory diff (DXVK games: 12 ms/submit over 144 MB mapped, LEGO Harry Potter 2026-10-18), async B waits, Alice Madness Returns: CTD at the intro video fixed (32-bit address space + stop handling), recheck in game, Wayland-native GL via Zink
3. [M2 — faster frame transport + measurements](docs/tasks/M2-frame-transport.md)
4. [M4 — head pose](docs/tasks/M4-head-pose.md): needs M3
5. [M5 — real applications](docs/tasks/M5-real-apps.md): after M1 for games, demos earlier
6. [M6 — single-GPU mode](docs/tasks/M6-single-gpu.md): alternate-eye rendering (2026-10-18); same-frame eyes optional
7. [Guided IPD calibration](docs/tasks/guided-calibration.md): small, independent
8. [Borrowed from vorpX/FlatVR](docs/tasks/borrowed-features.md): HUD depth, cinema/curved screen, screen grab (done), HUD scale (needs HUD vs post-process distinction), edge peek; depth reprojection (Z3D) = M7 on branch `single-gpu`
9. [Backlog: small defects and hardening](docs/tasks/backlog.md)
10. Game profiles for sponsors (owner's decision 2026-10-20): curated per-game
    settings (`kAppProfiles` today: built into the binary) become a sponsor
    feature. Needs a separate data source the code loads (e.g. a profiles file
    from a private repository, which GitHub Sponsors tiers can grant access to)
    and a decision on the licence of the public code (no LICENSE file yet).

Done: M0 PoC, M0.1 config + live IPD + calibration (2026-10-09). M1 mirror mode
(one process, hidden second GPU), Steam/Flatpak/32-bit/OpenGL (Zink) support,
M3 OpenXR output tested on Monado's simulated HMD (2026-10-18). Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); options and decisions in [docs/PLAN.md](docs/PLAN.md).
