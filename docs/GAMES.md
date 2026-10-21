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
| Alice: Madness Returns (19680) | Proton, D3D9 (UE3, DXVK, 32-bit) | works | `cached_wp`: pick fast or correct (see notes) | also single GPU. Two-GPU mirror: with `cached_wp = 1` fast (memory sync 16 → 0.2 ms/submit) but black flickering shadows/objects, mostly on the character, in both eyes (UE3 streams textures straight into GPU memory, those reads fail); with `cached_wp = 0` (default) no such artefacts (a few small flickers, maybe something else) but much slower. Worse with dynamic shadows on and with `head_look = mouse` than with `edge`. `head_look` mouse and edge both work (2026-10-21) |
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

## To test: showcase candidates (2026-10-21)

From [MY_GAMES.md](MY_GAMES.md) (owned): picked for looking good in stereo on a
fixed screen, a calm camera, Proton D3D9/D3D11 or native Vulkan, and no
anti-cheat. Third-person "diorama" games look like a model in front of you and
do not upset the inner ear; `world_scale` can shrink them further.

| game | why | risk | other VR route (as found 2026-10-21) |
|---|---|---|---|
| LEGO Marvel Super Heroes / LEGO Batman 3 / LEGO The Hobbit | LEGO Harry Potter already works; toy bricks on the desk; for everyone | post effects (DoF) off | none found (no VR mod, no vorpX profile confirmed) |
| Little Nightmares | dark miniature diorama, ideal scale and pace | UE4: 10-bit backbuffer, exported since 2026-10-21 (blit to 8-bit) | UEVR (generic, Windows); a separate VR game, Little Nightmares VR: Altered Echoes (2026) |
| Unravel / Unravel Two | photoreal small scale, calm | unknown engine path | Geo-11 (generic DX11 stereo, Windows) reported to work |
| Trine | fairy-tale 2.5D with many layers, strong depth | little camera movement | built-in stereoscopic 3D (side by side) in Trine 1–3 |
| Batman: Arkham Asylum / City | stereo classic of the 3D Vision era, third person, well known | UE3 D3D9 via DXVK: Alice's cached-memory trade-off until TODO 3 | none found (Arkham VR is a separate PS VR / PC VR game) |
| Tomb Raider (2013) / Rise of the Tomb Raider | spectacular vistas, third person | D3D11 / D3D12, heavier | Rise: only the Blood Ties DLC in VR; full game via generic stereo tools |
| Alien: Isolation (Epic) | the best-known flat-to-VR horror: slow, atmospheric, first person (`head_look = edge`) | D3D11 deferred renderer: screen-space effects may be off | MotherVR (Windows only): on Linux vrdual would be the only route |
| Cities: Skylines / Planet Zoo / Jurassic World Evolution / RollerCoaster Tycoon 3 | builders seen from above look like a model on a table; mouse play, the system cursor is drawn at the scene's depth (works in Hospital Tycoon) | Cities: Skylines has a native Linux (OpenGL → Zink) build | none found |
| LEGO Builder's Journey (Epic) | a pure diorama | Unity; ray tracing off | vorpX reported in the classic mode only |

First round (2026-10-22): LEGO Marvel Super Heroes or LEGO Batman 3, Alien:
Isolation, one builder (Cities: Skylines or Planet Zoo), Little Nightmares.

Second wave: Dead Space (2008), Mass Effect Legendary Edition, Unravel Two, BioShock Infinite, Dishonored, Shadow of Mordor, Metro 2033 Redux,
Stray, Ghost of a Tale, Spyro Reignited, Grow Home; first-person ones for
`head_look`. DOOM (2016) is native Vulkan (no DXVK, no cached-memory issue): a
good test of the Vulkan path, too fast for a showcase.

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
