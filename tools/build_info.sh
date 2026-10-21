#!/bin/sh
# One line for VRDUAL_BUILD_INFO (common/vrdual_version.h): version, build
# time (ISO-8601, SOURCE_DATE_EPOCH if set) and build machine. The version is
# `git describe` (tag v0.x.y, commits since, hash, -dirty); outside a git
# checkout (a release's source archive) the VERSION file, which `git archive`
# fills in (export-subst in .gitattributes): line 1 describe, line 2 the hash.
root=$(cd "$(dirname "$0")/.." && pwd)
# only our own checkout: a tarball unpacked inside another repo must not take its version
if [ "$(git -C "$root" rev-parse --show-toplevel 2>/dev/null)" = "$root" ]; then
    v=$(git -C "$root" describe --tags --dirty --always 2>/dev/null)
fi
if [ -z "$v" ] && [ -f "$root/VERSION" ]; then
    d=$(sed -n 1p "$root/VERSION") h=$(sed -n 2p "$root/VERSION")
    case $d in *'$Format'* | *'%('*) d= ;; esac  # not substituted, or a placeholder the archiver lacks
    case $h in *'$Format'*) h= ;; esac
    v=${d:-${h:+g$h}}
fi
[ -n "$v" ] || v=unknown
if [ -n "$SOURCE_DATE_EPOCH" ]; then t=$(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S %z'); else t=$(date '+%Y-%m-%d %H:%M:%S %z'); fi
printf 'vrdual %s, built %s on %s\n' "$v" "$t" "$(uname -n)"
