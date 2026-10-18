# Input — gamepad / VR controllers → what the game understands

**Goal:** play flat games with the headset on. Many games here are mouse-only
(Zen Bound 2) or windowed without pointer grab, so the desktop mouse is
unusable from inside the HMD.

**Context:** first Quest 3S sessions (2026-10-18). `vrdual-xr` already has an
OpenXR session with WiVRn's Touch Plus controllers. `/dev/uinput` is writable
by the user (ACL), so virtual devices need no sudo. The layer stays passive
(AGENTS invariant): input lives in a separate process, never in the game.

## Order of preference (per game, first that applies)

1. **Game supports a gamepad, a real pad is connected** → do nothing.
2. **Game supports a gamepad, only VR controllers** → virtual Xbox pad
   (uinput, Xbox 360/One VID:PID so SDL and Steam Input recognise it).
   Default map: sticks → sticks, triggers → LT/RT, grips → LB/RB,
   A/B/X/Y → A/B/X/Y, stick clicks → L3/R3, left menu → Start,
   long press left menu → Back/Select.
3. **No gamepad support** (mouse/keyboard game) → mouse + keyboard emulation
   from VR controllers or from a real pad. A real pad used this way is grabbed
   (`EVIOCGRAB`) so the game does not also see it.

## Mouse modes (switchable at runtime with a button, default per game)

| mode | how | good for | cost |
|---|---|---|---|
| `stick` | stick deflection → velocity, `v = sign(x)·|x|^γ`, γ≈2–3, dead zone | any pad, blind use; FPP turning | small UI targets are slow |
| `motion` | controller yaw/pitch deltas → relative mouse (gyro/air mouse); grip = clutch, release to re-centre the hand | FPP aiming, precise | needs a clutch habit |
| `aim` | controller ray ∩ virtual screen plane → absolute pointer (uinput tablet `ABS_X/Y`) | menus, RTS, point-and-click, windowed games | useless when the game grabs the pointer (relative mode) |

Trigger = left button, grip (when not clutch) = right button, stick click =
middle, the other stick = wheel. `aim` needs the screen plane: `vrdual-xr`
knows it (it places the projection layer); world-locked is easiest, head-locked
works too (the plane moves with the head).

**Auto-switch `aim` ↔ `motion`:** relative when the game hides/grabs the
pointer, absolute when a cursor is visible. On X11/XWayland: XFixes cursor
visibility + whether the pointer is grabbed. Later item; start with the
button and the per-game default.

## Per-game config (`[app:NAME]`, same precedence as the rest)

```ini
[app:ZenBound2.bin.x86]
input = mouse            # off | gamepad | mouse
mouse_mode = aim         # stick | motion | aim
mouse_speed = 1.0
mouse_curve = 2.5        # γ for stick mode
map_a = BTN_LEFT         # any KEY_*, BTN_* or pad button
map_b = KEY_ESC
map_menu = KEY_ESC
head_look = off          # see M4: off | mouse | edge
```

The profile is picked from the control block's `app` (already set by the
layer). Caveat: in Proton games several processes join the session
(xalia.exe, the game); take the one that owns the frame file.

## Where it lives

`common/vrdual_input.hpp`: mapping engine (sources → actions → uinput sinks).
Sources: OpenXR actions (inside `vrdual-xr`, it owns the session) and evdev
pads (standalone `vrdual input` for desktop use without the HMD). Sinks:
uinput pad, mouse (relative), tablet (absolute), keyboard.

## Done when

- selftest: a uinput reader sees the expected pad/mouse/key events for scripted
  inputs (simulated OpenXR actions via Monado's simulated controllers, and a
  fake evdev source)
- manual in the Quest: one gamepad game (LEGO Harry Potter) with VR
  controllers as pad; Zen Bound 2 in `aim` and `motion` mode.

**Workarounds until then:** run the game fullscreen (it grabs the mouse);
prefer games with gamepad support and a real pad.

**Notes:** Zen Bound 2 had trouble rotating objects with the mouse in the
first session; probably the game (windowed, no grab), recheck fullscreen.
