#!/usr/bin/env bash
# Re-blesses tests/seed from jitterbug as built (docs/testing.md, "Seed traces
# and re-blessing"): the hashes embedded in the traces, the golden PNGs and
# the other-scale references in tests/seed/scaled. Only for a deliberate
# fix to the renderer, recorded in tests/seed/CHANGES.md with its evidence
# and measured first (measure.sh). No test and no flake check runs it.
#
#   tools/seed-record/rebless.sh BUILD_DIR
#
# BUILD_DIR is a configured and built jitterbug tree (with JB_TESTS); its
# tests/test_seed_replay does the replaying. WORK (default
# /tmp/jb-seed-rebless) gets a new directory for each run.
#
# Everything is made in a copy of tests/seed in that directory: the
# re-blessed traces (ops, preludes and standalone flags kept), the end-state
# images and the other-scale references. The copy is then checked: every
# scene alone, and the sequence at the recorded scales and at 3x, 4x and
# 8x (which also checks the standalone flags). Only when all of that has
# passed is it installed: copied whole beside tests/seed and swapped in by
# rename (step 4), so on any failure, the install's included, tests/seed is
# left as it was.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
jb=$(cd "$here/../.." && pwd)
build=$(cd "${1:?usage: rebless.sh BUILD_DIR}" && pwd)
replay=$build/tests/test_seed_replay
scratch=${WORK:-/tmp/jb-seed-rebless}
mkdir -p "$scratch"
scratch=$(CDPATH= cd -- "$scratch" && pwd)
work=$(mktemp -d "$scratch/rebless.XXXXXX")
echo "working in $work"
# Mismatch images (*.png.out) land in the current directory: the work one.
cd "$work"

src=$jb/tests/seed
cand=$work/seed
[ -x "$replay" ] || { echo "no $replay"; exit 1; }
. "$here/scenes.sh"
seed_traces "$src" || exit 1
cp -r "$src" "$cand"
ctraces=()
for t in "${traces[@]}"; do
    ctraces+=("$cand/${t##*/}")
done

# 1. The sequence at the recorded scales, from the checked-in traces:
#    traces and end states, into the candidate.
mkdir "$work/bless"
"$replay" --bless "$work/bless" --sequence "${traces[@]}" >"$work/bless.log"
tail -n 13 "$work/bless.log"
cp "$work"/bless/*.jbt "$cand"/
python3 -I "$here/vram2png.py" --check "$cand" "$work"/bless/*.raw

# 2. The other scales (tests/seed/FORMAT.md, "Other scales"), with images
#    for the scenes that have them now, from the candidate traces.
for s in 3 4 8; do
    png=()
    for scene in $(awk '$1 == "golden" { print $2 }' "$src/scaled/${s}x.txt"); do
        png+=(--png "$scene")
    done
    "$replay" --record-scale "$s" "$cand/scaled" "${png[@]}" --sequence "${ctraces[@]}" \
        >"$work/scale$s.log"
    grep "reference lines" "$work/scale$s.log"
done

# 3. The checks, on the candidate: every scene alone, and the sequence at
#    each scale.
fail=0
for t in "${ctraces[@]}"; do
    "$replay" "$t" >"$work/alone.log" 2>&1 || { echo "FAIL alone: $t"; cat "$work/alone.log"; fail=1; }
done
"$replay" --sequence "${ctraces[@]}" >"$work/check.log" || { echo "FAIL: sequence"; fail=1; }
for s in 3 4 8; do
    "$replay" --scale "$s" "$cand/scaled" --sequence "${ctraces[@]}" >"$work/check$s.log" ||
        { echo "FAIL: sequence at $s"; fail=1; }
done
[ "$fail" = 0 ] || { echo "not installed: tests/seed is unchanged; the candidate is in $cand"; exit 1; }

# 4. Install, all or nothing: the candidate is copied whole beside
#    tests/seed (the same filesystem), checked equal to it, and then
#    swapped in by two renames; tests/seed is never partly written. If
#    anything fails before the swap, tests/seed is untouched; if the second
#    rename fails, the first is undone.
parent=$(dirname "$src")
stage=$(mktemp -d "$parent/.seed-new.XXXXXX")
prev=$parent/.seed-old.${stage##*.}
undo() {
    echo "not installed: $*; tests/seed is unchanged; the candidate is in $cand"
    rm -rf "$stage"
    exit 1
}
cp -r "$cand"/. "$stage"/ || undo "copying the candidate beside tests/seed failed"
diff -r "$cand" "$stage" >/dev/null || undo "the copy beside tests/seed differs from the candidate"
chmod --reference="$src" "$stage" || undo "chmod failed"
[ ! -e "$prev" ] || undo "$prev exists"
mv -T "$src" "$prev" || undo "moving tests/seed aside failed"
if ! mv -T "$stage" "$src"; then
    mv -T "$prev" "$src" || { echo "tests/seed is at $prev: move it back by hand"; exit 1; }
    undo "moving the candidate into place failed"
fi
rm -rf "$prev"
echo "re-blessed $src; git diff --stat tests/seed shows what moved"
