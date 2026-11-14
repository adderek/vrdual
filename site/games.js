// Game list: every owned game (library.json) with its vrdual status (games.json,
// linked by "tested"), dedicated and generic VR routes; details and media of a
// tested game on demand.
"use strict";
const GEN_URL = {"UEVR": "https://github.com/praydog/UEVR", "UUVR": "https://github.com/Raicuparta/uuvr",
  "vorpX": "https://www.vorpx.com/", "Geo-11": "https://www.geo-11.com/"};
const PCGW = "https://www.pcgamingwiki.com/wiki/";
const ORDER = ["works", "runs", "broken", "no effect", "retest", "untested"];
let tbl = null, rows = null;

const pcgwUrl = r => PCGW + encodeURIComponent(r.pcgw.replace(/ /g, "_"));
const nativeOf = r => r.pcgw_vr.filter(x => x === "native VR" || x === "native 3D")
  .map(x => x === "native VR" ? t("l_pcgw_vr", "native VR mode") : t("l_pcgw_3d", "native stereo 3D mode"));
const dedText = r => [r.vr, ...nativeOf(r)].filter(Boolean).join("; ");
const dedHtml = r => [r.vr ? markPaid(md(r.vr)) : "", ...nativeOf(r).map(x => `${esc(x)} <span class="muted small">(<a href="${pcgwUrl(r)}#VR_support">PCGamingWiki</a>)</span>`)]
  .filter(Boolean).join("; ");
const dual = r => r.t ? r.t.status : "untested", single = r => r.t && r.t.single || "untested";

function details(r) {
  const g = r.t;
  if (!g) return "";
  const s = slug(g);
  const sp = g.writeup_pending ? `<p class="pending">${t("g_pending", 'Tuned configuration, write-up pending (until then tune it yourself, <a href="{url}">CONFIG.md</a>).', {url: REPO + "CONFIG.md"})}</p>` : "";
  const media = g.media.length ? `<div class="media" data-media="${esc(s)}"><button type="button" class="load">${esc(t("g_show_media", "Show wiggle GIFs / videos ({n})", {n: g.media.length}))}</button></div>` : "";
  return `<dl>${detailRows(g)}</dl>${sp}${media}<p><a href="game.html?g=${encodeURIComponent(s)}">${esc(t("g_all_mat", "All materials for this game →"))}</a></p>`;
}

const COLS = [
  {k: "game", label: () => t("g_col_game", "game"), text: r => r.game, key: r => nameKey(r),
    html: r => {
      const g = r.t;
      const marks = g ? (g.media.length ? `<span class="cam" title="${esc(t("g_mark_media", "wiggle GIF / video"))}">◐</span>` : "") +
        (g.writeup_pending ? `<span class="spb" title="${esc(t("g_mark_pending", "tuned configuration, write-up pending"))}">★</span>` : "") : "";
      return `<div class="gcell"><span>${g ? '<span class="tw">▸</span> ' : ""}${esc(r.game)}</span>${marks ? `<span class="marks">${marks}</span>` : ""}</div>`;
    }},
  {k: "dual", label: () => t("g_two", "two GPUs"), text: r => dual(r) + " " + stLabel(dual(r)), key: r => ORDER.indexOf(dual(r)), html: r => stHtml(dual(r))},
  {k: "single", label: () => t("g_one", "one GPU"), text: r => single(r) + " " + stLabel(single(r)), key: r => ORDER.indexOf(single(r)), html: r => stHtml(single(r))},
  {k: "vr", label: () => t("l_col_ded", "dedicated VR"), text: dedText, key: r => dedText(r) ? 0 : 1, html: dedHtml},
  {k: "generic", label: () => t("l_col_gen", "generic VR"), text: r => r.generic.join(" "), key: r => -r.generic.length,
    html: r => r.generic.map(x => `<a class="tag" href="${GEN_URL[x]}">${esc(x)}${PAID.includes(x) ? paidMark() : ""}</a>`).join(" ")},
  {k: "platform", label: () => t("l_col_plat", "VR route runs on"), text: r => r.platform, html: r => esc(r.platform), hide: true},
  {k: "engine", label: () => t("l_col_engine", "engine"), text: r => r.engine.join(", "), html: r => esc(r.engine.join(", "))},
  {k: "api", label: () => t("l_col_api", "graphics API"), text: r => r.api.join(", "), html: r => esc(r.api.join(", ")), hide: true},
  {k: "stores", label: () => t("l_col_stores", "stores"), text: r => r.stores.join(" "), html: r => esc(r.stores.join(", ")), hide: true},
  {k: "pcgw", label: () => "PCGamingWiki", text: r => r.pcgw, hide: true,
    html: r => r.pcgw ? `<a href="${pcgwUrl(r)}">${esc(r.pcgw)}</a>` : ""},
];

function onRender() {
  const f = tbl.filters();
  document.querySelectorAll("#quick button").forEach(b => b.setAttribute("aria-pressed", String((f[b.dataset.f] || "") === b.dataset.v)));
  document.getElementById("single-note").hidden = !(f.single || "").trim();
  notFound();
}

// Nothing shown: hidden by the filters, a game the maintainer does not own
// (known.json: all games on PCGamingWiki, loaded only now), or a typo.
let known = null;
function notFound() {
  const none = document.getElementById("none");
  none.hidden = !!document.querySelector("#t tbody tr");
  if (none.hidden) return;
  const q = tbl.q(), words = q.toLowerCase().split(/\s+/).filter(Boolean);
  const hit = s => words.every(w => s.toLowerCase().includes(w));
  const filtersOnly = () => { none.textContent = t("g_not_owned", "No game matches. Only games the maintainer owns are listed (only those can be tested); check the filters above or the spelling."); };
  const owned = rows.filter(r => hit(r.game)).length;
  if (words.length && owned) {  // the column filters hide them (by default: untested games)
    none.innerHTML = `${esc(t("g_hidden", "Owned games match ({n}), but the filters hide them.", {n: owned}))} <button type="button" class="clear">${esc(t("g_show_all", "Show them"))}</button>`;
    return;
  }
  if (!words.length) return filtersOnly();
  none.textContent = "…";
  (known ||= getJson("known.json")).then(k => {
    if (tbl.q() !== q) return;  // typed on meanwhile
    const lib = new Set(rows.map(r => r.pcgw));
    const m = k.filter(x => hit(x) && !lib.has(x));
    if (!m.length) { none.textContent = t("g_unknown", "No known game matches; check the spelling."); return; }
    m.sort((a, b) => a.length - b.length);
    none.innerHTML = `${esc(t("g_known", "The maintainer does not own this game, so it cannot be tested with vrdual here. Matching games on PCGamingWiki (engine, graphics API and VR support on their pages):"))}
      <ul>${m.slice(0, 10).map(x => `<li><a href="${PCGW + encodeURIComponent(x.replace(/ /g, "_"))}">${esc(x)}</a></li>`).join("")}</ul>` +
      (m.length > 10 ? esc(t("g_known_more", "{n} more; refine the search.", {n: m.length - 10})) : "");
  }).catch(filtersOnly);
}

function start(want) {
  tbl = makeTable({table: document.getElementById("t"), cols: COLS, rows, store: "vrdual-games",
    tools: document.getElementById("tb"), onRender, detail: details,
    id: r => r.t ? slug(r.t) : "x-" + slug(r),
    search: {input: document.getElementById("q"), text: r => [r.game, dedText(r), r.engine.join(" "),
      ...(r.t ? [r.t.game, r.t.store, r.t.path, r.t.settings, ...r.t.notes] : [])].join(" ")},
    defaults: {filters: {dual: "!untested"}, sort: [["dual", 1], ["game", 1]]},
    count: (shown, all) => {
      document.getElementById("count").textContent = t("g_count", "games shown: {shown} of {total} · working on two GPUs: {works} · with a dedicated VR route: {ded}",
        {shown: shown.length, total: all.length, works: shown.filter(r => dual(r) === "works").length, ded: shown.filter(r => dedText(r)).length});
    }});
  tbl.render();
  const r = want && rows.find(x => x.t && slug(x.t) === want);
  if (r) tbl.show(r);
  const q = new URLSearchParams(location.search).get("q");  // shareable search: games.html?q=NAME
  if (q !== null) tbl.query(q);
}

document.getElementById("quick").addEventListener("click", e => {
  const b = e.target.closest("button"); if (!b || !tbl) return;
  tbl.filter(b.dataset.f, b.getAttribute("aria-pressed") === "true" ? "" : b.dataset.v);
});
document.getElementById("none").addEventListener("click", e => {
  if (e.target.closest("button.clear")) for (const k of Object.keys(tbl.filters())) tbl.filter(k, "");
});
document.querySelector("#t tbody").addEventListener("click", e => {
  const load = e.target.closest("button.load");
  if (!load) return;  // media only now: GIFs are 0.2-0.8 MB each
  const box = load.parentElement, r = rows.find(x => x.t && slug(x.t) === box.dataset.media);
  box.innerHTML = r.t.media.map(mediaHtml).join("");
});
onLang(() => { if (tbl) tbl.render(); });
Promise.all([loadLibrary(), loadGames()]).then(([lib, gs]) => {
  const by = new Map(gs.map(g => [g.game, g]));
  rows = lib.map(r => ({...r, t: by.get(r.tested) || null}));
  start(new URLSearchParams(location.search).get("g"));
}).catch(e => { document.getElementById("count").textContent = "games.json / library.json: " + e; });
