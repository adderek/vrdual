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

It also works on a single GPU (any vendor, also NVIDIA): the eyes are then
rendered on alternate frames, half the frame rate per eye. The game window on
the monitor shows left and right frames alternately, so it flickers; the
headset is fine.

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

# Tested

## Hardware

* 2x AMD rx 7900 xtx
* 1x NVIDIA RTX 5070 Ti (single GPU)

## Games (tested with dual GPU)

* American McGee Alice Madness Returns
* LEGO Harry Potter 1-4
  * Turn off video effects (I have not checked which but with enabled there is a "ghost" - likely DoF could be causing that)
* Portal 2
* Zen Bound 2
* Tomb Raider III: works (set a windowed / 1920x1080 mode, the portrait desktop otherwise gives 2880x5120)
* Fallout: New Vegas: runs, HUD shown
* Overlord (native, OpenGL via Zink): runs; green glow at the bottom of the screen
  is a Zink (Mesa 26) bug, it also shows with Zink alone, without vrdual
* Tomb Raider II: runs; menu needs keys (`input = mouse`: a = Enter, b = Esc)
* Tomb Raider I (DOSBox): no stereo (draws a flat 2D image); exits in windowed
  mode / on alt+tab (not checked without vrdual)
* Portal (native, OpenGL via Zink): menu works, crashes on level load (NULL GL
  buffer map in `shaderapidx9.so`); also with Zink alone, without vrdual
* Skyrim, Mirror's Edge (Proton): run; depth needs `world_scale` (~70, ~50-100)
* Firewatch: runs; the portrait desktop cuts its menu (needs a landscape mode)
* Psychonauts (native GL): runs, both eyes identical (not analysed yet)
* Shadow Warrior, Hospital Tycoon: crashed with SIGBUS in the frame export of
  2026-10-19 20:35 (reverted in 55c0b81), retest

## Games (tested with single GPU)

* American McGee Alice Madness Returns
* Overlord (green artifacts at the bottom of the screen: Zink bug, also without vrdual)
* Zen Bound 2
