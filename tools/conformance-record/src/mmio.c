/*
 * The GPU's ports at their console addresses. A program that writes
 * *(volatile uint32_t *)0x1F801810 itself (ps1-tests' clut-cache does)
 * must work unchanged, so the page holding the I/O registers is mapped at
 * that address, and the program's own sources are compiled with clang's
 * -fsanitize-coverage=trace-loads,trace-stores (CMakeLists.txt), which
 * calls the hooks below before every load and store they make.
 *
 * A store hook runs before the store, so it cannot see the value: it
 * remembers the port, and the value is taken from the page at the next
 * hook or recorder call (mmio_flush), when the store has happened. A load
 * hook writes the value to be read into the page first. Only 32-bit
 * accesses to GP0/GPUREAD (1F801810h) and GP1/GPUSTAT (1F801814h) are
 * modelled; any other access to the console's 4 KiB I/O page (1F801000h
 * to 1F801FFFh) stops the recording.
 *
 * The host's pages may be larger than 4 KiB (16 or 64 KiB on some arm64
 * kernels), so the mapping is the host pages that cover the I/O page,
 * from the host page boundary at or below 1F801000h. An access to the
 * rest of that mapping stops the recording as well.
 */
#include "rec.h"

#include <jbc_format.h>

#include <sys/mman.h>
#include <unistd.h>

#define IO_PAGE 0x1f801000u
#define IO_PAGE_SIZE 0x1000u
#define IO_GP0 0x1f801810u
#define IO_GP1 0x1f801814u

static volatile uint32_t *pending;
static uintptr_t map_start, map_end;

static void bad(const void *p, const char *what);

/* 1 for an address in the console's I/O page, 0 for one outside the
   mapping; one in the mapping but not in the I/O page stops the
   recording. */
static int in_page(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    if (a < map_start || a >= map_end)
        return 0;
    if (a < IO_PAGE || a >= IO_PAGE + IO_PAGE_SIZE)
        bad(p, "an access next to the I/O page");
    return 1;
}

__attribute__((constructor)) static void mmio_init(void)
{
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t size, start, end;
    void *p;

    if (page <= 0 || (page & (page - 1)) != 0)
        rec_fail("the host's page size (%ld) is not a power of two", page);
    size = (uintptr_t)page;
    start = (uintptr_t)IO_PAGE & ~(size - 1);
    end = ((uintptr_t)IO_PAGE + IO_PAGE_SIZE + size - 1) & ~(size - 1);
    p = mmap((void *)start, end - start, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)start)
        rec_fail("cannot map 0x%lx-0x%lx (the host pages holding the I/O page at 0x%x)",
                 (unsigned long)start, (unsigned long)end, IO_PAGE);
    map_start = start;
    map_end = end;
}

void mmio_flush(void)
{
    volatile uint32_t *p = pending;
    uint32_t v;

    if (p == NULL)
        return;
    pending = NULL;
    v = *p;
    if ((uintptr_t)p == IO_GP0) {
        rec_enter("GP0 port");
        rec_gp0(&v, 1, 0);
    } else {
        rec_enter("GP1 port");
        rec_gp1(v);
    }
    rec_leave();
}

static void bad(const void *p, const char *what)
{
    rec_fail("%s at 0x%lx: only 32-bit accesses to GP0 and GP1 are modelled", what,
             (unsigned long)(uintptr_t)p);
}

/* The hooks clang calls (its SanitizerCoverage interface). */
void __sanitizer_cov_8bit_counters_init(char *start, char *end);
void __sanitizer_cov_load1(uint8_t *p);
void __sanitizer_cov_load2(uint16_t *p);
void __sanitizer_cov_load4(uint32_t *p);
void __sanitizer_cov_load8(uint64_t *p);
void __sanitizer_cov_load16(void *p);
void __sanitizer_cov_store1(uint8_t *p);
void __sanitizer_cov_store2(uint16_t *p);
void __sanitizer_cov_store4(uint32_t *p);
void __sanitizer_cov_store8(uint64_t *p);
void __sanitizer_cov_store16(void *p);

void __sanitizer_cov_8bit_counters_init(char *start, char *end)
{
    (void)start;
    (void)end;
}

#define OTHER(name, type, what)        \
    void name(type p)                  \
    {                                  \
        mmio_flush();                  \
        if (in_page(p))                \
            bad(p, what);              \
    }
OTHER(__sanitizer_cov_load1, uint8_t *, "an 8-bit load")
OTHER(__sanitizer_cov_load2, uint16_t *, "a 16-bit load")
OTHER(__sanitizer_cov_load8, uint64_t *, "a 64-bit load")
OTHER(__sanitizer_cov_load16, void *, "a 128-bit load")
OTHER(__sanitizer_cov_store1, uint8_t *, "an 8-bit store")
OTHER(__sanitizer_cov_store2, uint16_t *, "a 16-bit store")
OTHER(__sanitizer_cov_store8, uint64_t *, "a 64-bit store")
OTHER(__sanitizer_cov_store16, void *, "a 128-bit store")

void __sanitizer_cov_load4(uint32_t *p)
{
    mmio_flush();
    if (!in_page(p))
        return;
    if ((uintptr_t)p == IO_GP0) {
        rec_enter("GPUREAD port");
        *(volatile uint32_t *)p = rec_read(JBC_PORT_GPUREAD);
    } else if ((uintptr_t)p == IO_GP1) {
        rec_enter("GPUSTAT port");
        *(volatile uint32_t *)p = rec_read(JBC_PORT_GPUSTAT);
    } else
        bad(p, "a load");
    rec_leave();
}

void __sanitizer_cov_store4(uint32_t *p)
{
    mmio_flush();
    if (!in_page(p))
        return;
    if ((uintptr_t)p != IO_GP0 && (uintptr_t)p != IO_GP1)
        bad(p, "a store");
    pending = p;
}
