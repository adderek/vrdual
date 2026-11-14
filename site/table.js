// Sortable, filterable table with a column chooser; the viewer's choices
// (hidden columns, filters, sort) are kept per browser under one storage key.
// cols: [{k, label, text(row) -> string for filter and sort, html(row) -> cell,
//         key(row) -> sort key (default: text, case-folded), hide: hidden by default}]
// Filter syntax per column: words must all match (substring, any case);
// !word = must not match; * = not empty; !* = empty. Hidden columns do not filter.
"use strict";

function makeTable({table, cols, rows, store, tools, count, onRender}) {
  const def = {hidden: cols.filter(c => c.hide).map(c => c.k), filters: {}, sort: [[cols[0].k, 1]]};
  let st = structuredClone(def);
  try { Object.assign(st, JSON.parse(localStorage.getItem(store) || "{}")); } catch (_) {}
  const known = new Set(cols.map(c => c.k));
  st.hidden = st.hidden.filter(k => known.has(k));
  st.sort = st.sort.filter(([k]) => known.has(k));
  const save = () => { try { localStorage.setItem(store, JSON.stringify(st)); } catch (_) {} };
  const fold = s => String(s).toLowerCase();
  const txt = new Map(cols.map(c => [c.k, new Map(rows.map(r => [r, fold(c.text(r))]))]));
  const skey = new Map(cols.map(c => [c.k, new Map(rows.map(r => [r, c.key ? c.key(r) : fold(c.text(r))]))]));

  function match(k, f) {
    const terms = f.trim().split(/\s+/).filter(Boolean).map(w => [w.startsWith("!"), fold(w.replace(/^!/, ""))]);
    const m = txt.get(k);
    return r => terms.every(([neg, w]) => (w === "*" ? m.get(r) !== "" : m.get(r).includes(w)) !== neg);
  }

  // header: sort row + filter row; tools: column chooser and reset
  function head() {
    const vis = cols.filter(c => !st.hidden.includes(c.k));
    const fph = t("tb_filter", "filter");
    table.tHead.innerHTML = `<tr>${vis.map(c => {
      const i = st.sort.findIndex(([k]) => k === c.k);
      const so = i < 0 ? "none" : st.sort[i][1] > 0 ? "ascending" : "descending";
      return `<th data-k="${c.k}" aria-sort="${so}" title="${esc(t("tb_sort_hint", "click: sort · shift+click: add as next sort key"))}">${esc(c.label())}${st.sort.length > 1 && i >= 0 ? `<sup>${i + 1}</sup>` : ""}</th>`;
    }).join("")}</tr><tr class="filters">${vis.map(c =>
      `<td data-h="${esc(c.label())}"><input type="search" data-f="${c.k}" value="${esc(st.filters[c.k] || "")}" placeholder="${esc(fph)}" aria-label="${esc(c.label() + ": " + fph)}"></td>`).join("")}</tr>`;
    tools.innerHTML = `<details class="cols"><summary>${esc(t("tb_cols", "Columns"))}</summary><div>${cols.map(c =>
      `<label><input type="checkbox" data-c="${c.k}"${st.hidden.includes(c.k) ? "" : " checked"}> ${esc(c.label())}</label>`).join("")}</div></details>
      <button type="button" class="reset">${esc(t("tb_reset", "Reset view"))}</button>
      <span class="muted small">${t("tb_syntax", "Filters: words must all match; <code>!word</code> excludes, <code>*</code> = not empty, <code>!*</code> = empty. Shift+click a header to sort by several columns.")}</span>`;
  }

  function body() {
    const vis = cols.filter(c => !st.hidden.includes(c.k));
    const fs = Object.entries(st.filters).filter(([k, f]) => f.trim() && vis.some(c => c.k === k)).map(([k, f]) => match(k, f));
    const cmp = (a, b) => {
      for (const [k, d] of st.sort) {
        const x = skey.get(k).get(a), y = skey.get(k).get(b);
        if (x < y) return -d; if (x > y) return d;
      }
      return 0;
    };
    const shown = rows.filter(r => fs.every(f => f(r))).sort(cmp);
    table.tBodies[0].innerHTML = shown.map(r => `<tr>${vis.map(c => `<td data-h="${esc(c.label())}">${c.html(r)}</td>`).join("")}</tr>`).join("");
    if (count) count(shown, rows);
    if (onRender) onRender(shown);
  }

  table.tHead.addEventListener("click", e => {
    const th = e.target.closest("th[data-k]"); if (!th) return;
    const k = th.dataset.k, i = st.sort.findIndex(([x]) => x === k);
    if (e.shiftKey) {
      if (i < 0) st.sort.push([k, 1]); else st.sort[i][1] *= -1;
    } else st.sort = [[k, i === 0 && st.sort.length === 1 ? -st.sort[0][1] : 1]];
    save(); head(); body();
  });
  let timer = 0;
  table.tHead.addEventListener("input", e => {
    const k = e.target.dataset.f; if (!k) return;
    st.filters[k] = e.target.value; save();
    clearTimeout(timer); timer = setTimeout(body, 120);
  });
  tools.addEventListener("change", e => {
    const k = e.target.dataset.c; if (!k) return;
    st.hidden = e.target.checked ? st.hidden.filter(x => x !== k) : [...st.hidden, k];
    save(); const open = tools.querySelector("details").open; head(); tools.querySelector("details").open = open; body();
  });
  tools.addEventListener("click", e => {
    if (!e.target.closest("button.reset")) return;
    st = structuredClone(def); save(); head(); body();
  });
  return {render() { head(); body(); }, filter(k, v) { st.filters[k] = v; save(); head(); body(); }};
}
