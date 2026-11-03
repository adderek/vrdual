#!/usr/bin/env python3
"""Game table: site/games.json is the source, docs/GAMES.md shows it.

gen_games.py           rewrite the table between the games:begin / games:end
                       markers in docs/GAMES.md (alphabetical)
gen_games.py --check   validate games.json and the site translations
                       (site/i18n.json: every key of the pages' data-i18n and
                       the scripts' t() calls); exit 1 if GAMES.md is out of date

The same games.json is the data of the GitHub Pages viewer (site/). Public
material only: unpublished notes never go here (TODO 18).
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "site" / "games.json"
DOC = ROOT / "docs" / "GAMES.md"
FIELDS = ["game", "store", "path", "status", "settings", "notes"]  # strings, except notes: a list
# notes: one current fact per item (what holds now, not a dated diary); the
# date of the last test goes in "tested" (ISO, "" = unknown).
COLUMNS = ["game", "store", "path", "status", "tested", "settings", "notes"]  # GAMES.md
# Site only: one-GPU status ("" = untested), other VR routes, files in
# screenshots/ tracked in git (gif = wiggle, mp4 = video), and whether a
# tuned configuration exists that is not written up yet.
EXTRA = {"tested": str, "single": str, "alt": str, "media": list, "writeup_pending": bool}
SHOTS = ROOT / "screenshots"
STATUS = {"works", "runs", "broken", "no effect", "retest"}
BEGIN, END = "<!-- games:begin (generated from site/games.json by tools/gen_games.py) -->", "<!-- games:end -->"


def sort_key(g):
    n = re.sub(r"\(.*?\)", "", g["game"]).strip().lower()
    n = re.sub(r"^(the|a) ", "", n)
    return re.sub(r"[^a-z0-9 ]", "", n)


def load():
    games = json.loads(DATA.read_text())
    errs = []
    for k, g in enumerate(games):
        where = f"{DATA.name}[{k}] {g.get('game', '?')!r}"
        if not isinstance(g, dict) or list(g) != FIELDS + list(EXTRA):
            errs.append(f"{where}: want exactly the keys {FIELDS + list(EXTRA)} in that order")
            continue
        if not all(isinstance(g[f], str) for f in FIELDS if f != "notes") or not all(isinstance(g[k], t) for k, t in EXTRA.items()) \
                or not isinstance(g["notes"], list) or not all(isinstance(n, str) and n for n in g["notes"]):
            errs.append(f"{where}: wrong value types (strings; notes and media lists of strings, writeup_pending true/false)")
        elif g["tested"] and not re.fullmatch(r"\d{4}-\d\d-\d\d", g["tested"]):
            errs.append(f"{where}: tested {g['tested']!r} is not an ISO date (YYYY-MM-DD) or empty")
        elif any(re.search(r"\b20\d\d-\d\d-\d\d\b", n) for n in g["notes"]):
            errs.append(f"{where}: a date in notes; notes say what holds now, the test date goes in \"tested\"")
        elif g["single"] not in STATUS | {""}:
            errs.append(f"{where}: single {g['single']!r} not in {sorted(STATUS)} or empty")
        elif any(not isinstance(m, str) or "/" in m or not (SHOTS / m).is_file() for m in g["media"]):
            errs.append(f"{where}: media must name files in screenshots/")
        elif not g["game"]:
            errs.append(f"{where}: empty game")
        elif g["status"] not in STATUS:
            errs.append(f"{where}: status {g['status']!r} not in {sorted(STATUS)}")
        elif any("|" in v or "\n" in v for f in FIELDS for v in (g[f] if f == "notes" else [g[f]])):
            errs.append(f"{where}: '|' or a newline breaks the Markdown table")
    names = [g.get("game") for g in games if isinstance(g, dict)]
    errs += [f"duplicate game {n!r}" for n in sorted({n for n in names if names.count(n) > 1})]
    if errs:
        sys.exit("\n".join(errs))
    return games


def i18n_keys():
    """{key: allowed placeholders}: data-i18n of the site's pages ({n} only) and
    t("KEY", "English") / tr(...) in its scripts (the English text's own)."""
    site = ROOT / "site"
    keys = {}
    for f in ("index.html", "games.html", "game.html"):
        for k in re.findall(r'data-i18n="(\w+)"', (site / f).read_text()):
            keys[k] = {"n"}
    eng, errs = {}, []
    for f in [*sorted(site.glob("*.js")), site / "game.html"]:
        for k, q in re.findall(r"""\b(?:t|tr)\("(\w+)",\s*("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')""", f.read_text()):
            if eng.setdefault(k, q[1:-1]) != q[1:-1]:
                errs.append(f"{f.name}: {k} has two English texts")
            keys[k] = keys.get(k, set()) | set(re.findall(r"\{(\w+)\}", q))
    for k in re.findall(r'"(st_\w+)"', (site / "common.js").read_text()):  # status labels, looked up by name
        keys.setdefault(k, set())
    return keys, errs


def check_i18n():
    """site/i18n.json: every language has exactly the site's keys (pages and scripts)."""
    keys, errs = i18n_keys()
    tr = json.loads((ROOT / "site" / "i18n.json").read_text())
    js = (ROOT / "site" / "i18n.js").read_text()
    for lang, d in tr.items():
        if set(d) != set(keys):
            errs.append(f"i18n.json {lang}: missing {sorted(set(keys) - set(d))}, unknown {sorted(set(d) - set(keys))}")
        errs += [f"i18n.json {lang}.{k}: unknown placeholder" for k, v in d.items() if set(re.findall(r"\{(\w+)\}", v)) - keys.get(k, set())]
        if f'["{lang}"' not in js:
            errs.append(f"i18n.json {lang}: not in LANGS of i18n.js")
    if errs:
        sys.exit("\n".join(errs))
    return len(tr)


def table(games):
    rows = ["| game (Steam appid) | store: test | path | status | tested | settings | notes |", "|---|---|---|---|---|---|---|"]
    cell = lambda g, f: "<br>".join("• " + n for n in g[f]) if f == "notes" else g[f]
    rows += ["| " + " | ".join(cell(g, f) for f in COLUMNS) + " |" for g in sorted(games, key=sort_key)]
    return "\n".join([BEGIN, *rows, END])


def main():
    games = load()
    doc = DOC.read_text()
    m = re.search(re.escape(BEGIN) + ".*?" + re.escape(END), doc, re.S)
    if not m:
        sys.exit(f"{DOC}: markers missing")
    new = doc[: m.start()] + table(games) + doc[m.end():]
    if sys.argv[1:] == ["--check"]:
        if new != doc:
            sys.exit(f"{DOC.relative_to(ROOT)} is out of date: run tools/gen_games.py")
        print(f"{len(games)} games, {DOC.relative_to(ROOT)} up to date; {check_i18n()} translations complete")
    elif not sys.argv[1:]:
        DOC.write_text(new)
        print(f"{len(games)} games written to {DOC.relative_to(ROOT)}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
