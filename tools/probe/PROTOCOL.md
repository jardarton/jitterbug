# Hardware probe serial protocol and result files

This document specifies the wire protocol (version 4) between the PlayStation hardware probe (`tools/probe/probe/probe.c`) and the host tool (`tools/probe/host/jbh_session.c`, `jbprobe.c`), together with the `.jbpr` result file format (`tools/probe/host/jbp_result.c`). Every constant and layout is defined in `tools/probe/common/jbp_proto.h`; framing and CRC-32 live in `tools/probe/common/jbp_frame.c`. See [docs/probe.md](../../docs/probe.md) for usage and hardware setup.

## Transport

SIO1, 8 data bits, no parity, 1 stop bit (8N1), no software or hardware flow control in the protocol (though console SIO1 hardware requires CTS asserted to transmit; see [docs/probe.md](../../docs/probe.md)). By default the console uses factor `MUL16` (`PSX_SIO_FACTOR = 2`) and reload `18` (`33868800 / 288 = 117600` baud), while the host uses `115200` baud (+2.08 %, within 8N1 tolerance).

All multi-byte integers on the wire and in `.jbpr` files are little-endian.

## Frame format

| Bytes | Field | Notes |
|---|---|---|
| 2 | `sync` | `0x4a`, `0x42` (`'J'`, `'B'`) |
| 1 | `type` | Frame type code |
| 1 | `seq` | Sequence number (`0..255`, mod 256) |
| 2 | `len` | Payload length in bytes (`0..4104`; `JBP_MAX_PAYLOAD = 8 + 2 * 2048`) |
| `len` | `payload` | Payload bytes |
| 4 | `crc32` | IEEE 802.3 reflected CRC-32 (polynomial `0xedb88320`, init/final XOR `0xffffffff`, as zlib `crc32`) over `type`, `seq`, `len` (2 bytes) and `payload` (`4 + len` bytes) |

Maximum frame size is `6 + 4104 + 4 = 4114` bytes (`JBP_MAX_FRAME`).

The byte-by-byte parser (`jbp_parse`):
- Hunts for `'J'`, `'B'` (a `'J'` followed by `'J'` stays in sync state 1 so `"JJB"` aligns at the second `'J'`).
- Reads `type`, `seq`, `len`. If `len > 4104`, counts a *damaged* frame (`bad++`), resets to sync hunt and returns `JBP_PARSE_BAD`.
- Reads `len` payload bytes and 4 CRC bytes. If the CRC mismatches, counts `bad++`, resets and returns `JBP_PARSE_BAD`.
- If more than `200 ms` (`JBP_BYTE_GAP_MS`) elapses between bytes of an in-progress frame (`jbp_parser_idle`), the partial frame is *cut*, counted (`cut++`) and dropped.

Before acting on any good frame, the receiver checks that `type` is valid from the peer and `len` matches the exact size defined below. An invalid control frame (`HELLO`, `PROGRESS`, `ACK`, `NAK`) is ignored and does not reset any timeout. An invalid data frame ends the session (`JBP_E_FRAME` on the probe for a malformed `LIST`; `jbp_result_apply` refusal on the host).

## Frame types

| Type | Name | Direction | Acked | Payload layout and exact size |
|---|---|---|---|---|
| `0x01` | `HELLO` | probe -> host | no | Identification block: `184` bytes (`JBP_HELLO_BYTES`), sent every `1000 ms` while idle |
| `0x02` | `PROGRESS` | probe -> host | no | `12` bytes (3 `u32`): `phase` (`1` list, `2` main loop, `3` read-back), `count`, `ms`; sent at most once per `1000 ms` while busy |
| `0x10` | `START` | host -> probe | yes | `32` bytes (8 `JBP_START_*` `u32` words), must have `seq = 0` |
| `0x11` | `LIST` | host -> probe | yes | `1..4096` bytes (`JBP_LIST_MIN..JBP_LIST_CHUNK`) of the `.jbc` file in order |
| `0x20` | `R_HEAD` | probe -> host | yes | `128` bytes (`32` `JBP_H_*` `u32` words) |
| `0x21` | `R_READS` | probe -> host | yes | `4 + 4k` bytes (`k >= 1`): `u32 first_index`, then `k` `u32` words read by `READ` ops |
| `0x22` | `R_EXPECTS` | probe -> host | yes | `4 + 20k` bytes (`k >= 1`): `u32 first_index`, then `k` records of 5 `u32` (`word_offset`, `read_index`, `mask`, `value`, `word_read`) |
| `0x23` | `R_WAITS` | probe -> host | yes | `4 + 20k` bytes (`k >= 1`): `u32 first_index`, then `k` records of 5 `u32` (`word_offset`, `mask`, `value`, `gpustat_last`, `pass`, where `pass` is `1` before `LOOP` and `2, 3, ...` in the main loop) |
| `0x24` | `R_VRAM` | probe -> host | yes | `4104` bytes: `u32 first_row`, `u32 row_count` (`2`), then `2 * 2048` bytes (`1024` 16-bit LE pixels per row) |
| `0x25` | `R_DONE` | probe -> host | yes | `20` bytes (5 `u32`): `vram_crc32`, `vram_frames` (`256`), `tx_resent`, `rx_bad`, `sio_errors` |
| `0x26` | `R_ERROR` | probe -> host | yes | `12` bytes (3 `u32`): `error_code` (`JBP_E_*`), `word_offset`, `bytes_received` |
| `0x27` | `R_IDENT` | probe -> host | yes | `184` bytes: identical layout to `HELLO`, sent as the first result frame |
| `0x30` | `ACK` | both | no | `0` bytes (non-empty `ACK` is ignored); `seq` is the acknowledged data frame's `seq` |
| `0x31` | `NAK` | both | no | `0` bytes (non-empty `NAK` is ignored); requests immediate resend |

### Identification payload (`HELLO` and `R_IDENT`, 184 bytes)

18 `u32` words (`JBP_ID_*`, 72 bytes) followed by 64 bytes read from `BFC00108h` (BIOS kernel/maker strings) and 48 bytes from `BFC7FF32h` (BIOS version string):

| Index | Constant | Meaning |
|---|---|---|
| 0 | `JBP_ID_PROTOCOL` | Protocol version (`4`) |
| 1 | `JBP_ID_PLATFORM` | `1` = console, `2` = host simulation |
| 2 | `JBP_ID_CPU_PRID` | COP0 `PRId` (`r15`) |
| 3 | `JBP_ID_GPUSTAT_BOOT` | `GPUSTAT` as found at boot |
| 4 | `JBP_ID_GPU_VERSION` | `0`, `1` or `2` (psx-spx GPU detection) |
| 5–10 | `JBP_ID_GP1_10_2`..`_5`, `_7`, `_8` | `GP1(10h)` query responses for indices 2, 3, 4, 5, 7, 8 |
| 11 | `JBP_ID_GPUSTAT_E1` | `GPUSTAT` after the v0/v1 `GP0(E1h)` test, or `0` |
| 12 | `JBP_ID_BIOS_DATE` | `BFC00100h` (BCD `YYYYMMDD`) |
| 13 | `JBP_ID_CONSOLE_TYPE` | `BFC00104h` |
| 14 | `JBP_ID_SIO_FACTOR` | `SIO1` mode register bits 0–1 |
| 15 | `JBP_ID_SIO_RELOAD` | `SIO1` baud reload register |
| 16 | `JBP_ID_LOOP_CAP` | Probe main-loop buffer capacity in words (`131072`) |
| 17 | `JBP_ID_READ_CAP` | Probe read buffer capacity in words (`16384`) |

### `START` payload (8 words, 32 bytes)

| Index | Constant | Meaning |
|---|---|---|
| 0 | `JBP_START_PROTOCOL` | Must be `4` |
| 1 | `JBP_START_SESSION` | Caller-chosen session identifier |
| 2 | `JBP_START_FLAGS` | Bit 0 (`JBP_FLAG_RESET`): reset GPU (`GP1(00h)`, `GP1(01h)`, `GP1(09h)=0`); bit 1 (`JBP_FLAG_CLEAR`): fill VRAM with 0. Default `3` |
| 3 | `JBP_START_LIST_BYTES` | Exact byte length of `.jbc` file (multiple of 4, `>= 28`) |
| 4 | `JBP_START_LIST_CRC` | IEEE 802.3 CRC-32 of the `.jbc` file |
| 5 | `JBP_START_PASSES` | Main-loop target pass `P` (`0` if no `LOOP`, `<= 1000000`) |
| 6 | `JBP_START_WINDOW` | Stability window `W` (`0` means default `2`; `<= 1000000`) |
| 7 | `JBP_START_WAIT_MS` | Timeout for `WAIT`/`DRAWSYNC`/DMA in ms (`0` means default `1000`) |

### `R_HEAD` payload (32 words, 128 bytes)

| Index | Constant | Meaning |
|---|---|---|
| 0 | `JBP_H_STATUS` | `0` on success, else `JBP_E_*` |
| 1 | `JBP_H_LIST_BYTES` | List bytes received |
| 2 | `JBP_H_LIST_CRC` | CRC-32 of list bytes received |
| 3 | `JBP_H_OPS` | Ops parsed during list upload (including `LOOP`, buffered loop-body ops parsed once during upload, and `END`; replays after `END` are not counted) |
| 4 | `JBP_H_READS` | Total words read by `READ` ops (`<= 16384`) |
| 5 | `JBP_H_READS_KEPT` | Words kept and sent (always equals `JBP_H_READS`) |
| 6 | `JBP_H_EXPECTS` | `EXPECT` ops evaluated (`<= 512`, all sent) |
| 7 | `JBP_H_EXPECTS_FAILED` | Count of `EXPECT` ops whose check failed on the probe |
| 8 | `JBP_H_WAITS` | Total `WAIT` and `DRAWSYNC` ops executed (all passes) |
| 9 | `JBP_H_WAITS_FAILED` | Count of waits that timed out (first `min(count, 64)` sent in `R_WAITS`) |
| 10 | `JBP_H_DMA_OPS` | DMA transfers started |
| 11 | `JBP_H_DMA_TIMEOUTS` | DMA transfers that timed out and were stopped |
| 12 | `JBP_H_VSYNCS` | `VSYNC` ops executed |
| 13 | `JBP_H_VSYNC_TIMEOUTS` | `VSYNC` ops that timed out (`200 ms` limit) |
| 14 | `JBP_H_LOOP` | `1` if the list has a `LOOP` op, else `0` |
| 15 | `JBP_H_PASSES` | Target pass `P` played |
| 16 | `JBP_H_WINDOW` | Window `W` played (default filled in) |
| 17 | `JBP_H_PLAYED` | Last pass played (`P + W`, or the window pass that changed VRAM) |
| 18 | `JBP_H_STEADY` | `1` if no window pass changed VRAM |
| 19 | `JBP_H_EARLY` | `1` if `P > 1` and pass `P` left VRAM equal to pass `P - 1` |
| 20 | `JBP_H_CHANGED_AT` | First window pass that changed VRAM, or `0` |
| 21 | `JBP_H_CRC_BEFORE` | VRAM CRC-32 after pass `P - 1` (`0` if `P < 2`) |
| 22 | `JBP_H_CRC_AT` | VRAM CRC-32 after pass `P` |
| 23 | `JBP_H_CRC_LAST` | VRAM CRC-32 after the last pass played |
| 24 | `JBP_H_GPUSTAT_END` | `GPUSTAT` immediately after the list finished |
| 25 | `JBP_H_GPUSTAT_READ` | `GPUSTAT` immediately before result VRAM read-back |
| 26 | `JBP_H_RESTORE` | Restore flags: bit 1 (`JBP_RESTORE_DMA_OFF`: `GP1(04h)=0`), bit 2 (`JBP_RESTORE_VRAM_SIZE`: `GP1(09h)=0`); bit 0 is unused in v4 |
| 27 | `JBP_H_SIO_ERRORS` | SIO1 overrun/framing/parity errors sampled just before sending `R_IDENT` |
| 28 | `JBP_H_RX_BAD` | Damaged or cut frames received up to `R_HEAD` |
| 29 | `JBP_H_TX_RESENT` | Frames resent up to `R_HEAD` |
| 30 | `JBP_H_LIST_MS` | Milliseconds from `START` to list end |
| 31 | `JBP_H_LOOP_MS` | Milliseconds spent in the main loop |

### Error codes (`R_ERROR`, `JBP_E_*`)

| Code | Name | Meaning |
|---|---|---|
| `1` | `JBP_E_MAGIC` | Not a `.jbc` list |
| `2` | `JBP_E_VERSION` | Unsupported `.jbc` version |
| `3` | `JBP_E_OP` | Unknown op or invalid op payload |
| `4` | `JBP_E_LOOP_FULL` | Main loop exceeds `131072` words |
| `5` | `JBP_E_LENGTH` | Byte count disagrees with `START`, data after `END`, or truncated list |
| `6` | `JBP_E_CRC` | List CRC-32 disagrees with `START` |
| `7` | `JBP_E_TIMEOUT` | Host went quiet for more than `30 s` |
| `8` | `JBP_E_PROTOCOL` | `START` protocol version is not `4` |
| `9` | `JBP_E_PASSES` | Main loop with `P = 0`, `P > 0` without a main loop, or `P`/`W > 1000000` |
| `10` | `JBP_E_FRAME` | `START` not 8 words or not `seq = 0`; `LIST` outside `1..4096` bytes |
| `11` | `JBP_E_NOT_IDLE` | GPU not ready (`GPUSTAT` bits 26 and 28) within `1000 ms` at restore or read-back |
| `12` | `JBP_E_READBACK` | GPU never set `GPUSTAT` bit 27 within `1000 ms` after `GP0(C0h)` |
| `13` | `JBP_E_READS` | A `READ` op would exceed `16384` total words read |
| `14` | `JBP_E_EXPECTS` | An `EXPECT` op would exceed `512` checks |
| `15` | `JBP_E_PREPARE` | GPU not ready (`GPUSTAT` bits 26 and 28) within `1000 ms` during pre-list preparation |

## Session flow

1. **Discovery (`HELLO` / `START`).** While idle, the probe emits `HELLO` every `1000 ms`. The host sends `START` (`seq = 0`) and resends every `3000 ms`, on a valid `HELLO` if `> 100 ms` since the last send, or on `NAK`/damaged frame, up to `60000 ms` total (`--start-ms`, including line write waits and `--tcp` connection time).
2. **Validation and preparation.** A `START` is *good* iff `len == 32`, `seq == 0`, `protocol == 4`, `P <= 1000000`, `W <= 1000000`, and `list_bytes >= 28` and divisible by 4.
   - While idle, any `START` is acknowledged; a bad one immediately replies with `R_ERROR` (`JBP_E_FRAME`, `JBP_E_PROTOCOL`, `JBP_E_PASSES`, `JBP_E_LENGTH`) without touching the GPU.
   - Inside an active session, a bad `START` is ignored (no `ACK`, no timeout reset). A good `START` identical word-for-word to the active session's `START` received before any list byte arrived is treated as a lost `ACK` and acknowledged again. Any other good `START` aborts the current session and starts a new one.
   - For a good `START`, the probe prepares the GPU: optional `GP1` resets (`JBP_FLAG_RESET`), wait up to `1000 ms` for `GPUSTAT` bits 26 and 28, optional two `1024 x 256` `GP0(02h)` zero fills (`JBP_FLAG_CLEAR`) followed by another `1000 ms` wait for bits 26 and 28. Any timeout fails the session with `JBP_E_PREPARE` before playing any list word. The probe sends `ACK(0)` only after preparation finishes.
3. **List transfer (`LIST`).** The host sends `.jbc` bytes in `LIST` frames of `1..4096` bytes with `seq = 1, 2, ... (mod 256)`, stop-and-wait. The probe executes words as each frame arrives and sends `ACK(seq)` once processed. A duplicate of the last accepted `LIST` `seq` is re-acknowledged without re-execution (before any `LIST` is accepted, `seq = 0` is ignored). Any other unexpected `seq` is ignored. A `LIST` outside `1..4096` bytes fails the session (`JBP_E_FRAME`) before `END` and is ignored after `END`. Any byte after `END`, or reaching `list_bytes` before `END`, triggers `JBP_E_LENGTH` immediately.
4. **Main loop and results (`R_*`).** After `END`, the probe verifies exact length and CRC-32, runs the main loop if the list has a `LOOP` (see [The main loop](#the-main-loop)), restores read-back state, and sends result frames sequentially (`seq = 0, 1, ...`, stop-and-wait): `R_IDENT`, `R_HEAD`, optional `R_READS`, `R_EXPECTS`, `R_WAITS`, 256 `R_VRAM` frames (rows `0, 2, ..., 510`), and `R_DONE`. On a list, session, prepare, restore or read-back error, the probe sends `R_ERROR` and never sends `R_DONE` for that session. If a result frame exhausts its 16 sends (or a new good `START` arrives), the probe aborts transmission without sending `R_DONE` and without sending `R_ERROR`.
5. **Host verification.** The host accepts result frames strictly in `seq` order after `jbp_result_apply` validates type, size, duplicate singletons, contiguous indices/rows and limits (it does not enforce overall ordering between different result frame types). A frame with the previous `seq` is re-acknowledged only if its type, length and payload CRC-32 match the previous frame exactly; anything else at that `seq` is ignored. Receiving a `HELLO` after `START` was acknowledged fails the session immediately (the probe returned to idle). At completion the host verifies `R_HEAD` matches the list sent, all 512 rows arrived in order, computed VRAM CRC-32 equals `R_DONE`'s, read/check/wait counts match `R_HEAD`, every check's word matches its referenced read, and the main-loop rule holds.

## Read-backs

Every VRAM read-back (at main-loop checkpoints and for results) proceeds two rows (`1024` words) at a time:

1. Wait up to `1000 ms` (`JBP_READY_MS`) for `GPUSTAT` bits 26 and 28 (ready for command, FIFO empty); timeout fails the session with `JBP_E_NOT_IDLE`.
2. Write `GP0(C0h)`, `(0, y)`, `(1024, 2)`.
3. Wait up to `1000 ms` for `GPUSTAT` bit 27 (VRAM ready to send); timeout fails the session with `JBP_E_READBACK`.
4. Read `1024` 32-bit words from `GPUREAD` (two 16-bit pixels per word, low half first).

Before result read-back, `restore` captures `GPUSTAT` (`JBP_H_GPUSTAT_END`), waits up to `1000 ms` for bits 26 and 28 (`JBP_E_NOT_IDLE` on timeout), then sends `GP1(04h) = 0` and `GP1(09h) = 0`. Read-back never seals stale `GPUREAD` words with a CRC-32 on timeout.

## Limits and timeouts

| Resource / Timer | Limit | Behaviour beyond limit |
|---|---|---|
| `LIST` payload | `1..4096` bytes | `JBP_E_FRAME` |
| Main loop body | `131072` words (`PROBE_LOOP_WORDS`) | `JBP_E_LOOP_FULL` |
| `READ` words | `16384` (`JBP_READS_MAX`) | `JBP_E_READS` before executing the overflowing `READ` |
| `EXPECT` ops | `512` (`JBP_EXPECTS_MAX`) | `JBP_E_EXPECTS` |
| Timed-out waits kept | `64` (`JBP_WAITFAIL_KEPT`) | All counted in `R_HEAD`; first `min(count, 64)` sent in `R_WAITS` |
| `WAIT` / `DRAWSYNC` / DMA wait | `START` `wait_ms` (default `1000 ms`) | Counted in `R_HEAD` (marks result unclean; stalled DMA is stopped) |
| `VSYNC` wait | `200 ms` (`T_VSYNC`) | Counted in `R_HEAD` (marks result unclean) |
| Prepare / restore / read-back wait | `1000 ms` (`JBP_READY_MS`) | Session fails (`JBP_E_PREPARE`, `JBP_E_NOT_IDLE`, `JBP_E_READBACK`) |
| Data-frame resend | Probe: `1000 ms`, host: `3000 ms`; max `16` sends total (`1` initial + `15` resends) | Immediate resend on `NAK` or damaged frame; gives up after 16th send |
| `PROGRESS` interval | At most every `1000 ms` during waits/read-backs | Resets host's quiet and resend timers so host does not flood SIO1's 8-byte FIFO |
| Probe quiet timeout | `30000 ms` (`T_SESSION`) without an acted-on host frame | Session fails (`JBP_E_TIMEOUT`) |
| Host quiet timeout (results) | `60000 ms` (`quiet_ms`) without an acted-on probe frame | Host fails session |
| Post-session drain | `1500 ms` | Host stays briefly to re-ACK a resent `R_DONE` if its last `ACK` was lost |

Both sides check timeouts on every loop iteration regardless of incoming traffic, so a flood of ignored or damaged frames cannot hold a session open past its limit.

## The main loop

The probe follows the same rule as the conformance runner ([tests/conformance/FORMAT.md](../../tests/conformance/FORMAT.md)): pass 1 ends at `LOOP`; each replay of `LOOP..END` is passes `2, 3, ...`. Given `P` and `W` (default `2`) from `START`:

- A list with `LOOP` and `P == 0`, or without `LOOP` and `P > 0`, fails with `JBP_E_PASSES`.
- The probe plays through pass `P` and up to `W` window passes, stopping at the first window pass whose VRAM differs.
- Full VRAM CRC-32 read-backs occur only after pass `P - 1` (if `P > 1`), pass `P`, and each window pass.
- For a list with `LOOP`, the host rejects the result unless `JBP_H_STEADY` is non-zero, `JBP_H_EARLY == 0`, `P` and `W` are in `1..1000000`, `JBP_H_PLAYED == P + W`, and `JBP_H_CRC_AT == JBP_H_CRC_LAST == R_DONE.vram_crc32`. For a list without `LOOP`, no stability check runs and `JBP_H_PASSES` must be `0`.

## Acting on a frame

### Probe (`tools/probe/probe/probe.c`)

| State | Received | Preconditions | Action |
|---|---|---|---|
| Idle (`probe_serve`) | `START` | type | Begin session (`start_session`): validate via `start_check`, prepare GPU (if good) or set error, send `ACK(seq)` |
| Idle | any other good frame | type | Ignored |
| Idle | damaged / cut | none | Dropped (not counted, no `NAK`) |
| In session (`handle_host_frame`) | `START` | `start_check` | Bad: ignored (no `ACK`, no timer). Good and identical to session's `START` with `0` list bytes and no `LIST` taken: reset timer, send `ACK(0)`. Good otherwise: reset timer, restart session |
| In session | `LIST` | `len` in `1..4096` (else `JBP_E_FRAME` before `END`, ignored after), then `seq` | `seq == last_taken`: reset timer, send `ACK(seq)`. `seq == next_expected` and `!ended && !error`: reset timer, play bytes, send `ACK(seq)`. Otherwise ignored |
| In session | other good frame | type | Ignored |
| Sending results (`send_data`) | `ACK` | `len == 0` and `seq == tx_seq` | Reset timer, advance to next frame; otherwise ignored |
| Sending results | `NAK` | `len == 0` | Immediate resend (counts towards 16-send limit); non-empty ignored |
| Sending results | other good frame | as `handle_host_frame` | Handled as in session; a new good `START` aborts sending |
| In session (not sending) | damaged | none | Count `RX_BAD`, send `NAK` |
| Sending results | damaged | none | Count `RX_BAD`, immediate resend |
| In session / sending | cut (`> 200 ms` gap) | none | Dropped; counted in `RX_BAD` when detected by idle check |

### Host (`tools/probe/host/jbh_session.c`)

| Context | Received | Preconditions | Action |
|---|---|---|---|
| `frame` | `ACK` | `len == 0`, frame outstanding, `seq` matches, session running | Reset timer, advance; else ignored |
| `frame` | `NAK` | `len == 0`, frame outstanding, session running | Reset timer, resend immediately (within `start_ms` or 16-send limit); else ignored |
| `frame` | `HELLO` | `len == 184`, `protocol == 4` | Before `START` ACKed: reset timer, store identification, resend `START` if `> 100 ms` since last send. After `START` ACKed: fail session |
| `frame` | `PROGRESS` | `len == 12`, `phase` in `1..3` | Reset quiet timer and outstanding frame's resend timer |
| `frame` | `START`, `LIST`, unknown | type | Ignored |
| `probe_data` | `R_*` | Results phase (or `R_ERROR` any time after `START`), then `seq` | Previous `seq`: if type, length and payload CRC-32 match last frame, reset timer and re-`ACK`; else ignored. Next `seq` while running: apply via `jbp_result_apply` (fail on rejection), reset timer, send `ACK`, validate `R_HEAD`/`R_ERROR`/`R_DONE`. Other `seq`: ignored |
| `jbh_input` | damaged | none | Count; if running, resend outstanding frame or send `NAK` if none outstanding |
| `jbh_poll` | cut | none | Count and drop |

## Result files (`.jbpr`)

A `.jbpr` file stores one session's complete record (maximum parsed file size `8 MiB`):

| Words | Field | Notes |
|---|---|---|
| `0`–`1` | `magic` | Exact bytes `"JBPROBE\0"` |
| `2` | `version` | `4` (versions `1..3` are refused) |

Followed by records to end of file, each structured as:

- `u32 type`
- `u32 n` (payload byte count)
- `uint8_t[n]` payload
- `(4 - n % 4) % 4` zero padding bytes to a 4-byte boundary

Record types reuse the wire frame types with identical payloads (`R_IDENT` or `HELLO`, `R_HEAD`, `R_READS`, `R_EXPECTS`, `R_WAITS`, 256 `R_VRAM` records of 2 rows each, `R_DONE`, `R_ERROR` — with `R_READS`/`R_EXPECTS`/`R_WAITS` each written as a single record from index `0` when non-empty), plus one host record:

- **`0x80` (`JBP_REC_HOST`):**
  - `list_name`: NUL-terminated string, at most `127` non-NUL bytes (`"gpu/triangle"`, etc.)
  - `u32 list_bytes`: `.jbc` file length in bytes
  - `u32 list_crc`: IEEE 802.3 CRC-32 of `.jbc` file
  - `note`: NUL-terminated string, at most `511` non-NUL bytes, no interior NUL bytes

### Validation and cleanliness

- **Structural (`jbp_result_parse` / `jbp_result_apply`):** refuses any record whose `n` or padding exceeds the remaining bytes (checked before addition to avoid 32-bit wrap), wrong exact payload size, non-contiguous or overflowing `R_READS`/`R_EXPECTS`/`R_WAITS`/`R_VRAM`, duplicate `R_IDENT`/`R_HEAD`/`R_DONE`/`R_ERROR`/`JBP_REC_HOST`, missing or interior string NUL bytes, or unknown record type.
- **Complete and sound (`jbp_result_check`):** no `R_ERROR`, all of `R_IDENT` (protocol `4`), `R_HEAD` (`status == 0`), `R_DONE` and `JBP_REC_HOST` present, `R_HEAD` list length/CRC matching `JBP_REC_HOST`, all 512 rows received (`done_frames == 256`), computed VRAM CRC-32 matching `R_DONE`, read/check/wait counts matching `R_HEAD`, every check consistent with its referenced read, and main-loop rule satisfied.
- **Clean (`jbp_result_clean`):** passes `jbp_result_check`, with `JBP_H_WAITS_FAILED == 0`, `JBP_H_DMA_TIMEOUTS == 0` and `JBP_H_VSYNC_TIMEOUTS == 0`. Only clean results are accepted as conformance references (`conformance --probe` and `jbprobe compare`, which also verify that the evaluated checks match the list's `EXPECT` ops in order). For diagnosis, `jbprobe info` can inspect any structurally valid capture (including incomplete or unclean ones), whereas `jbprobe to-png` requires a complete, sound result passing `jbp_result_check` (it accepts unclean captures, not incomplete ones).
