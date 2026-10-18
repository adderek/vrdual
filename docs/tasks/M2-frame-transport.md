# M2 — faster frame transport between GPUs

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
