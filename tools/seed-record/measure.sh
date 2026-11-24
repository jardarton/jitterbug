#!/usr/bin/env bash
# Measures what a change to the renderer does to the seed scenes, before
# they are re-blessed (docs/testing.md, "Seed traces and re-blessing"): the
# VRAM at every capture of the sequence, at the recorded scales and at 3x,
# from the
# replayer of the build before the change and of the build after it.
#
#   tools/seed-record/measure.sh OLD_REPLAY NEW_REPLAY [SCALES]
#
# OLD_REPLAY and NEW_REPLAY are test_seed_replay binaries (copy the old
# one aside before rebuilding). SCALES (default "1 3") are the runs: 1 is
# the sequence as recorded (1x and 2x), 3, 4 or 8 the sequence with every
# scale above 1x replaced. Prints one DIFF line per VRAM image that
# differs: pixels, bounding box, the range of each channel's change in
# 5-bit steps, pixels changed by more than a step, mask bit changes. The
# dumps of both (oldS, newS) are kept in a new directory under WORK
# (default /tmp/jb-seed-measure), for a closer look.
#
# Golden mismatches (exit status 1) are what it measures and are allowed.
# Anything else fails the script with 1: a replayer that exits otherwise,
# tests/seed without exactly the scenes of scenes.txt (checked before
# anything runs), a run that did not replay all of them, two runs whose dumps are not
# the same set of images, or a comparison that missed any of them.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
jb=$(cd "$here/../.." && pwd)
# Absolute, but symlinks kept (a multi-call binary goes by its name).
abspath() { case $1 in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
old=$(abspath "${1:?usage: measure.sh OLD_REPLAY NEW_REPLAY [SCALES]}")
new=$(abspath "${2:?usage: measure.sh OLD_REPLAY NEW_REPLAY [SCALES]}")
scales=${3:-1 3}
scratch=${WORK:-/tmp/jb-seed-measure}
mkdir -p "$scratch"
scratch=$(CDPATH= cd -- "$scratch" && pwd)
work=$(mktemp -d "$scratch/measure.XXXXXX")
cd "$jb"

die() {
    echo "measure.sh: $*" >&2
    exit 1
}

# The 28 scenes of scenes.txt, all of them, before anything runs.
. "$here/scenes.sh"
seed_traces tests/seed || die "tests/seed does not hold the expected scenes"

# One replay: exit 0 or 1 (a mismatch), and the summary of every scene.
replay() {
    local log=$1 rc=0
    shift
    "$@" >"$log" 2>&1 || rc=$?
    [ "$rc" -le 1 ] || { tail -n 5 "$log" >&2; die "$1 exited with $rc (log: $log)"; }
    grep -q "^${#traces[@]} scenes in sequence" "$log" ||
        die "$1 did not replay the ${#traces[@]} scenes (log: $log)"
}

for s in $scales; do
    args=()
    [ "$s" = 1 ] || args=(--scale "$s" tests/seed/scaled)
    mkdir -p "$work/old$s" "$work/new$s"
    replay "$work/old$s.log" "$old" "${args[@]}" --dump "$work/old$s" --sequence "${traces[@]}"
    echo "== sequence${args[*]:+ at ${s}x}"
    replay "$work/new$s.log" "$new" "${args[@]}" --dump "$work/new$s" --compare "$work/old$s" \
        --sequence "${traces[@]}"
    n=$(find "$work/old$s" -name '*.raw' | wc -l)
    [ "$n" -gt 0 ] || die "no dumps from $old at ${s}x"
    diff <(cd "$work/old$s" && ls) <(cd "$work/new$s" && ls) >/dev/null ||
        die "the two runs at ${s}x dumped different sets of images"
    grep -q "compared with $work/old$s: $n VRAM images, [0-9]* differ, 0 missing" "$work/new$s.log" ||
        die "the comparison at ${s}x did not cover the $n images dumped"
    grep -E "DIFF|compared with" "$work/new$s.log" || true
done
echo "dumps in $work"
