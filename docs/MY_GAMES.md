# My games

The maintainer's library (every game vrdual can be tested on) lives in
[site/library.json](../site/library.json) and is shown, with column choice,
filters and sorting, on the site:
<https://adderek.github.io/vrdual/library.html>.

One row per game: `stores`, `vr` (dedicated VR routes for that game, by hand
and from catalogs of VR mods), `platform` (where that route runs),
`generic` (UEVR / UUVR / vorpX / Geo-11, derived from `engine` and `api`),
`engine`, `api`, `pcgw_vr` (native VR or stereo 3D listed on
PCGamingWiki), `pcgw` (the page), `tested` (the game's entry in
[site/games.json](../site/games.json), whose status the page shows).
`tools/library.py`: `pcgw` refreshes the PCGamingWiki fields, `link` fills
`tested`, `--check` validates (part of `vrdual ci`).
