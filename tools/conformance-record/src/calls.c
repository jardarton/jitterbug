/*
 * ps1-tests' common GPU library (common/gpu.c, later gpu.cpp) is compiled
 * unmodified, from the commit each program is recorded from
 * (CMakeLists.txt), with clang's -finstrument-functions: clang calls the
 * two hooks below when each of its functions is entered and when it
 * returns. They give the library's helpers what the recorder's own SDK
 * calls do themselves:
 *
 *   - a name for the CALL op (rec_enter, rec_leave), the helper's own,
 *     found by its address in the table below; a function of the library
 *     that is not a helper (setResolution) is only ever called by one;
 *   - on return, the delivery of a port store the helper has just made
 *     (writeGP1 stores to GP1 itself; mmio.c), so that the word is
 *     recorded inside the helper's call;
 *   - for ReadGPUstat (gpustat.c), whether the helper that calls it polls
 *     GPUSTAT until some bits are set: vramGet and vramReadDMA loop until
 *     bit 27 (ready to send VRAM to the CPU) is, which is one WAIT op.
 *
 * Every helper of every version is in the table; those a version lacks are
 * weak references, null when it is linked.
 */
#include "rec.h"

#include <stddef.h>
#include <stdint.h>

#define JBREC_HELPERS(X)                                                                 \
    X(initVideo, 0) X(fillRect, 0) X(clearScreenColor, 0) X(clearScreen, 0)              \
    X(setMaskBitSetting, 0) X(gpuNop, 0) X(writeGP0, 0) X(writeGP1, 0) X(readGPU, 0)     \
    X(vramPut, 0) X(vramGet, 0x08000000u) X(vramReadDMA, 0x08000000u) X(vramWrite, 0)    \
    X(vramWriteDMA, 0) X(vramToVramCopy, 0)

/* Only the address of each is used; the type is not theirs. */
#define JBREC_DECLARE(name, polls) void name(void) __attribute__((weak));
JBREC_HELPERS(JBREC_DECLARE)

typedef struct {
    const char *name;
    void (*fn)(void);
    uint32_t polls; /* GPUSTAT bits it waits for with ReadGPUstat, or 0 */
} helper;

#define JBREC_ENTRY(name, polls) {#name, name, polls},
static const helper helpers[] = {JBREC_HELPERS(JBREC_ENTRY)};

/* The library's calls in progress; NULL for a function that is not a
   helper. */
enum { MAX_DEPTH = 64 };
static const helper *active[MAX_DEPTH];
static unsigned depth;

void __cyg_profile_func_enter(void *fn, void *site);
void __cyg_profile_func_exit(void *fn, void *site);

static const helper *find(void *fn)
{
    size_t i;

    for (i = 0; i < sizeof helpers / sizeof helpers[0]; ++i)
        if (helpers[i].fn != NULL && (uintptr_t)helpers[i].fn == (uintptr_t)fn)
            return &helpers[i];
    return NULL;
}

void __cyg_profile_func_enter(void *fn, void *site)
{
    const helper *h = find(fn);

    (void)site;
    if (h == NULL && depth == 0)
        rec_fail("the program called a function of ps1-tests' common library that is not "
                 "one of its helpers (at %p)",
                 fn);
    if (depth == MAX_DEPTH)
        rec_fail("ps1-tests' common library: calls nested too deep");
    active[depth++] = h;
    rec_enter(h != NULL ? h->name : "ps1-tests common");
}

void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn;
    (void)site;
    if (depth == 0)
        rec_fail("ps1-tests' common library: a return without a call");
    mmio_flush();
    rec_leave();
    --depth;
}

uint32_t rec_poll_bits(void)
{
    const helper *h = depth > 0 ? active[depth - 1] : NULL;

    return h != NULL ? h->polls : 0;
}
