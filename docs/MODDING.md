# Modding old games (textures, shaders, models)

Idea, not started: improve old games beyond stereo (better textures, shaders,
smoother models) with vrdual. Low-res textures are more visible in an HMD
than on a monitor. Research 2026-10-21; may be outdated, check before
relying on it.

## Prior art

- **Alice (American McGee's Alice, 2000)**: HD Fan Remaster (Omri Koresh,
  ~2 GB): textures upscaled 4x, some only 2x because a full 4x crashed the
  engine; models not touched. VorpalFix (Wemino): widescreen HUD/FMV, FOV,
  Linux/Steam Deck; needs the 2011 EA re-release. Full 1k Texture Pack
  (ModDB), also for the 2011 release.
- **RTX Remix** (NVIDIA, a DXVK fork, runtime open source): asset replacement
  + path tracing for DX8/9 fixed-function games. 100+ released mods
  (Half-Life 2 RTX, Morrowind RTX, Painkiller RTX). AMD/RADV support:
  nothing confirmed found.
- **cheeky-imp** (JnCrMx): Vulkan layer for Linux, DXVK or native Vulkan:
  dump and replace shaders, textures, meshes. Closest to what vrdual would
  do. WIP.
- **Emulators** (DuckStation, Dolphin, PCSX2): texture hash → dump →
  replacement file. Same pattern.
- **ReShade / vkBasalt**: post-process only.

## Candidates from MY_GAMES.md

Already well modded: test the existing mods with vrdual, don't redo them
(from memory, verify each):

| game | existing mods |
|---|---|
| Morrowind | OpenMW (native GL/Vulkan), MGE XE, Morrowind RTX; parallax / normal-mapped textures wanted (user, 2026-10-29: runs well in vrdual without mods) |
| Deus Ex GOTY | Kentie's D3D10/11 renderer, GMDX, Revision, HDTP |
| Thief Gold / Thief 2 | NewDark, TFix/T2Fix, HD texture packs |
| System Shock 2 | SS2Tool, SHTUP |
| Unreal Gold | OldUnreal 227/469 |
| Doom 1/2, Doom 3, Quake II | GZDoom; Perfected Doom 3 / Sikkmod; Q2 RTX, remaster |
| Daggerfall Unity | texture and model packs |
| Skyrim, Fallout 3 / NV / 4 | large modding scenes |
| Half-Life / HL2 | HD pack, Black Mesa (owned), HL2 RTX |
| Max Payne 1/2, Mafia (Classic) | widescreen fixes, texture packs |

Little coverage (own assessment, not checked): where vrdual-side tuning would
add most: Alice 2000 (first target: OpenGL id Tech 3 via Zink, the layer
already sees it), Legacy of Kain: Blood Omen 2 / Defiance, Giants: Citizen
Kabuto, MDK, Psychonauts, Star Wars Bounty Hunter, Oddworld: Munch's
Oddysee, Overlord, Gorky 17, Tomb Raider Legend / Anniversary / Underworld.

Alice 2000 is not in MY_GAMES.md (the Epic and Origin sections are not
filled in yet); the Steam copy of Madness Returns does not contain it.

## In vrdual

- **Textures**: hash at upload (`vkCmdCopyBufferToImage`), replace from
  disk. A larger replacement needs `vkCreateImage`/view interception and
  rescaled copy regions. Generic for DX9 (DXVK) and GL (Zink).
- **Shaders**: the SPIR-V patch hook exists; whole-module replacement by
  hash is a small step.
- **Models**: generic mesh replacement without engine knowledge is fragile.
  Cheaper: injected tessellation in the style of ATI TruForm (PN triangles):
  smoother silhouettes, no new detail.
- Per-game tuning profiles fit the sponsorware model (early access per game).

## Sources

- https://www.dsogaming.com/?p=143095 (Alice HD Fan Remaster)
- https://modstalgia.com/american-mcgees-alice/mods/vorpalfix
- https://www.moddb.com/mods/full-1k-texture-pack
- https://github.com/JnCrMx/cheeky-imp
- https://repo.retrodeck.net/Xargon/Duckstation/src/branch/main/src/core/texture_replacements.cpp
- https://www.gamingonlinux.com/2023/05/nvidia-open-sources-more-of-rtx-remix-with-v02/
- https://nvidia.com/HalfLife2RTX
- https://www.kitguru.net/gaming/matthew-wilson/nvidia-reveals-new-rtx-remix-projects-new-geforce-game-bundle-and-more-at-gamescom/
