/*
 * The machine does what src/core/assumptions.c asserts at compile time.
 * The values go through volatile so they are computed when the test runs,
 * by the instructions the compiler picks for the target.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failed;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    volatile int32_t m1 = -1, m5 = -5, big = INT32_MIN;
    volatile int64_t wide = -(INT64_C(1) << 40);
    volatile uint32_t all = 0xffffffffu, word = 0x18000u;
    volatile int s1 = 1, s31 = 31, s33 = 33;
    const uint32_t bytes = 0x04030201u;
    unsigned char first;

    check((m1 >> s1) == -1, "-1 >> 1 is -1");
    check((m5 >> s1) == -3, "-5 >> 1 is -3");
    check((big >> s31) == -1, "INT32_MIN >> 31 is -1");
    check((wide >> s33) == -128, "-(1 << 40) >> 33 is -128");
    check((int32_t)all == -1, "0xffffffff converts to int32_t -1");
    check((int16_t)word == -32768, "0x18000 converts to int16_t -32768");
    check((m1 & 3) == 3, "-1 & 3 is 3");

    memcpy(&first, &bytes, 1);
    check(first == 1, "the host is little-endian");

    if (!failed)
        printf("ok\n");
    return failed;
}
