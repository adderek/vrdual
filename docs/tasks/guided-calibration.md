# Guided IPD calibration (small, independent)

**Goal:** replace "press arrows until it feels right" with a short guided flow
in the compositor overlay: show one post at a time (20 m, then 0.5 m), ask
nearer / farther / right, bisect the IPD, and save to the app profile.

**Context:** `compositor/main.cpp` (`handle_key`, `draw_overlay`), calib scene
in `testapp/main.cpp`. Meaningful only with HMD output (M3), but the UI flow
can be built and tested now via `vrdual-ctl` + snapshots.

**Done when:** the flow converges (scripted answers in a selftest check drive it
to a target IPD within 0.5 mm) and writes the profile.
