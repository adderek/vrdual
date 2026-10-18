# M6 — single-GPU mode

**Goal:** both eyes on one GPU, for users with one card (and as a fallback).

**Status 2026-10-18:** works through mirror mode: when no other GPU matches
`right_gpu` (or it names the app's GPU), the twin device is created on the
app's own GPU. Calib snapshot passes `measure_posts.py` (selftest "mirror
single GPU"). Cost: every frame rendered twice plus the memory sync, about
half the frame rate. Options below remain for a cheaper path.

**Options (PLAN §B4, §A3):** (a) replay each render pass twice with different
eye params (needs the M1 interception machinery, one device); (b) depth-based
reprojection of one rendered view (cheap, has disocclusion holes).

**Done when:** `vrdual run --single-gpu -- APP` produces calib snapshots that
pass `measure_posts.py`.
