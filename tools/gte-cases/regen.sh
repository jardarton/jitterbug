#!/usr/bin/env bash
# Regenerates third_party/ps1-tests/gte from ps1-tests' GTE suites
# (third_party/ps1-tests/README.md, "GTE cases").
#
#   tools/gte-cases/regen.sh           convert and install the case files
#                                      and LICENSE
#   tools/gte-cases/regen.sh --check   convert into a scratch directory and
#                                      compare with what is checked in
#
# Environment: PS1_TESTS_REPO (default
# ~/.cache/checkouts/github.com/JaCzekanski/ps1-tests), a clone of
# https://github.com/JaCzekanski/ps1-tests containing commit $REV. The
# files are taken from that commit (git show), whatever is checked out;
# the clone is not written to. Or PS1_TESTS_DIR, a plain tree of commit
# $REV (the flake's ps1-tests input; the flake check gte-cases runs
# --check so). Needs git (for a clone) and python3 (nix develop).
set -euo pipefail

REV=f727802fead11f1daa7549285548392ef87749cb
here=$(cd "$(dirname "$0")" && pwd)
jb=$(cd "$here/../.." && pwd)
src=${PS1_TESTS_REPO:-$HOME/.cache/checkouts/github.com/JaCzekanski/ps1-tests}
dst=$jb/third_party/ps1-tests
check=0
[ "${1:-}" = --check ] && check=1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/in/gte/test-all" "$work/in/gte-fuzz" "$work/out/gte"
for f in LICENSE gte/test-all/tests.c gte-fuzz/gte_valid_0xc0ffee_50.log; do
    if [ -n "${PS1_TESTS_DIR:-}" ]; then
        cp "$PS1_TESTS_DIR/$f" "$work/in/$f"
    else
        git -C "$src" show "$REV:$f" >"$work/in/$f"
    fi
done
python3 -I "$here/convert.py" "$work/in" "$work/out/gte"
cp "$work/in/LICENSE" "$work/out/LICENSE"

# gte-fuzz.txt is converted too, and convert.py fails unless it holds the
# same cases as test-all.txt; only one copy of them is checked in.
files="LICENSE gte/test-all.txt"
if [ $check = 1 ]; then
    status=0
    for f in $files; do
        if cmp -s "$work/out/$f" "$dst/$f"; then
            echo "$f: up to date"
        else
            echo "$f: differs from a fresh conversion"
            status=1
        fi
    done
    exit $status
fi
mkdir -p "$dst/gte"
for f in $files; do
    cp "$work/out/$f" "$dst/$f"
done
echo "installed $files in $dst"
