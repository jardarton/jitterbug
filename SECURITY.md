# Security policy

## Supported versions

Security fixes are provided for the current `0.1.x` release line.

| Version | Supported |
|---|---|
| `0.1.x` | Yes |

## Scope

The library's binary asset loaders (`jb_texture_parse`, `jb_texture_load_mem`, `jb_texture_load`, `jb_mesh_load_mem`, `jb_mesh_load`) and GPU command-stream entry points (`jb_gp0`, `jb_gp1`, and the rest of the public API) accept untrusted input and are intended to reject malformed data cleanly without crashing, leaking memory or accessing memory out of bounds.

Both surfaces are fuzzed as part of the test suite: `fuzz_files` and the Clang `fuzz_files_libfuzzer` target (run under ASan and UBSan in the `sanitize` check) exercise the `.jbt` and `.jbm` loaders, and `fuzz_robust` and `api_robust` exercise malformed and truncated GP0/GP1 command streams and API calls (see [docs/testing.md](docs/testing.md)). Bugs causing crashes, out-of-bounds reads or writes, undefined behaviour, or memory leaks on untrusted inputs — including in the `jb-tex` and `jb-mesh` CLI converters — are in scope.

## Reporting a vulnerability

Please report suspected security vulnerabilities privately using GitHub's private vulnerability reporting on the repository's Security tab:

- [https://github.com/jardarton/jitterbug/security](https://github.com/jardarton/jitterbug/security)

Please do not open a public issue or pull request for an undisclosed security vulnerability. Include a minimal reproducer (for example, a malformed `.jbt`/`.jbm` file, GP0/GP1 word sequence, or small C program), the jitterbug version and your build configuration.
