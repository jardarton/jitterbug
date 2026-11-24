/*
 * hal.h on the console, by direct port access, from psx-spx:
 *   GPU I/O Ports: GP0 1F801810h (write: commands; read: GPUREAD), GP1
 *     1F801814h (write: display control; read: GPUSTAT)
 *   DMA Channels: DMA2 MADR 1F8010A0h, BCR 1F8010A4h, CHCR 1F8010A8h, DPCR
 *     1F8010F0h; 01000201h starts a block transfer to the GPU, 01000401h
 *     a linked list
 *   Serial Interfaces: SIO1 DR 1F801050h (8-bit), SR 1F801054h, MR
 *     1F801058h, CR 1F80105Ah, BR 1F80105Eh
 *   Timers: timer 2 COUNT 1F801120h, MODE 1F801124h (bit 9: system clock
 *     divided by 8)
 *   Interrupts: I_STAT 1F801070h (bit 0 VBLANK; write 0 to acknowledge),
 *     I_MASK 1F801074h
 *   BIOS: BFC00100h kernel date, BFC00104h console type, BFC00108h kernel
 *     strings, BFC7FF32h version string
 * The ports are reached through KSEG1 (BF80xxxxh), uncached.
 */
#include "hal.h"

#include "jbp_proto.h"

#define IO32(a) (*(volatile uint32_t *)(0xbf800000u + (a)))
#define IO16(a) (*(volatile uint16_t *)(0xbf800000u + (a)))
#define IO8(a) (*(volatile uint8_t *)(0xbf800000u + (a)))

#define GP0 IO32(0x1810)
#define GP1 IO32(0x1814)
#define SIO_DR IO8(0x1050)
#define SIO_SR IO16(0x1054)
#define SIO_MR IO16(0x1058)
#define SIO_CR IO16(0x105a)
#define SIO_BR IO16(0x105e)
#define I_STAT IO32(0x1070)
#define I_MASK IO32(0x1074)
#define D2_MADR IO32(0x10a0)
#define D2_BCR IO32(0x10a4)
#define D2_CHCR IO32(0x10a8)
#define DPCR IO32(0x10f0)
#define T2_COUNT IO32(0x1120)
#define T2_MODE IO32(0x1124)

/*
 * SIO1 at 115200 baud, 8N1 (docs/probe.md, "Serial line and cable"). psx-spx gives
 * bits per second = 33868800 / MAX((reload * factor) & ~1, factor). With
 * the factor MUL16 (MR bits 0-1 = 2) and reload 18: 33868800 / 288 =
 * 117600 baud, 2.08 % above 115200, inside what an 8N1 receiver takes
 * (a byte's 10 bits drift by 0.2 bit). The other choice, MUL1 and reload
 * 294 (33868800 / 294 = 115200 exactly), samples each bit once; untested,
 * so not the default: build with -DPSX_SIO_FACTOR=1 -DPSX_SIO_RELOAD=294.
 */
#ifndef PSX_SIO_FACTOR
#define PSX_SIO_FACTOR 2
#endif
#ifndef PSX_SIO_RELOAD
#define PSX_SIO_RELOAD 18
#endif
#define SIO_MR_VALUE (0x40u | 0x0cu | PSX_SIO_FACTOR) /* 1 stop bit, 8 bits, no parity */
#define SIO_CR_ON 0x27u                           /* TXEN, DTR, RXEN, RTS */

#ifndef PROBE_DMA_WORDS
#define PROBE_DMA_WORDS (16u * 1024u)
#endif

uint32_t psx_prid(void); /* crt0.S */
void psx_init(void);

static uint32_t clock_last, clock_rem, clock_ms;
static uint32_t sio_errors;
static uint32_t boot_stat;
/* Linked-list nodes for a DMA whose length is not a multiple of 8. */
static uint32_t nodes[PROBE_DMA_WORDS + PROBE_DMA_WORDS / 16 + 2];

void psx_init(void)
{
    I_MASK = 0;
    I_STAT = 0;
    T2_MODE = 0x0200u;
    clock_last = T2_COUNT & 0xffffu;
    SIO_CR = 0x40u; /* reset */
    SIO_MR = SIO_MR_VALUE;
    SIO_BR = PSX_SIO_RELOAD;
    SIO_CR = SIO_CR_ON;
    DPCR |= 0x00000800u; /* DMA2 enable, its priority as it was */
    boot_stat = GP1;
}

uint32_t hal_ms(void)
{
    /* Timer 2 at 33868800 / 8 Hz: 4233.6 ticks a millisecond. */
    uint32_t now = T2_COUNT & 0xffffu, d = (now - clock_last) & 0xffffu;

    clock_last = now;
    clock_rem += d * 10u;
    clock_ms += clock_rem / 42336u;
    clock_rem %= 42336u;
    return clock_ms;
}

void hal_gp0(uint32_t word)
{
    GP0 = word;
}

void hal_gp1(uint32_t word)
{
    GP1 = word;
}

uint32_t hal_gpuread(void)
{
    return GP0;
}

uint32_t hal_gpustat(void)
{
    return GP1;
}

void hal_gp0_dma_start(const uint32_t *words, uint32_t n)
{
    if (n == 0)
        return;
    if (n % 8 == 0) {
        D2_MADR = (uint32_t)words & 0x00ffffffu;
        D2_BCR = (n / 8u) << 16 | 8u;
        D2_CHCR = 0x01000201u;
    } else {
        uint32_t i = 0, k = 0;
        while (i < n) {
            uint32_t m = n - i < 16 ? n - i : 16, j, *node = nodes + k;
            k += 1 + m;
            node[0] = m << 24 | (i + m < n ? ((uint32_t)(nodes + k) & 0x00ffffffu) : 0x00ffffffu);
            for (j = 0; j < m; ++j)
                node[1 + j] = words[i + j];
            i += m;
        }
        D2_MADR = (uint32_t)nodes & 0x00ffffffu;
        D2_BCR = 0;
        D2_CHCR = 0x01000401u;
    }
}

int hal_gp0_dma_busy(void)
{
    return (D2_CHCR & 0x01000000u) != 0; /* bit 24: start/busy */
}

void hal_gp0_dma_stop(void)
{
    D2_CHCR = 0;
}

void hal_vblank_clear(void)
{
    I_STAT = ~1u; /* acknowledge VBLANK, leave the rest */
}

int hal_vblank_seen(void)
{
    return (I_STAT & 1u) != 0;
}

static void sio_check(uint32_t sr)
{
    if (sr & 0x38u) { /* parity, overrun, framing (sticky) */
        ++sio_errors;
        SIO_CR = SIO_CR_ON | 0x10u;
    }
}

int hal_serial_get(uint32_t timeout_ms)
{
    uint32_t t0 = hal_ms();

    for (;;) {
        uint32_t sr = SIO_SR;
        sio_check(sr);
        if (sr & 2u)
            return SIO_DR;
        if (hal_ms() - t0 >= timeout_ms)
            return -1;
    }
}

int hal_serial_put(uint8_t byte)
{
    /* TX needs CTS on (psx-spx: "CTS required for TX"); a few ms for a byte
       that takes 87 us, then it is dropped. */
    uint32_t t0 = hal_ms();

    while (!(SIO_SR & 1u))
        if (hal_ms() - t0 >= 5)
            return -1;
    SIO_DR = byte;
    return 0;
}

uint32_t hal_serial_errors(void)
{
    return sio_errors;
}

void hal_identify(hal_ident *id)
{
    const volatile uint8_t *rom = (const volatile uint8_t *)0xbfc00000u;
    unsigned i;

    id->platform = 1;
    id->cpu_prid = psx_prid();
    id->bios_date = *(const volatile uint32_t *)0xbfc00100u;
    id->console_type = *(const volatile uint32_t *)0xbfc00104u;
    for (i = 0; i < sizeof id->maker; ++i)
        id->maker[i] = (char)rom[0x108 + i];
    for (i = 0; i < sizeof id->version; ++i)
        id->version[i] = (char)rom[0x7ff32 + i];
    id->sio_factor = PSX_SIO_FACTOR;
    id->sio_reload = PSX_SIO_RELOAD;
}

/* A 320 x 240 screen of one colour (docs/probe.md, "First-run checklist"):
   blue idle, yellow idle with CTS off (nothing can be sent), red after a
   session
   that failed. */
void hal_show(int state)
{
    uint32_t colour, pal = boot_stat >> 20 & 1u;

    if (state == HAL_SHOW_ERROR)
        colour = 0x0000ffu;
    else if (!(SIO_SR & 0x100u))
        colour = 0x00ffffu;
    else
        colour = 0xff0000u;
    GP1 = 0x00000000u;
    GP1 = 0x08000001u | pal << 3;            /* 320 wide, 240 lines */
    GP1 = 0x06000000u | 0x260u | 0xc60u << 12; /* horizontal range */
    GP1 = pal ? 0x07000000u | 0x23u | 0x113u << 10 : 0x07000000u | 0x10u | 0x100u << 10;
    GP1 = 0x05000000u;                       /* display at (0, 0) */
    GP0 = 0x02000000u | colour;              /* fill (0, 0), 320 x 240 */
    GP0 = 0x00000000u;
    GP0 = 0x00f00140u;
    GP1 = 0x03000000u;                       /* display on */
}
