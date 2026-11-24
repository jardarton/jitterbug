/*
 * A libFuzzer target for the texture and mesh loaders: every input through
 * both, with fuzz_files' checks (fuzz_files.h); a failed check aborts, which
 * libFuzzer reports with the input. Built with clang and -DJB_LIBFUZZER=ON
 * (tests/CMakeLists.txt, "Files and converters"); not run by ctest beyond a
 * short smoke run over the pinned files. For a longer run:
 *
 *   build/tests/fuzz_files_libfuzzer -max_total_time=600 corpus/ tests/files/expected/
 */
#include "fuzz_files.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static jbf_state st;
    static int ready;

    if (!ready) {
        if (jbf_init(&st) != 0)
            abort();
        ready = 1;
    }
    if (jbf_one(&st, data, size, 0) != 0)
        abort();
    return 0;
}
