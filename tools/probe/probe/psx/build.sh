#!/usr/bin/env bash
# Builds the probe for the console: OUT/probe.elf, OUT/probe.exe (PS-X EXE)
# and OUT/probe.map, and checks the EXE (mkexe.py --check).
#
#   tools/probe/probe/psx/build.sh OUT [EXTRA CFLAGS...]
#
# Needs a MIPS cross GCC and binutils: CROSS (default
# mipsel-unknown-linux-gnu-) is their prefix, as the dev shell and
# `nix build .#probe` provide (pkgsCross.mipsel-linux-gnu, unwrapped: no
# C library, start files or libgcc are used).
set -euo pipefail

out=${1:?usage: build.sh OUT [CFLAGS...]}
shift
here=$(cd "$(dirname "$0")" && pwd)
probe=$here/..
common=$here/../../common
cross=${CROSS:-mipsel-unknown-linux-gnu-}
mkdir -p "$out"

cflags=(
  -std=c99 -march=mips1 -mabi=32 -EL -msoft-float -mno-abicalls -fno-pic -G0
  -ffreestanding -nostdlib -fno-builtin -fno-tree-loop-distribute-patterns
  -fno-stack-protector -fno-strict-aliasing -mno-check-zero-division
  -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
  -I"$probe" -I"$common"
  "$@"
)

objs=()
for src in "$probe/probe.c" "$common/jbp_frame.c" "$here/hal_psx.c" "$here/main.c" "$here/libc.c"; do
  o=$out/$(basename "${src%.c}").o
  "${cross}gcc" "${cflags[@]}" -c "$src" -o "$o"
  objs+=("$o")
done
"${cross}gcc" -march=mips1 -mabi=32 -EL -msoft-float -mno-abicalls -fno-pic -G0 \
  -c "$here/crt0.S" -o "$out/crt0.o"
"${cross}ld" -EL -nostdlib --no-warn-rwx-segments -T "$here/probe.ld" -Map "$out/probe.map" \
  -o "$out/probe.elf" "$out/crt0.o" "${objs[@]}"
python3 "$here/mkexe.py" "$out/probe.elf" "$out/probe.exe"
python3 "$here/mkexe.py" --check "$out/probe.elf" "$out/probe.exe"
# MIPS I only: no instruction a later ISA added (the R3000A has no
# interlock on loads, which GCC's -march=mips1 schedules around). The
# mnemonic is objdump's third field; one without operands ends the line.
# MIPS II: branch-likely, ll/sc, sync, traps, ldc/sdc; MIPS III and later:
# 64-bit loads, stores and arithmetic; MIPS32/IV: mul, madd, msub, clz, clo,
# movn, movz, pref; release 2: ehb, ei, di, seb, seh, wsbh, ins, ext, rotr.
later='(beql|bnel|blezl|bgtzl|bltzl|bgezl|bltzall|bgezall|ll|sc|sync|teq|tne|tge|tgeu|tlt|tltu|teqi|tnei|tgei|tgeiu|tlti|tltiu|ldc1|sdc1|ldc2|sdc2|ld|sd|lld|scd|ldl|ldr|sdl|sdr|lwu|dadd|daddi|daddiu|daddu|dsub|dsubu|dsll|dsrl|dsra|dsll32|dsrl32|dsra32|dsllv|dsrlv|dsrav|dmult|dmultu|ddiv|ddivu|mul|madd|maddu|msub|msubu|clz|clo|movn|movz|pref|ehb|ei|di|seb|seh|wsbh|ins|ext|rotr|rotrv)'
if "${cross}objdump" -d "$out/probe.elf" | grep -E "^ *[0-9a-f]+:"$'\t'"[0-9a-f]+ *"$'\t'"$later("$'\t'"|$)"; then
  echo "probe.elf: instructions the R3000A does not have"
  exit 1
fi
# The check itself must see instructions, or it proves nothing.
n=$("${cross}objdump" -d "$out/probe.elf" | grep -cE "^ *[0-9a-f]+:"$'\t'"[0-9a-f]+ *"$'\t'"[a-z]")
if [ "$n" -lt 1000 ]; then
  echo "probe.elf: only $n instructions found by the MIPS I check"
  exit 1
fi
echo "probe.elf: $n instructions, none later than MIPS I"
"${cross}size" "$out/probe.elf"
