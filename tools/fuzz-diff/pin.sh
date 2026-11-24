# The reference fuzz-diff compares with (docs/testing.md, "Differential fuzzing").
# flake.nix pins the same commit as its psxstack input; CMakeLists.txt reads
# it from here.
#   psxstack: only psyq/gpu.c is used, compiled at build time (MIT)
PSXSTACK_REV=40071978ec43f76757a5e7fc5c89ee680bccd3bb
