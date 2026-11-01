// Game list: status per GPU mode, other VR routes, details and media on demand.
"use strict";
let games = [], mode = "dual", sortK = "status", sortDir = 1, query = "", hidden = new Set(), open = new Set();

const modeSt = g => mode === "dual" ? g.status : g.single || "untested";
const order = ["works", "runs", "broken", "no effect", "retest", "untested"];
const keyOf = (g, k) => k === "game" ? nameKey(g) : k === "status" ? order.indexOf(modeSt(g))
  : k === "dual" ? order.indexOf(g.status) : k === "single" ? order.indexOf(g.single || "untested") : g.alt ? 0 : 1;

function details(g) {
  const s = slug(g);
  const rows = [["store: test", g.store], ["path", g.path], ["settings", g.settings], ["notes", g.notes]]
    .filter(r => r[1]).map(([h, v]) => `<dt>${h}</dt><dd>${md(v)}</dd>`).join("");
  const sp = g.sponsor ? `<p class="sponsor">Ready-made configuration for <a href="https://github.com/sponsors/adderek">sponsors</a> (or tune it yourself, <a href="${REPO}CONFIG.md">CONFIG.md</a>).</p>` : "";
  const media = g.media.length ? `<div class="media" data-media="${esc(s)}"><button type="button" class="load">Show ${g.media.length} wiggle GIF / video</button></div>` : "";
  return `<tr class="det" id="d-${esc(s)}"><td colspan="5"><dl>${rows}</dl>${sp}${media}<p><a href="game.html?g=${encodeURIComponent(s)}">All materials for this game →</a></p></td></tr>`;
}

function render() {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  const hay = g => [g.game, g.store, g.path, g.settings, g.notes, g.alt].join(" ").toLowerCase();
  const rows = games.filter(g => !hidden.has(modeSt(g)) && words.every(w => hay(g).includes(w)))
    .sort((a, b) => { const x = keyOf(a, sortK), y = keyOf(b, sortK); return (x < y ? -1 : x > y ? 1 : 0) * sortDir || (nameKey(a) < nameKey(b) ? -1 : 1); });
  document.querySelector("#t tbody").innerHTML = rows.map(g => {
    const s = slug(g), o = open.has(s);
    return `<tr class="row${o ? " open" : ""}" data-s="${esc(s)}" tabindex="0" aria-expanded="${o}">
      <td data-h="game"><span class="tw">▸</span> ${esc(g.game)}${g.media.length ? ' <span class="cam" title="wiggle GIF / video">◐</span>' : ""}${g.sponsor ? ' <span class="spb" title="ready-made configuration for sponsors">★</span>' : ""}</td>
      <td data-h="two GPUs" class="${mode === "dual" ? "hl" : ""}">${stHtml(g.status)}</td>
      <td data-h="one GPU" class="${mode === "single" ? "hl" : ""}">${stHtml(g.single)}</td>
      <td data-h="other VR routes">${g.alt ? md(g.alt) : '<span class="muted">not researched</span>'}</td>
      <td class="more"><span class="muted">${o ? "less" : "more"}</span></td></tr>` + (o ? details(g) : "");
  }).join("");
  const works = rows.filter(g => modeSt(g) === "works").length;
  document.getElementById("count").textContent = `${rows.length} of ${games.length} games shown, ${works} work${mode === "single" ? " on one GPU" : " on two GPUs"}`;
  document.querySelectorAll("th[data-k]").forEach(th => th.setAttribute("aria-sort", th.dataset.k === sortK ? (sortDir > 0 ? "ascending" : "descending") : "none"));
  document.getElementById("single-note").hidden = mode !== "single";
}

function chips() {
  const box = document.getElementById("status");
  const list = mode === "single" ? order : STATUS;
  box.innerHTML = list.map(s => `<button type="button" data-s="${s}" aria-pressed="${!hidden.has(s)}">${s} (${games.filter(g => modeSt(g) === s).length})</button>`).join("");
}

document.getElementById("status").addEventListener("click", e => {
  const b = e.target.closest("button"); if (!b) return;
  const s = b.dataset.s;
  hidden.has(s) ? hidden.delete(s) : hidden.add(s);
  b.setAttribute("aria-pressed", String(!hidden.has(s)));
  render();
});
document.getElementById("mode").addEventListener("change", e => {
  mode = e.target.value; hidden.clear(); sortK = "status"; sortDir = 1;
  try { localStorage.setItem("vrdual-mode", mode); } catch (_) {}
  chips(); render();
});
document.getElementById("q").addEventListener("input", e => { query = e.target.value; render(); });
document.querySelectorAll("th[data-k]").forEach(th => th.addEventListener("click", () => {
  sortDir = sortK === th.dataset.k ? -sortDir : 1; sortK = th.dataset.k; render();
}));
const tbody = document.querySelector("#t tbody");
function toggle(tr) { const s = tr.dataset.s; open.has(s) ? open.delete(s) : open.add(s); render(); }
tbody.addEventListener("click", e => {
  const load = e.target.closest("button.load");
  if (load) {  // media only now: GIFs are 0.2-0.8 MB each
    const box = load.parentElement, g = games.find(x => slug(x) === box.dataset.media);
    box.innerHTML = g.media.map(mediaHtml).join("");
    return;
  }
  const tr = e.target.closest("tr.row");
  if (tr && !e.target.closest("a")) toggle(tr);
});
tbody.addEventListener("keydown", e => { const tr = e.target.closest("tr.row"); if (tr && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); toggle(tr); } });

try { mode = localStorage.getItem("vrdual-mode") === "single" ? "single" : "dual"; } catch (_) {}
document.querySelector(`#mode input[value="${mode}"]`).checked = true;
const want = new URLSearchParams(location.search).get("g");
loadGames().then(d => {
  games = d;
  if (want) open.add(want);
  chips(); render();
  if (want) document.getElementById("d-" + want)?.scrollIntoView({block: "center"});
}).catch(e => { document.getElementById("count").textContent = "games.json: " + e; });
