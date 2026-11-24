#!/bin/sh
# Fixed cases of the tool itself (docs/testing.md, "Self-tests and what
# `checks.fuzz-diff` runs"): lists `play`
# must judge right, and inputs it must refuse.
#   tools/fuzz-diff/cases.sh FUZZ_DIFF WORK            # on a clean build
#   tools/fuzz-diff/cases.sh FUZZ_DIFF WORK mask-through
# The second form, for selftest.sh's mask-through build, plays only the
# grouped Gouraud list, which must fail there on jitterbug's protected
# pixel alone (its VRAM at the end equal to psxstack's).
set -eu
fd=$1
work=$2
mut=${3:-}
fail=0
mkdir -p "$work"

# A list (tests/conformance/FORMAT.md) from hexadecimal words: the header
# (name "case", no source), then the ops given, then END.
le() { # WORD...: little-endian bytes
    for x in "$@"; do
        v=$((0x$x))
        # shellcheck disable=SC2059
        printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $((v & 255)) $((v >> 8 & 255)) \
            $((v >> 16 & 255)) $((v >> 24 & 255)))"
    done
}
list() { # FILE OPWORD...
    f=$1
    shift
    { le 4c43424a 00545349 00000001 0000001c 00000004 65736163 00000000; le "$@" 00000000; } > "$f"
}

# Load 8001 at (0,0), the mask check on, GP1(00h) (which turns it off on
# both sides), load 0002 there, the check off, load 0002 again.
list "$work/mask-reset.jbc" \
    01000004 a0000000 00000000 00010001 00008001 01000001 e6000002 03000001 00000000 \
    01000004 a0000000 00000000 00010001 00000002 01000001 e6000000 \
    01000004 a0000000 00000000 00010001 00000002
# Load 8001 at (0,0), the check on, then one GP0 op of three commands:
# load 0002 there (kept), the check off, load 0002 (written).
list "$work/mask-grouped.jbc" \
    01000004 a0000000 00000000 00010001 00008001 01000001 e6000002 \
    01000009 a0000000 00000000 00010001 00000002 e6000000 a0000000 00000000 00010001 00000002
# Commands cut across ops: under the check, a 2 x 2 load over the
# protected (0,0), a flat and a Gouraud polyline (horizontal and vertical
# segments, one colour), each to its terminator;
# then the check off and a load of 0003 over (0,0). A decoder that missed
# a command's end would leave the check on in its tracked state and see
# that last load change a protected pixel.
list "$work/mask-split.jbc" \
    01000004 a0000000 00000000 00010001 00008001 \
    01000004 e407ffff e6000002 a0000000 00000000 \
    01000002 00020002 00020002 \
    01000003 00020002 48ffffff 00100010 \
    01000002 00100020 00200020 \
    01000003 55555555 58808080 00100040 \
    01000003 00808080 00100060 00808080 \
    01000002 00300060 55555555 \
    01000005 e6000000 a0000000 00000000 00010001 00000003
# One GP0 op: the drawing area, load 8001 at (10,10), the check on, a
# Gouraud triangle over it, the check off, load 8001 there again. The
# triangle honours the check on both sides of a clean build; on
# mask-through's jitterbug it writes the protected pixel, which the last
# load puts back, so only the stepped check sees it.
list "$work/mask-gouraud.jbc" \
    01000012 e3000000 e407ffff a0000000 000a000a 00010001 00008001 e6000002 \
    300000ff 00080008 0000ff00 00080018 00ff0000 00180008 \
    e6000000 a0000000 000a000a 00010001 00008001

play() { # LIST EXPECT_RC PATTERN...: play exits EXPECT_RC, printing each PATTERN
    l=$1
    want=$2
    shift 2
    rc=0
    "$fd" play "$work/$l" > "$work/$l.log" 2>&1 || rc=$?
    if [ "$rc" != "$want" ]; then
        echo "cases.sh: play $l exited $rc, not $want" >&2
        cat "$work/$l.log" >&2
        fail=1
    fi
    for p in "$@"; do
        grep -qF -- "$p" "$work/$l.log" || {
            echo "cases.sh: play $l does not print '$p'" >&2
            cat "$work/$l.log" >&2
            fail=1
        }
    done
    sed "s|^$work/||" "$work/$l.log"
}

if [ "$mut" = mask-through ]; then
    play mask-gouraud.jbc 1 "0 colour, 0 mask pixels differ" \
        "protected pixels changed under the mask check: 1 by jitterbug, 0 by psxstack"
    exit $fail
fi

same="0 colour, 0 mask pixels differ"
none="protected pixels changed under the mask check: 0 by jitterbug, 0 by psxstack"
play mask-reset.jbc 0 "$same" "$none" "1 GP1(00h) after GP0 words"
play mask-grouped.jbc 0 "$same" "$none"
play mask-split.jbc 0 "$same" "$none"
play mask-gouraud.jbc 0 "$same" "$none"

# Inputs to refuse (exit 2), each with what it would have been read as.
refuse() { # WHAT ARG...
    what=$1
    shift
    rc=0
    "$fd" "$@" > "$work/refuse.log" 2>&1 || rc=$?
    if [ "$rc" != 2 ]; then
        echo "cases.sh: $what: fuzz-diff $* exited $rc, not 2" >&2
        cat "$work/refuse.log" >&2
        fail=1
    else
        echo "refused, $what: $(head -n 1 "$work/refuse.log")"
    fi
}
words() { # NAME LINE: a words file of one line
    printf '%s\n' "$2" > "$work/$1.txt"
    echo "$work/$1.txt"
}
refuse "a read count past 2^32 (was 0)" words "$(words wrap 'c0000000 00000000 00010001 r 4294967296')"
refuse "a read count past the largest transfer" words "$(words big 'c0000000 00000000 02000400 r 262145')"
refuse "a negative read count" words "$(words neg 'c0000000 00000000 00010001 r -1')"
refuse "a read count with more after it" words "$(words more 'c0000000 00000000 00010001 r 1 2')"
refuse "a word of 9 digits (was cut to 32 bits)" words "$(words nine '100000000')"
refuse "a word with a stray letter (was the end of the line)" words "$(words junk 'e6000000 zz 1')"
refuse "a negative --seed (was 2^64 - 1)" strict --seed -1 --streams 1
refuse "an octal-looking --seed" strict --seed 010 --streams 1
refuse "--streams 0" strict --streams 0
refuse "--streams with a suffix (was 5)" strict --streams 5x
refuse "--first past the largest index" strict --first 99999999999 --streams 1
refuse "--commands past int" run --commands 4294967297 --streams 1
refuse "a negative --repros" strict --repros -1 --streams 1
refuse "an unknown --focus (was ignored)" run --focus nothing --streams 1
refuse "an unknown --mode (was exploratory)" repro 1 0 --mode strictly
refuse "a repro INDEX with a suffix" repro 1 0x --mode strict
FD_SHOW=1000,0,100,1 refuse "FD_SHOW past VRAM's right edge" words "$(words show 'e6000000')"
FD_SHOW=1,2,3 refuse "FD_SHOW of three numbers (was ignored)" words "$(words show 'e6000000')"
# a zero byte in a line would hide what follows it from the parser
printf 'e6000000\000 zz\n' > "$work/zero.txt"
refuse "a zero byte in a line (was the end of the line)" words "$work/zero.txt"
# and what must still be read
for l in 'c0000000 00000000 00410041 r 2113' 'c0000000 00000000 02000400 r 262144' \
    '0xe6000000	e1000000 # comment' '# a comment line' 'e6000000# a comment after a word' \
    'c0000000 00000000 00010001 r 1# a comment after a count'; do
    printf '%s\n' "$l" > "$work/ok.txt"
    rc=0
    "$fd" words "$work/ok.txt" > "$work/ok.log" 2>&1 || rc=$?
    if [ "$rc" != 0 ]; then
        echo "cases.sh: words '$l' exited $rc, not 0" >&2
        cat "$work/ok.log" >&2
        fail=1
    fi
done
[ "$fail" = 0 ] && echo "cases.sh: all cases as they must be"
exit $fail
