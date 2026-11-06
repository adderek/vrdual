# docs/ as a knowledge base: analysis brief

For an agent (or the owner) to analyse and propose, not to execute blindly.
Owner's observation (2026-11-06): `docs/` has become the project's
knowledge base; it may need reorganising so that people and agents find
things faster.

## Current state (2026-11-06)

- ~4800 lines of Markdown: `docs/` (ARCHITECTURE 680, PLAN 250, CONFIG 245,
  GAMES 190 (generated table), MY_GAMES 1920, MODDING, TESTING), 14 task
  files in `docs/tasks/` (milestones M1-M6, features, backlog 280), plus
  README, INSTALL, CHANGELOG, TODO, AGENTS.md at the root.
- Knowledge lives in several shapes: reference (CONFIG: one row per key),
  design + rationale (ARCHITECTURE, PLAN, TESTING), work items (TODO, tasks,
  backlog), per-game facts (games.json → GAMES.md, MY_GAMES, profiles/,
  autotest/recipes/), hard-won pitfalls (AGENTS.md "Gotchas already paid
  for"), history (CHANGELOG, git log).
- Agents enter through AGENTS.md (read order, file map, gotchas); humans
  through README and the site.
- Known pain points:
  - one fact in several places (a game's settings in games.json, its
    profile, the user's config, MY_GAMES' `vrdual` column, the recipe);
    MY_GAMES' `vrdual` column is filled by hand and lags behind games.json;
  - ARCHITECTURE mixes how-it-works with long per-feature sections that
    are also the place to look up a config key's background;
  - task files grow into logs of what was done (dated) instead of what is
    true now; finished tasks are not archived;
  - searching needs knowing the file: "why do we X" may be in PLAN,
    ARCHITECTURE, a task file, AGENTS.md or a commit message;
  - MY_GAMES.md is large (owner's library) and is mostly empty columns.

## What we want to achieve

1. **Find by question.** For "why…", "how does…", "which key…", "what
   happened with game X" there is one obvious place to look, and AGENTS.md
   says which.
2. **One fact, one place.** Everything else links or is generated (as
   GAMES.md is from games.json). Candidates: MY_GAMES' `vrdual` column from
   games.json; per-game links between games.json, profile, recipe.
3. **Current vs history.** Docs say what holds now; history goes to
   CHANGELOG / git. Finished task files move to an archive (or shrink to a
   decision record).
4. **Decisions findable.** Rationale ("why own runner, not SikuliX", "why
   OpenXR via WiVRn", "game_fixes exception") as short decision records
   with date, context, decision, consequences; PLAN's option space stays.
5. **Agent-friendly.** Stable headings and anchors, small files with a
   one-line summary at the top, an index (generated?) of files and their
   summaries; consider a front-matter line per file (`summary:`, `kind:`
   reference / design / task / decision / game).
6. **Checked.** `vrdual ci` already checks CONFIG lists every key and
   games.json vs GAMES.md; extend to: every docs file is linked from the
   index, no dead relative links, generated parts up to date.

## Constraints

- Public repo, public site: no unpublished notes, local paths, GPU
  addresses (AGENTS.local.md stays local); donor rule in the owner's memory:
  no perks for donors in public text.
- English, terse (AGENTS.md conventions). Do not lose content: every move
  keeps the text or links to where it went.
- Keep links from the site, README, issues and the wiki-like anchors that
  others may have bookmarked working (or redirect notes).
- Selftest and ci stay green; `gen_games.py` and the site keep working.

## Deliverable of the analysis

A proposal (a new section in this file or a separate plan): target layout,
a mapping old file/section → new place, what becomes generated, which ci
checks to add, and the order of moves in small commits. Ask the owner before
moving files.
