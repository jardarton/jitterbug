# cgltf

glTF 2.0 reader used by `tools/jb-mesh` (`.gltf` and `.glb`, with embedded buffers, data URIs or sidecar files). Not part of the jitterbug library.

- **Origin:** [https://github.com/jkuhlmann/cgltf](https://github.com/jkuhlmann/cgltf), release tag `v1.15`, commit `360db1a95480fe102ae9c69b27c5d101167ff5ba` (2025-02-09).
- **Files:** `cgltf.h` (version 1.15) and `LICENSE` (MIT), copied unchanged.
  - SHA-256 `cgltf.h`: `e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec`
  - SHA-256 `LICENSE`: `f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95`
- **Local changes:** none.

The implementation is compiled once in `tools/jb-mesh/cgltf_impl.c` without the project's warning flags and with `-ffp-contract=off`. That file defines `CGLTF_ATOF` to `jb-mesh`'s own number parser before including `cgltf.h` (integers keep the C library's `atoi` and `atoll`, which are exact), so floating-point numbers in glTF JSON parse to the same floats on every platform regardless of the C library's `atof` (see [docs/tools.md](../../docs/tools.md)). This uses the configuration hook the header provides and makes no change to `cgltf.h`.
