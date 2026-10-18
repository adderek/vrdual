# TODO

Ordered by what to do next. Each item links to a self-contained task file.
Before you start: `./vrdual selftest` must pass (see [AGENTS.md](AGENTS.md)).

1. Quest 3S works (2026-10-18: Zen Bound 2 in the HMD via WiVRn, per-eye parallax confirmed). Next: [input — controllers as gamepad/mouse/keyboard](docs/tasks/input-emulation.md), then the rest of [M3](docs/tasks/M3-openxr-output.md) (overlay in the HMD)
2. [M1](docs/tasks/M1-single-process-mirroring.md) follow-ups: dirty tracking instead of full memory diff (DXVK games: 12 ms/submit over 144 MB mapped, LEGO Harry Potter 2026-10-18), async B waits, Alice Madness Returns stops presenting after ~65 frames (cause unknown), Wayland-native GL via Zink
3. [M2 — faster frame transport + measurements](docs/tasks/M2-frame-transport.md)
4. [M4 — head pose](docs/tasks/M4-head-pose.md): needs M3
5. [M5 — real applications](docs/tasks/M5-real-apps.md): after M1 for games, demos earlier
6. [M6 — single-GPU mode](docs/tasks/M6-single-gpu.md)
7. [Guided IPD calibration](docs/tasks/guided-calibration.md): small, independent
8. [Backlog: small defects and hardening](docs/tasks/backlog.md)

Done: M0 PoC, M0.1 config + live IPD + calibration (2026-10-09). M1 mirror mode
(one process, hidden second GPU), Steam/Flatpak/32-bit/OpenGL (Zink) support,
M3 OpenXR output tested on Monado's simulated HMD (2026-10-18). Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); options and decisions in [docs/PLAN.md](docs/PLAN.md).
