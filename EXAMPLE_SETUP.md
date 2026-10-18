```
git clone https://github.com/adderek/vrdual.git /home/adderek/src/vrdual
cd /home/adderek/src/vrdual
git submodule update --init  # vulkan headers
# you might need to instal tools:
# Packages: g++, make, the Vulkan loader (libvulkan), sdl3 (needed because plain make builds everything), glslc (shaderc), python3, and linux-api-headers (for /usr/include/linux/input-event-codes.h)
make
make lib32  # for 32-bit games
# Install wivrn, for example:
# `flatpak install flathub io.github.wivrn.wivrn`

./vrdual steam-install  # need to run this after every code change/rebuild
./vrdual wivrn  # Run the WiVrn server
steam &
```

Now select a game, right click, "properties"

General / Launch Options / "Advanced users may choose to enter modifications to theis launch options.".

Use the line `./vrdual steam-install` printed. For Flatpak Steam:
```
/home/adderek/vrdual/run %command%
```
or for native Steam:
```
/home/adderek/.local/share/vrdual/run %command%
```

Run the game. If it fails - it needs some extra work.

```
/home/adderek/src/vrdual/vrdual xr
```

Now run WiVrn on your VR gogles, connect to computer and play
