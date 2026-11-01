# Guided IPD calibration (small, independent)

**Status 2026-11-02:** done in the compositor (key G, ARCHITECTURE
"Calibration UI"); selftest: simulated viewers 50–80 mm (`vrdual-calib-test`)
and a scripted run through `--keys`. Saves to `[global]`, not the app
profile: profiles never hold IPD keys (`ctl_save`). The scene still shows all
posts; the prompt names the one to judge. Left: the same flow in `vrdual-xr`
(needs controller input for the answers and the HMD overlay, M3).

**Goal:** replace "press arrows until it feels right" with a short guided flow
in the compositor overlay: show one post at a time (20 m, then 0.5 m), ask
nearer / farther / right, bisect the IPD, and save to the app profile.

**Context:** `compositor/main.cpp` (`handle_key`, `draw_overlay`), calib scene
in `testapp/main.cpp`. Meaningful only with HMD output (M3), but the UI flow
can be built and tested now via `vrdual-ctl` + snapshots.

**Done when:** the flow converges (scripted answers in a selftest check drive it
to a target IPD within 0.5 mm) and writes the profile.
