# stb

PNG reading and writing for the tests, converter tools, hardware probe host tool and headless examples. Not part of the jitterbug library.

- **Origin:** [https://github.com/nothings/stb](https://github.com/nothings/stb), branch `master`, commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799` (2026-11-19).
- **Files:** `stb_image.h` (v2.34), `stb_image_write.h` (v1.19) and `LICENSE` (public domain or MIT, at the user's choice), copied unchanged.
  - SHA-256 `stb_image.h`: `dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020`
  - SHA-256 `stb_image_write.h`: `5d89edc8bffd848f3e138b2a1dbec67aadbed591894bde16ade7c83dc233af9b`
  - SHA-256 `LICENSE`: `eb36e45c704dd0b59730115523e089b051852625b4db0a6fe76c991900af20e7`
- **Local changes:** none.

The implementations are compiled without the project's warning flags in four places:

- `tests/stb.c` (`stb_image.h` and `stb_image_write.h`): for the test suite, `jbprobe to-png` and `tools/fuzz-diff`.
- `tools/common/png.c` (`stb_image.h` with `STBI_ONLY_PNG`, `STBI_NO_LINEAR`, `STBI_NO_HDR`): integer-only PNG decoding for `jb-tex` and `jb-mesh`.
- `examples/first-light/png.c` (`stb_image_write.h`): PNG output for `first-light-headless --png`.
- `examples/demo/png.c` (`stb_image_write.h`): PNG output for `demo-headless --png` and `demo-make-sources`.
