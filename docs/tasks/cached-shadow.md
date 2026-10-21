# Cached memory: shadow + GPU write-back (fast and correct)

**Problem.** In mirror mode every push compares all mapped *cached*
(driver-mapped, `HOST_CACHED`) memory with what A and B hold: the app reads it
back, so it gets A's real mapping, and userfaultfd cannot track driver
mappings (VM_PFNMAP). DXVK d3d9 keeps hundreds of MB there: CoJ Gunslinger 288
MB → ~24 ms/submit, Alice: Madness Returns 192 MB → ~16 ms/submit.

`cached_wp = 1` (mprotect + SIGSEGV) is fast but wrong for some games: a
kernel write into a protected page (`read(2)` straight into the buffer, UE3
texture streaming) fails with EFAULT and the data never arrive; Alice shows
black flickering textures in both eyes. Hence opt-in per game today.
Importing tracked host pages into A (`VK_EXT_external_memory_host`) was tried
and fails on amdgpu userptr (ARCHITECTURE "Host memory").

**Goal.** Cached memory as fast as `cached_wp = 1`, without its bugs, so it
can be the default.

## Design

1. **Shadow cached memory too.** The app gets an anonymous shadow registered
   with userfaultfd (async write-protect), like non-cached memory today.
   Kernel writes work (async mode resolves them), CPU writes show up in
   `PAGEMAP_SCAN`, pushes copy only those pages to A and B.
2. **GPU A writes back into the shadow.** The app must still read what A's GPU
   wrote (readback). The mirror records every command already; for the
   commands that write host memory, remember (memory, offset, size) per
   command buffer:
   `vkCmdCopyImageToBuffer(2)`, `vkCmdCopyBuffer(2)`, `vkCmdFillBuffer`,
   `vkCmdUpdateBuffer`, `vkCmdCopyQueryPoolResults`, resolves/blits into
   linear images bound to such memory (rare).
3. **When the app can see the result** (fence wait/status, timeline
   semaphore wait/value, `vkQueueWaitIdle`, `vkDeviceWaitIdle`; the mirror
   hooks these already for B), copy those ranges A → shadow and also into
   `last`, so the next push does not send A's GPU data to B as a CPU change
   (B's GPU wrote its own).
4. **Unknown writers.** Shader storage writes (storage buffers / texel
   buffers / storage images) into host-visible cached memory: detect at
   descriptor update or bind; such an allocation falls back to the full diff
   (logged once). D3D9 under DXVK reads back with copies.

## Checks (selftest)

- testapp: `read(2)` from a file straight into mapped cached memory, then
  draw from it: both eyes correct (the case that breaks `cached_wp = 1`).
- testapp: `vkCmdCopyImageToBuffer` into cached memory, fence wait, the app
  reads the expected pixels from the mapping.
- the existing late-write check, with cached memory.
- Games: Alice (no flicker, speed like `cached_wp = 1`), Gunslinger.

**Done when** the full diff of cached memory is gone for DXVK d3d9 games,
Alice shows no artefacts, and `cached_wp` can be removed or made default.
