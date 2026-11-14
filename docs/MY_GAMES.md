# My games

The maintainer's library (every game vrdual can be tested on) lives in
[site/library.json](../site/library.json) and is the site's game list, with
column choice, filters and sorting (the vrdual status joined from
games.json): <https://adderek.github.io/vrdual/games.html>.

One row per game: `stores`, `vr` (dedicated VR routes for that game, by hand
and from catalogs of VR mods), `platform` (where that route runs),
`generic` (UEVR / UUVR / vorpX / Geo-11, derived from `engine` and `api`),
`engine`, `api`, `pcgw_vr` (native VR or stereo 3D listed on
PCGamingWiki), `pcgw` (the page), `tested` (the game's entry in
[site/games.json](../site/games.json), whose status the page shows).
`tools/library.py`: `pcgw` refreshes the PCGamingWiki fields, `link` fills
`tested`, `known` rewrites [site/known.json](../site/known.json) (titles of
all games on PCGamingWiki: a search for a game that is not owned says so
and links its PCGamingWiki page), `--check` validates (part of `vrdual ci`).
