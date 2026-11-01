#!/usr/bin/env python3
"""Game table: site/games.json is the source, docs/GAMES.md shows it.

gen_games.py           rewrite the table between the games:begin / games:end
                       markers in docs/GAMES.md (alphabetical)
gen_games.py --check   validate games.json; exit 1 if GAMES.md is out of date

The same games.json is the data of the GitHub Pages viewer (site/). Public
material only: sponsor configurations never go here (TODO 18).
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "site" / "games.json"
DOC = ROOT / "docs" / "GAMES.md"
FIELDS = ["game", "store", "path", "status", "settings", "notes"]
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
        if not isinstance(g, dict) or list(g) != FIELDS:
            errs.append(f"{where}: want exactly the keys {FIELDS} in that order")
            continue
        if not all(isinstance(v, str) for v in g.values()):
            errs.append(f"{where}: values must be strings")
        elif not g["game"]:
            errs.append(f"{where}: empty game")
        elif g["status"] not in STATUS:
            errs.append(f"{where}: status {g['status']!r} not in {sorted(STATUS)}")
        elif any("|" in v or "\n" in v for v in g.values()):
            errs.append(f"{where}: '|' or a newline breaks the Markdown table")
    names = [g.get("game") for g in games if isinstance(g, dict)]
    errs += [f"duplicate game {n!r}" for n in sorted({n for n in names if names.count(n) > 1})]
    if errs:
        sys.exit("\n".join(errs))
    return games


def table(games):
    rows = ["| game (Steam appid) | store: test | path | status | settings | notes |", "|---|---|---|---|---|---|"]
    rows += ["| " + " | ".join(g[f] for f in FIELDS) + " |" for g in sorted(games, key=sort_key)]
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
        print(f"{len(games)} games, {DOC.relative_to(ROOT)} up to date")
    elif not sys.argv[1:]:
        DOC.write_text(new)
        print(f"{len(games)} games written to {DOC.relative_to(ROOT)}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
