# Testing

How vrdual is tested, and why the game tests use an own runner.

## What exists

| test | runs | what it covers |
|---|---|---|
| `./vrdual ci` | seconds, no GPU, no window; GitHub CI | config parser, profiles, games.json + site translations, SPIR-V patcher (incl. fuzz), frame reader, input mapper, game fixes (byte checks) |
| `./vrdual selftest` | ~1 min, both GPUs, small windows | everything in `ci` plus the layer in real Vulkan / GL (Zink) apps: per-eye shift, mirror mode, auto_noshift, depth export, HUD, input routes, OpenXR output on Monado's simulated HMD |
| `./vrdual selftest --background` | the same inside a headless gamescope | no window on the desktop; the two checks that need the desktop (X focus, cursor via Wayland) are skipped |
| game recipes `autotest/recipes/*.toml` | by hand today; runner planned | a real game: launch, settings, scene, stereo checks ([task](tasks/game-autotest.md)) |
| in the headset | by the owner | what no automatic check sees: comfort, depth that looks right, HUD at a sane depth |

Rules (AGENTS.md "Golden rule"): selftest passes before and after every
change; new behaviour gets a new check, or a stated reason why it cannot be
checked. A check is only trusted after it was seen failing without the change.

## Why an own runner for game tests, not SikuliX

SikuliX was the obvious candidate: it finds images on the screen and drives
mouse and keyboard, which is what getting through a game's menus needs. It
does not fit here, for these reasons, in order of weight:

1. **A SikuliX script is a program.** Scripts are Jython (or Ruby / Java)
   with the full language and the user's rights. Game recipes will come from
   contributors; a recipe that is code can do anything on the machine that
   runs it (read keys, install a service, push with the owner's credentials).
   Reviewing every recipe as security-relevant code does not scale. Our
   recipes are TOML data in a closed vocabulary that a validator checks
   before anything happens: no command execution, no environment, no paths
   outside named roots, no network.
2. **It drives the visible desktop.** SikuliX captures the screen and sends
   input to whatever window has the focus. A test run takes over the
   owner's screen, and a click or key meant for the game can land in a
   terminal or a browser (a second attack path, and a plain accident risk).
   Our runner sends input only through the layer's input ring into the
   game's own X server inside a headless gamescope; there is nothing else
   to type into.
3. **It sees the wrong picture.** It sees the desktop's composited image,
   i.e. one eye at best, scaled. The checks that matter for vrdual compare
   the two eyes (parallax, shadows agreeing, a doubled crosshair). The layer
   already exports both eyes' frames exactly as rendered (`/dev/shm`), and
   the selftest tools measure them (`tools/measure_posts.py`,
   `tools/frame_hash.py`).
4. **Weight and dependencies.** A JRE plus OpenCV through Java for what is
   image template matching, keys and clicks. We already have Python, Pillow,
   gamescope and the input path (selftest "input_route auto inside
   gamescope" exercises it).

What we give up: SikuliX's IDE and its mature image matching. Template
matching with Pillow is slower but enough for menus at a few checks per
second; if not, OpenCV can be added to the runner later without changing
the recipe format.

Other tools considered: `xdotool` / `ydotool` (input to the desktop or to
the kernel: same problem as point 2), Selenium-style UI automation (no
access to a game's own UI), game-specific console commands (useful, but per
engine; a recipe may use them via the game's own config files, not via
command execution).
