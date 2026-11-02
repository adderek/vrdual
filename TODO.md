# TODO

Ordered by what to do next. Each item links to a self-contained task file.
Before you start: `./vrdual selftest` must pass (see [AGENTS.md](AGENTS.md)).

1. Quest 3S works (2026-10-18). Input + head motion implemented 2026-10-18 ([input](docs/tasks/input-emulation.md), [M4](docs/tasks/M4-head-pose.md) steps 1–3, rotation via clip transform `head_look = clip` 2026-11-02): test in the Quest after a reboot (uinput module), then auto aim/motion switch, the rest of [M3](docs/tasks/M3-openxr-output.md) (overlay in the HMD)
2. [M1](docs/tasks/M1-single-process-mirroring.md) follow-ups: dirty tracking for shadows done 2026-10-21 (userfaultfd + PAGEMAP_SCAN; measure in a game: CoJ Gunslinger had 26 ms/submit over 336 MB); cached mappings: shadowed by default since 2026-10-21 (item 3), the older `cached_wp = 1` (mprotect + SIGSEGV, EFAULT on kernel writes) is superseded; async B waits, Alice Madness Returns: CTD at the intro video fixed (32-bit address space + stop handling), recheck in game, Wayland-native GL via Zink
3. [Cached memory: shadow + GPU write-back](docs/tasks/cached-shadow.md): implemented 2026-10-21 (default; selftest: pread into the mapping, GPU fill read back); left: measure and check in games (Alice: no flicker and fast? Gunslinger), then drop `cached_wp`
4. [M2 — faster frame transport + measurements](docs/tasks/M2-frame-transport.md): async double-buffered readback done 2026-10-21 (1080p mirror 6.6 → 4.0 ms/frame); left: dma-buf (no CPU copy) if needed
5. [M4 — head pose](docs/tasks/M4-head-pose.md): needs M3; left: latency hiding for `head_look = mouse`, Quest check
6. [M5 — real applications](docs/tasks/M5-real-apps.md): after M1 for games, demos earlier
7. [M6 — single-GPU mode](docs/tasks/M6-single-gpu.md): alternate-eye rendering (2026-10-18); same-frame eyes optional
8. [Guided IPD calibration](docs/tasks/guided-calibration.md): done 2026-11-02 in the compositor (G); left: the same flow in the HMD (`vrdual-xr`)
9. [Borrowed from vorpX/FlatVR](docs/tasks/borrowed-features.md): HUD depth, cinema/curved screen, screen grab (done), HUD scale (needs HUD vs post-process distinction), edge peek; depth reprojection (Z3D) = M7 on branch `single-gpu`
10. [Backlog: small defects and hardening](docs/tasks/backlog.md)
11. Game profiles as data files: done 2026-11-02 (`profiles/*.conf`, one per
    game, matched by exe name or Steam app id; `vrdual profiles list|sync`,
    `vrdual install` copies them; `kAppProfiles` removed). Left: an online
    sync (e.g. the profiles published with the Pages site) so users without a
    checkout update, other store ids (GOG, Epic, umu `GAMEID`) as aliases,
    button maps per profile; `shader_rules` in `[settings]` already work, but
    their keys hold for one translator version only (shader-rules step 4).
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
16. Skip intro videos (owner's idea 2026-10-23): done 2026-11-02, `vrdual
    intro off|on|status GAME` renames the profile's `[game] intro` globs to
    `.orig` and back; first profile: Overlord (`profiles/overlord.conf`). Left:
    intro lists for more games (each needs the install checked by hand).
17. [Shader rules](docs/tasks/shader-rules.md) (from witcher3-vr,
    2026-10-31): per-shader `noshift | hud | hide` by SPIR-V hash in
    `shader_rules`, live bisection `_ hunt` to find a shader. Step 1 done;
    next: try it in a DXVK game (key stability), then HUD scale.
18. Public docs on GitHub Pages (owner's decision 2026-11-01): public
    material only; unpublished notes stay local until written up. Plan:
    `docs/games.json` as the source of the GAMES.md table (generator +
    `vrdual ci` check) and of a static viewer (filter / sort); per game
    `writeup_pending` (tuned configuration not written up yet).
    Owner enables Pages (Settings → Pages → GitHub Actions). Step 1 done
    2026-11-01: `site/games.json` + generator + ci check, viewer in `site/`,
    workflow `pages.yml`. Next: write up the pending configurations, `_ site`.
19. Per-game performance records (owner's request 2026-11-01): FPS per eye
    for a game in a given configuration, plus the list of mods if any are
    enabled. Collected by an agent with the owner, not by hand:
    - Agent: find the game's own config (often a guess: the Proton prefix
      `steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser/` →
      `Documents/My Games`, `AppData/Local|Roaming`, `Saved/Config/` for
      UE, `*.ini` / `*.cfg` / `*.xml` next to the exe; native: `~/.config`,
      `~/.local/share`), read resolution, quality preset and the settings
      that cost GPU time (AA, shadows, upscaler / frame generation, vsync,
      fps cap); list mods (mod folders, mod managers, replaced files); note
      the vrdual side: mode (mirror / per-eye / single GPU), GPUs, the
      game's `[app:]` profile, Proton / DXVK / vkd3d version.
    - Owner: plays a representative scene and says which (level, area,
      in-game options changed). FPS depends on settings, level and the
      rendered scene, so one number without that context means little.
    - FPS from the logs, not by eye: `vrdual-xr`'s stats line (game fps,
      headset Hz, frame age, L-R skew), the layer's `export: ... ms/frame`
      line, the mirror's memory sync line; average over the scene, plus the
      same scene without vrdual (or `stereo = 0`) as the baseline.
    - Where: a `perf` list per game in `site/games.json` (date, scene,
      settings, mods, mode, fps per eye, baseline), rendered on the game's
      page; private / unpublished configurations stay local (item 18).
      First step done 2026-11-02: `vrdual perf APPID|EXE` (Flatpak Steam /
      Proton games) prints what the machine knows as key/value lines: Proton,
      DXVK / vkd3d, layer version, mode, GPUs, resolution, non-default config,
      export / memory sync lines, game fps of the game's last `vrdual xr`
      session, the game's config files (prefix = the player's settings,
      install dir) and mod hints. Left: native games, writing the entry into
      `site/games.json` and rendering it, the first real records.
20. Shader hunt in the headset (owner's request 2026-11-02): done
    2026-11-02 as control mode `hunt` (docs/CONFIG.md `input`); left: try it
    in the Quest. Original request: `_ hunt find`
    driven by the controllers in `vrdual-xr`, no terminal. Each step asks
    in the overlay "did exactly the artefact we hunt disappear?" (note:
    other things may appear or vanish too, judge only that one) with yes /
    no / can't tell on buttons. One button toggles the current step's
    shader group on / off, so the user can flip it quickly and confirm
    before answering. At the end other buttons decide: save the rule
    (`hunt rule KEY MODE`) or discard. Same bisection as `_ hunt` (one
    implementation, two front ends); needs the HMD overlay (M3) and an
    input mode for it (like `tune`).
21. Controller modes the user never uses can be switched off (owner's
    request 2026-11-02): done 2026-11-02, `input_modes` (docs/CONFIG.md);
    left: try it in the Quest.
22. Research (owner's question 2026-11-02): frame generation and extra
    effects with our approach. Findings and next steps:
    [framegen-effects](docs/tasks/framegen-effects.md). In short:
    - two GPUs: FSR 3 FG should work per eye in the mirror (to test);
    - one GPU: the game's FG must stay off; best route is depth-based
      synthesis of the missing eye in `vrdual-xr`, which needs depth export
      first;
    - own shadows: not possible app-unaware; SSAO from depth is.
    - depth export done 2026-11-02 (`depth_export = 1`, not in mirror mode
      yet); next: DIBR of the missing eye in `vrdual-xr`.
    - off-screen passes (shadow maps, baked textures): `auto_noshift = 1`
      done 2026-11-02 (backlog "Shadow-map passes"); to confirm in Civ V.

Done: M0 PoC, M0.1 config + live IPD + calibration (2026-10-09). M1 mirror mode
(one process, hidden second GPU), Steam/Flatpak/32-bit/OpenGL (Zink) support,
M3 OpenXR output tested on Monado's simulated HMD (2026-10-18). Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); options and decisions in [docs/PLAN.md](docs/PLAN.md).
