# Hardware probe

The hardware probe (`tools/probe/`) is a freestanding PlayStation program (`probe.exe`) and host tool (`jbprobe`) that play conformance lists ([tests/conformance/FORMAT.md](../tests/conformance/FORMAT.md)) within the console program's RAM limits (up to `131,072` main-loop words, `16,384` reads and `512` checks) on a real console's GPU and return the results over the serial port:

- All 1024 x 512 pixels of VRAM read via `GP0(C0h)`, all 16 bits per pixel (including mask bit 15, which the public `ps1-tests` PNG dumps omit).
- Every `GPUREAD` and `GPUSTAT` word read by `READ` ops (up to `16,384` words; more fails closed with `JBP_E_READS`).
- Every `EXPECT` check evaluated on the console (up to `512` checks; more fails closed with `JBP_E_EXPECTS`).
- Timeout counts for `WAIT`/`DRAWSYNC`, DMA and `VSYNC` (plus detail records for the first `64` timed-out `WAIT`/`DRAWSYNC` ops), `GPUSTAT` at list end, and software-readable hardware identification.

> **Status:** No console has run the probe yet. Everything proven so far comes from host simulation, MIPS I cross-build verification and emulator runs in PCSX-Redux with OpenBIOS (see [What is proven](#what-is-proven) and [What is not yet proven](#what-is-not-yet-proven)).

## Layout

| Path | Purpose |
|---|---|
| `tools/probe/common/` | Shared freestanding C99 protocol definitions (`jbp_proto.h`) and framing/CRC-32 (`jbp_frame.c`) |
| `tools/probe/probe/probe.c` | Console-side session protocol and list player over `probe/hal.h` |
| `tools/probe/probe/psx/` | PlayStation HAL (`hal_psx.c`), start-up (`crt0.S`), linker script (`probe.ld`), build script (`build.sh`) and ELF-to-PS-X-EXE converter (`mkexe.py`) |
| `tools/probe/host/` | Host session state machine (`jbh_session.c`), `.jbpr` file I/O (`jbp_result.c`) and CLI (`jbprobe.c`) |
| `tools/probe/emulator/` | Script (`run-pcsx-redux.sh`) and Lua hook (`vram-at-session-end.lua`) for exercising `probe.exe` in PCSX-Redux |
| [tools/probe/PROTOCOL.md](../tools/probe/PROTOCOL.md) | Complete serial wire protocol (v4) and `.jbpr` file format specification |
| `tests/probe/` | Host simulation (`sim.c`), host parser/file tests (`host.c`) and PTY/TCP timeout tests (`line.c`) |

## Building

Build the console executable (`probe.exe`, `probe.elf`, `probe.map`):

```sh
nix build .#probe
# or inside the dev shell:
nix develop -c tools/probe/probe/psx/build.sh out-probe
```

`jbprobe` is built automatically with the tests (`build/tests/jbprobe`) on POSIX hosts (not Emscripten). `nix flake check` builds and verifies `probe.exe` (`checks.probe`, on x86-64 hosts) and runs the simulation and `jbprobe` tests in every test build.

### Why freestanding (no SDK)

The probe touches only four GPU ports, DMA channel 2, SIO1, timer 2 and `I_STAT`, all simple polled registers documented in psx-spx. It installs no interrupt handlers, makes no BIOS calls after start-up and links no C library (`-march=mips1 -msoft-float -mno-abicalls -fno-pic -G0 -ffreestanding -nostdlib` using `pkgsCross.mipsel-linux-gnu` unwrapped). `mkexe.py` (Python standard library only) emits and validates the PS-X EXE header. Avoiding an external SDK avoids a second toolchain and an MPL-2.0 dependency, and ensures no SDK helper (`ResetGraph`, `DrawSync`, interrupt setup) sits between the `.jbc` list and the GPU.

### Memory map

Main RAM is 2 MiB at `80000000h` (the BIOS kernel reserves the first 64 KiB):

| Address | Contents |
|---|---|
| `80010000h` | Executable code and read-only data (~12 KiB; exact size reported by `mkexe.py --check`) |
| Following load | BSS zeroed by `crt0.S`: 512 KiB main-loop buffer (`131072` words), 64 KiB DMA buffer, 68 KiB DMA linked-list nodes, 64 KiB read buffer (`16384` words), 512 checks, 64 timed-out waits, frame buffer and 2-row VRAM strip (ending around `800CB030h`) |
| `801FFFF0h` | Stack growing downward; `probe.ld` asserts at least 64 KiB between `__bss_end` and `801F0000h` |

Lists are streamed and executed as frames arrive (lists like `texture-flip` are 2 MiB, the size of RAM). Only `GP0_DMA` bursts (in chunks of up to 16,384 words), the `LOOP..END` body (up to 131,072 words; the largest recorded loop is `lines` at 2,407 words), `READ` results (up to 16,384 words), `EXPECT` checks (up to 512) and the first 64 timed-out waits are buffered in RAM. Valid `.jbc` lists that exceed the loop, read or check limits fail closed (`JBP_E_LOOP_FULL`, `JBP_E_READS`, `JBP_E_EXPECTS`).

## How the probe plays a list

Ops from the `.jbc` stream execute in order:

| Op | Console behaviour |
|---|---|
| `GP0` | Written word-by-word to `1F801810h` by the CPU back to back |
| `GP0_DMA` | Buffered in RAM (chunks of up to 16,384 words) and sent via DMA channel 2: SyncMode 1 (8-word blocks, matching `LoadImage`) when count is a multiple of 8, else SyncMode 2 linked list (nodes up to 16 words). Waits for completion up to the wait limit (emitting `PROGRESS`), stopping and counting any transfer that times out. Uses whatever DMA direction the list set with `GP1(04h)` |
| `GP1` | Written word-by-word to `1F801814h` |
| `READ` | Reads `GPUREAD` or `GPUSTAT` `count` times; failing with `JBP_E_READS` before overflowing 16,384 reads |
| `WAIT` | Polls `GPUSTAT` until `(stat & mask) == value` up to the wait limit (`1000 ms` default); timeouts are counted and the first 64 are recorded in `R_WAITS` |
| `DRAWSYNC` | Waits for `GPUSTAT` bit 28 when DMA direction is `0`, else bits 26 and 28 |
| `VSYNC` | Acknowledges `I_STAT` bit 0 and polls for vertical blank (`200 ms` limit; timeouts are counted) |
| `LOOP` | Buffers ops up to `END` without replaying yet |
| `EXPECT` | Evaluates immediately against the referenced read; fails with `JBP_E_EXPECTS` beyond 512 checks |
| `CALL` | Skipped |
| `END` | If the list has a `LOOP`, runs the main loop to pass `P` plus the stability window (default 2 passes); then restores read-back state and transmits results |

### Preparation, restore and read-back

Before playing a list (both enabled by default in `START`, disabled with `--no-reset` / `--no-clear`):

1. If reset is enabled: writes `GP1(00h)`, `GP1(01h)` and `GP1(09h) = 0` (`GP1(09h)` is not cleared by `GP1(00h)`).
2. Waits up to `1000 ms` for `GPUSTAT` bits 26 and 28 (command ready, FIFO empty).
3. If clear is enabled: sends two `1024 x 256` `GP0(02h)` zero fills (setting bit 15 to 0 across all of VRAM), then waits up to `1000 ms` for bits 26 and 28 again.

If either preparation wait times out, the session fails with `JBP_E_PREPARE` before playing a single word of the list.

After the list and main loop complete:
1. Records `GPUSTAT` (`JBP_H_GPUSTAT_END`), waits up to `1000 ms` for bits 26 and 28 (`JBP_E_NOT_IDLE` on timeout), and writes `GP1(04h) = 0` (DMA off) and `GP1(09h) = 0`.
2. Reads VRAM two rows (`1024` words) at a time via `GP0(C0h)`, waiting up to `1000 ms` for bits 26/28 before each command (`JBP_E_NOT_IDLE`) and up to `1000 ms` for bit 27 before reading `GPUREAD` (`JBP_E_READBACK`).

Every wait fails closed or records its timeout in `R_HEAD` (marking the result unclean so the conformance runner refuses it as a reference); see [tools/probe/PROTOCOL.md](../tools/probe/PROTOCOL.md) for the full timeout table.

### Hardware identification

At boot, before any reset, the probe records: boot `GPUSTAT` (bit 20 indicates NTSC/PAL as the BIOS left it), GPU version via psx-spx detection (`GP1(10h)` indices 4 and 7; `GP0(E1h)` bit 12 for v0 vs v1), `GP1(10h)` indices 2, 3, 4, 5, 7 and 8, COP0 `PRId`, BIOS date (`BFC00100h`), console type (`BFC00104h`), BIOS kernel strings (`BFC00108h`, 64 bytes) and version string (`BFC7FF32h`, 48 bytes). Physical model number and mainboard revision cannot be read in software and must be recorded by the operator.

## Serial line and cable

SIO1 (`1F801050h`), polled, 8N1, DTR and RTS asserted. By default `hal_psx.c` uses factor `MUL16` (`PSX_SIO_FACTOR=2`) and reload `18`, giving `33868800 / 288 = 117600` baud (+2.08 % above the host's `115200`, within 8N1 mid-bit sampling tolerance). Exact `115200` baud is available as an untested build option: `-DPSX_SIO_FACTOR=1 -DPSX_SIO_RELOAD=294`.

At `115200` baud, 1 MiB of VRAM takes ~90 s to return, and list upload takes ~1 s per 11 KiB (~3 minutes for `texture-flip`).

`jbprobe send` supports `--tty DEV [--baud N]` (default `115200`) and `--tcp HOST:PORT` (for emulators or network serial bridges). Every read, write and TCP connect is bounded by session deadlines (`--start-ms`, default `60000 ms`; `16` sends of a `LIST` frame 3 s apart; `60000 ms` quiet timeout during results; `1500 ms` drain at end), and pending tty output is flushed with `tcflush` on close.

### Console SIO pinout

Only the original PlayStation has the 8-pin serial port (the PSone PM-41 board has internal solder pads only). Looking at the rear port, pins 8 to 1 left to right (from psx-spx):

| Pin | Signal | Direction |
|---|---|---|
| 1 | `RXD` | In (from host `TXD`) |
| 2 | `3.5 V` | Supply (do not connect adaptor power) |
| 3 | `DSR` | In |
| 4 | `TXD` | Out (to host `RXD`) |
| 5 | `CTS` | In |
| 6 | `DTR` | Out |
| 7 | `GND` | Ground |
| 8 | `RTS` | Out |

Use a **3.3 V TTL** USB serial adaptor — never RS-232 voltage levels. According to psx-spx, `DTR`, `RTS`, `DSR` and `CTS` are inverted on the board, and SIO1 transmits only while `CTS` is asserted. A 3-wire cable (`TXD`, `RXD`, `GND`) may leave `CTS` unasserted (shown as a **yellow** idle screen); wire the adaptor's `RTS` to pin 5 or tie pin 5 to the active level (untested on hardware).

## Booting on a console (untested)

None of the boot routes below have been tried on physical hardware yet. Any loader that starts `probe.exe` at `80010000h` in kernel mode with RAM from `80010000h` to `801FFFF0h` free should work:

1. **Memory card exploit + serial loader (untested):** a FreePSXBoot memory card image matching the console's BIOS version chaining to Unirom, which receives `probe.exe` over the same serial cable (`nops /exe probe.exe /dev/ttyUSB0` via NOTPSXSerial at `115200` baud).
2. **Burned CD-R (untested):** `SYSTEM.CNF` with `BOOT = cdrom:\PROBE.EXE;1` on a console able to boot CD-Rs.
3. **Parallel-port cheat cartridge with PC link (untested):** older console models with a parallel port.

### First-run checklist

1. Build and check the binary:
   ```sh
   nix build .#probe
   python3 tools/probe/probe/psx/mkexe.py --check result/probe.elf result/probe.exe
   ```
2. Connect the 3.3 V TTL cable (`TXD` 4 -> adaptor `RXD`, `RXD` 1 -> adaptor `TXD`, `GND` 7 -> `GND`, `CTS` 5 wired; leave pin 2 disconnected).
3. Boot the loader and run `probe.exe`. Screen colour indicates state: **blue** = idle with `CTS` asserted (ready); **yellow** = idle with `CTS` off (fix `CTS`); **red** = last session failed; blank = probe did not start.
4. Run `triangle` first:
   ```sh
   build/tests/jbprobe send --tty /dev/ttyUSB0 --fixtures tests/conformance/fixtures.txt \
     --note "SCPH-..., board ..., cable ..." \
     tests/conformance/triangle.jbc triangle.jbpr
   ```
5. Inspect hardware identification and compare against jitterbug (all 16 bits; create the output directory first because neither `compare --out` nor `to-png` creates it):
   ```sh
   mkdir -p out-triangle
   build/tests/jbprobe info triangle.jbpr
   build/tests/jbprobe compare triangle.jbpr tests/conformance/triangle.jbc \
     --fixtures tests/conformance/fixtures.txt --out out-triangle
   ```
6. Sanity-check against `ps1-tests`' public console dump:
   ```sh
   build/tests/jbprobe to-png triangle.jbpr out-triangle tests/conformance/triangle.jbc
   ```
   `to-png` writes `out-triangle/vram.png` (opaque RGBA, discarding mask bit 15), `vram16.png` (encoding bit 15 in alpha), `vram.bin` and `log.txt`. Compare the decoded 15-bit pixel colours of `out-triangle/vram.png` with `third_party/ps1-tests/gpu/triangle/vram.png` — every pixel's 15 colour bits should match (the PNG files themselves differ in format/compression).
7. Run the seven open-question lists in `tests/conformance/open/` with `--fixtures tests/conformance/open/fixtures.txt`, then the remaining `ps1-tests` lists (the three fetched-only lists are in `nix build .#conformance-lists`).

## Running the open-question experiments

The seven lists in `tests/conformance/open/` are the first experiments to run on a console (see [docs/testing.md](testing.md) and [docs/conformance.md](conformance.md)):

```sh
mkdir -p out-open
build/tests/jbprobe send --tty /dev/ttyUSB0 --fixtures tests/conformance/open/fixtures.txt \
  tests/conformance/open/<name>.jbc <name>.jbpr
build/tests/jbprobe compare <name>.jbpr tests/conformance/open/<name>.jbc \
  --fixtures tests/conformance/open/fixtures.txt --out out-open
```

| List | Kind | Match confirms | Difference indicates |
|---|---|---|---|
| `offset-wrap` | open | Vertex + offset is not wrapped (rows 100–107 blank) | 11-bit wrapping (red at `x = 0..14`, rows 100–107) |
| `quad-order` | confirmation | `v1 v2 v3` drawn last (blue in overlap, psx-spx) | `v0 v1 v2` drawn last (red in overlap) |
| `raw-gouraud-dither` | confirmation | Raw-textured Gouraud is undithered (copies equal, psx-spx) | Dithered (second copy 1 step lower on negative matrix entries) |
| `dither-semi` | open | Dither to 5 bits then blend (`10` only at dither `-4`) | Blend in 8 bits then dither (`10` at `-4, -3, -2`) |
| `modulated-semi-quarter` | confirmation | Mode 3 quarters after modulation (psx-spx) | Quarters before modulation (or another rule) |
| `modulated-semi-saturate` | open | Modulated texel saturated to 31 before blend (`19` for `t >= 17`) | Unsaturated into blend |
| `gouraud-texture-pairs` | open | Gouraud textured colour per pixel | Colour evaluated per pixel pair (`2k+1 == 2k`) |

## Turning console dumps into golden references

To check a real-console result into the test suite:

1. Copy `<name>.jbpr` into `tests/conformance/probe/` (never for the three fetched-only `ps1-tests` programs whose VRAM holds third-party images).
2. Save `jbprobe info <name>.jbpr` plus the console model, mainboard, region, cable, date and operator as `tests/conformance/probe/<name>.console.txt`.
3. Add `<name> ps1-tests` or `<name> open` to `tests/conformance/probe/fixtures.txt`.
4. CMake registers `conformance_probe_<name>`, which binds the result to its fixture table, verifies cleanliness (`jbp_result_clean`), compares all 16 bits of VRAM, reads and checks, and fails if the console itself failed a check.
5. Update [docs/conformance.md](conformance.md) (and fix the renderer in the same commit if the console refutes jitterbug's current rule).

## What is proven

Everything below has been tested without a console:

- **Host simulation (`tests/probe/sim.c`):** runs `probe.c` and `jbh_session.c` unmodified over a byte-level loopback against a jitterbug context accessed solely through `GP0`, `GP1`, `GPUREAD` and `GPUSTAT` in every test build (Clang, GCC, ASan+UBSan, `-O0`, `x86-64-v3`, `aarch64` under QEMU, `wasm32` under Node). Verifies all 10 (or 13 with `JB_CONFORMANCE_FETCHED`) `ps1-tests` lists and all 7 open lists, byte flips and drops in both directions (`probe_sim_faults_*`, `probe_sim_naks`), main-loop late-change and early-settle refusals, simulated GPU prepare/restore/read-back/DMA-stall/DMA-direction faults, malformed/flooding frames on both sides, seeded session and `.jbpr` file fuzzing, and field-by-field `.jbpr` round-trip checks.
- **`jbprobe` CLI & line deadlines (`probe_cli_*`, `probe_pty_*`, `probe_line_*`):** real-time `jbprobe send --tty` over a pseudo-terminal, `info`/`to-png`/`compare` including unclean and malformed `.jbpr` files under LeakSanitizer, and bounded exit under NAK-flooding PTY/TCP peers, full TCP listen queues and late `START` ACKs.
- **Runner binding (`probe_runner_*`, `probe_runner_bad_*`):** verifies refusal of wrong list, wrong pass, wrong platform (simulation where console is required), corrupted CRC, truncation, unclean captures (wait/DMA/VSYNC timeout, wrong check) and detection of single colour-bit or mask-bit flips.
- **MIPS I binary (`checks.probe`):** builds `probe.exe` from the flake, validates all PS-X EXE header fields, code bytes, memory map (code ~11.8 KiB, BSS ending ~1.2 MiB below stack) and disassembly (~2,900 instructions, strictly MIPS I).
- **Real MIPS binary in PCSX-Redux:** `tools/probe/emulator/run-pcsx-redux.sh` runs `probe.exe` headlessly in PCSX-Redux (commit `807e5cc8`) with OpenBIOS (no Sony BIOS) over its raw TCP SIO1 bridge, comparing `jbprobe`'s returned 16-bit VRAM against a Lua breakpoint dumping the emulator's VRAM directly. All 13 `ps1-tests` lists and the 7 open lists matched the emulator's 16-bit VRAM identically with no timeouts, and representative lists (`triangle`, `mask-bit`, `gp0-e1`, `texture-overflow`, `quad-order`) were re-verified after protocol v4 updates. (Not part of `nix flake check` because PCSX-Redux is not in pinned nixpkgs.)

## What is not yet proven

- **Real console execution and timing:** back-to-back CPU `GP0` write pacing, `1000 ms` ready/wait and `200 ms` VSYNC limits, `GPUREAD` read speed, DMA channel 2 handshakes, preparation/restore ready waits and SIO1's 8-byte receive FIFO behaviour on real hardware.
- **Physical serial line:** `117600` vs `115200` baud margin, `CTS` transistor polarity, SIO1 error flags and cable wiring.
- **Visual idle screens:** blue/yellow/red status screens were not inspected in headless emulator runs.
- **Real BIOS/GPU identification:** OpenBIOS returns synthetic strings and GPU version 0 in PCSX-Redux.
- **Boot loaders:** FreePSXBoot, Unirom and NOTPSXSerial have not been run with `probe.exe`.
- **DMA directions other than 2:** the probe does not enforce `GPUSTAT` DMA direction (lists set `GP1(04h)` themselves); the simulation rejects any transfer whose direction is not 2, so hardware behaviour under directions 0, 1 or 3 is untested.
