# M6 — single-GPU mode

**Goal:** both eyes on one GPU, for users with one card (and as a fallback).

**Options (PLAN §B4, §A3):** (a) replay each render pass twice with different
eye params (needs the M1 interception machinery, one device); (b) depth-based
reprojection of one rendered view (cheap, has disocclusion holes).

**Done when:** `vrdual run --single-gpu -- APP` produces calib snapshots that
pass `measure_posts.py`.
