#!/bin/sh
# The check's mutation self-test (docs/testing.md, "Self-tests and what
# `checks.fuzz-diff` runs"): builds
# fuzz-diff against copies of the library with one deliberate bug each
# (mutate.sh), runs strict and bounded mode on the flake check's seeds, and
# fails unless exactly the modes recorded below catch each bug. The one
# neither mode can catch (line-clip) must be caught by its direct test,
# tests/line_clip.c, built against that copy. Three fixed streams must fail
# on the build recorded and pass on every other one: two draws whose stray
# pixel the next CPU-to-VRAM load overwrites, and a 65 x 65 VRAM-to-CPU
# read (2,113 GPUREAD words). Every failure is then reproduced (the first
# failing stream of each mode, shrunk, `repro --out`; each fixed stream
# that must fail, `words --out`), and the saved list played (`play`) on the
# same build must fail too. On mask-through's build, play must also fail
# cases.sh's list of one GP0 op that holds the whole case (the protected
# pixel written by a Gouraud triangle and loaded back), on jitterbug's
# protected pixel alone. The mutations are built and run in parallel.
#   tools/fuzz-diff/selftest.sh PSXSTACK_DIR WORK [STRICT_STREAMS [BOUNDED_STREAMS]]
# FD_REPROS=K shrinks and prints up to K failures of each run (default 0);
# FD_CMAKE_ARGS adds configure options and FD_CFLAGS compiler options for
# line_clip (for a sanitized self-test: CC=clang, -DJB_SANITIZE=ON and
# -fsanitize=address,undefined).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
psx=$1
work=$2
ns=${3:-600}
nb=${4:-200}
fail=0
mkdir -p "$work"
# NAME:the modes that must catch it[:the fixed streams that must fail on it]
# (prelude-stray: every stream, since the prelude runs before each)
specs="line-clip: gouraud-tex:bounded uv-varies:strict,bounded avg-up:strict
outside-right:strict,bounded:right stray-above:strict:above near-drop:bounded
prelude-stray:strict,bounded:right,above,read read-last:strict:read mask-through:strict,bounded"
streams="right above read"

# The fixed streams (fuzz-diff words): the state (drawing area (100,100)-
# (199,199), offset 0, no mask settings), a draw clipped at the area's edge,
# then a 2 x 1 load over the pixel the mutation writes outside the area.
# right: a flat triangle (150,120) (250,120) (150,180), first row 120: the
# stray pixel (200,120). above: a flat 30 x 30 rectangle at (120,90): the
# stray pixel (120,99).
state="e1000000
e2000000
e3019064
e4031cc7
e5000000
e6000000"
printf '%s\n20ff0000 00780096 007800fa 00b40096\na0000000 007800c8 00010002 12341234\n' \
    "$state" > "$work/stream-right.txt"
printf '%s\n6000ff00 005a0078 001e001e\na0000000 00630078 00010002 12341234\n' \
    "$state" > "$work/stream-above.txt"
# read: 65 x 65 pixels from (0,0), all 2,113 words read and compared.
printf 'c0000000 00000000 00410041 r 2113\n' > "$work/stream-read.txt"

one() { # NAME: build the mutated copy and run everything on it
    m=$1
    dir=$work/mut-$m
    rm -rf "$dir"
    mkdir -p "$dir"
    (cd "$root" && cp -r CMakeLists.txt LICENSE examples include src tests third_party tools "$dir"/)
    sh "$root/tools/fuzz-diff/mutate.sh" "$dir" "$m"
    # shellcheck disable=SC2086
    cmake -S "$dir/tools/fuzz-diff" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPSXSTACK_DIR="$psx" ${FD_CMAKE_ARGS:-} > "$dir/configure.log"
    cmake --build "$dir/build" > "$dir/build.log"
    fd=$dir/build/fuzz-diff
    for mode in strict bounded; do
        n=$ns
        [ "$mode" = bounded ] && n=$nb
        rc=0
        "$fd" "$mode" --seed 1 --streams "$n" --repros "${FD_REPROS:-0}" --max-failures 1 \
            > "$dir/$mode.log" || rc=$?
        echo "$rc" > "$dir/$mode.rc"
        if [ "$rc" = 1 ]; then # reproduce, save, play
            i=$(sed -n 's/^FAIL: stream \([0-9]*\) .*/\1/p' "$dir/$mode.log" | head -n 1)
            mkdir -p "$dir/repro-$mode"
            rc=0
            "$fd" repro 1 "$i" --mode "$mode" --out "$dir/repro-$mode" > "$dir/repro-$mode.log" || rc=$?
            [ "$rc" = 1 ] || echo "repro exited $rc" >> "$dir/repro-$mode.log"
            rc=0
            "$fd" play "$dir/repro-$mode"/*.jbc > "$dir/play-$mode.log" || rc=$?
            echo "$rc" > "$dir/play-$mode.rc"
        fi
    done
    for s in $streams; do
        rc=0
        mkdir -p "$dir/repro-stream-$s"
        "$fd" words "$work/stream-$s.txt" --out "$dir/repro-stream-$s" > "$dir/stream-$s.log" || rc=$?
        echo "$rc" > "$dir/stream-$s.rc"
        if [ "$rc" = 1 ]; then
            rc=0
            "$fd" play "$dir/repro-stream-$s"/*.jbc > "$dir/play-stream-$s.log" || rc=$?
            echo "$rc" > "$dir/play-stream-$s.rc"
        fi
    done
    if [ "$m" = mask-through ]; then # play's stepped check, on one GP0 op of several commands
        rc=0
        sh "$root/tools/fuzz-diff/cases.sh" "$fd" "$dir/cases" mask-through > "$dir/cases.log" 2>&1 || rc=$?
        echo "$rc" > "$dir/cases.rc"
    fi
}

# Whether the list saved for a failure (play-NAME) fails when played, as
# it must; prints what the run found and what play found.
played() { # DIR NAME WHAT
    prc=$(cat "$1/play-$2.rc" 2>/dev/null || echo missing)
    if [ "$prc" != 1 ]; then
        echo "selftest.sh: $(basename "$1"): the list saved for $3 does not fail when played (exit $prc)" >&2
        cat "$1/repro-$2.log" "$1/play-$2.log" >&2 2>/dev/null || true
        fail=1
    fi
}

for spec in $specs; do
    m=${spec%%:*}
    (one "$m" > "$work/$m.log" 2>&1 || echo "build or run failed" >> "$work/$m.log") &
done
wait

printf '%-14s %-16s %-16s %-19s %s\n' mutation strict bounded "fixed streams" "must catch it"
for spec in $specs; do
    m=${spec%%:*}
    rest=${spec#*:}
    want=${rest%%:*}
    stream=
    case $rest in *:*) stream=${rest#*:} ;; esac
    dir=$work/mut-$m
    caught=
    line=
    for mode in strict bounded; do
        rc=$(cat "$dir/$mode.rc" 2>/dev/null || echo missing)
        case $rc in
        0) r="does not see it" ;;
        1) r="fails"
           caught="$caught,$mode" ;;
        *) echo "selftest.sh: fuzz-diff $mode exited $rc on $m" >&2
           cat "$work/$m.log" >&2
           exit 2 ;;
        esac
        line="$line$(printf '%-17s' "$r")"
        [ "$rc" = 1 ] && played "$dir" "$mode" "$mode mode's failure"
    done
    caught=${caught#,}
    fixed=
    for s in $streams; do
        rc=$(cat "$dir/stream-$s.rc" 2>/dev/null || echo missing)
        case ,$stream, in *,$s,*) must=1 ;; *) must=0 ;; esac
        if [ "$must" = 1 ]; then
            [ "$rc" = 1 ] && fixed="$fixed$s fails " ||
                { echo "selftest.sh: stream-$s does not fail on $m (exit $rc)" >&2; fail=1; }
            [ "$rc" = 1 ] && played "$dir" "stream-$s" "stream $s"
        elif [ "$rc" != 0 ]; then
            echo "selftest.sh: stream-$s fails on $m (exit $rc), which must pass it" >&2
            fail=1
        fi
    done
    printf '%-14s %s%-19s %s\n' "$m" "$line" "${fixed:-all pass}" "${want:-neither (tests/line_clip.c)}"
    if [ "$caught" != "$want" ]; then
        echo "selftest.sh: $m caught by '$caught', expected '$want'" >&2
        grep -h '^FAIL' "$dir"/*.log >&2 || true
        fail=1
    fi
    grep -h '^FAIL' "$dir"/strict.log "$dir"/bounded.log | sed "s/^/  $m: /" || true
    for f in "$dir"/play-*.log; do
        [ -f "$f" ] || continue
        k=$(basename "$f" .log)
        k=${k#play-}
        printf '  %s: %s reproduced, saved and played: %s\n' "$m" "$k" \
            "$(sed 's/^[^:]*: //' "$f" | head -n 1)"
    done
done
# mask-through's grouped list (cases.sh): play must fail it on jitterbug's
# protected pixel alone
if [ "$(cat "$work/mut-mask-through/cases.rc" 2>/dev/null || echo missing)" = 0 ]; then
    echo "mask-through: cases.sh's grouped Gouraud list played: $(sed 's/^[^:]*: //' "$work/mut-mask-through/cases.log" | head -n 1)"
else
    echo "selftest.sh: mask-through: play does not fail cases.sh's grouped Gouraud list as it must" >&2
    cat "$work/mut-mask-through/cases.log" >&2 2>/dev/null || true
    fail=1
fi
# line-clip's guard
# shellcheck disable=SC2086
cc -std=c99 ${FD_CFLAGS:-} -I"$root/include" "$root/tests/line_clip.c" \
    "$work/mut-line-clip/build/jitterbug/libjitterbug.a" -lpthread -lm -o "$work/line_clip-mutated"
if "$work/line_clip-mutated" > "$work/line_clip-mutated.log" 2>&1; then
    echo "selftest.sh: tests/line_clip.c passes on the line-clip build" >&2
    fail=1
else
    echo "line-clip: tests/line_clip.c fails on it ($(grep -c '^FAIL' "$work/line_clip-mutated.log") checks), as it must"
fi
exit $fail
