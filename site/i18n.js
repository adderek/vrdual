// Site translations: English stays in the HTML (data-i18n="KEY") or in the
// script (t(key, english, {vars})), other languages come from i18n.json
// (one flat dict per language, keys shared by all pages). Picked: saved
// choice, else the browser's languages, else English (?lang=CODE overrides,
// not saved). data-i18n-attr="NAME" translates an attribute instead of the
// content; SVG text gets plain text. Pages that build HTML in script render
// again from onLang(fn).
"use strict";
const LANGS = [  // flag, own name, English name; ordered by English name
  ["zh-CN", "🇨🇳", "简体中文", "Chinese (Simplified)"],
  ["en", "🇬🇧", "English", "English"],
  ["fr", "🇫🇷", "Français", "French"],
  ["de", "🇩🇪", "Deutsch", "German"],
  ["ja", "🇯🇵", "日本語", "Japanese"],
  ["pl", "🇵🇱", "Polski", "Polish"],
  ["pt-BR", "🇧🇷", "Português (Brasil)", "Portuguese (Brazil)"],
  ["ru", "🇷🇺", "Русский", "Russian"],
  ["es", "🇪🇸", "Español", "Spanish"],
];
const I18N = {tr: null, en: {}, lang: "en", vars: {}, hooks: []};

// Script text: KEY's translation (English given here), {name} from vars.
function t(k, en, vars) {
  if (!(k in I18N.en)) I18N.en[k] = en;
  const dict = I18N.lang === "en" ? null : I18N.tr && I18N.tr[I18N.lang];
  const v = (dict && dict[k]) ?? en;
  return v.replace(/\{(\w+)\}/g, (m, n) => vars && n in vars ? vars[n] : m);
}
function onLang(fn) { I18N.hooks.push(fn); }

function pickLang() {
  const ok = c => LANGS.some(l => l[0] === c) ? c : null;
  const q = ok(new URLSearchParams(location.search).get("lang"));  // shareable ?lang=pl
  if (q) return q;
  try { const s = ok(localStorage.getItem("vrdual-lang")); if (s) return s; } catch (_) {}
  for (const b of navigator.languages || [navigator.language || "en"]) {
    const [base, region] = b.split("-");
    if (ok(b)) return b;
    if (base === "zh" && !/^(TW|HK|MO|Hant)$/i.test(region || "")) return "zh-CN";  // Simplified only
    if (base === "pt") return "pt-BR";
    if (ok(base)) return base;
  }
  return "en";
}

function applyLang(lang) {
  const dict = lang === "en" ? I18N.en : (I18N.tr && I18N.tr[lang]) || I18N.en;
  I18N.lang = dict === I18N.en ? "en" : lang;
  document.documentElement.lang = I18N.lang;
  document.querySelectorAll("[data-i18n]").forEach(el => {
    let v = dict[el.dataset.i18n] ?? I18N.en[el.dataset.i18n];
    if (/\{(\w+)\}/.test(v) && [...v.matchAll(/\{(\w+)\}/g)].some(m => !(m[1] in I18N.vars))) v = "";  // value not known yet
    v = v.replace(/\{(\w+)\}/g, (m, k) => I18N.vars[k]);
    const attr = el.dataset.i18nAttr;
    if (attr) el.setAttribute(attr, v);
    else if (el instanceof SVGElement) el.textContent = v;
    else el.innerHTML = v;
  });
  const sel = document.getElementById("lang");
  if (sel) sel.value = I18N.lang;
  I18N.hooks.forEach(f => f());
}

function setVar(k, v) { I18N.vars[k] = v; applyLang(I18N.lang); }

(function initI18n() {
  document.querySelectorAll("[data-i18n]").forEach(el => {  // English from the page itself
    const a = el.dataset.i18nAttr;
    I18N.en[el.dataset.i18n] = a ? el.getAttribute(a) : el instanceof SVGElement ? el.textContent : el.innerHTML.trim();
  });
  applyLang("en");  // hides "{n}" until the count is known
  const sel = document.getElementById("lang");
  sel.innerHTML = LANGS.map(([c, f, own, en]) => `<option value="${c}">${f} ${own}${own === en ? "" : " · " + en}</option>`).join("");
  const want = pickLang();
  sel.value = "en";
  sel.addEventListener("change", () => {
    try { localStorage.setItem("vrdual-lang", sel.value); } catch (_) {}
    applyLang(sel.value);
  });
  fetch("i18n.json").then(r => r.json()).then(t => { I18N.tr = t; applyLang(want); }).catch(() => {});
})();
