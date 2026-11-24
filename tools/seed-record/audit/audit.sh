#!/usr/bin/env bash
# The polygon-interpolation audit (docs/testing.md, "Polygon-interpolation audit";
# tests/seed/CHANGES.md, "Polygon colours and texture coordinates").
#
#   tools/seed-record/audit/audit.sh [TREE]
#
# TREE is a jitterbug source tree with the fix (default: a new git worktree
# of HEAD under WORK, removed afterwards). Two copies of it are made under
# WORK (default /tmp/jb-seed-audit): "new", with instrument.patch, and
# "old", with instrument.patch and old.patch (the 1x triangle
# interpolation before the gradient fix: unrounded gradients, the start
# 2^-20 above vertex 0). Each builds test_seed_replay. The 28 scenes are replayed
# in sequence at their recorded scales on one thread, dumping the 1x VRAM
# at every capture, three times: old, plain; new, with the history of
# every pixel that differs from the old run's dump (JB_AUDIT,
# JB_AUDIT_OTHER); old again, with the history of every pixel that
# differs from the new run's, and the same VRAM as the first old run.
# compare.c then checks every pixel that differs. Exit status: compare's
# (1 when a pixel is outside the envelope, 2 when its input is wrong), or
# 1 when a step fails. It needs a few GB in WORK for the dumps, which it
# removes unless KEEP=1 is set.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
scratch=${WORK:-/tmp/jb-seed-audit}
mkdir -p "$scratch"
scratch=$(CDPATH= cd -- "$scratch" && pwd)
work=$(mktemp -d "$scratch/audit.XXXXXX")
echo "working in $work"

worktree=
cleanup() {
    [ -n "${KEEP:-}" ] || rm -rf "$work"/old/dump "$work"/old/plain "$work"/new/dump
    if [ -n "$worktree" ]; then
        git -C "$repo" worktree remove --force "$worktree"
    fi
}
trap cleanup EXIT

if [ $# -ge 1 ]; then
    tree=$(cd "$1" && pwd)
else
    worktree=$work/head
    git -C "$repo" worktree add --detach "$worktree" HEAD >/dev/null
    tree=$worktree
fi
echo "auditing $tree"
. "$here/../scenes.sh"
seed_traces "$tree/tests/seed" || exit 1

for side in new old; do
    mkdir "$work/$side"
    (cd "$tree" && tar cf - --exclude=./build --exclude='./build-*' --exclude=./.git .) |
        (cd "$work/$side" && mkdir src-tree && cd src-tree && tar xf -)
    patch -s -p1 -d "$work/$side/src-tree" <"$here/instrument.patch"
    [ "$side" = new ] || patch -s -p1 -d "$work/$side/src-tree" <"$here/old.patch"
    cmake -S "$work/$side/src-tree" -B "$work/$side/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DJB_TESTS=ON -DJB_EXAMPLES=OFF -DJB_TOOLS=OFF -DJB_VULKAN=OFF >"$work/$side/cmake.log"
    cmake --build "$work/$side/build" --target test_seed_replay >"$work/$side/build.log"
done

# One replay: SIDE OUT [VAR=VALUE...]; the old build's mismatches with the
# goldens (exit status 1) are expected, the new one must match.
replay() {
    local side=$1 out=$2 rc=0
    shift 2
    mkdir "$work/$side/$out"
    (cd "$work/$side" && env "$@" build/tests/test_seed_replay --threads 1 --dump "$out" \
        --sequence "${traces[@]}" >"$out.log" 2>&1) || rc=$?
    if [ "$side" = new ] && [ "$rc" != 0 ]; then
        echo "the instrumented new build does not replay the traces"; tail "$work/new/$out.log"
        exit 1
    fi
    [ "$rc" -le 1 ] || { echo "the $side replay ($out) failed ($rc)"; tail "$work/$side/$out.log"; exit 1; }
    grep -q "^28 scenes in sequence" "$work/$side/$out.log" ||
        { echo "the $side replay ($out) did not finish"; exit 1; }
}
replay old plain
replay new dump JB_AUDIT=1 JB_AUDIT_OTHER="$work/old/plain"
replay old dump JB_AUDIT=1 JB_AUDIT_OTHER="$work/new/dump"
# The second old run drew what the first did (the maps are of that VRAM).
for f in "$work"/old/plain/*.1x.raw; do
    cmp -s "$f" "$work/old/dump/${f##*/}" || { echo "the old runs differ at ${f##*/}"; exit 1; }
done
rm -rf "$work/old/plain"

(cd "$work/new/dump" && ls -- *.audit | sed 's/\.audit$//') >"$work/captures.txt"
diff <(cd "$work/old/dump" && ls) <(cd "$work/new/dump" && ls) >/dev/null ||
    { echo "the two runs dumped different captures"; exit 1; }
# Every capture has its map (compare.c checks each map has exactly the
# pixels that differ).
diff <(sort "$work/captures.txt") \
    <(cd "$work/new/dump" && ls -- *.1x.raw | sed 's/\.1x\.raw$//' | sort) >/dev/null ||
    { echo "a capture without its audit map"; exit 1; }
echo "$(wc -l <"$work/captures.txt") captures"
cc -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -o "$work/compare" "$here/compare.c"
rc=0
"$work/compare" "$work/old/dump" "$work/new/dump" "$work/captures.txt" | tee "$work/report.txt" ||
    rc=$?
echo "report in $work/report.txt"
exit "$rc"
