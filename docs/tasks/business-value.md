# Business value and usefulness: what to add

Owner's request (2026-11-19): consider what would raise the project's business
value and usefulness. Analysis brief for the next agent or the owner; no code
yet. Decisions marked OWNER need the owner before work starts.

Assumed: "business value" = more users, less support load per user, then
revenue. Revenue is donations only; no paid features, no donor perks in public
text (tax, see the owner's support-model memory). The layer stays passive: no
UI, network or threads inside the app process (AGENTS.md invariants).

## Candidates, ranked by value / cost

1. **`vrdual doctor`: one-command diagnostics.** Checks uinput, WiVRn, GPU
   count and link, Flatpak Steam and its layer copy, `ipv6` boot flag,
   runtime-PM state, `spirv-val`/`glslc` for builds. Support is the hidden
   cost of a one-person project; most reports are environment problems that a
   doctor catches. Output as plain lines (machine-readable with `--json`).
   Check: selftest-style test with a fake environment.

2. **Online sync of profiles and the game list (TODO 11, left open).** Users
   without a checkout get no new profiles today. Source: the published
   `site/games.json` and `profiles/*.conf` on Pages; `vrdual profiles sync`
   fetches a versioned index, verifies it, writes to the host config dir. The
   app process does nothing. Needs: pinned index version, signature or hash
   check, no user data sent.

3. **Public performance records and a compatibility matrix (TODO 18, 19).**
   Non-technical visitors read pictures and numbers, technical ones read logs
   (owner's note). A table of game x configuration x GPU setup x mode x FPS per
   eye, with the baseline, is the strongest trust signal for new users and
   donors. Collection stays manual or via `vrdual perf` plus an issue form; no
   telemetry. Private configurations stay local (item 18).

4. **Single-GPU quality.** Assumed: most VR users have one GPU, and the
   two-GPU requirement limits adoption. Mode M6 (alternate-eye) exists but
   halves the frame rate. Depth-based synthesis of the missing eye in
   `vrdual-xr` (framegen-effects.md, DIBR) is the largest reach lever and the
   most expensive. Start with a measurement: `vrdual measure` numbers on a
   single-GPU setup.

5. **Packaging.** AUR package, Flatpak of the tools next to Steam's layer copy,
   `.deb` / `.rpm`. Today install means checkout, SDL3, glslc, multilib. Cheap
   reach gain. OWNER: which distributions to target.

6. **Nightly regression of game recipes (TODO 23, autotest).** Driver, Mesa,
   Proton and DXVK updates break games silently. A scheduled run turns the
   public "works" status into something current. Blocked on the runner; the
   recipe validator (hostile-recipe tests) comes first.

7. **B2B offer (no product change).** Arcades and cafés with VR headsets,
   labs and museums running old games, accessibility projects. Paid setup,
   support contract, custom profiles. OWNER: whether to offer this, because it
   touches the public site and the no-perks rule.

Not recommended: telemetry in the layer, UI inside the game process, paid
features or donor-only profiles, changes to games or engines (app-unaware is
the point).

## Suggested order

1 → 2 → 3 as one loop: doctor cuts support, sync keeps profiles current
without a checkout, the public matrix brings new users and donations. Then 5
(cheap reach), 4 (after measurement), 6 (after the validator). 7 is a decision,
not engineering.

## Open questions (OWNER)

- Which distributions for packaging (5)?
- Is a B2B offer (7) allowed under the public-site rules?
- Is a single-GPU user base the goal, or do two-GPU users stay the target (4)?

## Verified

Read only. Nothing built or run. Status: proposal.
