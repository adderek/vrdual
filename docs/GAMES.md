# Games

Status per game, dual GPU (2x RX 7900 XTX, host "fractal") unless noted.
Launch option for all: `/home/adderek/vrdual/run %command%` (from
`vrdual steam-install`); `vrdual xr` runs once for all games. Last update
2026-10-21.

Status: **works** = stereo in the HMD confirmed; **runs** = renders, stereo
not confirmed or weak; **broken** = crash or no picture.

| game (Steam appid) | path | status | settings | notes |
|---|---|---|---|---|
| Zen Bound 2 (61600) | native GL → Zink | works | | first game tested in the Quest |
| Alice: Madness Returns (19680) | Proton, D3D9 | works | | also single GPU |
| Portal 2 (620) | native GL → Zink | works | `-windowed -w 1920 -h 1080` | fullscreen picks the portrait 2880x5120 |
| LEGO Harry Potter 1-4 (21130) | Proton | works | turn off video effects | a "ghost" with effects on (likely DoF) |
| Tomb Raider III (225320) | Proton | works | windowed / 1920x1080 | |
| Psychonauts (3830) | native GL → Zink | works | `skip_w1 = 0` (built-in) | draws the scene with `w == 1`; one SIGSEGV seen, not analysed |
| Mirror's Edge (17410) | Proton, D3D9 (UE3) | works | `world_scale = 50`, `hud_depth = 1` (built-in); launch in a 1920x1080 gamescope window (below) | confirmed 2026-10-20; stereo works but less impressive than expected |
| Hogwarts Legacy (990080) | Proton, D3D12 (UE4) | works | special configuration (sponsor profile) | impressive stereo; slow on Linux (vkd3d-proton + mirror memory sync ~12 ms/submit); screenshots show only the GUI, the game image is black; no native VR (a UEVR mod exists for Windows); confirmed 2026-10-21 |
| Call of Juarez: Gunslinger (204450) | Proton, DXVK d3d9 (32-bit) | works | special configuration (sponsor profile) | game leaves alpha 0 (black in the HMD before the layer forced alpha 1); weak depth at default settings; mirror memory sync 24 ms → 0.13 ms/submit with cached write tracking; confirmed 2026-10-21 |
| Firewatch (383870) | Unity | runs | landscape resolution | the portrait desktop cuts the menu |
| Skyrim (72850) | Proton, D3D9 | runs | `world_scale = 70` (built-in, not tuned) | weak depth at scale 1; long loading |
| Fallout: New Vegas (22380) | Proton, D3D9 | runs | `world_scale = 70` (built-in, not tuned) | HUD shown; `hud_depth` not confirmed yet |
| Tomb Raider II (225300) | Proton | runs | `input = mouse` (a = Enter, b = Esc) | the menu wants keys, pad buttons do nothing |
| Overlord (11450) | native GL → Zink | runs | | green glow at the bottom: Zink bug, also without vrdual |
| Tomb Raider I (224960) | DOSBox | broken | | flat 2D image, no stereo; exits in windowed mode / on alt+tab |
| Portal (400) | native GL → Zink | broken | | crash on level load (NULL GL buffer map), also with Zink alone |
| Shadow Warrior (233130) | native GL → Zink | retest | | SIGBUS in the 2026-10-19 frame export (reverted 55c0b81); window scaling issues |
| Hospital Tycoon (11590) | Proton | works | gamescope 1920x1080 (below) | crashes (NULL read in the game) on the portrait desktop; system cursor drawn by `vrdual xr` at the scene's depth (`cursor_depth = auto`), confirmed 2026-10-20 |

Single GPU (RTX 5070 Ti, host "sharkoon", alternate-eye): Alice: Madness
Returns, Zen Bound 2, Overlord (same Zink glow).

Gamescope keeps a game at 1920x1080 whatever the (portrait) desktop is, e.g.
Mirror's Edge launch option. `gamescope-pv-display` is a local helper
(`~/.var/app/com.valvesoftware.Steam/bin/`, the Flatpak's `~/bin`): Flatpak
Steam's container start resets `DISPLAY` to the desktop's, the helper puts
gamescope's `DISPLAY` back inside the container.

```
gamescope -w 1920 -h 1080 -W 1920 -H 1080 --force-grab-cursor -- /home/adderek/vrdual/run /home/adderek/bin/gamescope-pv-display %command%
```

## Tuning a new game

1. **Resolution**: the desktop here is portrait (2880x5120). Use windowed
   1920x1080 or the game's resolution setting, otherwise the HMD shows a tall
   picture and some games cut their menu.
2. **Depth** (`world_scale`, game units per meter): flat image = too small.
   Unreal 100 (some UE3 games 50), Gamebryo/Creation ~70, Source ~39. Tune
   live, then keep it for the game:
   ```bash
   ./vrdual set world_scale 50
   ./vrdual save            # into [app:<game>] of the config
   ```
3. **Both eyes identical**: the game may draw the scene with `w == 1` (CPU
   transformed vertices). Try `VRDUAL_SKIP_W1=0` in the launch option.
4. **Crosshair / HUD doubled**: HUD draws sit at infinity by default. Put them
   at a distance: `./vrdual set hud_depth 5`.
5. **Crash**: check whether Zink alone crashes too
   (`MESA_LOADER_DRIVER_OVERRIDE=zink %command%`, OpenGL games). Logs: Proton
   `~/.var/app/com.valvesoftware.Steam/steam-<appid>.log`, native
   `~/.var/app/com.valvesoftware.Steam/vrdual/game.log`, cores `coredumpctl`.

Found values for everyone go into `kAppProfiles` in
`common/vrdual_config.hpp` (and this table).
