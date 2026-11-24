#!/usr/bin/env bash
# Plays lists through the probe's real MIPS binary in PCSX-Redux, over the
# emulator's SIO1 (its TCP bridge in raw mode), and checks that what came
# back is the emulator's VRAM, all 16 bits (docs/probe.md, "What is proven").
# The emulator is a stand-in for a console: only the probe is under test.
#
#   run-pcsx-redux.sh PCSX_REDUX PROBE_DIR JBPROBE OUT
#                     (--fixtures TABLE LIST.jbc...)...
#
# PCSX_REDUX: the emulator's binary (it falls back to its own OpenBIOS: no
# console BIOS is used). PROBE_DIR: probe.exe and probe.elf
# (tools/probe/probe/psx/build.sh or `nix build .#probe`). JBPROBE: the host
# tool. OUT: a scratch directory: NAME.jbpr (the probe's result), vram-N.bin
# (the emulator's VRAM at the end of session N), NAME/ (jbprobe to-png),
# NAME.compare.txt (jbprobe compare: the emulator against jitterbug, for
# the record only). Each list is played as its line in the TABLE before it
# says (tests/conformance/fixtures.txt or tests/conformance/open/fixtures.txt:
# to pass PASSES, then the two-pass window), so every list needs one; the
# three fetched lists are in `nix build .#conformance-lists`'s
# tests/conformance.
set -euo pipefail

redux=$1 probe=$2 jbprobe=$3 out=$4
shift 4
mkdir -p "$out"
out=$(cd "$out" && pwd)
port=${JBP_PORT:-6699}
hal_show=$(nm "$probe/probe.elf" 2>/dev/null | awk '/ T hal_show$/ {print $1}')
[ -n "$hal_show" ] || hal_show=$(python3 - "$probe/probe.elf" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
shoff, = struct.unpack_from("<I", d, 32); shentsize, shnum = struct.unpack_from("<HH", d, 46)
secs = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
for s in secs:
    if s[1] == 2:
        strs = secs[s[6]]
        for k in range(s[5] // 16):
            nm, val = struct.unpack_from("<II", d, s[4] + 16 * k)
            n = d[strs[4] + nm:]; n = n[:n.index(b"\0")]
            if n == b"hal_show": print("%08x" % val)
PY
)
rm -f "$out"/vram-*.bin
# Portable mode in OUT: its own settings, the SIO1 server on in raw mode.
printf '{"emulator":{"Debug":{"SIO1Server":true,"SIO1ServerPort":%d,"SIO1Mode":1}}}\n' "$port" >"$out/pcsx.json"
here=$(cd "$(dirname "$0")" && pwd)
{ echo "JBP_HAL_SHOW = 0x$hal_show"; echo "JBP_VRAM_DIR = '$out'"; cat "$here/vram-at-session-end.lua"; } \
  >"$out/vram-at-session-end.lua"
(cd "$out" && exec "$redux" -no-ui -portable "$out" -noupdate -interpreter -debugger \
  -dofile "$out/vram-at-session-end.lua" -loadexe "$probe/probe.exe" -run </dev/null >"$out/emulator.log" 2>&1) &
emu=$!
trap 'kill $emu 2>/dev/null || true' EXIT
for _ in $(seq 50); do
  (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break
  sleep 0.2
done

n=0 bad=0 table=
while [ $# -gt 0 ]; do
  if [ "$1" = --fixtures ]; then
    table=$2
    shift 2
    continue
  fi
  list=$1
  shift
  [ -n "$table" ] || { echo "$list: no --fixtures TABLE before it"; exit 2; }
  name=$(basename "$list" .jbc)
  n=$((n + 1))
  echo "== $name"
  if ! "$jbprobe" send --quiet --tcp 127.0.0.1:$port --fixtures "$table" \
    --note "PCSX-Redux, OpenBIOS, interpreter" "$list" "$out/$name.jbpr"; then
    echo "$name: the session FAILED"
    bad=1
    continue
  fi
  "$jbprobe" info "$out/$name.jbpr" | grep -E '^(main loop|waits|status):' || true
  "$jbprobe" compare "$out/$name.jbpr" "$list" --fixtures "$table" >"$out/$name.compare.txt" || true
  grep -E '^(VRAM: |reads: [0-9]+, )' "$out/$name.compare.txt" | sed "s/^/$name, the emulator against jitterbug: /" || true
  mkdir -p "$out/$name"
  "$jbprobe" to-png "$out/$name.jbpr" "$out/$name" "$list" >/dev/null
  # The emulator writes its file when the probe calls hal_show: give it up
  # to a minute (a loaded machine has taken more than 10 s).
  for _ in $(seq 600); do [ -s "$out/vram-$n.bin" ] && break; sleep 0.1; done
  # The emulator's VRAM is 1 MB (or 2 with 8 MB of RAM); the first 1 MB is
  # the 1024 x 512 that the probe read back.
  if cmp -n 1048576 "$out/$name/vram.bin" "$out/vram-$n.bin"; then
    echo "$name: the probe's result is the emulator's VRAM, all 16 bits"
  else
    echo "$name: the probe's result DIFFERS from the emulator's VRAM"
    bad=1
  fi
done
exit $bad
