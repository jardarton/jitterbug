#!/usr/bin/env bash
# Builds the recorder and records every program in programs.txt
# (docs/testing.md, "Conformance tests and the recorder"):
#
#   record.sh PS1_TESTS_DIR PSN00BSDK_DIR WORK OUT REV=DIR...
#
# PS1_TESTS_DIR and PSN00BSDK_DIR are checkouts at the pinned commits
# (pins.sh), read only; each REV=DIR is a ps1-tests checkout at a commit
# programs.txt names (every one must be given). WORK is a scratch
# directory. OUT receives
#   OUT/tests/conformance/NAME.jbc                 the recorded lists
#   OUT/third_party/ps1-tests/gpu/NAME/vram.png    the console's dumps
#   OUT/third_party/ps1-tests/gpu/NAME/psx.log     the console's logs
#   OUT/third_party/ps1-tests/LICENSE              ps1-tests' licence (MIT)
#   OUT/third_party/psn00bsdk/LICENSE.md           PSn00bSDK's (MPL-2.0)
#   OUT/fixtures-identity.txt                      each program's identity
#                                                  as tests/conformance/
#                                                  fixtures.txt states it
#                                                  (below)
#   OUT/NOTICE.md                                  what all this is and
#                                                  under which terms
# (the dumps, logs and ps1-tests' licence from PS1_TESTS_DIR). Every
# program is recorded twice, and the two lists must be identical.
# fixtures-identity.txt has a line for each program, in programs.txt's
# order: NAME WORDS CHECKSUM REFERENCE REFSUM, the list's size in words and
# FNV-1a, dump or log, and the FNV-1a of that file's bytes (vram.png for a
# dump, psx.log for a log), the same fields as the program's line in
# tests/conformance/fixtures.txt (FORMAT.md, "Fixture tables"). Needs
# cmake, ninja and clang on PATH.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/pins.sh"
ps1=$1 sdk=$2 work=$3 out=$4
shift 4
declare -A at=()
for a in "$@"; do at[${a%%=*}]=${a#*=}; done

font=$sdk/libpsn00b/psxgpu/dbugfont.tim
[ -d "$ps1/gpu" ] && [ -f "$ps1/LICENSE" ] || { echo "not a ps1-tests checkout: $ps1" >&2; exit 1; }
[ -f "$font" ] && [ -f "$sdk/LICENSE.md" ] || { echo "not a PSn00bSDK checkout: $sdk" >&2; exit 1; }

# Which of ps1-tests' common GPU libraries a commit has, by the sha256 of
# its common/gpu.c or gpu.cpp and of its common/gpu.h: none, or the version
# (the commit that made it; docs/testing.md, "How `tools/conformance-record`
# works"). The recorder compiles the known versions unmodified; any other is
# refused.
common_version() { # CHECKOUT
    local f=$1/common/gpu.cpp v
    [ -f "$f" ] || f=$1/common/gpu.c
    if [ ! -f "$f" ]; then
        echo none
        return
    fi
    case $(sha256sum <"$f" | cut -c1-64):$(sha256sum <"$1/common/gpu.h" | cut -c1-64) in
    41241430013f0ff2c62cd1faa6f62a523dd9da90872f44cb0bc7f878434e22a1:848a9fadcc60e201537f2a7a6de8144f3247ba1bf25d93c016865349884021bd) v=79dfb13 ;;
    5f60dbd30a1c06b1f751bc9a7e74c99c5aa921e785f8c367667852ce2615265e:c4bb200c5dea5929afb09e8a0a30c13b048fb4842071060f258b7108272c7e41) v=9729d94 ;;
    e3bc3ada9d9b355ccbe4ebbe922a290215643cc7633d4fe417420ba2c2b4799c:42ea6df5c6b6a3d0baabefefde611b93c022c4a11b499130ea0e7d23f625369b) v=584a831 ;;
    555025bf298f92f05e5f008a96b2eb63317d2f5017844e41840a2b55d5148831:42ea6df5c6b6a3d0baabefefde611b93c022c4a11b499130ea0e7d23f625369b) v=93568c5 ;;
    e0d38b5aff1ab5d2e11aab71a6c899957e26ddf11cf04a0fd9c99d6ea2719064:49bb64f23adf85911e62040dd40417450e40737a756782877dd21dddd120b373) v=87ed1db ;;
    *)
        echo "$1/common: a version of ps1-tests' common library the recorder does not know" >&2
        return 1
        ;;
    esac
    # Its gpu.h and gpu.c or gpu.cpp, alone, as they are: their includes of
    # ps1-tests' other headers find the recorder's (include/).
    if [ ! -d "$work/src/common/$v" ]; then
        mkdir -p "$work/src/common/$v"
        cp "$f" "$1/common/gpu.h" "$work/src/common/$v/"
        chmod -R u+w "$work/src/common/$v"
    fi
    echo "$v"
}

checkout() { # REV
    local dir=${at[$1]:-}
    [ -n "$dir" ] || { echo "no checkout given for ps1-tests $1" >&2; return 1; }
    echo "$dir"
}

# The programs' sources, each from its commit, in one tree for CMake.
programs=()
rm -rf "$work/src/common"
mkdir -p "$work/src/gpu"
while read -r p rev words; do
    case $p in '' | '#'*) continue ;; esac
    for d in "$p:$rev" ${words:+"$p.words:$words"}; do
        name=${d%%:*} r=${d#*:}
        dir=$(checkout "$r")
        [ -d "$dir/gpu/$p" ] || { echo "$p: not in ps1-tests $r" >&2; exit 1; }
        rm -rf "$work/src/gpu/$name"
        cp -r "$dir/gpu/$p" "$work/src/gpu/$name"
        chmod -R u+w "$work/src/gpu/$name"
        common_version "$dir" >"$work/src/gpu/$name.common"
    done
    programs+=("$p:$rev:${words:-}")
done <"$here/programs.txt"

cmake -S "$here" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DPS1_TESTS_DIR="$work/src" \
    >"$work/build.log" 2>&1 && cmake --build "$work/build" >>"$work/build.log" 2>&1 ||
    { tail -n 40 "$work/build.log" >&2; exit 1; }

mkdir -p "$out/tests/conformance" "$out/third_party/ps1-tests/gpu" "$work/again" "$work/words"
: >"$out/fixtures-identity.txt"
for pr in "${programs[@]}"; do
    IFS=: read -r p rev words <<<"$pr"
    vars=(JBREC_NAME="gpu/$p" JBREC_FONT="$font"
        JBREC_SOURCE="ps1-tests $rev gpu/$p; PSn00bSDK $PSN00BSDK_REV")
    [ -f "$ps1/gpu/$p/psx.log" ] && vars+=(JBREC_PSXLOG="$ps1/gpu/$p/psx.log")
    for dest in "$out/tests/conformance" "$work/again"; do
        env -i PATH="$PATH" "${vars[@]}" JBREC_OUT="$dest/$p.jbc" \
            "$work/build/record-$p" >"$work/$p.out" 2>"$work/$p.err" ||
            { cat "$work/$p.err" >&2; exit 1; }
    done
    cmp "$out/tests/conformance/$p.jbc" "$work/again/$p.jbc"
    tail -n 1 "$work/$p.err"
    if [ -n "$words" ]; then
        # The dump-era source, for its words only: they must be the list's.
        env -i PATH="$PATH" JBREC_NAME="gpu/$p" JBREC_FONT="$font" JBREC_WORDS_ONLY=1 \
            JBREC_OUT="$work/words/$p.jbc" "$work/build/record-$p-words" \
            >"$work/$p.words.out" 2>"$work/$p.words.err" ||
            { cat "$work/$p.words.err" >&2; exit 1; }
        "$work/build/jbc-words" "$out/tests/conformance/$p.jbc" >"$work/words/$p.list.txt"
        "$work/build/jbc-words" "$work/words/$p.jbc" >"$work/words/$p.$words.txt"
        if ! cmp -s "$work/words/$p.list.txt" "$work/words/$p.$words.txt"; then
            echo "gpu/$p: ps1-tests $words sends other words than $rev:" >&2
            diff "$work/words/$p.list.txt" "$work/words/$p.$words.txt" | head -n 40 >&2
            exit 1
        fi
        echo "conformance-record: gpu/$p: ps1-tests $words sends the same" \
            "$(wc -l <"$work/words/$p.list.txt") GPU words as $rev"
    fi
    mkdir -p "$out/third_party/ps1-tests/gpu/$p"
    for f in vram.png psx.log; do
        if [ -f "$ps1/gpu/$p/$f" ]; then cp "$ps1/gpu/$p/$f" "$out/third_party/ps1-tests/gpu/$p/"; fi
    done
    # The identity: a dump if the program has one, else its log.
    if [ -f "$out/third_party/ps1-tests/gpu/$p/vram.png" ]; then
        ref=dump reffile=vram.png
    elif [ -f "$out/third_party/ps1-tests/gpu/$p/psx.log" ]; then
        ref=log reffile=psx.log
    else
        echo "gpu/$p: no vram.png and no psx.log in ps1-tests" >&2
        exit 1
    fi
    read -r bytes sum < <("$work/build/jbc-fnv" "$out/tests/conformance/$p.jbc")
    read -r _ refsum < <("$work/build/jbc-fnv" "$out/third_party/ps1-tests/gpu/$p/$reffile")
    echo "$p $((bytes / 4)) $sum $ref $refsum" >>"$out/fixtures-identity.txt"
done
cp "$ps1/LICENSE" "$out/third_party/ps1-tests/LICENSE"
mkdir -p "$out/third_party/psn00bsdk"
cp "$sdk/LICENSE.md" "$out/third_party/psn00bsdk/LICENSE.md"
sed -e "s/@PS1_TESTS_REV@/$PS1_TESTS_REV/g" -e "s/@PSN00BSDK_REV@/$PSN00BSDK_REV/g" \
    "$here/NOTICE.md.in" >"$out/NOTICE.md"
chmod -R u+w "$out"
echo "recorded ${#programs[@]} lists, each twice, byte-identical"
