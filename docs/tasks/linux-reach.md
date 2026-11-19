# Linux reach

Goal: more Linux users run VR with vrdual.
Date: 2026-11-19. Details: [business-value.md](business-value.md).
Owner decisions: marked **OWNER**.

## Do, in this order

| # | Step | Why |
|---|---|---|
| 1 | Packages: AUR, Flathub, `.deb`/`.rpm` | Install today = git + SDL3 + glslc + multilib |
| 2 | One start page: "1 GPU, WiVRn, Steam game → do this" | Users meet architecture before results |
| 3 | `vrdual doctor` | Most reports are environment problems |
| 4 | Measure on one GPU, publish numbers | Most VR users have one GPU; vrdual needs two |
| 5 | Post where Linux VR users are (WiVRn, Monado, r/linux_gaming) | One game + "works/runs/broken" table |
| 6 | Public compatibility matrix | Reproducible proof, not marketing |
| 7 | `vrdual profiles sync` | New profiles without a checkout |
| 8 | Nightly run of game recipes | Keeps "works" status true |

## Do not

- Telemetry, UI inside the game, donor-only features.

## OWNER

- Which distributions for step 1?
- Measure one-GPU first (step 4), or keep two-GPU as the target?

Status: proposal. Nothing built or run.
