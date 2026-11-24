# The pinned sources the recorder reads (docs/testing.md, "How
# `tools/conformance-record` works").
# flake.nix pins the same commits as its ps1-tests and psn00bsdk inputs.
#   ps1-tests: the console's dumps and logs and ps1-tests' licence (MIT);
#              each program's sources come from the commit programs.txt
#              names for it (flake.nix: the ps1-tests-* inputs)
#   PSn00bSDK: only libpsn00b/psxgpu/dbugfont.tim, the debug font FntLoad
#              loads, and LICENSE.md (MPL-2.0); the SDK version ps1-tests'
#              build image uses
PS1_TESTS_REV=f727802fead11f1daa7549285548392ef87749cb
PSN00BSDK_REV=b0659ad85b7aa6e74d2c3eac29281636a0c2bc5e
