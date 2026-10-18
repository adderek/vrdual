#!/usr/bin/env python3
"""Check stereo parallax in an SBS snapshot of `vrdual-cube --scene calib`.

The calib scene has 5 posts at 0.5, 1, 2, 5, 20 m, hfov 90 (P00 = 1). For a
parallel-camera pair the horizontal disparity (left-eye x minus right-eye x)
of a post at distance d is  ipd(d) / d * eye_width / 2  pixels, where ipd(d)
follows the near/far blend from common/vrdual_ctl.hpp.

  measure_posts.py SNAPSHOT.png --ipd 0.064 [--ipd-near 0.2 --near 0.5 --far 5] [--anchor center]

--anchor also checks where the LEFT eye sees each post: the original camera
sees post k at x = (1 + (-0.5 + 0.25k)) * eye_width / 2; the left eye adds
0 (anchor left), disparity/2 (center) or the full disparity (anchor right).

Prints one line per post; exit 0 if every post is within tolerance, 1 if not,
2 on usage/parse errors. Needs ImageMagick (`magick`) to decode the PNG.
"""
import argparse
import subprocess
import sys

DIST = [0.5, 1.0, 2.0, 5.0, 20.0]


def runs(row, x0, x1, bg):
    """Centers of horizontal runs of non-background pixels in [x0, x1)."""
    out, start = [], None
    for x in range(x0, x1 + 1):
        on = x < x1 and sum(abs(a - b) for a, b in zip(row[x], bg)) > 40
        if on and start is None:
            start = x
        elif not on and start is not None:
            if x - start >= 2:
                out.append((start + x - 1) / 2 - x0)
            start = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--ipd", type=float, required=True)
    ap.add_argument("--ipd-near", type=float, default=0.0)
    ap.add_argument("--near", type=float, default=0.5)
    ap.add_argument("--far", type=float, default=5.0)
    ap.add_argument("--anchor", choices=["center", "left", "right"])
    ap.add_argument("--tol-rel", type=float, default=0.04)
    ap.add_argument("--tol-px", type=float, default=1.0)
    a = ap.parse_args()

    ident = subprocess.run(["magick", "identify", "-format", "%w %h", a.png], capture_output=True, text=True)
    if ident.returncode:
        print(ident.stderr, file=sys.stderr)
        return 2
    W, H = map(int, ident.stdout.split())
    raw = subprocess.run(["magick", a.png, "-depth", "8", "rgb:-"], capture_output=True).stdout
    y = int(H * 0.70)  # crosses every post, below the far post's top, above the HUD bar
    row = [tuple(raw[(y * W + x) * 3:(y * W + x) * 3 + 3]) for x in range(W)]
    bg = row[2]
    ew = W // 2
    L, R = runs(row, 0, ew, bg), runs(row, ew, W, bg)
    if len(L) != 5 or len(R) != 5:
        print(f"expected 5 posts per eye on row {y}, got L={len(L)} R={len(R)}", file=sys.stderr)
        return 2

    ok = True
    share = {"left": 0.0, "center": 0.5, "right": 1.0}
    for k, (d, l, r) in enumerate(zip(DIST, L, R)):
        ipd = a.ipd
        if a.ipd_near > 0 and a.far > a.near:
            t = min(max((a.far - d) / (a.far - a.near), 0.0), 1.0)
            ipd = a.ipd + (a.ipd_near - a.ipd) * t
        want = ipd / d * ew / 2
        got = l - r
        good = abs(got - want) <= max(a.tol_px, a.tol_rel * want)
        line = f"post {d:5.1f} m  disparity {got:7.1f} px  expected {want:7.1f} px"
        if a.anchor:
            base = (1 + (-0.5 + 0.25 * k)) * ew / 2
            lwant = base + share[a.anchor] * want
            lgood = abs(l - lwant) <= max(a.tol_px, a.tol_rel * want) + 0.5
            good &= lgood
            line += f"  left-eye x {l:6.1f} (expected {lwant:6.1f})"
        ok &= good
        print(line + ("  ok" if good else "  FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
