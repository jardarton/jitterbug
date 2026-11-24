# ps1-tests

Real-console GPU and GTE test results taken from [ps1-tests](https://github.com/JaCzekanski/ps1-tests), a collection of PlayStation test programs by Jakub Czekański and contributors, under the MIT licence ([LICENSE](LICENSE), copied unchanged from upstream).

## GPU dumps and logs

- **Origin:** `ps1-tests`, commit `f727802fead11f1daa7549285548392ef87749cb` (2023-03-05).
- **Taken:** `gpu/<name>/vram.png` for eight of the eleven programs with a VRAM dump (`triangle`, `quad`, `lines`, `transparency`, `uv-interpolation`, `clipping`, `texture-flip`, `clut-cache`), and `gpu/<name>/psx.log` for `mask-bit` and `gp0-e1`, at the same relative paths as upstream.
- **Not taken:** the dumps and recorded lists of `rectangles`, `texture-overflow` and `vram-to-vram-overlap`. Those programs load third-party images rather than computing their own texels: `rectangles` and `texture-overflow` embed a 128 x 128 crop of a photograph (`lena.tim` / `lena.h`), `texture-overflow` also embeds `cube.tim` / `cube.h` (origin unrecorded), and `vram-to-vram-overlap` loads PSn00bSDK's debug font (MPL-2.0). No redistribution licence for the photograph or `cube.tim` could be established, and the font is under MPL-2.0, so none of those files are checked into this repository. `nix build .#conformance-lists` fetches and records all 13 programs for local testing with a `NOTICE.md` stating the terms, and every library test build in `nix flake check` (`clang`, `gcc`, `sanitize`, `shared`, `O0`, `wasm`, `x86-64-v3`, `arm64`, `arm64-clang`) runs them from there (`checks.package` tests the source release tarball offline without `JB_CONFORMANCE_FETCHED`; see [docs/testing.md](../../docs/testing.md) and [tests/conformance/FORMAT.md](../../tests/conformance/FORMAT.md)).
- **Not taken either:** upstream program sources and `ps1-tests`' common GPU library (`common/gpu.c` / `gpu.cpp`, `common/gpu.h`). `tools/conformance-record` fetches each program at the commit that last changed its dump or log (`tools/conformance-record/programs.txt`) — with one exception, `mask-bit`, which is recorded from commit `79dfb13` and verified word for word against its log-era commit `d190785` — compiles it unmodified against its own host shim, and writes out the recorded command list (`tests/conformance/*.jbc`). The checked-in lists contain only geometry, colours and textures/CLUTs computed by the programs themselves.
- **Local changes:** none. Every checked-in file is byte-identical to upstream, verified by the `conformance-record` flake check.
- **Format:** each `vram.png` is a 1024 x 512 opaque RGB PNG of VRAM with each 5-bit channel shifted left by 3 (no mask bit). Each `psx.log` is the program's text output from the console.

### Console identification

Upstream does not record the console's model, region or GPU revision. Indirectly, `timers/psx.log` in upstream prints `Detected NTSC system.`, and two GPU results match a v2 GPU as psx-spx describes the revisions: `gp0-e1`'s log shows `GP1(09h)` taking effect ("supported only on v2 GPUs") and `texture-flip`'s dump shows textured rectangle flipping working ("v2 GPU only"). Whether all dumps came from the same console is not recorded.

## GTE cases

Real-console GTE register results used by `tests/gte/conformance.c` (`gte_test_all`, which asserts that the file holds cases 1 to 1150, 50 for each of the 22 commands and 50 without one; `gte_test_all_partial` and `gte_test_all_no_case` verify that a truncated file and `--case 1151` are refused). Data only; no upstream source code is copied.

- **Origin:** `ps1-tests`, commit `f727802fead11f1daa7549285548392ef87749cb` (2023-03-05), MIT licence ([LICENSE](LICENSE), byte-identical to upstream).
- **Taken:** the register values from `gte/test-all/tests.c`, converted to `gte/test-all.txt` (whose `source` header records the SHA-256 of the upstream file). `gte-fuzz/gte_valid_0xc0ffee_50.log` holds the exact same 1150 cases in another order; the converter verifies that equivalence and only one file is kept.
- **Local changes:** none to any register value. The data is re-encoded, not edited.

### What upstream measured

`gte-fuzz` (`gte-fuzz/main.c` upstream) runs, for each of the 22 documented GTE commands and then for no command, 50 cases with its Mersenne Twister reseeded to `0xc0ffee` before each command. Each case writes all 64 registers in order (data registers 0–31 with `mtc2`, control registers 32–63 with `ctc2`). Each word is one of ten fixed patterns (`0`, `0xffffffff`, `0x7fffffff`, `0x80000000`, `0x7fff7fff`, `0x80008000`, `0x55555555`, `0xaaaaaaaa`, `0x1000`, `0xd15ea5ed`), a random word, or a random word's low or high half. It then executes the command once (taking `sf`, `lm` and `MVMVA`'s three fields from one further random word) and reads all 64 registers back (`mfc2`/`cfc2`). The COP2 immediate encodes the command number in bits 0–5, `lm` in bit 10, translation in bits 13–14, vector in bits 15–16, matrix in bits 17–18 and `sf` in bit 19.

`test-all` is upstream's table of those same cases with the 50 no-command cases moved first; before each case it clears all 64 registers to 0. Upstream's `psx.log` records a console passing all 1150 cases.

### Counts

Verified directly by `tools/gte-cases/convert.py`:

| Suite | Cases | With a command | Without a command | Distinct input sets |
|---|---|---|---|---|
| `test-all` | 1150 | 1100 (50 each of 22 commands) | 50 | 99 |
| `gte-fuzz` | 1150 | 1100 (50 each of 22 commands) | 50 | 99 |

The 22 commands (50 cases each): `RTPS`, `NCLIP`, `OP`, `DPCS`, `INTPL`, `MVMVA`, `NCDS`, `CDP`, `NCDT`, `NCCS`, `CC`, `NCS`, `NCT`, `SQR`, `DCPL`, `DPCT`, `AVSZ3`, `AVSZ4`, `RTPT`, `GPF`, `GPL`, `NCCT`.

Because the two suites hold the same 1150 `(inputs, command, outputs)` triples in different orders (enforced by the converter), the evidence is 1150 cases, not 2300. Because the generator reseeds before each command, case `k` of every command shares the same input set and command flags (`MVMVA`, for example, exercises 46 distinct command words out of 256). The no-command cases draw no flags word, so their inputs diverge after the first case, giving 50 + 49 = 99 distinct input sets.

### Conversion

`tools/gte-cases/regen.sh` reads both files from commit `f727802` of a `ps1-tests` clone via `git show` (or from `PS1_TESTS_DIR` in the `gte-cases` flake check) and runs `tools/gte-cases/convert.py`, which validates case counts, numbering, 64-register order and command opcodes, then re-parses its own output to verify a lossless round trip.

In `gte/test-all.txt`:

- `prelude clear` (or `prelude none`): whether all 64 registers are zeroed before each case.
- `in K w0 ... w63`: input set `K`, written to registers 0–63 in order (order matters because `SXYP` (15) shifts `SXY0`–`2`, `IRGB` (28) overwrites `IR1`–`3`, `LZCS` (30) updates `LZCR` (31), and `ORGB` (29) and `LZCR` ignore writes).
- `case N COMMAND K [reg=word ...]`: upstream case number `N`, 8-hex-digit command word (or `-` for none), input set `K`, and every register whose expected read-back differs from the word written to it (all unlisted registers read back unchanged).

`tools/gte-cases/regen.sh --check` converts again and compares with `gte/test-all.txt`; the `gte-cases` flake check runs it against the pinned `ps1-tests` input.

### What the cases leave untested

Because inputs are random, many results saturate: of the 100 `RTPS` and `RTPT` cases, 65 overflow the divide (`FLAG` bit 17). Only two `RTPT` cases detect dropping the divide's rounding term (`+0x8000`) or changing the reciprocal table by one, and no case — nor the hand-worked tests in `tests/gte/gte_test.c` — detects changing the rounding term by one.
