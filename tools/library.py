#!/usr/bin/env python3
"""The owner's game library: site/library.json is the source, the site's
library page (site/library.html) shows it.

library.py --check     validate library.json (CI)
library.py pcgw        fill engine / api / pcgw_vr from PCGamingWiki
                       (MediaWiki API, wikitext of each game's page; raw
                       extracts cached in ~/.cache/vrdual/pcgw.json, so a
                       rerun only asks for games not seen yet; --refresh
                       asks again for all)
library.py generic     recompute "generic" from engine / api / pcgw_vr
library.py known       write site/known.json: titles of all game pages on
                       PCGamingWiki (Category:Games); the game list loads it
                       only when a search finds nothing in the library, to
                       tell a game the maintainer does not own from a typo
library.py link        fill "tested" (the game's name in site/games.json)
                       where the names match; others stay as written

One row per game; "stores" lists where it is owned. Hand-written fields:
"vr" (dedicated VR routes for this game: VR editions, native VR, mods;
Markdown subset of the site) and "platform" (where that route runs). The
vrdual status is not stored here: "tested" names the game's entry in
site/games.json ("" = untested) and the page shows that entry's status.
Public material only (the site publishes it).
"""
import difflib
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "site" / "library.json"
GAMES = ROOT / "site" / "games.json"
KNOWN = ROOT / "site" / "known.json"
CACHE = Path.home() / ".cache" / "vrdual" / "pcgw.json"
API = "https://www.pcgamingwiki.com/w/api.php"
UA = "vrdual-library/1 (https://github.com/adderek/vrdual)"
FIELDS = {"game": str, "stores": list, "vr": str, "platform": str, "generic": list,
          "engine": list, "api": list, "pcgw_vr": list, "pcgw": str, "tested": str}
STORES = ["Steam", "GOG", "Humble", "Epic", "Amazon", "EA"]
# Generic routes: one per engine or API family, shown with a legend on the page.
GENERIC = ["UEVR", "UUVR", "vorpX", "Geo-11"]
PCGW_VR = ["native VR", "vorpX profile", "vorpX user profile", "native 3D"]


def norm(s):
    s = re.sub(r"[™®©:’'`.,!?\-–—_/&+]", " ", s.lower())
    s = re.sub(r"\b(the|a|an|and|edition|definitive|enhanced|remastered|goty|game of the year|complete|classic|hd)\b", " ", s)
    return " ".join(s.split())


def load():
    lib = json.loads(DATA.read_text())
    errs = []
    for k, g in enumerate(lib):
        where = f"{DATA.name}[{k}] {g.get('game', '?')!r}"
        if not isinstance(g, dict) or list(g) != list(FIELDS):
            errs.append(f"{where}: want exactly the keys {list(FIELDS)} in that order")
        elif not all(isinstance(g[f], t) for f, t in FIELDS.items()) or not all(isinstance(x, str) for f, t in FIELDS.items() if t is list for x in g[f]):
            errs.append(f"{where}: wrong value types")
        elif not g["game"] or not g["stores"] or set(g["stores"]) - set(STORES):
            errs.append(f"{where}: empty game or stores not in {STORES}")
        elif set(g["generic"]) - set(GENERIC) or set(g["pcgw_vr"]) - set(PCGW_VR):
            errs.append(f"{where}: generic must be in {GENERIC}, pcgw_vr in {PCGW_VR}")
        elif re.search(r"\b20\d\d-\d\d-\d\d\b", g["vr"]):
            errs.append(f"{where}: a date in vr; say what holds now")
    tested = {g["game"] for g in json.loads(GAMES.read_text())}
    errs += [f"{DATA.name} {g['game']!r}: tested {g['tested']!r} not in games.json" for g in lib
             if isinstance(g, dict) and g.get("tested") and g["tested"] not in tested]
    linked = [g.get("tested") for g in lib if isinstance(g, dict) and g.get("tested")]
    errs += [f"games.json {n!r} linked twice" for n in sorted({n for n in linked if linked.count(n) > 1})]
    errs += [f"games.json {n!r}: no library.json row has it as \"tested\" (run library.py link, or set it by hand)"
             for n in sorted(tested - set(linked))]
    keys = [norm(g.get("game", "")) for g in lib]
    errs += [f"duplicate game {n!r}" for n in sorted({n for n in keys if keys.count(n) > 1})]
    if errs:
        sys.exit("\n".join(errs))
    return lib


def save(lib):
    lib.sort(key=lambda g: norm(g["game"]))
    # one game per line: short diffs, greppable
    DATA.write_text("[\n" + ",\n".join(json.dumps(g, ensure_ascii=False) for g in lib) + "\n]\n")


def get(params):
    params = {**params, "format": "json", "formatversion": "2"}
    req = urllib.request.Request(API + "?" + urllib.parse.urlencode(params), headers={"User-Agent": UA})
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.load(r)
        except Exception as e:  # rate limit (429), timeout: back off
            print(f"  retry {attempt + 1}: {e}", file=sys.stderr)
            time.sleep(30 * (attempt + 1))
    raise RuntimeError("PCGamingWiki API unreachable")


def template(text, name):
    """{param: value} of the first {{name ...}} (values up to the next |param =)."""
    m = re.search(r"\{\{" + re.escape(name) + r"\s*\n(.*?)\n\}\}", text, re.S)
    if not m:
        return {}
    out = {}
    for line in m.group(1).split("\n"):
        p = re.match(r"\|\s*([\w\- ]+?)\s*=\s*(.*)", line)
        if p:
            out[p.group(1).lower()] = p.group(2).strip()
    return out


def extract(text):
    engines = re.findall(r"\{\{Infobox game/row/engine\|([^|}]+)", text)
    api = template(text, "API")
    vr = template(text, "VR support")
    return {"engine": [e.strip() for e in engines],
            "api": {k: v for k, v in api.items() if k.endswith("versions") or k in ("vulkan versions",) if v},
            "vr": {k: v for k, v in vr.items() if not k.endswith("notes") and v in ("true", "hackable", "limited")}}


def fetch_pages(titles):
    """{asked title: (page title, extract)} for existing pages, redirects followed."""
    out = {}
    for i in range(0, len(titles), 40):
        part = titles[i:i + 40]
        d = get({"action": "query", "prop": "revisions", "rvprop": "content", "rvslots": "main", "redirects": "1", "titles": "|".join(part)})
        q = d["query"]
        alias = {}
        for kind in ("normalized", "redirects"):
            for r in q.get(kind, []):
                alias[r["from"]] = r["to"]
        pages = {p["title"]: p for p in q["pages"] if "missing" not in p and "revisions" in p}
        for t in part:
            r = t
            while r in alias:
                r = alias[r]
            if r in pages:
                out[t] = (r, extract(pages[r]["revisions"][0]["slots"]["main"]["content"]))
        print(f"  pages {i + len(part)}/{len(titles)}: {len(out)} found", file=sys.stderr)
        time.sleep(1)
    return out


def search(name):
    """Best page title for a name, or None: accepted only when close to the name."""
    d = get({"action": "query", "list": "search", "srsearch": name, "srlimit": "5", "srnamespace": "0"})
    best = None
    for h in d["query"]["search"]:
        r = difflib.SequenceMatcher(None, norm(name), norm(h["title"])).ratio()
        if r >= 0.9 and (not best or r > best[0]):
            best = (r, h["title"])
    time.sleep(2)
    return best and best[1]


def clean_title(g):
    t = re.sub(r"[™®©]", "", g).replace("  ", " ").strip()
    return t[:1].upper() + t[1:]


def pcgw(refresh):
    lib = load()
    cache = {} if refresh or not CACHE.exists() else json.loads(CACHE.read_text())
    keep = lambda: (CACHE.parent.mkdir(parents=True, exist_ok=True), CACHE.write_text(json.dumps(cache, indent=0, ensure_ascii=False)))
    todo = [g["game"] for g in lib if g["game"] not in cache]
    found = fetch_pages(sorted({clean_title(n) for n in todo}))
    miss = []
    for n in todo:
        t = clean_title(n)
        if t in found:
            cache[n] = {"title": found[t][0], **found[t][1]}
        else:
            miss.append(n)
    keep()
    print(f"{len(miss)} not found by title, searching (~2 s each)", file=sys.stderr)
    hits, done = {}, []
    try:
        for k, n in enumerate(miss):
            h = search(clean_title(n))
            if h:
                hits[n] = h
            done.append(n)
            if k % 50 == 49:
                print(f"  search {k + 1}/{len(miss)}: {len(hits)} found", file=sys.stderr)
    except RuntimeError as e:  # unsearched games stay out of the cache: the next run asks again
        print(f"search stopped: {e}", file=sys.stderr)
    got = fetch_pages(sorted(set(hits.values())))
    for n in done:
        cache[n] = {"title": got[hits[n]][0], **got[hits[n]][1]} if n in hits and hits[n] in got else {"title": ""}
    keep()
    for g in lib:
        c = cache.get(g["game"], {"title": ""})
        g["pcgw"] = c["title"]
        g["engine"] = c.get("engine", [])
        g["api"] = api_list(c.get("api", {}))
        g["pcgw_vr"] = pcgw_vr(c.get("vr", {}))
        g["generic"] = generic(g)
    save(lib)
    print(f"{sum(bool(g['pcgw']) for g in lib)} of {len(lib)} games found on PCGamingWiki; "
          f"{len(miss) - len(done)} left unsearched (run again)", file=sys.stderr)


def known():
    titles, cont = [], {}
    while True:
        d = get({"action": "query", "list": "categorymembers", "cmtitle": "Category:Games", "cmnamespace": "0",
                 "cmlimit": "500", "cmprop": "title", **cont})
        titles += [m["title"] for m in d["query"]["categorymembers"]]
        if "continue" not in d:
            break
        cont = {"cmcontinue": d["continue"]["cmcontinue"]}
        if len(titles) % 5000 < 500:
            print(f"  {len(titles)} titles", file=sys.stderr)
        time.sleep(1)
    titles = sorted(set(titles), key=str.casefold)
    KNOWN.write_text(json.dumps(titles, ensure_ascii=False, separators=(",", ":")) + "\n")
    print(f"{len(titles)} titles written to {KNOWN.relative_to(ROOT)}", file=sys.stderr)


def api_list(a):
    names = {"direct3d versions": "D3D", "directdraw versions": "DirectDraw ", "opengl versions": "OpenGL ",
             "glide versions": "Glide ", "vulkan versions": "Vulkan "}
    out = []
    for k, pre in names.items():
        for v in re.split(r"\s*,\s*", a.get(k, "")):
            v = re.sub(r"<ref.*", "", v).strip()
            if not v or v in ("false", "unknown", "n/a"):
                continue
            out.append(pre.strip() + (" (hack)" if v == "hackable" else "") if v in ("true", "hackable") else pre + v)
    return list(dict.fromkeys(x.replace("D3D", "D3D", 1) for x in out))


def pcgw_vr(v):
    out = []
    if any(v.get(k) == "true" for k in ("htc vive", "oculus rift", "windows mixed reality", "osvr", "openxr", "steamvr", "oculusvr")):
        out.append("native VR")
    if v.get("vorpx") == "true":
        out.append("vorpX profile")
    elif v.get("vorpx") == "hackable":
        out.append("vorpX user profile")
    if v.get("native 3d") == "true":
        out.append("native 3D")
    return out


def generic(g):
    e = " ".join(g["engine"])
    d3d = {m for a in g["api"] for m in re.findall(r"^D3D(\d+)", a)}
    out = []
    if re.search(r"Unreal Engine [45]", e):
        out.append("UEVR")
    if re.search(r"\bUnity\b", e):
        out.append("UUVR")
    if any(x.startswith("vorpX") for x in g["pcgw_vr"]):
        out.append("vorpX")
    if "11" in d3d:
        out.append("Geo-11")
    return out


def main():
    a = sys.argv[1:]
    if a == ["--check"]:
        lib = load()
        k = json.loads(KNOWN.read_text()) if KNOWN.exists() else None
        if not isinstance(k, list) or not k or not all(isinstance(x, str) and x for x in k):
            sys.exit(f"{KNOWN.name}: want a non-empty list of titles (library.py known)")
        print(f"{len(lib)} games in {DATA.relative_to(ROOT)}, {len(k)} known titles")
    elif a[:1] == ["pcgw"]:
        pcgw("--refresh" in a)
    elif a == ["known"]:
        known()
    elif a == ["link"]:
        lib = load()
        idx = {norm(g["game"]): g for g in lib}
        for t in json.loads(GAMES.read_text()):
            g = idx.get(norm(re.sub(r"\(.*?\)", "", t["game"])))
            if g and not g["tested"] and t["game"] not in {x["tested"] for x in lib}:
                g["tested"] = t["game"]
        save(lib)
        names = {g["tested"] for g in lib}
        for t in json.loads(GAMES.read_text()):
            if t["game"] not in names:
                print(f"not linked: {t['game']!r}", file=sys.stderr)
    elif a == ["generic"]:
        lib = load()
        for g in lib:
            g["generic"] = generic(g)
        save(lib)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
