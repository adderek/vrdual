#!/usr/bin/env bash
# Maintainer only (not part of `vrdual`): release source tarball, optionally
# published as a GitHub release. GitHub's own "Source code" archives lack the
# Vulkan-Headers submodule and do not build; this one has it, and VERSION is
# filled in by git archive (export-subst in .gitattributes).
#
#   tools/release.sh [REF]             build/release/vrdual-<v>.tar.gz + SHA256SUMS
#   tools/release.sh --publish TAG     the same for TAG (pushed), then
#                                      gh release create (0.x: --prerelease),
#                                      notes = TAG's section of CHANGELOG.md
#
# Refuses uncommitted changes. stdout = the files; prose on stderr.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
say() { printf '%s\n' "$*" >&2; }
publish=0
[[ ${1:-} == --publish ]] && { publish=1; shift; }
ref=${1:-HEAD}
cd "$root"
[[ -z $(git status --porcelain --ignore-submodules=none) ]] || { say "error: uncommitted changes; commit first"; exit 1; }
v=$(git describe --tags --always "$ref")
if ((publish)); then
    git describe --tags --exact-match "$ref" >/dev/null 2>&1 || { say "error: --publish needs a tag (git tag -a v0.x.y), got $ref"; exit 1; }
    git ls-remote --exit-code --tags origin "refs/tags/$v" >/dev/null || { say "error: tag $v is not on origin (git push origin $v)"; exit 1; }
fi
name=vrdual-${v#v}
out=$root/build/release
mkdir -p "$out"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
git archive --format=tar --prefix="$name/" "$ref" >"$t/src.tar"
while read -r _ _ sha p; do
    [[ -e $p/.git ]] || { say "error: submodule $p not checked out (git submodule update --init)"; exit 1; }
    git -C "$p" archive --format=tar --prefix="$name/$p/" "$sha" >"$t/sub.tar"
    tar --concatenate -f "$t/src.tar" "$t/sub.tar"
done < <(git ls-tree -r "$ref" | awk '$2 == "commit"')
gzip -n -9 <"$t/src.tar" >"$out/$name.tar.gz"
(cd "$out" && sha256sum "$name.tar.gz" >SHA256SUMS)
say "release tarball for $v: sources + submodules, VERSION filled in"
printf '%s\n' "$out/$name.tar.gz" "$out/SHA256SUMS"
((publish)) || exit 0

# notes: the "## <version>" section of CHANGELOG.md (up to the next "## ")
awk -v h="## ${v#v}" 'index($0, h) == 1 {on = 1; next} on && /^## / {exit} on' CHANGELOG.md >"$t/notes.md"
[[ -s $t/notes.md ]] || { say "error: no '## ${v#v}' section in CHANGELOG.md"; exit 1; }
pre=()
[[ $v == v0.* ]] && pre=(--prerelease)
gh release create "$v" "${pre[@]}" --verify-tag --title "vrdual ${v#v}" --notes-file "$t/notes.md" "$out/$name.tar.gz" "$out/SHA256SUMS" >&2
say "published $v"
