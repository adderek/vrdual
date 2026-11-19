# Market position in a market flooded with AI content: recommendation

Date: 2026-11-19. Follows [business-value.md](business-value.md), which lists
candidates. This note says where vrdual's value sits now and what to do first.
Analysis only; no code. Decisions marked OWNER need the owner.

Assumed: "flooded with AI content" means generated text, images and listicles
that bury trustworthy information in search and social feeds. Based on the repo
only; no external market research was done.

## Position

- vrdual generates no content. There is no AI in its pipeline. It makes existing
  flat games stereo through a Vulkan layer, app-unaware. The content glut does not
  compete with it directly.
- The glut raises the value of verified, reproducible results. The site's
  `games.json` already carries status, test date, and notes that name what was
  confirmed (headset, snapshot, selftest). The layer's game fixes are byte-checked,
  it writes nothing to disk, and it has no telemetry. A model cannot produce these
  facts; they come from running the software on real hardware.
- Discovery is the risk. Search and social are filling with AI "best VR games"
  listicles. Concrete entries with dates and headset screenshots are the
  differentiator, not prose.
- Real gap: there is no working VR path for old flat games on Linux. Niche, but
  vrdual is the only route the repo knows of.

## Value to deliver (ranked)

1. **Trust as product.** Every public claim traceable to a test: date, setup,
   evidence. Keep notes to current facts (already the rule in AGENTS.md).
2. **`vrdual doctor`** (business-value.md item 1). Cuts support, the main hidden
   cost of one maintainer. Plain output, `--json` for machines.
3. **Public performance matrix** (items 3, TODO 18/19). Game x config x GPU setup
   x mode x FPS per eye, with baseline. The strongest signal for new users and
   donors that does not depend on prose.
4. **Profile sync** (item 2, TODO 11). Users without a checkout get new profiles.
   Pinned index, hash check, no user data sent.
5. **Packaging** (item 5). Cheap reach. OWNER: target distributions.

## Avoid

- **AI texture or model upscaling as a product line.** It is the adjacent trend
  (RTX Remix, generative textures) and would put vrdual in competition with AI
  tools. If pursued at all, treat it as a separate decision, not a default step
  of `docs/MODDING.md`.
- Telemetry in the layer; UI inside the game process; paid or donor-only features.
  These break invariants in AGENTS.md and the donation rule (no perks in public
  text).
- Generated copy on the site. Site text stays human-written and short.

## Open (OWNER)

- Is the goal a niche base (Linux, WiVRn) or a wider one (single-GPU users)?
  This decides whether single-GPU work (item 4 in business-value.md) comes before
  packaging.
- Is the modding track (textures, shaders) in scope, given the AI overlap?
- Is a B2B offer allowed under the public-site rules (business-value.md item 7)?

## Verified

Read only: business-value.md, MODDING.md, site/games.json, AGENTS.md. Nothing
built or run. Status: recommendation.
