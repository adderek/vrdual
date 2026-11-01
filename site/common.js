// Shared by the site's pages: games.json loading and the Markdown subset the
// game table uses.
"use strict";
const REPO = "https://github.com/adderek/vrdual/blob/master/docs/";
const STATUS = ["works", "runs", "broken", "no effect", "retest"];

const esc = s => String(s).replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
// `code`, **bold**, [text](url); escaped first. Relative links point into docs/.
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
const slug = g => g.game.replace(/\(.*?\)/g, "").trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
const stCls = s => "st st-" + (s || "untested").replace(" ", "-");
const stHtml = s => `<span class="${stCls(s)}">${esc(s || "untested")}</span>`;
const loadGames = () => fetch("games.json").then(r => { if (!r.ok) throw new Error(r.status); return r.json(); });

// One media file: gif = wiggle (left / right eye alternated), mp4 = video.
function mediaHtml(f) {
  const src = "screenshots/" + encodeURIComponent(f);
  if (/\.mp4$/i.test(f)) return `<figure><span class="shot"><video src="${src}" controls preload="none" playsinline></video></span><figcaption>video</figcaption></figure>`;
  return `<figure><a class="shot" href="${src}"><img src="${src}" loading="lazy" alt="${esc(f)}: left and right eye alternated"></a><figcaption>wiggle: eyes alternated</figcaption></figure>`;
}

// Theme: auto (system) / light / dark; the choice is kept per browser. The
// head of each page applies a saved one before the first paint.
(function themeButton() {
  const order = ["auto", "light", "dark"], icon = {auto: "◐", light: "☀", dark: "☾"};
  const nav = document.querySelector("nav.top");
  if (!nav) return;
  let t = document.documentElement.dataset.theme || "auto";
  const b = document.createElement("button");
  b.type = "button"; b.className = "theme";
  const show = () => { b.textContent = icon[t]; b.title = "Theme: " + t; b.setAttribute("aria-label", b.title); };
  b.addEventListener("click", () => {
    t = order[(order.indexOf(t) + 1) % order.length];
    if (t === "auto") delete document.documentElement.dataset.theme; else document.documentElement.dataset.theme = t;
    try { t === "auto" ? localStorage.removeItem("vrdual-theme") : localStorage.setItem("vrdual-theme", t); } catch (_) {}
    show();
  });
  show(); nav.appendChild(b);
})();
