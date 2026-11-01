# Install

Linux, a VR headset with [WiVRn](https://github.com/WiVRn/WiVRn) (Quest 3 / 3S
tested), games from Steam or Heroic.

## 1. Once: packages

Arch Linux (tested):

```bash
sudo pacman -S --needed git base-devel python shaderc sdl3 vulkan-icd-loader \
    libx11 libxcb libglvnd lib32-glibc lib32-gcc-libs flatpak
flatpak install flathub io.github.wivrn.wivrn
```

Other distributions (untested): the same pieces under their names: git, make,
g++ with 32-bit support, python3, glslc (shaderc), SDL3, the Vulkan loader,
X11 / xcb / GL headers, flatpak + WiVRn.

On the headset: the WiVRn app ([WiVRn's README](https://github.com/WiVRn/WiVRn)
says where to get it).

## 2. Install

```bash
git clone --recursive https://github.com/adderek/vrdual
cd vrdual
./vrdual install
./vrdual xr
```

`install` builds everything (the first time takes a few minutes) and prints
the launch option, a line ending in `/vrdual/run %command%`. `xr` starts the
WiVRn server and sends the game to the headset; leave it running across games.

## 3. Each game

- **Steam**: game → Properties → Launch options: paste the line `install` printed.
- **Heroic**: game settings → Advanced → Wrapper command: the same path without
  ` %command%`; Other → Gamescope 1920x1080.

Start the game, put the headset on, connect in the WiVRn app.

With one graphics card: turn off temporal AA / upscaling (TAA, DLSS, FSR,
XeSS), motion blur and frame generation in the game; they mix the eyes.

## Update

```bash
git pull && ./vrdual xr
```

`xr` rebuilds and refreshes the copy games load at every start.

## More

Which games work: [game list](https://adderek.github.io/vrdual/games.html).
Per-game settings: [docs/GAMES.md](docs/GAMES.md). All settings:
[docs/CONFIG.md](docs/CONFIG.md). Something wrong: `./vrdual selftest`, then
an issue (`./vrdual report` prefills one).
