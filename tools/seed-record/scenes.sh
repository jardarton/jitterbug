# Sourced by measure.sh, rebless.sh and audit/audit.sh: the expected seed
# scenes (scenes.txt here, also read by tests/CMakeLists.txt).
#
#   seed_traces DIR   sets the array traces to DIR/NAME.jbt for each scene
#                     of scenes.txt, in order; returns 1 with a message when
#                     DIR's *.jbt are not exactly those (one missing, one
#                     more, one renamed).
seed_traces() {
    local dir=$1 list name have want
    list=$(dirname "${BASH_SOURCE[0]}")/scenes.txt
    want=$(grep -v '^#' "$list")
    have=$(cd "$dir" && for t in *.jbt; do [ -e "$t" ] && echo "${t%.jbt}"; done)
    if [ "$have" != "$want" ]; then
        echo "$dir: the traces are not the $(echo "$want" | wc -l) scenes of $list:" >&2
        diff <(echo "$want") <(echo "$have") | grep '^[<>]' | sed 's/^</  missing:/; s/^>/  not a scene:/' >&2
        return 1
    fi
    traces=()
    for name in $want; do
        traces+=("$dir/$name.jbt")
    done
}
