#!/usr/bin/env bash
# Regenerates the checked-in conformance lists (tests/conformance/*.jbc) and
# third_party/ps1-tests' dumps and logs (docs/testing.md, "Conformance tests
# and the recorder").
#
#   tools/conformance-record/regen.sh           from the flake's pinned inputs
#                                               (nix build .#conformance-lists;
#                                               the flake sees git-tracked
#                                               files only, so git add changes
#                                               to the recorder first)
#   tools/conformance-record/regen.sh --local   from local clones, in the dev
#                                               shell, untracked changes too
#
# Only the programs tests/conformance/fixtures.txt marks "repo" are
# installed; the lists and dumps of the "fetched" ones are never copied
# into the repository (third_party/ps1-tests/README.md). The recording,
# all 13, is left in WORK (its path is printed), and is what
# JB_CONFORMANCE_FETCHED can name.
#
# --local reads PS1_TESTS_REPO (default ~/.cache/checkouts/github.com/
# JaCzekanski/ps1-tests) and PSN00BSDK_REPO (default ~/.cache/checkouts/
# github.com/Lameguy64/PSn00bSDK), git clones containing the pinned commits
# (pins.sh, programs.txt); they are only read, through git archive. WORK
# (default /tmp/jb-conformance-record) gets a new directory each run.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
jb=$(cd "$here/../.." && pwd)
. "$here/pins.sh"
scratch=${WORK:-/tmp/jb-conformance-record}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/regen.XXXXXX")
echo "working in $work"

if [ "${1:-}" = --local ]; then
    ps1_repo=${PS1_TESTS_REPO:-$HOME/.cache/checkouts/github.com/JaCzekanski/ps1-tests}
    sdk_repo=${PSN00BSDK_REPO:-$HOME/.cache/checkouts/github.com/Lameguy64/PSn00bSDK}
    export_rev() { # REPO REV DIR
        mkdir -p "$3"
        git -C "$1" archive "$2" | tar -x -C "$3"
    }
    export_rev "$ps1_repo" "$PS1_TESTS_REV" "$work/ps1-tests"
    export_rev "$sdk_repo" "$PSN00BSDK_REV" "$work/psn00bsdk"
    extra=()
    for rev in $(awk '!/^#/ && NF >= 2 { print $2; if (NF == 3) print $3 }' "$here/programs.txt" | sort -u); do
        export_rev "$ps1_repo" "$rev" "$work/ps1-tests-$rev"
        extra+=("$rev=$work/ps1-tests-$rev")
    done
    (cd "$jb" && nix develop -c bash "$here/record.sh" "$work/ps1-tests" "$work/psn00bsdk" \
        "$work/scratch" "$work/out" "${extra[@]}")
    out=$work/out
else
    (cd "$jb" && nix build .#conformance-lists -L --out-link "$work/result")
    out=$work/result
fi

repo=$(awk '!/^#/ && NF >= 7 && $2 == "repo" { print $1 }' "$jb/tests/conformance/fixtures.txt")
rm -f "$jb"/tests/conformance/*.jbc
rm -rf "$jb/third_party/ps1-tests/gpu"
n=0
for p in $repo; do
    cp "$out/tests/conformance/$p.jbc" "$jb/tests/conformance/"
    mkdir -p "$jb/third_party/ps1-tests/gpu/$p"
    cp "$out/third_party/ps1-tests/gpu/$p"/* "$jb/third_party/ps1-tests/gpu/$p/"
    n=$((n + 1))
done
cp "$out/third_party/ps1-tests/LICENSE" "$jb/third_party/ps1-tests/LICENSE"
chmod -R u+w "$jb/tests/conformance" "$jb/third_party/ps1-tests"
echo "installed $n lists in tests/conformance and their dumps and logs in" \
    "third_party/ps1-tests; the recording of all of them: $out"
echo "if a list, dump or log changed, update its WORDS, CHECKSUM or REFSUM in" \
    "tests/conformance/fixtures.txt from $out/fixtures-identity.txt"
