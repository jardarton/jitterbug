# Performance

Frame times of the software renderer by internal scale and thread count, natively on Linux x86-64 and as single-threaded `wasm32` under Node. Measurements come from `jb-bench` ([docs/tools.md](tools.md)). They are observations from two test machines, not pass/fail tests: nothing in `ctest` or `nix flake check` asserts a frame rate. The desktop tables and demo scene frame times below were measured on version 0.1.0 (while the machine carried other background work); the laptop tables were measured on an earlier pre-0.1 build before the conformance fixes and have not been re-measured.

## What is measured

- **Whole frame cost:** elapsed time from the first call of a frame (`jb_gte_begin_frame`) until `jb_frame` returns the RGBA8 pixels — including the caller's GTE traffic, culling, ordering-table sorting and `jb_prim_*` calls. `jb_frame`'s own share (scanout and BGR555-to-RGBA8 conversion after workers finish) is timed separately.
- **Sampling:** 10 untimed warm-up frames followed by 120 timed frames in a fixed deterministic order (`frame k = (37k + 11) mod 360`), stopping early after 10 s once at least 30 frames have completed. Tables show `median (p10-p90) fps`.
- **Threads:** `1`, `2`, `4`, `8`, and `auto` (`threads = 0`, one per logical CPU up to 16). Threads are not pinned.
- **Options:** `off` is the console look (integer path); `on` enables both `precise_vertices` and `perspective_correct`.
- **Clock:** `CLOCK_MONOTONIC` (`performance.now()` under Emscripten).

### Workloads

All three workloads are deterministic, integer-only, 320x240 double-buffered scenes using the public API (`jb-bench --stats` over frames 0–359 at 1x):

| Workload | Prims/frame (min–max) | Triangles | On screen | Textured 4/8/15-bit | Semi-transparent | Rects | Lines | px/tri | px/rect | Overdraw |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| `first-light` | 56 (55–57) | 11 | 11 | 30% / 49% / 21% | 19 | 31 | 20 | 8866 | 122.8 | 1.29 |
| `heavy` | 1674 (1542–1842) | 1428 | 1394 | 68% / 26% / 6% | 400 | 346 | 125 | 87 | 125.5 | 2.16 |
| `fill` | 4 (4–4) | 8 | 8 | 25% / 50% / 25% | 4 | 0 | 0 | 38400 | 0.0 | 4.00 |

("Prims" counts drawing commands, where a quad is one; overdraw is total covered pixels divided by 76,800, excluding the per-frame `jb_vram_fill` clear.)

- **`first-light`:** the console-level `first-light` scene (`tests/console_scene/scene.c`) — a light scene with a floor quad, lit cube, sprite, line ring and HUD panel.
- **`heavy`:** a synthetic flight over procedural terrain (`tools/jb-bench/heavy.c`) using GTE `RTPT`/`RTPS`/`NCLIP`/`AVSZ`/`NCCT`, a 1024-bucket ordering table, 4- and 8-bit Gouraud-textured terrain, semi-transparent palette-animated water, lit blocks, 15-bit backdrop, additive sprites, Gouraud lines and a HUD.
- **`fill`:** fill-rate worst case — four full-screen tilted Gouraud-textured semi-transparent quads (4-, 8- and 15-bit across all four blend modes), projected via the GTE so float options apply.

## How to measure your own

```sh
# Optimised native benchmark via Nix (Release, no -march)
nix build .#bench
result/bin/jb-bench --markdown          # full matrix (~10 minutes on a fast desktop)
result/bin/jb-bench --quick             # quick subset
result/bin/jb-bench --stats             # workload statistics only
result/bin/jb-bench --csv               # CSV output

# wasm32 under Node (single-threaded, scales 1-3 by default)
nix build .#checks.x86_64-linux.wasm -o wasm-result
nix develop -c node wasm-result/jb-bench.js --markdown

# Plain CMake build
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DJB_TOOLS=ON
cmake --build build-rel
build-rel/tools/jb-bench/jb-bench
```

See [docs/tools.md](tools.md#jb-bench) for all `jb-bench` options.

## Desktop (AMD Ryzen 7 7700X)

- **CPU:** AMD Ryzen 7 7700X (8 cores, 16 logical CPUs online, boost up to 5.58 GHz, 32 MB L3), `amd-pstate-epp` (`powersave`, EPP `balance_performance`, boost on), 32 GB RAM.
- **OS & builds:** Linux 6.18.56 (NixOS), x86-64, jitterbug 0.1.0. Native: `nix build .#bench` (`packages.bench`, Release, Clang 21.1.8, `-O3 -DNDEBUG -ffp-contract=off`, no `-march`, standard Nix hardening). Wasm: `checks.x86_64-linux.wasm`'s `jb-bench.js` (Release, Emscripten 6.0.9, Clang 22.1.8, `-O3 -DNDEBUG`, single-threaded, no SIMD) under Node.
- **Background load:** measured while the machine was running other work on 16 logical CPUs — 1-minute load average 5.21 before / 3.98 after the native run, and 3.98 before / 3.40 after the wasm run (roughly 3–4 during measurement). Multi-threaded runs are sensitive to competing CPU load because workers spin briefly before sleeping, which can inflate 8- and 16-thread medians on sub-millisecond configurations (for example `fill` 1x at 8 threads) and widen p90 tails at higher thread counts.
- **Hardening check:** an earlier check on this machine comparing the Nix-hardened binary against an unhardened Clang Release build over 36 configurations gave a median ratio of 1.008 (range 0.98–1.04; one outlier of 1.11 on `fill` 8x 16 threads).

### Native — `first-light`, options off (console)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (16) |
|---|---|---|---|---|---|
| 1x | 0.32 (0.31-0.32) 3154 | 0.18 (0.18-0.19) 5499 | 0.11 (0.11-0.12) 9007 | 0.07 (0.07-0.08) 13709 | 0.08 (0.08-0.09) 12222 |
| 2x | 1.08 (1.03-1.12) 924 | 0.60 (0.58-0.62) 1659 | 0.36 (0.35-0.38) 2769 | 0.24 (0.23-0.25) 4110 | 0.24 (0.23-0.25) 4199 |
| 3x | 1.97 (1.87-2.04) 507 | 1.12 (1.07-1.57) 894 | 0.67 (0.64-0.71) 1490 | 0.47 (0.45-0.62) 2129 | 0.45 (0.43-0.46) 2245 |
| 4x | 3.18 (3.01-3.30) 314 | 1.80 (1.72-1.86) 554 | 1.08 (1.03-1.12) 926 | 0.72 (0.70-0.75) 1383 | 0.73 (0.70-0.75) 1372 |
| 6x | 6.65 (6.26-6.95) 150 | 3.80 (3.59-3.93) 263 | 2.28 (2.17-2.49) 438 | 1.54 (1.47-2.16) 650 | 1.51 (1.46-1.56) 663 |
| 8x | 12.16 (11.34-12.70) 82 | 6.80 (6.43-7.23) 147 | 4.46 (4.21-4.82) 224 | 3.11 (2.92-3.58) 322 | 2.99 (2.82-3.26) 335 |

### Native — `first-light`, options on (precise + perspective)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (16) |
|---|---|---|---|---|---|
| 1x | 0.53 (0.52-0.54) 1890 | 0.29 (0.28-0.30) 3449 | 0.18 (0.17-0.19) 5559 | 0.11 (0.11-0.12) 8853 | 0.12 (0.11-0.14) 8230 |
| 2x | 1.55 (1.51-1.61) 643 | 0.84 (0.81-0.87) 1185 | 0.49 (0.47-0.51) 2051 | 0.32 (0.31-0.33) 3128 | 0.30 (0.29-0.31) 3344 |
| 3x | 2.98 (2.86-3.08) 336 | 1.62 (1.56-1.67) 618 | 0.94 (0.90-0.98) 1064 | 0.63 (0.60-0.74) 1590 | 0.55 (0.54-0.56) 1817 |
| 4x | 4.90 (4.70-5.06) 204 | 2.68 (2.59-2.77) 373 | 1.53 (1.48-1.58) 654 | 0.97 (0.94-1.00) 1034 | 0.89 (0.86-0.93) 1124 |
| 6x | 10.46 (10.00-10.81) 96 | 5.70 (5.49-5.90) 175 | 3.28 (3.15-3.50) 305 | 2.06 (2.00-2.13) 485 | 1.84 (1.80-1.96) 543 |
| 8x | 18.80 (17.92-19.38) 53 | 10.42 (9.98-10.79) 96 | 6.12 (5.81-6.58) 163 | 4.05 (3.86-4.49) 247 | 3.68 (3.40-4.03) 271 |

### Native — `heavy`, options off (console)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (16) |
|---|---|---|---|---|---|
| 1x | 1.06 (1.01-1.11) 942 | 0.83 (0.79-0.86) 1212 | 0.64 (0.61-0.67) 1555 | 0.58 (0.55-0.61) 1720 | 0.66 (0.59-0.75) 1518 |
| 2x | 3.94 (3.73-4.12) 254 | 2.36 (2.25-2.49) 423 | 1.55 (1.46-1.64) 647 | 1.09 (1.01-1.34) 915 | 1.12 (1.06-1.20) 891 |
| 3x | 6.91 (6.52-7.21) 145 | 3.96 (3.76-4.14) 252 | 2.37 (2.26-2.48) 422 | 1.62 (1.54-1.88) 619 | 1.57 (1.51-1.74) 638 |
| 4x | 10.81 (10.19-11.30) 92 | 5.98 (5.68-6.26) 167 | 3.50 (3.33-3.66) 285 | 2.36 (2.21-3.22) 423 | 2.22 (2.09-2.39) 451 |
| 6x | 21.23 (19.97-22.15) 47 | 11.52 (10.85-11.99) 87 | 6.65 (6.27-6.94) 150 | 4.02 (3.84-4.21) 249 | 3.92 (3.74-4.24) 255 |
| 8x | 36.27 (34.29-37.88) 28 | 19.52 (18.39-20.56) 51 | 11.21 (10.66-11.77) 89 | 7.08 (6.54-9.88) 141 | 6.84 (6.54-7.27) 146 |

### Native — `heavy`, options on (precise + perspective)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (16) |
|---|---|---|---|---|---|
| 1x | 1.62 (1.54-1.70) 617 | 1.17 (1.11-1.22) 855 | 0.90 (0.85-0.96) 1106 | 0.88 (0.80-0.95) 1141 | 0.83 (0.76-0.91) 1203 |
| 2x | 4.80 (4.54-5.00) 209 | 2.85 (2.72-2.98) 351 | 1.89 (1.78-2.04) 530 | 1.30 (1.21-1.53) 771 | 1.32 (1.25-1.39) 756 |
| 3x | 8.61 (8.16-9.01) 116 | 4.90 (4.64-5.13) 204 | 2.97 (2.79-3.43) 336 | 1.97 (1.88-2.28) 508 | 1.82 (1.74-1.96) 548 |
| 4x | 13.69 (12.84-14.32) 73 | 7.54 (7.11-7.88) 133 | 4.41 (4.15-4.75) 227 | 2.88 (2.70-3.85) 348 | 2.57 (2.43-2.96) 390 |
| 6x | 28.15 (26.34-29.46) 36 | 15.02 (14.04-15.75) 67 | 8.54 (8.00-8.92) 117 | 5.12 (4.82-5.41) 195 | 4.55 (4.34-5.00) 220 |
| 8x | 48.13 (44.92-50.26) 21 | 25.45 (23.95-26.61) 39 | 14.76 (13.77-15.67) 68 | 8.76 (8.25-11.37) 114 | 7.93 (7.44-8.91) 126 |

### Native — `fill`, options off (console)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (16) |
|---|---|---|---|---|---|
| 1x | 1.28 (1.28-1.30) 778 | 0.70 (0.70-0.73) 1419 | 0.37 (0.37-0.47) 2691 | 0.28 (0.20-0.34) 3594 | 0.19 (0.19-0.19) 5328 |
| 2x | 4.98 (4.96-5.01) 201 | 2.57 (2.56-2.59) 388 | 1.41 (1.40-1.44) 708 | 0.76 (0.76-1.24) 1307 | 0.74 (0.73-0.74) 1354 |
| 3x | 9.36 (9.33-9.40) 107 | 4.89 (4.87-4.91) 205 | 2.66 (2.64-2.70) 376 | 1.46 (1.45-1.47) 686 | 1.40 (1.39-1.41) 714 |
| 4x | 15.26 (15.17-15.34) 66 | 7.83 (7.81-7.87) 128 | 4.15 (4.13-4.17) 241 | 2.40 (2.39-4.03) 416 | 2.33 (2.32-2.34) 429 |
| 6x | 32.15 (31.99-32.51) 31 | 16.68 (16.56-17.76) 60 | 8.98 (8.92-9.32) 111 | 5.08 (5.04-7.96) 197 | 4.95 (4.93-6.53) 202 |
| 8x | 56.49 (56.20-56.87) 18 | 29.05 (28.87-29.61) 34 | 15.67 (15.50-16.18) 64 | 8.93 (8.64-9.12) 112 | 8.85 (8.52-9.52) 113 |

### Native — `fill`, options on (precise + perspective)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (16) |
|---|---|---|---|---|---|
| 1x | 2.85 (2.83-2.87) 351 | 1.53 (1.52-1.54) 654 | 0.79 (0.78-0.91) 1266 | 0.42 (0.41-0.42) 2408 | 0.31 (0.31-0.32) 3189 |
| 2x | 8.19 (8.16-8.23) 122 | 4.18 (4.16-4.20) 239 | 2.30 (2.27-2.64) 435 | 1.22 (1.21-1.23) 820 | 1.00 (0.99-1.00) 1002 |
| 3x | 16.50 (16.44-16.59) 61 | 8.53 (8.51-8.57) 117 | 4.61 (4.59-4.67) 217 | 2.47 (2.45-2.50) 405 | 1.99 (1.98-2.13) 502 |
| 4x | 28.05 (27.94-28.20) 36 | 14.20 (14.15-14.24) 70 | 7.42 (7.39-7.52) 135 | 4.21 (4.17-5.79) 238 | 3.36 (3.35-3.59) 297 |
| 6x | 61.00 (60.76-61.20) 16 | 30.82 (30.74-30.94) 32 | 16.62 (16.38-17.30) 60 | 9.24 (9.08-12.63) 108 | 7.28 (7.23-9.20) 137 |
| 8x | 106.85 (106.52-107.21) 9 | 54.43 (54.25-55.18) 18 | 28.88 (28.53-29.46) 35 | 15.60 (15.39-18.19) 64 | 13.01 (12.55-14.91) 77 |

### `jb_frame` scanout share (desktop)

`jb_frame` runs serially on the calling thread and writes 19.7 MB of RGBA at 8x (320x240). Median `jb_frame` time and share of the frame on `heavy`, options off:

| Scale | 1 thread | auto (16) |
|---|---:|---:|
| 1x | 0.02 ms (2%) | 0.02 ms (3%) |
| 2x | 0.07 ms (2%) | 0.07 ms (6%) |
| 3x | 0.16 ms (2%) | 0.17 ms (11%) |
| 4x | 0.28 ms (3%) | 0.30 ms (13%) |
| 6x | 0.65 ms (3%) | 0.66 ms (17%) |
| 8x | 1.36 ms (4%) | 1.34 ms (20%) |

On the lighter `first-light` workload (options off, auto (16)), `jb_frame` reaches 44% of the frame at 8x (1.33 ms out of 2.99 ms). (Vectorising `jb_frame`'s in-place 16-to-32-bit widening in disjoint halves originally cut `jb_frame` by ~5x, from 6.11 ms down to 1.18 ms at 8x on an idle machine, making whole 16-thread frames 1.8x faster at 4x and 8x; all tables above include this optimisation.)

### `wasm32` under Node (desktop), 1 thread

| Workload | Options | 1x | 2x | 3x |
|---|---|---|---|---|
| `first-light` | off | 0.51 (0.50-0.52) 1949 | 1.77 (1.72-1.84) 566 | 3.35 (3.25-3.43) 298 |
| `first-light` | on | 0.75 (0.74-0.77) 1327 | 2.29 (2.24-2.36) 436 | 4.57 (4.43-4.69) 219 |
| `heavy` | off | 1.38 (1.33-1.43) 723 | 5.03 (4.79-5.20) 199 | 8.96 (8.53-9.31) 112 |
| `heavy` | on | 2.16 (2.04-2.24) 464 | 6.14 (5.81-6.37) 163 | 11.07 (10.48-11.51) 90 |
| `fill` | off | 1.82 (1.82-1.84) 548 | 6.75 (6.74-6.77) 148 | 12.85 (12.77-12.90) 78 |
| `fill` | on | 3.62 (3.61-3.63) 276 | 10.66 (10.64-10.69) 94 | 21.53 (21.49-21.57) 46 |

In wasm (built without SIMD), `jb_frame` takes 0.12 / 0.41 / 0.92 ms at 1x / 2x / 3x (0.93 ms on `fill` with options on at 3x). Single-threaded wasm is ~1.3x slower than native on `heavy` (`heavy` off: 1.30x at 1x, 1.28x at 2x, 1.30x at 3x; 1.27–1.42x on `fill`, and 1.42–1.70x on `first-light`, where the unvectorised `jb_frame` scanout is a larger share of the frame). Canvas upload (`putImageData`) is not included.

## Laptop (Intel Core i5-8250U)

> **Note:** these laptop figures were measured on an earlier pre-0.1 build before the conformance fixes (CLUT cache and 12-bit polygon/line interpolation) and have not been re-measured on 0.1.0. Based on the desktop re-measurement, expect scaled integer-path times (2x–8x, options off) to be roughly 6–8% higher on 0.1.0, with 1x and float-option timings largely unchanged.

A 2017 laptop running native and wasm builds of that earlier revision:

- **CPU:** Intel Core i5-8250U (4 cores, 8 threads, 1.6 GHz base, turbo to 3.4 GHz, 6 MB L3), `intel_pstate` (`powersave`, EPP `balance_performance`, turbo on), mains power.
- **OS & runtime:** Linux 6.18.60 (NixOS), x86-64; Node 24.22.2. Machine idle during measurement. `auto` is 8 threads here.

### Native — `heavy`, options off (console)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (8) |
|---|---|---|---|---|---|
| 1x | 2.52 (2.41-2.64) 397 | 2.05 (1.95-2.22) 487 | 1.79 (1.58-2.12) 559 | 1.78 (1.67-1.92) 560 | 1.72 (1.61-1.92) 581 |
| 2x | 8.52 (8.10-8.93) 117 | 5.37 (5.09-5.70) 186 | 3.58 (3.39-3.79) 280 | 3.26 (3.10-3.50) 306 | 3.26 (3.09-3.50) 307 |
| 3x | 14.70 (13.89-15.45) 68 | 8.67 (8.21-9.12) 115 | 5.41 (5.11-8.08) 185 | 4.98 (4.72-5.45) 201 | 4.99 (4.70-5.39) 200 |
| 4x | 23.17 (21.87-24.23) 43 | 13.67 (12.94-14.31) 73 | 7.81 (7.39-8.29) 128 | 7.53 (7.06-7.89) 133 | 7.47 (7.03-7.90) 134 |
| 6x | 44.43 (41.93-46.07) 23 | 25.49 (24.11-26.71) 39 | 15.97 (14.26-16.63) 63 | 13.91 (13.16-15.49) 72 | 13.86 (13.13-15.59) 72 |
| 8x | 73.42 (69.80-76.86) 14 | 42.35 (40.25-44.11) 24 | 25.56 (24.28-26.51) 39 | 23.75 (22.89-24.97) 42 | 24.02 (22.99-25.27) 42 |

### Native — `heavy`, options on (precise + perspective)

| Scale | 1 thread | 2 threads | 4 threads | 8 threads | auto (8) |
|---|---|---|---|---|---|
| 1x | 4.32 (4.05-4.48) 231 | 3.11 (2.92-3.27) 322 | 2.41 (2.21-3.00) 414 | 2.35 (2.21-2.52) 425 | 2.39 (2.29-2.63) 418 |
| 2x | 11.56 (10.79-12.00) 87 | 7.01 (6.61-7.33) 143 | 4.65 (4.35-6.41) 215 | 4.17 (3.99-4.56) 240 | 4.22 (3.93-4.60) 237 |
| 3x | 20.62 (19.26-21.49) 48 | 12.11 (11.33-12.60) 83 | 7.08 (6.73-7.44) 141 | 6.50 (6.14-6.84) 154 | 6.56 (6.17-7.04) 152 |
| 4x | 32.85 (30.64-34.12) 30 | 18.79 (17.64-19.54) 53 | 10.76 (10.08-11.85) 93 | 9.89 (9.33-10.51) 101 | 9.91 (9.32-10.35) 101 |
| 6x | 65.07 (60.72-67.93) 15 | 36.26 (33.83-37.77) 28 | 21.96 (20.73-23.08) 46 | 19.76 (18.85-20.73) 51 | 19.86 (18.89-20.89) 50 |
| 8x | 108.78 (101.92-112.74) 9 | 60.44 (57.07-62.78) 17 | 36.49 (34.49-37.90) 27 | 32.87 (31.11-34.88) 30 | 32.79 (31.12-34.16) 30 |

### Native — `first-light` and `fill`, options off

| Workload | Scale | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---|---|---|---|---|
| `first-light` | 1x | 0.70 (0.68-0.71) 1434 | 0.40 (0.39-0.41) 2489 | 0.25 (0.24-0.25) 4054 | 0.25 (0.24-0.25) 4026 |
| `first-light` | 2x | 2.27 (2.19-2.35) 441 | 1.28 (1.23-1.31) 782 | 0.81 (0.76-1.28) 1235 | 0.79 (0.76-0.82) 1271 |
| `first-light` | 4x | 6.72 (6.38-6.96) 149 | 4.03 (3.87-4.17) 248 | 2.63 (2.54-2.72) 380 | 2.67 (2.56-2.84) 374 |
| `first-light` | 8x | 26.19 (24.77-27.05) 38 | 15.74 (14.96-16.59) 64 | 10.76 (10.41-11.10) 93 | 10.79 (10.44-11.24) 93 |
| `fill` | 1x | 2.87 (2.86-2.87) 349 | 1.56 (1.52-1.63) 639 | 0.81 (0.81-1.02) 1229 | 0.74 (0.73-0.74) 1357 |
| `fill` | 2x | 10.47 (10.34-10.67) 95 | 5.47 (5.40-5.57) 183 | 3.01 (2.95-3.08) 332 | 2.84 (2.79-3.11) 352 |
| `fill` | 4x | 33.47 (32.90-33.82) 30 | 17.74 (17.44-18.06) 56 | 9.16 (9.05-9.33) 109 | 9.14 (8.86-10.23) 109 |
| `fill` | 8x | 121.10 (120.91-122.05) 8 | 64.05 (63.81-65.39) 16 | 35.37 (35.07-36.22) 28 | 33.94 (33.48-38.68) 29 |

With options on, `fill` on the laptop takes 64.3 / 33.1 / 17.5 / 16.2 ms at 4x (1 / 2 / 4 / 8 threads) and 244.9 / 126.1 / 67.2 / 58.9 ms at 8x. `jb_frame` takes 0.04 / 0.17 / 0.4–0.5 / 0.8–1.2 / 2.2–3.1 / 4.2–5.3 ms at 1x / 2x / 3x / 4x / 6x / 8x.

### `wasm32` under Node (laptop), 1 thread

| Workload | Options | 1x | 2x | 3x |
|---|---|---|---|---|
| `first-light` | off | 1.05 (1.02-1.06) 953 | 3.80 (3.68-4.01) 263 | 7.26 (7.08-7.44) 138 |
| `first-light` | on | 2.02 (1.98-2.07) 495 | 5.87 (5.71-6.02) 170 | 11.92 (11.63-12.27) 84 |
| `heavy` | off | 3.04 (2.92-3.19) 329 | 12.29 (11.57-12.75) 81 | 21.27 (20.07-22.12) 47 |
| `heavy` | on | 5.57 (5.23-5.83) 180 | 16.33 (15.36-17.04) 61 | 29.98 (28.32-31.44) 33 |
| `fill` | off | 3.97 (3.97-3.98) 252 | 14.75 (14.69-14.87) 68 | 28.05 (27.71-28.60) 36 |
| `fill` | on | 11.28 (11.17-11.29) 89 | 29.22 (29.15-29.38) 34 | 60.38 (60.20-60.90) 17 |

`jb_frame` in wasm on the laptop takes 0.23 / 0.76 / 1.75 ms at 1x / 2x / 3x.

## Cost of the conformance fixes

The 0.1.0 desktop tables above include all conformance fixes ([docs/conformance.md](conformance.md)): the CLUT cache, 12-bit polygon and line interpolation, and the scaled triangle set-up's fast 64-bit path with a 128-bit fallback for very thin slivers (`tests/hires_wide.c`). Comparing the 0.1.0 desktop re-measurement against the earlier pre-fix desktop baseline (bearing in mind the 0.1.0 run carried a background load average of roughly 3–4 on 16 logical CPUs, which mostly affects 8- and 16-thread medians and p90 tails):

- **Scaled console path (`options off`, 2x–8x, native):** `heavy` moved up by roughly 6–8% on 1–4 threads (for example, 4x 1 thread `10.07` → `10.81` ms, +7.3%; 4x 4 threads `3.26` → `3.50` ms, +7.4%; 8x 1 thread `33.79` → `36.27` ms, +7.3%), and `first-light` by 3–8% on 1 thread (`3.00` → `3.18` ms at 4x, `11.23` → `12.16` ms at 8x).
- **1x native, `fill`, float options (`options on`) and `wasm32`:** effectively unchanged within noise. `heavy` at 1x on 1 thread moved from `1.07` to `1.06` ms; `heavy` with options on at 2x–8x moved by +0.6–1.8% on 1–2 threads; `fill` with options off was flat to ~2% faster (`57.49` → `56.49` ms at 8x on 1 thread); and single-threaded `wasm32` under Node was flat to ~4% faster (`heavy` off at 2x `5.25` → `5.03` ms, 3x `9.15` → `8.96` ms).

Two isolated before-and-after measurements taken on the desktop when the fixes landed explain that split:

- **CLUT cache (`heavy`, options off, Clang Release, two runs each):**

| Revision | 1 thread, 1x | 4 threads, 1x | 1 thread, 4x | 4 threads, 4x |
|---|---|---|---|---|
| Before CLUT cache | 1.05, 1.05 ms | 0.62, 0.62 ms | 9.86, 9.88 ms | 3.18, 3.18 ms |
| With CLUT cache | 1.05, 1.07 ms | 0.64, 0.64 ms | 9.53, 9.59 ms | 3.15, 3.14 ms |

  Within run-to-run noise at 1x and roughly 3% faster at 4x on 1 thread (cached palettes eliminate CLUT tile hazards).

- **12-bit polygon interpolation (first implementation, GCC Release, options off, 4x):** `heavy` moved from 9.92 to 10.75 ms (+8.4%) on 1 thread and 3.25 to 3.51 ms (+8.0%) on 4 threads; `first-light` moved from 3.47 to 3.54 ms (+2.0%) on 1 thread and 1.17 to 1.20 ms (+2.6%) on 4 threads; 1x was unchanged (1.09 ms). Even with the 64-bit fast path for ordinary scaled triangles in 0.1.0, the 12-bit interpolation accounts for the ~6–8% net increase on scaled integer `heavy` frames.

## Demo scene frame times

Measured with `demo-headless` (CMake Release, Clang) over the full 2,768-frame archipelago loop (~10,400 terrain triangles submitted to the GTE per frame from ~80 chunks, plus sea, pier, lighthouse, stones, trees, sky and haze) on the Ryzen 7 7700X desktop shared with other builds (so these are upper bounds; see [examples/demo/README.md](../examples/demo/README.md)):

| Scale | 1 thread | 16 threads | `wasm32`, Node, 1 thread |
|---|---:|---:|---:|
| 1x (320x240) | 4.7 ms | 4.4 ms | 6.4 ms |
| 2x (640x480) | 8.7 ms | 6.0 ms | 11.4 ms |
| 3x (960x720) | — | — | 16.7 ms |
| 4x (1280x960) | 18.7 ms | 10.0 ms | 23.9 ms |

## Summary and limits

- **Thread scaling:** large scales scale cleanly up to physical core count (`heavy` off at 8x reaches 5.1x speedup on 8 threads of the desktop, `fill` off 6.3x; with float options on, 5.5x and 6.9x on 8 threads, rising to 6.1x and 8.2x on 16). SMT threads add little with console options off (at most ~6%, with small regressions at lower scales under background load) and roughly 8–21% with float options on at 3x–8x. At 1x, serial work (caller geometry, queue bookkeeping and `jb_frame`) caps `heavy` speedup under 2x.
- **Options overhead:** enabling both `precise_vertices` and `perspective_correct` adds roughly 22–33% on `heavy` at 2x–8x (53% at 1x) on 1 thread, and 65–90% on `fill` at 2x–8x (123% at 1x).
- **60 fps envelope (16.7 ms budget, `heavy`, options off):**
  - Desktop (0.1.0): 4x on 1 thread (10.8 ms), 8x on 4+ threads (11.2 ms on 4); wasm32 under Node holds 1x–3x (9.0 ms at 3x).
  - Laptop (pre-0.1): 3x on 1 thread (14.7 ms), 4x on 2+ threads (13.7 ms on 2, 7.8 ms on 4), 6x on 4–8 threads (16.0 / 13.9 ms), while 8x peaks at 42 fps (23.8 ms); wasm32 under Node holds 1x and 2x (12.3 ms at 2x), not 3x (21.3 ms).
- **Limits of these numbers:** only two x86-64 Linux machines were measured (no arm64 hardware and no mobile device), `jb-bench` workloads are synthetic, browser canvas upload time is not included, the 0.1.0 desktop `jb-bench` runs were taken while the machine carried other background work (load average ~3–5 on 16 logical CPUs), and the laptop tables still come from an earlier pre-0.1 build before the conformance fixes.
