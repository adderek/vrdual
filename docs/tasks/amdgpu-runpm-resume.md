# amdgpu: runtime-PM resume of an idle GPU fails (PSP resume failed)

Status: observed once (2026-11-09), not worked on. Owner's note: a known AMD
problem; AMD runs a bug bounty for it on the RX 9070 (not checked here). Worth
working on later: vrdual keeps a headless GPU idle most of the time, so it
wakes it often.

Raw kernel log, addresses and the full wake-up timeline: the local file
`docs/tasks/amdgpu-runpm-resume.local.md` (maintainer's machine only).

## What happened

Reference machine "fractal" (AGENTS.md), two RX 7900 XTX (Navi 31, same
board and VBIOS), Linux 7.2.9-zen1, linux-firmware 20260916, Mesa 26.2.4,
`amdgpu.runpm = -1` (auto), `power/control = auto`, `d3cold_allowed = 1`,
autosuspend 5000 ms, cmdline `amdgpu.ppfeaturemask=0xfff7ffff`.

The headless GPU (no monitor, idle between tests) suspends a few seconds after
its last user and resumes on the next Vulkan device open. One resume during a
selftest failed:

```
[drm] PCIE GART of 512M enabled
PSP is resuming...
(2.2 s later)
psp gfx command ID_LOAD_TOC(0x20) failed and response status is (0x0)
Failed to load toc
PSP tmr init failed!
PSP resume failed
resume of IP block <psp> failed -22
amdgpu_device_ip_resume failed (-22).
```

A good resume of the same card takes ~0.4 s: PSP resuming, `reserve ... for
PSP TMR`, RAP / SECUREDISPLAY ucode notes, SMU resumed, DMUB initialized.

Afterwards: `power/runtime_status = error`, Vulkan (RADV) lists only the
display GPU, `gpu = auto` puts both eyes there, the two-GPU and mirror
selftest checks fail. Only a reboot recovers it.

## Topology correlation (kernel logs of the last 12 boots)

| boots | headless GPU's slot | resumes | failed |
|---|---|---|---|
| 2026-10-20 .. 2026-11-06 | CPU-direct x16, Gen4 | ~1040 | 0 |
| 2026-11-07 22:31 .. 2026-11-09 (bifurcated x8x8, both GPUs behind one root port pair, link Gen3 x8) | bifurcated x8, Gen3 | 45 | 1 (the 39th of the boot) |

Correlation only: one failure, different slot and link speed (the root ports
max out at 8 GT/s x8), possibly a riser. Not reproduced on purpose.

## Workaround

Keep the card awake (root, persistent):

```
# /etc/udev/rules.d/80-vrdual-gpu-awake.rules
ACTION=="add", SUBSYSTEM=="pci", KERNEL=="<headless GPU's PCI address>", ATTR{power/control}="on"
```

or `amdgpu.runpm=0` for all GPUs. Cost: idle power of the headless card (tens
of W on a 7900 XTX; measure before deciding).

## Ideas for later

- Reproduce: a loop of Vulkan device open / close with > autosuspend delay in
  between, counting wake-ups until one fails, on the bifurcated slot vs the
  x16 slot (carefully: AGENTS.local.md forbids tight GPU loops; this needs
  seconds between wake-ups, and a planned reboot).
- Check whether the failure needs D3cold (`d3cold_allowed = 0` keeps D3hot)
  or the Gen3 link / riser (BIOS: force Gen4 on the bifurcated slot if the
  riser allows it).
- Upstream: search the amdgpu issue tracker for `ID_LOAD_TOC` resume
  failures on Navi 31; report with the logs from the local file.
- vrdual side: `vrdual gpus` / selftest could detect `runtime_status = error`
  and say "reboot needed" instead of failing two-GPU checks one by one.
