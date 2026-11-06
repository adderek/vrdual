#!/usr/bin/env python3
"""Owned Steam games from the local client cache, marked tested / installed.

Reads appcache/packageinfo.vdf (licences: package -> app ids) and
appcache/appinfo.vdf (names, types) of a Steam install; nothing is sent
anywhere. Prints TSV: appid, T (in site/games.json) or -, I (installed) or -,
Steam Deck category (1 unsupported, 2 playable, 3 verified), OS list, name.
Only apps of type "game". Default root: Flatpak Steam.

  tools/steam_library.py [--root STEAM_DIR] [--untested]
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def kv(b, p, keys):
    """Binary KeyValues at p; keys = string table (appinfo v29) or None (inline)."""
    d = {}
    while True:
        t = b[p]
        p += 1
        if t == 8:
            return d, p
        if keys is None:
            e = b.index(b"\0", p)
            k = b[p:e].decode("utf8", "replace")
            p = e + 1
        else:
            k = keys[struct.unpack_from("<I", b, p)[0]]
            p += 4
        if t == 0:
            v, p = kv(b, p, keys)
        elif t == 1:
            e = b.index(b"\0", p)
            v = b[p:e].decode("utf8", "replace")
            p = e + 1
        elif t in (2, 3, 4, 6):
            v = struct.unpack_from("<I", b, p)[0]
            p += 4
        elif t in (7, 10):
            v = struct.unpack_from("<Q", b, p)[0]
            p += 8
        else:
            raise ValueError("unknown KeyValues type %d" % t)
        d[k] = v


def appinfo(path):
    b = open(path, "rb").read()
    magic = struct.unpack_from("<I", b, 0)[0]
    keys, p = None, 8
    if magic == 0x07564429:  # v29: string table at the offset in the header
        sto = struct.unpack_from("<q", b, 8)[0]
        n = struct.unpack_from("<I", b, sto)[0]
        q, keys = sto + 4, []
        for _ in range(n):
            e = b.index(b"\0", q)
            keys.append(b[q:e].decode("utf8", "replace"))
            q = e + 1
        p = 16
    apps = {}
    while True:
        aid, size = struct.unpack_from("<II", b, p)
        if aid == 0:
            return apps
        d, _ = kv(b, p + 8 + 60, keys)  # infostate, last_updated, token, sha1, change, binary sha1
        apps[aid] = d.get("appinfo", d)
        p += 8 + size


def licensed_apps(path):
    b = open(path, "rb").read()
    magic = struct.unpack_from("<I", b, 0)[0]
    p, out = 8, set()
    while True:
        pid = struct.unpack_from("<I", b, p)[0]
        if pid == 0xFFFFFFFF:
            return out
        p += 4 + 20 + 4 + (8 if magic >= 0x06565528 else 0)
        d, p = kv(b, p, None)
        d = d.get(str(pid), d)
        if pid:
            out.update(d.get("appids", {}).values())


def main():
    a = sys.argv[1:]
    root = os.path.expanduser("~/.var/app/com.valvesoftware.Steam/.local/share/Steam")
    if "--root" in a:
        root = a[a.index("--root") + 1]
    untested = "--untested" in a
    apps = appinfo(os.path.join(root, "appcache/appinfo.vdf"))
    owned = licensed_apps(os.path.join(root, "appcache/packageinfo.vdf"))
    inst = {int(f[12:-4]) for f in os.listdir(os.path.join(root, "steamapps")) if f.startswith("appmanifest_")}
    games = json.load(open(os.path.join(HERE, "..", "site", "games.json")))
    norm = lambda s: re.sub(r"[^a-z0-9]", "", re.sub(r"\(.*?\)", "", s.lower().replace("the ", "")))
    tested_names = {norm(g["game"]) for g in games}
    tested_ids = {int(m) for g in games for m in re.findall(r"\b(\d{3,7})\b", g["game"] + str(g["path"]))}
    for aid in sorted(owned):
        c = apps.get(aid, {}).get("common", {})
        if str(c.get("type", "")).lower() != "game":
            continue
        t = aid in tested_ids or norm(c.get("name") or "") in tested_names
        if untested and t:
            continue
        deck = c.get("steam_deck_compatibility", {}).get("category", "")
        print(aid, "T" if t else "-", "I" if aid in inst else "-", deck, c.get("oslist", ""), c.get("name", ""), sep="\t")


if __name__ == "__main__":
    main()
