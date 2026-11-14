// Library page: the owner's games (library.json) with their VR routes; the
// vrdual status joined from games.json through "tested".
"use strict";
const GEN_URL = {"UEVR": "https://github.com/praydog/UEVR", "UUVR": "https://github.com/Raicuparta/uuvr",
  "vorpX": "https://www.vorpx.com/", "Geo-11": "https://www.geo-11.com/"};
const PCGW = "https://www.pcgamingwiki.com/wiki/";
const PCGW_LABEL = {"native VR": () => t("l_pcgw_vr", "native VR mode"), "native 3D": () => t("l_pcgw_3d", "native stereo 3D mode")};
let tbl = null, lib = null, tested = null;

const stOf = g => tested.get(g.tested)?.status || "untested";
// dedicated: hand-written text, then native modes listed on PCGamingWiki
const dedText = g => [g.vr, ...g.pcgw_vr.filter(x => PCGW_LABEL[x]).map(x => PCGW_LABEL[x]())].filter(Boolean).join("; ");
const dedHtml = g => [g.vr ? md(g.vr) : "", ...g.pcgw_vr.filter(x => PCGW_LABEL[x]).map(x =>
  `${esc(PCGW_LABEL[x]())} <span class="muted small">(<a href="${PCGW + encodeURIComponent(g.pcgw.replace(/ /g, "_"))}#VR_support">PCGamingWiki</a>)</span>`)]
  .filter(Boolean).join("; ");
const genHtml = g => g.generic.map(x => `<a class="tag" href="${GEN_URL[x]}">${esc(x)}</a>`).join(" ");

const COLS = [
  {k: "game", label: () => t("g_col_game", "game"), text: g => g.game, key: g => nameKey(g),
    html: g => `<b>${esc(g.game)}</b>`},
  {k: "stores", label: () => t("l_col_stores", "stores"), text: g => g.stores.join(" "), html: g => esc(g.stores.join(", "))},
  {k: "vrdual", label: () => "vrdual", text: g => stOf(g) + " " + stLabel(stOf(g)),
    key: g => ["works", "runs", "broken", "no effect", "retest", "untested"].indexOf(stOf(g)),
    html: g => g.tested ? `<a href="games.html?g=${encodeURIComponent(slug({game: g.tested}))}">${stHtml(stOf(g))}</a>` : stHtml("untested")},
  {k: "vr", label: () => t("l_col_ded", "dedicated VR"), text: dedText, key: g => dedText(g) ? 0 : 1, html: dedHtml},
  {k: "generic", label: () => t("l_col_gen", "generic VR"), text: g => g.generic.join(" "), key: g => -g.generic.length, html: genHtml},
  {k: "platform", label: () => t("l_col_plat", "VR route runs on"), text: g => g.platform, html: g => esc(g.platform)},
  {k: "engine", label: () => t("l_col_engine", "engine"), text: g => g.engine.join(", "), html: g => esc(g.engine.join(", "))},
  {k: "api", label: () => t("l_col_api", "graphics API"), text: g => g.api.join(", "), html: g => esc(g.api.join(", ")), hide: true},
  {k: "pcgw", label: () => "PCGamingWiki", text: g => g.pcgw, hide: true,
    html: g => g.pcgw ? `<a href="${PCGW + encodeURIComponent(g.pcgw.replace(/ /g, "_"))}">${esc(g.pcgw)}</a>` : ""},
];

function quickState() {
  document.querySelectorAll("#quick button").forEach(b => {
    const inp = document.querySelector(`#lib input[data-f="${b.dataset.f}"]`);
    b.setAttribute("aria-pressed", String(!!inp && inp.value === b.dataset.v));
  });
}

function start() {
  tbl = makeTable({table: document.getElementById("lib"), cols: COLS, rows: lib, store: "vrdual-library",
    tools: document.getElementById("tb"), onRender: quickState,
    count: (shown, all) => {
      const n = f => shown.filter(f).length;
      document.getElementById("count").textContent = t("l_count", "games shown: {shown} of {total} · with a dedicated VR route: {ded} · generic only: {gen}",
        {shown: shown.length, total: all.length, ded: n(g => dedText(g)), gen: n(g => !dedText(g) && g.generic.length)});
    }});
  tbl.render();
}

document.getElementById("quick").addEventListener("click", e => {
  const b = e.target.closest("button"); if (!b || !tbl) return;
  tbl.filter(b.dataset.f, b.getAttribute("aria-pressed") === "true" ? "" : b.dataset.v);
});
onLang(() => { if (tbl) tbl.render(); });
Promise.all([fetch("library.json").then(r => { if (!r.ok) throw new Error(r.status); return r.json(); }), loadGames()])
  .then(([l, gs]) => { lib = l; tested = new Map(gs.map(g => [g.game, g])); start(); })
  .catch(e => { document.getElementById("count").textContent = "library.json: " + e; });
