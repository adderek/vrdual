# Automated game tests (recipes + runner)

Owner's request (2026-11-06): for every tested game, keep instructions to run
it automatically: start it, get through the menus, change the graphics
settings (in the game's menu or its `.ini` files), reach a scene and check the
stereo, so a vrdual change can be regression-tested on real games. Recipes
must be safe to accept from contributors: a recipe from a pull request must not
be able to take over the machine that runs it.

Status: design. The first recipes are written as data now
(`autotest/recipes/*.toml`: Civ V, Alan Wake's American Nightmare, Mafia,
Islands of Insight) and serve as human instructions until the runner
exists.

## Decision: own small runner, not SikuliX

Why: [docs/TESTING.md](../TESTING.md) "Why an own runner". In short, SikuliX
scripts are code (a hostile recipe runs anything) and drive the visible
desktop; we already have both eyes' frames (`/dev/shm` export), input into
the game's own X server (the layer's input ring, `input_route = xtest`) and
isolation (headless gamescope, as `selftest --background`).

So: `autotest/` in this repo (one checkout, selftest can exercise the runner
with `vrdual-gltest`); split into a submodule / own repo once it is useful
outside vrdual. Python stdlib + Pillow (already a selftest dependency),
recipes in TOML (`tomllib`, stdlib).

## Recipe = data, closed vocabulary

A recipe is a TOML file the runner validates completely before doing
anything; an unknown key, step or argument rejects the whole file. No step
runs a program, sets an environment variable, names an executable or opens a
network connection.

```toml
[game]
slug = "civ5"
steam = 8930                # launched only as steam://rungameid/<id>
profile = "civ5"            # vrdual profile expected in the layer log

[[config]]                  # .ini edits, applied before launch, restored after
root = "aspyr:Sid Meier's Civilization 5"   # named root, see below
file = "GraphicsSettingsDX9.ini"
set = { WindowResX = "1920", WindowResY = "1080", ShadowLevel = "3" }

[[step]]
wait_image = "civ5/main-menu.png"   # template under autotest/templates/
timeout = 120
[[step]]
click = "civ5/main-menu-load.png"   # click the centre of the template's match
[[step]]
key = "Return"                      # X keysym names only
[[step]]
wait_frames = 300
[[step]]
check = "eyes_differ"               # built-in checks by name
[[step]]
check = "region_agrees"             # e.g. a unit shadow: same in both eyes
region = [0.40, 0.55, 0.20, 0.15]   # x, y, w, h as fractions of the frame
[[step]]
snapshot = "civ5-1080p-high"        # both eyes, into the run's output dir
```

Steps: `wait_image`, `wait_frames`, `click` (template or `[x, y]` fractions),
`key`, `type` (printable ASCII, length-limited), `scroll`, `check`,
`snapshot`, `quit` (the game's own exit; the runner kills only the PID it
started, never by pattern).

## Security model

Threat: a contributor's recipe (or template, or save game) that makes the
maintainer's machine do something else than drive the game.

- **No code in recipes.** Closed vocabulary above; the parser is the only
  thing that reads them. Templates are PNGs decoded by Pillow with a size cap.
- **Input cannot leave the game.** Keys and clicks go through the layer's
  input ring into the nested (headless gamescope) X server only. The runner
  never uses `xdotool`, uinput keyboards or the desktop's X server.
- **File edits only under named roots.** `[[config]] root` is a name from a
  table in the runner (game install dir, its Wine prefix's user dir, Aspyr /
  Feral config dirs inside Flatpak Steam), resolved by the runner; `file` is
  relative, no `..`, no absolute path, `realpath` must stay inside the root
  (no symlink escape), only `.ini` / `.cfg` / `.xml` / `.json` text files,
  key=value edits only (no raw file content). Every edited file is backed up
  and restored after the run.
- **No save games from pull requests.** Save files are parsed by old,
  unhardened game code: a crafted save is an exploit vector. Saves come only
  from the maintainer's own runs and stay out of the repo (local dir).
- **Never automatic.** No CI runs recipes (GitHub CI has no GPUs and no
  self-hosted runner, keep it that way). The runner shows the parsed plan
  (`--dry-run`, default for a recipe changed since the last approved run) and
  records the approved recipe's hash; changed recipes need a new approval.
- **Review.** `autotest/` under CODEOWNERS (maintainer); a pull request
  touching the runner itself is reviewed as code, recipes as data.

## Steps

1. Recipe format + validator (`autotest/recipe.py`, `vrdual autotest check
   FILE`), unit tests with hostile recipes (path escape, symlink, unknown
   keys, oversize template) in `vrdual ci`.
2. Runner on `vrdual-gltest` inside headless gamescope: launch, `wait_frames`,
   `key` / `click` through the ring, `snapshot`, `eyes_differ`; selftest check.
3. Template matching (`wait_image`, `click` on a template) with Pillow;
   capture templates from a run (`vrdual autotest grab NAME REGION`).
4. Steam launch inside headless gamescope (Flatpak: `gamescope-pv-display`
   path as in docs/GAMES.md) and the `[[config]]` roots.
5. First real recipes: Civ V (ini-only settings, menu by template), then one
   Proton game. Regression set = the recipes marked `regression = true`.

## Per game, from now on

After testing a game, write or update `autotest/recipes/<slug>.toml`: how it
starts, which settings were used (ini keys or the menu path), the scene, what
to check. Until the runner exists the recipe is the written instruction;
unknown parts stay as `# TODO` comments.
