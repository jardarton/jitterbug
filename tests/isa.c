/*
 * The CPU running the tests has every instruction-set extension the
 * compiler was told it may use, and executes those instructions with the
 * architecture's answers (docs/determinism.md, "What `x86-64-v3` and
 * `arm64` prove").
 *
 *   test_isa [--expect x86-64-v3]
 *
 * With --expect x86-64-v3 (the x86-64-v3 check in flake.nix passes it) the
 * test fails unless, for each requirement of x86-64-v3 beyond the baseline
 * (the x86-64 psABI's levels: CMPXCHG16B, LAHF-SAHF, POPCNT, SSE3, SSE4.1,
 * SSE4.2 and SSSE3 from x86-64-v2; AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT,
 * MOVBE and OSXSAVE),
 *   - the compiler was told it may use it: it defined
 *     __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16, __LAHF_SAHF__, __POPCNT__,
 *     __SSE3__, __SSE4_1__, __SSE4_2__, __SSSE3__, __AVX__, __AVX2__,
 *     __BMI__, __BMI2__, __F16C__, __FMA__, __LZCNT__ and __MOVBE__
 *     (OSXSAVE has no macro: it is required with AVX);
 *   - the CPU (there, QEMU's model of one) advertises it in its CPUID bit
 *     (cpuid_bit[] below), and x86-64-v3 as a whole
 *     (__builtin_cpu_supports);
 *   - it executes one or two instructions of it, listed in probes[] below,
 *     each with an answer that the architecture fixes and that a CPU
 *     without it would not give (or it would fault); for OSXSAVE, xgetbv,
 *     which must also report the SSE and AVX state enabled.
 * Without --expect it checks only what the compiler was told it may use
 * (none on the x86-64 baseline, or elsewhere), and says when that is
 * nothing.
 *
 * Each instruction is written in inline assembly, with its inputs read
 * from volatile variables, so the compiler can neither fold it at compile
 * time nor replace it by other instructions; the x86-64-v3 check also
 * looks for each mnemonic in the binary (objdump).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define JB_ISA_X86 1
#endif

static int failed, checked;

static void check(int ok, const char *what)
{
    ++checked;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failed = 1;
}

#ifdef JB_ISA_X86
#include <cpuid.h>

/* x86-64-v3 beyond the baseline, as the x86-64 psABI defines its levels:
   x86-64-v2 adds CMPXCHG16B, LAHF-SAHF, POPCNT, SSE3, SSE4.1, SSE4.2 and
   SSSE3; x86-64-v3 adds AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE and
   OSXSAVE. */
enum {
    CX16, LAHF, POPCNT, SSE3, SSE4_1, SSE4_2, SSSE3,
    AVX, AVX2, BMI, BMI2, F16C, FMA, LZCNT, MOVBE, OSXSAVE, NCLASS
};

/* Whether the compiler was told it may use each (its predefined macro). */
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
#define HAS_CX16 1
#else
#define HAS_CX16 0
#endif
#ifdef __LAHF_SAHF__
#define HAS_LAHF 1
#else
#define HAS_LAHF 0
#endif
#ifdef __POPCNT__
#define HAS_POPCNT 1
#else
#define HAS_POPCNT 0
#endif
#ifdef __SSE3__
#define HAS_SSE3 1
#else
#define HAS_SSE3 0
#endif
#ifdef __SSE4_1__
#define HAS_SSE4_1 1
#else
#define HAS_SSE4_1 0
#endif
#ifdef __SSE4_2__
#define HAS_SSE4_2 1
#else
#define HAS_SSE4_2 0
#endif
#ifdef __SSSE3__
#define HAS_SSSE3 1
#else
#define HAS_SSSE3 0
#endif
#ifdef __AVX__
#define HAS_AVX 1
#else
#define HAS_AVX 0
#endif
#ifdef __AVX2__
#define HAS_AVX2 1
#else
#define HAS_AVX2 0
#endif
#ifdef __BMI__
#define HAS_BMI 1
#else
#define HAS_BMI 0
#endif
#ifdef __BMI2__
#define HAS_BMI2 1
#else
#define HAS_BMI2 0
#endif
#ifdef __F16C__
#define HAS_F16C 1
#else
#define HAS_F16C 0
#endif
#ifdef __FMA__
#define HAS_FMA 1
#else
#define HAS_FMA 0
#endif
#ifdef __LZCNT__
#define HAS_LZCNT 1
#else
#define HAS_LZCNT 0
#endif
#ifdef __MOVBE__
#define HAS_MOVBE 1
#else
#define HAS_MOVBE 0
#endif

/* OSXSAVE is the operating system's state, not an instruction class: no
   macro says the compiler may rely on it. Code with AVX does (the
   registers' upper halves are saved only when the OS enabled them), so it
   is required wherever AVX is, and with --expect x86-64-v3. */
static const int compiled[NCLASS] = {
    HAS_CX16, HAS_LAHF, HAS_POPCNT, HAS_SSE3, HAS_SSE4_1, HAS_SSE4_2, HAS_SSSE3, HAS_AVX,
    HAS_AVX2, HAS_BMI,  HAS_BMI2,   HAS_F16C, HAS_FMA,    HAS_LZCNT,  HAS_MOVBE, HAS_AVX,
};

static const char *const names[NCLASS] = {
    "cmpxchg16b", "lahf-sahf", "popcnt", "sse3", "sse4.1", "sse4.2", "ssse3", "avx",
    "avx2",       "bmi1",      "bmi2",   "f16c", "fma",    "lzcnt",  "movbe", "osxsave (with avx)",
};

/* Where the CPU advertises each: CPUID leaf (subleaf 0), register, bit
   (Intel SDM vol. 2A, CPUID; AMD APM vol. 3, CPUID). */
enum { EBX, ECX };
static const struct {
    unsigned leaf;
    int reg, bit;
} cpuid_bit[NCLASS] = {
    {1, ECX, 13},          /* CMPXCHG16B */
    {0x80000001u, ECX, 0}, /* LAHF-SAHF (LAHF/SAHF in 64-bit mode) */
    {1, ECX, 23},          /* POPCNT */
    {1, ECX, 0},           /* SSE3 */
    {1, ECX, 19},          /* SSE4.1 */
    {1, ECX, 20},          /* SSE4.2 */
    {1, ECX, 9},           /* SSSE3 */
    {1, ECX, 28},          /* AVX */
    {7, EBX, 5},           /* AVX2 */
    {7, EBX, 3},           /* BMI1 */
    {7, EBX, 8},           /* BMI2 */
    {1, ECX, 29},          /* F16C */
    {1, ECX, 12},          /* FMA */
    {0x80000001u, ECX, 5}, /* LZCNT (ABM) */
    {1, ECX, 22},          /* MOVBE */
    {1, ECX, 27},          /* OSXSAVE (the OS set CR4.OSXSAVE) */
};

static int advertised(int c)
{
    unsigned a, b, cx, d;

    if (!__get_cpuid_count(cpuid_bit[c].leaf, 0, &a, &b, &cx, &d))
        return 0;
    return (int)(((cpuid_bit[c].reg == EBX ? b : cx) >> cpuid_bit[c].bit) & 1);
}

/* The probes: each runs its instructions and checks the answers. */

static void probe_sse3(void)
{
    /* haddpd: the two halves of each source added. */
    volatile double a0 = 1.5, a1 = 2.25, b0 = -4.0, b1 = 0.5;
    double in[2] = {a0, a1}, in2[2] = {b0, b1}, out[2];
    __asm__("movupd %1, %%xmm0\n\t"
            "movupd %2, %%xmm1\n\t"
            "haddpd %%xmm1, %%xmm0\n\t"
            "movupd %%xmm0, %0"
            : "=m"(out)
            : "m"(in), "m"(in2)
            : "xmm0", "xmm1");
    check(out[0] == 3.75 && out[1] == -3.5, "SSE3 haddpd");
}

static void probe_ssse3(void)
{
    /* pshufb: bytes reversed; a set top bit in the control zeroes. */
    volatile uint8_t seed = 0;
    uint8_t in[16], ctl[16], out[16];
    int i, ok = 1;
    for (i = 0; i < 16; ++i) {
        in[i] = (uint8_t)(seed + 0x10 + i);
        ctl[i] = (uint8_t)(i == 3 ? 0x80 : 15 - i);
    }
    __asm__("movdqu %1, %%xmm0\n\t"
            "movdqu %2, %%xmm1\n\t"
            "pshufb %%xmm1, %%xmm0\n\t"
            "movdqu %%xmm0, %0"
            : "=m"(out)
            : "m"(in), "m"(ctl)
            : "xmm0", "xmm1");
    for (i = 0; i < 16; ++i)
        ok &= out[i] == (i == 3 ? 0 : 0x10 + 15 - i);
    check(ok, "SSSE3 pshufb");
}

static void probe_sse4_1(void)
{
    /* pmulld: the low 32 bits of each product; roundsd toward -inf. */
    volatile int32_t a = 0x12345, b = -0x10001;
    volatile double x = -2.5;
    int32_t in[4] = {a, a, a, a}, in2[4] = {b, b, b, b}, out[4];
    double r;
    __asm__("movdqu %1, %%xmm0\n\t"
            "movdqu %2, %%xmm1\n\t"
            "pmulld %%xmm1, %%xmm0\n\t"
            "movdqu %%xmm0, %0"
            : "=m"(out)
            : "m"(in), "m"(in2)
            : "xmm0", "xmm1");
    check(out[0] == (int32_t)(uint32_t)((uint64_t)0x12345 * (uint64_t)(uint32_t)-0x10001) &&
              out[3] == out[0],
          "SSE4.1 pmulld");
    __asm__("roundsd $1, %1, %0" : "=x"(r) : "x"((double)x));
    check(r == -3.0, "SSE4.1 roundsd");
}

static void probe_sse4_2(void)
{
    /* crc32 (CRC-32C) of the bytes "123456789" is 0xe3069283. */
    volatile uint32_t lo = 0x34333231u, hi = 0x38373635u;
    volatile uint8_t last = 0x39;
    uint32_t c = 0xffffffffu;
    __asm__("crc32l %1, %0" : "+r"(c) : "r"((uint32_t)lo));
    __asm__("crc32l %1, %0" : "+r"(c) : "r"((uint32_t)hi));
    __asm__("crc32b %1, %0" : "+r"(c) : "q"((uint8_t)last));
    check((c ^ 0xffffffffu) == 0xe3069283u, "SSE4.2 crc32");
}

static void probe_cx16(void)
{
    /* cmpxchg16b: rdx:rax equal to the memory, so rcx:rbx is stored and ZF
       set; then unequal, so the memory is loaded into rdx:rax, ZF clear. */
    volatile uint64_t lo = 0x1111222233334444ull, hi = 0x5555666677778888ull;
    volatile uint64_t nlo = 0x9999aaaabbbbccccull, nhi = 0xddddeeeeffff0000ull;
    struct {
        uint64_t q[2];
    } __attribute__((aligned(16))) m;
    uint64_t a = lo, d = hi;
    uint8_t z1, z2;

    m.q[0] = lo;
    m.q[1] = hi;
    __asm__("lock cmpxchg16b %0\n\t"
            "setz %1"
            : "+m"(m), "=&r"(z1), "+a"(a), "+d"(d)
            : "b"((uint64_t)nlo), "c"((uint64_t)nhi)
            : "cc");
    check(z1 == 1 && m.q[0] == nlo && m.q[1] == nhi && a == lo && d == hi,
          "CMPXCHG16B cmpxchg16b, equal");
    a = d = 0;
    __asm__("lock cmpxchg16b %0\n\t"
            "setz %1"
            : "+m"(m), "=&r"(z2), "+a"(a), "+d"(d)
            : "b"((uint64_t)lo), "c"((uint64_t)hi)
            : "cc");
    check(z2 == 0 && a == nlo && d == nhi && m.q[0] == nlo && m.q[1] == nhi,
          "CMPXCHG16B cmpxchg16b, unequal");
}

static void probe_lahf(void)
{
    /* sahf loads SF, ZF, AF, PF and CF from AH; lahf stores them back, with
       bit 1 always set. In 64-bit mode a CPU without LAHF-SAHF faults. */
    volatile uint32_t set = 0xd5, clear = 0;
    uint32_t a, b;

    __asm__("movl %1, %%eax\n\t"
            "shll $8, %%eax\n\t"
            "sahf\n\t"
            "movl $0, %%eax\n\t"
            "lahf\n\t"
            "shrl $8, %%eax"
            : "=&a"(a)
            : "r"((uint32_t)set)
            : "cc");
    __asm__("movl %1, %%eax\n\t"
            "shll $8, %%eax\n\t"
            "sahf\n\t"
            "movl $0, %%eax\n\t"
            "lahf\n\t"
            "shrl $8, %%eax"
            : "=&a"(b)
            : "r"((uint32_t)clear)
            : "cc");
    check(a == 0xd7 && b == 0x02, "LAHF-SAHF sahf, lahf");
}

static void probe_popcnt(void)
{
    volatile uint64_t v = 0xf0f0f0f00000ffffull;
    uint64_t n;
    __asm__("popcntq %1, %0" : "=r"(n) : "r"((uint64_t)v));
    check(n == 32, "POPCNT popcnt");
}

static void probe_avx(void)
{
    /* vaddps on all eight lanes of a ymm register. */
    volatile float a = 1.25f, b = -0.5f;
    float in[8], in2[8], out[8];
    int i, ok = 1;
    for (i = 0; i < 8; ++i) {
        in[i] = a * (float)i;
        in2[i] = b;
    }
    __asm__("vmovups %1, %%ymm0\n\t"
            "vmovups %2, %%ymm1\n\t"
            "vaddps %%ymm1, %%ymm0, %%ymm0\n\t"
            "vmovups %%ymm0, %0\n\t"
            "vzeroupper"
            : "=m"(out)
            : "m"(in), "m"(in2)
            : "xmm0", "xmm1");
    for (i = 0; i < 8; ++i)
        ok &= out[i] == 1.25f * (float)i - 0.5f;
    check(ok, "AVX vaddps ymm");
}

static void probe_avx2(void)
{
    /* vpaddd on all eight lanes of a ymm register, wrapping. */
    volatile int32_t a = 0x7fffffff, b = 1;
    int32_t in[8], in2[8], out[8];
    int i, ok = 1;
    for (i = 0; i < 8; ++i) {
        in[i] = a - i;
        in2[i] = b + i;
    }
    __asm__("vmovdqu %1, %%ymm0\n\t"
            "vmovdqu %2, %%ymm1\n\t"
            "vpaddd %%ymm1, %%ymm0, %%ymm0\n\t"
            "vmovdqu %%ymm0, %0\n\t"
            "vzeroupper"
            : "=m"(out)
            : "m"(in), "m"(in2)
            : "xmm0", "xmm1");
    for (i = 0; i < 8; ++i)
        ok &= out[i] == INT32_MIN;
    check(ok, "AVX2 vpaddd ymm");
}

static void probe_fma(void)
{
    /* e*e - 1 for e = 1 + 2^-30: fused, exactly 2^-29 + 2^-60; a separate
       multiply rounds e*e to 1 + 2^-29 first. */
    volatile double e = 1.0 + 0x1p-30, m1 = -1.0;
    double r = m1, x = e, y = e;
    __asm__("vfmadd231sd %2, %1, %0" : "+x"(r) : "x"(x), "x"(y));
    check(r == 0x1p-29 + 0x1p-60, "FMA vfmadd231sd is fused");
}

static void probe_bmi(void)
{
    volatile uint32_t a = 0x0f0f0f0fu, b = 0xffff0000u, z = 0x10u, zero = 0;
    uint32_t r, t, t0;
    __asm__("andnl %2, %1, %0" : "=r"(r) : "r"((uint32_t)a), "r"((uint32_t)b));
    check(r == 0xf0f00000u, "BMI1 andn");
    /* tzcnt of 0 is 32; without BMI1 the same bytes are bsf, which leaves
       the destination as it was. */
    __asm__("tzcntl %1, %0" : "=r"(t) : "r"((uint32_t)z));
    t0 = 7;
    __asm__("tzcntl %1, %0" : "+r"(t0) : "r"((uint32_t)zero));
    check(t == 4 && t0 == 32, "BMI1 tzcnt");
}

static void probe_bmi2(void)
{
    volatile uint32_t v = 0xabu, m = 0x0f0f0u, w = 0x0a0b0u;
    uint32_t d, x;
    __asm__("pdepl %2, %1, %0" : "=r"(d) : "r"((uint32_t)v), "r"((uint32_t)m));
    __asm__("pextl %2, %1, %0" : "=r"(x) : "r"((uint32_t)w), "r"((uint32_t)m));
    check(d == 0x0a0b0u, "BMI2 pdep");
    check(x == 0xabu, "BMI2 pext");
}

static void probe_f16c(void)
{
    volatile float f = 1.5f;
    float in[4] = {f, f, f, f}, back[4];
    uint16_t h[8];
    __asm__("vmovups %2, %%xmm0\n\t"
            "vcvtps2ph $0, %%xmm0, %%xmm1\n\t"
            "vmovdqu %%xmm1, %0\n\t"
            "vcvtph2ps %%xmm1, %%xmm2\n\t"
            "vmovups %%xmm2, %1"
            : "=m"(h), "=m"(back)
            : "m"(in)
            : "xmm0", "xmm1", "xmm2");
    check(h[0] == 0x3e00 && h[3] == 0x3e00, "F16C vcvtps2ph");
    check(back[0] == 1.5f, "F16C vcvtph2ps");
}

static void probe_lzcnt(void)
{
    /* lzcnt of 1 is 31; without LZCNT the same bytes are bsr, giving 0. */
    volatile uint32_t v = 1, zero = 0;
    uint32_t a, b;
    __asm__("lzcntl %1, %0" : "=r"(a) : "r"((uint32_t)v));
    __asm__("lzcntl %1, %0" : "=r"(b) : "r"((uint32_t)zero));
    check(a == 31 && b == 32, "LZCNT lzcnt");
}

static void probe_movbe(void)
{
    volatile uint32_t v = 0x01020304u;
    uint32_t out;
    __asm__("movbel %1, %0" : "=m"(out) : "r"((uint32_t)v));
    check(out == 0x04030201u, "MOVBE movbe");
}

static void probe_osxsave(void)
{
    /* xgetbv faults unless the OS set CR4.OSXSAVE; XCR0 bits 1 and 2: the
       OS saves the SSE and the AVX state. */
    volatile uint32_t xcr = 0;
    uint32_t lo, hi;

    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"((uint32_t)xcr));
    (void)hi;
    check((lo & 6) == 6, "OSXSAVE xgetbv: XCR0 enables the SSE and AVX state");
}

static void (*const probes[NCLASS])(void) = {
    probe_cx16, probe_lahf, probe_popcnt, probe_sse3, probe_sse4_1, probe_sse4_2,
    probe_ssse3, probe_avx, probe_avx2, probe_bmi, probe_bmi2, probe_f16c,
    probe_fma, probe_lzcnt, probe_movbe, probe_osxsave,
};
#endif

int main(int argc, char **argv)
{
    int expect_v3 = 0;

    (void)check; /* unused where there is nothing to check */
    if (argc == 3 && strcmp(argv[1], "--expect") == 0 && strcmp(argv[2], "x86-64-v3") == 0) {
        expect_v3 = 1;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--expect x86-64-v3]\n", argv[0]);
        return 2;
    }
#ifdef JB_ISA_X86
    {
        int c;

        __builtin_cpu_init();
        for (c = 0; c < NCLASS; ++c) {
            char what[64];

            if (expect_v3 && !compiled[c]) {
                snprintf(what, sizeof what, "compiled for %s (x86-64-v3)", names[c]);
                check(0, what);
                continue;
            }
            if (!compiled[c])
                continue;
            snprintf(what, sizeof what, "compiled for %s; the CPU advertises it", names[c]);
            check(advertised(c), what);
        }
        if (expect_v3)
            check(__builtin_cpu_supports("x86-64-v3"), "the CPU advertises x86-64-v3");
        /* Run only what the CPU advertises: an instruction it lacks would
           kill the test before it could report. */
        if (!failed)
            for (c = 0; c < NCLASS; ++c)
                if (compiled[c])
                    probes[c]();
    }
#else
    if (expect_v3) {
        printf("FAIL: --expect x86-64-v3, and this is not an x86-64 build\n");
        return 1;
    }
#endif
    if (checked == 0)
        printf("no extension beyond the target's baseline to check\n");
    else
        printf("%d checks%s\n", checked, failed ? ", FAILED" : "");
    return failed;
}
