// Game table viewer: games.json (also the source of docs/GAMES.md).
"use strict";
const REPO = "https://github.com/adderek/vrdual/blob/master/docs/";
const STATUS = ["works", "runs", "broken", "no effect", "retest"];
const COLS = ["game", "store", "path", "status", "settings", "notes"];
const HEAD = ["game", "store: test", "path", "status", "settings", "notes"];
let games = [], sortK = "game", sortDir = 1, query = "", shown = new Set(STATUS);

const esc = s => s.replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
// The Markdown the table uses: `code`, **bold**, [text](url). Escaped first.
function md(s) {
  const code = [];
  s = esc(s).replace(/`([^`]+)`/g, (_, c) => `\u0000${code.push(c) - 1}\u0000`);
  s = s.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>")
    .replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (_, t, u) => {
      if (!/^https?:\/\//.test(u)) u = /^[\w./#-]+$/.test(u) ? REPO + u : "#";
      return `<a href="${u}">${t}</a>`;
    });
  return s.replace(/\u0000(\d+)\u0000/g, (_, i) => `<code>${code[i]}</code>`);
}
const nameKey = g => g.game.replace(/\(.*?\)/g, "").trim().toLowerCase().replace(/^(the|a) /, "").replace(/[^a-z0-9 ]/g, "");
const keyOf = (g, k) => k === "game" ? nameKey(g) : k === "status" ? STATUS.indexOf(g.status) : g[k].toLowerCase();

function render() {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  const rows = games.filter(g => shown.has(g.status) && words.every(w => COLS.some(k => g[k].toLowerCase().includes(w))))
    .sort((a, b) => { const x = keyOf(a, sortK), y = keyOf(b, sortK); return (x < y ? -1 : x > y ? 1 : 0) * sortDir || (nameKey(a) < nameKey(b) ? -1 : 1); });
  document.querySelector("#t tbody").innerHTML = rows.map(g => "<tr>" + COLS.map((k, i) =>
    k === "status" ? `<td data-h="${HEAD[i]}"><span class="st st-${g.status.replace(" ", "-")}">${esc(g.status)}</span></td>`
                   : `<td data-h="${HEAD[i]}">${md(g[k])}</td>`).join("") + "</tr>").join("");
  document.getElementById("count").textContent = `${rows.length} of ${games.length} games`;
  document.querySelectorAll("th").forEach(th => th.setAttribute("aria-sort", th.dataset.k === sortK ? (sortDir > 0 ? "ascending" : "descending") : "none"));
}

function chips() {
  const box = document.getElementById("status");
  box.innerHTML = STATUS.map(s => `<button type="button" data-s="${s}" aria-pressed="true">${s} (${games.filter(g => g.status === s).length})</button>`).join("");
  box.addEventListener("click", e => {
    const b = e.target.closest("button"); if (!b) return;
    const s = b.dataset.s, on = b.getAttribute("aria-pressed") === "true";
    on ? shown.delete(s) : shown.add(s);
    b.setAttribute("aria-pressed", String(!on));
    render();
  });
}

document.getElementById("q").addEventListener("input", e => { query = e.target.value; render(); });
document.querySelectorAll("th").forEach(th => th.addEventListener("click", () => {
  sortDir = sortK === th.dataset.k ? -sortDir : 1; sortK = th.dataset.k; render();
}));
fetch("games.json").then(r => r.json()).then(d => { games = d; chips(); render(); })
  .catch(e => { document.getElementById("count").textContent = "games.json: " + e; });
