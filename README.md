# vrdual

VR for ordinary 3D apps on two GPUs: each eye is rendered on its own graphics card.
The apps don't need to know about it. Proof of concept, Linux, Vulkan, AMD.

```bash
./_                  # menu
./vrdual demo        # test scene in stereo (window, side by side)
./vrdual calib       # IPD setup: arrows = IPD, W = save
./vrdual selftest    # does everything work? (exit 0 = yes)
```

Settings: `~/.config/vrdual/vrdual.conf` (`./vrdual config init`).

Note: in most PCs the second graphics card sits behind the chipset and has a
slower link (here: PCIe Gen3 x4 instead of Gen4 x16). Check with `./vrdual gpus`.

Steam games (also OpenGL ones, also Flatpak Steam), one process, two GPUs:

```bash
./vrdual steam-install   # prints the launch option: .../vrdual/run %command%
./vrdual view            # side by side window while the game runs
./vrdual wivrn           # streaming server for the Quest (WiVRn app on the headset)
./vrdual xr              # game into the headset
```

More: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) · plan: [TODO.md](TODO.md) · for AI agents: [AGENTS.md](AGENTS.md)
