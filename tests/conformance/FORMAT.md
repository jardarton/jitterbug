# Conformance list format

A conformance list (`NAME.jbc`) records everything a GPU test program sends to the PlayStation GPU, in order: `GP0` and `GP1` words, `GPUREAD` and `GPUSTAT` reads, waits, main-loop boundaries and expected read-back checks. Lists for `ps1-tests` are recorded by `tools/conformance-record` (see [docs/testing.md](../../docs/testing.md)) and replayed by `tests/conformance/conformance.c` against jitterbug's public API. The hardware probe (`tools/probe/`, [docs/probe.md](../../docs/probe.md)) plays the exact same `.jbc` files on a console, so the format captures what the console program did rather than anything specific to jitterbug.

Constants live in `tests/conformance/jbc_format.h`; the reference reader and player are in `tests/conformance/jbc.c`.

Ten of the 13 `ps1-tests` lists are checked into `tests/conformance/`. The lists and dumps of `rectangles`, `texture-overflow` and `vram-to-vram-overlap` carry third-party images and are never checked in ([third_party/ps1-tests/README.md](../../third_party/ps1-tests/README.md)); they are produced by `nix build .#conformance-lists` and enabled in CMake via `-DJB_CONFORMANCE_FETCHED=<path>` (set automatically by `nix flake check`).

## Conventions

The file consists solely of little-endian 32-bit unsigned words (file byte length must be a multiple of 4).

A *string* is encoded as one 32-bit length word `L` (byte count, without a terminating NUL), followed by `ceil(L / 4)` 32-bit words packing byte `k` in bits `8 * (k % 4)` of word `k / 4`, with trailing pad bytes set to zero. A string therefore occupies `1 + (L + 3) / 4` words.

## File layout

### Header

| Word | Field | Notes |
|---|---|---|
| `0`–`1` | `magic` | Exact bytes `"JBCLIST\0"` (`0x4c43424a`, `0x00545349`) |
| `2` | `version` | `1` |
| `3` | `header_bytes` | Byte offset of the first op (must be a multiple of 4 and equal `4 * first_op_word`) |
| `4..` | `name` | String: program identifier, `gpu/NAME` for `ps1-tests` or `fuzz/NAME` for `tests/conformance/open/` |
| `...` | `source` | String: provenance (upstream commits recorded from) |

### Operations

Starting at word `header_bytes / 4`, operations continue until an `END` op that must be the final word of the file.

Each op starts with a 32-bit header word:
- bits `24..31`: `code` (`0x00`..`0x0a`)
- bits `0..23`: `n`, the number of payload 32-bit words immediately following (`0`..`0xffffff`)

A reader can advance past any op by skipping `1 + n` words.

| Code | Op | Payload (`n` words) | Meaning |
|---|---|---|---|
| `0x00` | `END` | none (`n = 0`) | End of list; must be the last word in the file |
| `0x01` | `GP0` | `words[n]` (`n >= 1`) | Words written to `GP0` by the CPU, in order |
| `0x02` | `GP0_DMA` | `words[n]` (`n >= 1`) | Words transferred to `GP0` by DMA channel 2 (block transfer or linked list); the GPU sees the same word stream as `GP0` |
| `0x03` | `GP1` | `words[n]` (`n >= 1`) | Words written to `GP1`, in order |
| `0x04` | `READ` | `port`, `count` (`n = 2`) | `count` (`>= 1`) reads of `port`: `0` = `GPUREAD`, `1` = `GPUSTAT`. Every word read is numbered sequentially from `0` across the list |
| `0x05` | `WAIT` | `mask`, `value` (`n = 2`) | Poll `GPUSTAT` until `(GPUSTAT & mask) == value` |
| `0x06` | `DRAWSYNC` | none (`n = 0`) | SDK `DrawSync(0)`: if `GPUSTAT` bits `29..30` (DMA direction) are `0`, wait for bit `28` (`0x10000000`); otherwise wait for DMA channel 2 to finish and then bits `26` and `28` (`0x14000000`) |
| `0x07` | `VSYNC` | none (`n = 0`) | Wait for a vertical blank |
| `0x08` | `LOOP` | none (`n = 0`) | Start of the program's infinite main loop; ops between `LOOP` and `END` repeat |
| `0x09` | `EXPECT` | `read`, `mask`, `value`, `label` (`n >= 4`) | Console assertion: `(reads[read] & mask) == value`, with string `label` occupying the remaining `n - 3` words |
| `0x0a` | `CALL` | `label` (`n >= 1`) | Annotation naming the call (`DrawPrim`, `fillRect`, `GP0 port`, etc.) that emitted subsequent ops up to the next `CALL`, `VSYNC` or `LOOP` |

### Structural validation rules

`jbc.c` refuses any file violating these rules:

- File byte length must be a multiple of 4, with at least 6 words, valid magic, version `1`, and `header_bytes` matching the end of the two header strings.
- `GP0`, `GP0_DMA` and `GP1` require `n >= 1`. (The recorder merges adjacent runs of the same op when no `CALL` intervenes.)
- `READ` requires `n == 2`, `port <= 1`, `count >= 1`, and may not appear after `LOOP`.
- `WAIT` requires `n == 2`; `DRAWSYNC`, `VSYNC`, `LOOP` and `END` require `n == 0`.
- At most one `LOOP` may appear.
- `EXPECT` requires `n >= 4`, `read < total_reads_so_far`, a valid string filling the remaining `n - 3` words, and may not appear after `LOOP`.
- `CALL` requires a valid string filling all `n` words, and has no effect on execution.
- `END` must appear as the exact last word of the file.
- **Packet boundary rule:** `LOOP` and `END` must fall between `GP0` packets (see [Playing on a console](#playing-on-a-console)).

## Playing a list

Ops are executed in order:

- `GP0` and `GP0_DMA` feed the same `GP0` word stream and may be split across `jb_gp0` calls arbitrarily or merged across adjacent `GP0`/`GP0_DMA`/`CALL` ops. Any other op acts as a barrier that all preceding words must reach first.
- On a console, `WAIT` and `DRAWSYNC` poll `GPUSTAT`. Because jitterbug has no timing, its player checks the condition once; a false condition is recorded as a failure (the program would have hung).
- `VSYNC` is a no-op in jitterbug's player.

### Main loop stability rule

For a list with a `LOOP` op, everything before `LOOP` is **pass 1** (in recorded `ps1-tests` lists this ends at the program's first vertical blank, including the loop's first iteration, though the format itself ends pass 1 at `LOOP` regardless of `VSYNC`). Each subsequent execution of the ops from `LOOP` to `END` is **pass 2, 3, ...**. A list without a `LOOP` runs straight to `END` with `P = 0` and no stability window.

For a list with a `LOOP`, each fixture in `fixtures.txt` states a target pass `P >= 1`. The player executes through the end of pass `P`, snapshots VRAM and all publicly readable GPU state, and then executes up to `W = 2` further passes (the stability window), stopping early if a window pass changes VRAM (a change in readable state alone does not stop the window early). The run fails unless:

1. All `W` window passes leave VRAM bit-identical to pass `P` (passes `P + 1` and `P + 2` for `W = 2`).
2. Readable GPU state after each window pass is identical to pass `P`.
3. When `P > 1`, pass `P` changed VRAM compared to pass `P - 1` (showing that VRAM had not already settled at `P - 1`, though not checking whether that VRAM state occurred on an earlier pass before `P - 1`).

Readable GPU state consists of `jb_gpustat` plus `GP1(10h)` indices `2`–`5` read via `jb_gp1` and `jb_gpuread` (texture window, drawing-area top-left, drawing-area bottom-right and drawing offset). `GP1(10h)` is queried only when `GPUSTAT` bit 27 (VRAM-to-CPU data ready) is clear; if a pass ends with transfer data waiting, reading would disturb the replay and the window fails.

This is a bounded three-pass check (`P`, `P + 1`, `P + 2`). It does not establish anything about passes after `P + 2` and cannot observe state unreadable through the public API: the CLUT cache contents, `GP1(09h)`'s gate bit (though its effect on `GPUSTAT` bit 15 is checked), `GP0(E1h)` bits 12–13 (rectangle flips), `GP1(05h)`–`(07h)` display geometry, latched `GPUREAD` values or console-only hardware state (FIFO, texture cache, timing). For example, `tests/conformance/selftest.c` demonstrates a synthetic CLUT-cache rotation loop that keeps VRAM and readable state unchanged across passes 1–3 and only changes pixels in pass 5.

Separately, comparing a `ps1-tests` dump assumes the console dump was captured once the program reached pass `P` (for `rectangles`, VRAM changes in passes 2–6 and stabilises at pass 6, which matches the console dump).

## Playing on a console

`tools/probe/` plays `.jbc` files directly on console hardware (see [docs/probe.md](../../docs/probe.md) and [tools/probe/PROTOCOL.md](../../tools/probe/PROTOCOL.md)):

- `GP0` writes words via CPU; `GP0_DMA` transfers via DMA channel 2; `GP1` writes to `GP1`; waits and vertical blanks poll with timeouts; `EXPECT` ops evaluate on the console; when the list has a `LOOP`, the main loop runs through pass `P` and up to `W` (default `2`) window passes (stopping at the first window pass whose VRAM differs), checked via full VRAM read-back CRC-32s after pass `P - 1` (if `P > 1`), pass `P`, and each window pass played (VRAM only; the readable-state check runs in the host runner).
- Before playing, the probe resets the GPU (`GP1(00h)`, `GP1(01h)`, `GP1(09h) = 0`) and clears VRAM to zero by default, matching a fresh `jb_context`.

### Packet boundaries at LOOP and END

Because the probe skips checkpoints before pass `P - 1`, it reads VRAM back with `GP0(C0h)` at `LOOP` (end of pass 1) when `P` is `1` or `2`, and at `END` after checkpointed passes and at list end; an unfinished `GP0` packet at either boundary would consume `GP0(C0h)` as packet parameters on a console (and split a packet across loop iterations). `jbc.c` tracks packet lengths according to psx-spx (`20h`–`3Fh` polygons, `40h`–`5Fh` lines and polylines terminated by `(w & 0xF000F000) == 0x50005000` at vertex 2+, `60h`–`7Fh` rectangles, `80h`–`9Fh` VRAM copies, `A0h`–`BFh` CPU-to-VRAM loads including pixel data words `((w * h) + 1) / 2`, `C0h`–`DFh` VRAM-to-CPU, `02h` fill; `GP1(00h)`/`GP1(01h)` and their 6-bit mirrors `40h`/`41h`/`80h`/`81h`/`C0h`/`C1h` drop any in-progress packet) and refuses any list whose `LOOP` or `END` falls inside a packet. Note: `GP0(03h)` is counted as a 1-word command (psx-spx marks its effect unknown).

### Using a probe result as a reference

When the runner is given `--probe RESULT.jbpr --platform console|simulation` or `--probe-fixtures TABLE`:

- It compares all 16 bits of VRAM (including mask bit 15; mask-only differences are drawn yellow in `NAME.diff.png`), every word read by `READ` (masking off `GPUSTAT` timing bits 25–28 and 31), and every `EXPECT` outcome and masked value against the console's.
- It refuses (exit 2) any result that is incomplete, corrupted, from the wrong list (name, byte length or CRC-32), played to the wrong pass or window, from the wrong platform, or not *clean* (`jbp_result_clean`: any `WAIT`/`DRAWSYNC`, DMA or vertical-blank timeout, or checks not matching the list's `EXPECT` ops in order).
- If the console itself failed one of the list's `EXPECT` checks, the runner reports `THE CONSOLE FAILED check N` and fails (exit 1) even if jitterbug matches the console, so discrepancies between consoles are never silently accepted.

## Console dumps and logs

- `third_party/ps1-tests/gpu/NAME/vram.png`: `1024 x 512` opaque RGB PNG where each 5-bit channel is shifted left by 3 and mask bit 15 is absent. Comparisons against dumps check only the 15 colour bits.
- `third_party/ps1-tests/gpu/NAME/psx.log`: console text log for `mask-bit` and `gp0-e1`. Their expected values are baked into the lists' `EXPECT` ops by the recorder.

## Fixture tables

Fixture tables are the sole authority for what a conformance test plays and compares. Lines starting with `#` and blank lines are ignored; fields are separated by spaces.

### Program tables (`fixtures.txt` and `open/fixtures.txt`)

```text
NAME WHERE REFERENCE PASSES EXPECTS WORDS CHECKSUM [REFSUM [EXCLUDE PIXELS EXCLSUM]]
```

| Field | Format / Values | Meaning |
|---|---|---|
| `NAME` | `[a-z0-9-]+` | Fixture key (`gpu/NAME` for `repo`/`fetched`, `fuzz/NAME` for `open`) |
| `WHERE` | `repo`, `fetched`, or `open` | Where the `.jbc` and reference live |
| `REFERENCE` | `dump`, `log`, or `probe` | Reference kind (`probe` has 7 fields total; `dump`/`log` require `REFSUM`) |
| `PASSES` | decimal integer | Target main-loop pass `P` (`0` if no `LOOP`) |
| `EXPECTS` | decimal integer | Exact count of `EXPECT` ops |
| `WORDS` | decimal integer | Exact list length in 32-bit words |
| `CHECKSUM` | 1–16 lowercase hex digits | 64-bit FNV-1a of the `.jbc` file's bytes |
| `REFSUM` | 16 lowercase hex digits | Required for `dump`/`log`: 64-bit FNV-1a of `vram.png` or `psx.log` |
| `EXCLUDE` | filename (no leading `.`) | Optional (`dump` only): exclusion file beside the table |
| `PIXELS` | decimal integer | Exact excluded pixel count |
| `EXCLSUM` | 1–16 lowercase hex digits | 64-bit FNV-1a of the excluded pixel indices (see [Exclusions](#exclusions)) |

The runner (`conformance --fixtures TABLE --fixture NAME LIST.jbc`) reads the entire table first and refuses (exit 2) if any line is malformed, contains NUL bytes, or duplicates any `NAME`. It locates `third_party/ps1-tests/gpu/NAME/{vram.png,psx.log}` two directories above the table (for `repo`) or two directories above the list (for `fetched`), verifies `REFSUM` before comparing, and reads `expected-failures.txt` beside the table. Legacy CLI overrides (`--reference`, `--passes`, `--expects`, `--words`, `--checksum`, `--dump`, `--exclude`, `--exclude-pixels`, `--exclude-checksum`, `--expect-mismatch`) are rejected with exit 2.

For `ps1-tests`, the `conformance-record` flake check verifies that `NAME`, `WORDS`, `CHECKSUM`, `REFERENCE` and `REFSUM` in `tests/conformance/fixtures.txt` match the recorder's `fixtures-identity.txt` across all 13 programs.

### Console probe results table (`probe/fixtures.txt`)

```text
NAME TABLE [EXCLUDE PIXELS EXCLSUM]
```

- `NAME`: corresponds to `NAME.jbpr` beside `probe/fixtures.txt`.
- `TABLE`: `ps1-tests` (binds to `NAME` in `../fixtures.txt`) or `open` (binds to `NAME` in `../open/fixtures.txt`).
- `EXCLUDE PIXELS EXCLSUM`: optional exclusion file beside `probe/fixtures.txt` (dump exclusions such as `lines.exclude` are never inherited by probe results).

Any `.jbpr` or `.exclude` file in `tests/conformance/probe/` not listed in `probe/fixtures.txt` fails CMake configuration.

## Exclusions

Exclusions exist strictly for pixels where a test program left packet data uninitialised (not for renderer bugs). Only one exclusion exists: `tests/conformance/lines.exclude` (64 pixels in `lines`' dump where the fourth vertex colour of two Gouraud polylines was never initialised; see [docs/conformance.md](../../docs/conformance.md)).

Exclusion files are plain text; `#` starts a comment line. Every other non-empty line is one of:

| Entry | Pixels excluded |
|---|---|
| `run X Y DX DY N REASON` | `N` pixels starting at `(X, Y)`, advancing by `(DX, DY)` where `DX, DY in {-1, 0, 1}` |
| `rect X Y W H REASON` | `W x H` rectangle with top-left `(X, Y)` |

Every pixel must lie within `0 <= x < 1024`, `0 <= y < 512`, and `REASON` is mandatory. Duplicate pixels are deduplicated into a sorted list of linear indices `y * 1024 + x` (as little-endian 32-bit words) whose 64-bit FNV-1a must match `EXCLSUM` and whose count must match `PIXELS`, or the runner exits with status 2.

Excluded pixels are omitted from the comparison, reported in the summary (`lines: MATCHES (64 pixels excluded)`), and coloured blue instead of magenta in `NAME.diff.png`. Any `.exclude` file in `tests/conformance/` not named by `fixtures.txt` fails CMake configuration.
