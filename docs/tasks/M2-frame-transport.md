# M2 — faster frame transport between GPUs

**Status 2026-10-21:** steps 1–2 done. Readback is double-buffered (frame
N-1 published while N copies). testapp, mirror mode, time per frame (both eyes
exported) vs without export:

| per eye | no export | sync readback | double-buffered |
|---|---|---|---|
| 1920x1080 (8.3 MB) | 0.6 ms | 6.6 ms | 4.0 ms (0.8 ms of it CPU copies to /dev/shm) |
| 2160x2160 (18.7 MB) | 1.1 ms | | 7.8 ms (~128 fps max) |

What is left is GPU A's PCIe Gen3 x4 link (~2.4 ms per 1080p frame, ~5.3 ms
at 2160², in the background now): fine for 90 Hz at 1080p, tight at 2160²
for 120 Hz. Next: step 3 (dma-buf, no CPU copy) only if a game needs it.

**Goal:** cut the per-frame readback stall and CPU copies, then measure what
the chipset link (`06:00.0`, Gen3 x4 ≈ 3.9 GB/s) really costs per eye.

**Context:** `vrd_QueuePresentKHR` in `layer/vrdual_layer.cpp` currently
submits a copy and **waits on its fence** before presenting (synchronous),
then memcpys into `/dev/shm`. PLAN §D lists D2 (async) → D3 (dma-buf) → D4 (P2P).

**Steps**
1. Benchmark first: add timing (readback wait, memcpy, publish) to the layer
   log and a `--size 2160x2160` run of the test app. Record numbers in
   docs/ARCHITECTURE.md "Hardware note".
2. D2: ring of N readback buffers, publish frame F−1 while F is in flight.
3. D3: allocate the readback target as exportable dma-buf (GTT) and import it
   in the consumer (compositor on the other GPU) — no CPU copy.
4. D4 only if D3 is not enough: test whether peer BAR writes work through the
   B550 switch (it already fails AtomicOp routing, so expect trouble).

**Done when:** numbers for 1080p and 2160² per eye at 90/120 Hz are in
ARCHITECTURE.md, and selftest still passes.
