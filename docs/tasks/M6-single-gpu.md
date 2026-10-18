# M6 — single-GPU mode

**Goal:** both eyes on one GPU, for users with one card (and as a fallback).

**Status 2026-10-18:** alternate-eye rendering in mirror mode
(`VRDUAL_EYE=both`) when no other GPU matches `right_gpu`: even frames left,
odd frames right, on the app's GPU (ARCHITECTURE "Single GPU"). Calib
snapshots pass `measure_posts.py`, also with app device addresses (selftest
"single GPU", "single GPU --bda"); tested on an RTX 5070 Ti (NVIDIA 5xx).
The first attempt, a twin device on the same GPU, failed on NVIDIA for any
app using device addresses (DXVK, Zink). Known cost: half the frame rate per
eye and one frame between the eyes (ghosting in fast motion, TAA may smear).
Options below would give both eyes from the same frame.

**Options (PLAN §B4, §A3):** (a) replay each render pass twice with different
eye params (needs the M1 interception machinery, one device); (b) depth-based
reprojection of one rendered view (cheap, has disocclusion holes).

**Done when:** `vrdual run --single-gpu -- APP` produces calib snapshots that
pass `measure_posts.py`.
