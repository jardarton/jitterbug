/*
 * The smallest stand-in for psxstack's psyq/psyq_internal.h that lets its
 * psyq/gpu.c compile on the host as a reference (tools/fuzz-diff;
 * docs/testing.md, "Differential fuzzing"). Written here from what gpu.c
 * uses; nothing of psxstack is in
 * this repository. ref.c defines the few symbols declared below that
 * gpu.c refers to and that live elsewhere in psxstack.
 */
#ifndef FUZZ_DIFF_PSYQ_INTERNAL_H
#define FUZZ_DIFF_PSYQ_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;

typedef struct {
    int x, y, r, g, b, u, v, fx, fy, z;
} GpuVertex;

typedef enum {
    GPU_EV_TRIANGLE,
    GPU_EV_RECT,
    GPU_EV_SEGMENT,
    GPU_EV_FILL,
    GPU_EV_COPY,
    GPU_EV_LOAD,
    GPU_EV_POWER_ON
} GpuEventKind;

typedef struct {
    GpuEventKind kind;
    GpuVertex v[3];
    int textured, raw, semi, abr, gouraud, dither;
    int tex_x, tex_y, clut_x, clut_y;
    int u_and, u_or, v_and, v_or;
    int area_x0, area_y0, area_x1, area_y1;
    int set_mask, check_mask;
    int x, y, w, h, sx, sy;
    u16 color;
} GpuEvent;

/* Save states are not used here. */
typedef struct PortState PortState;
void port_state_bytes(PortState *s, const char *name, void *p, size_t n);
int port_state_loading(PortState *s);

/* psxstack's sub-pixel shadow of the GTE: always off here. */
extern int psyq_gte_shadow_on;
int psyq_gte_shadow_find(const void *src, u32 word, int *fx, int *fy, int *z);

/* What gpu.c defines and ref.c calls. */
void gpu_power_on(void);
void gpu_gp0_words(const u32 *w, u32 n);
const u16 *gpu_vram_pixels(void);
void gpu_set_listener(void (*listener)(const GpuEvent *ev));
const u32 *gpu_block_stamps(void);
u32 gpu_stamp_bump(void);
u32 gpu_stamp_generation(void);
void gpu_draw_state(u32 *e1, u32 *e3, u32 *e4, u32 *e5);
void gpu_vram_extend(void);
int gpu_vram_rows(void);
void gpu_reset_state(void);
void gpu_gp0_write(u32 word);
void gpu_load_image(int x, int y, int w, int h, const u16 *pixels);
void gpu_segment_walk(const GpuVertex *a, const GpuVertex *b, int gouraud,
                      void (*pixel)(void *ctx, int x, int y, int r, int g, int b), void *ctx);
void gpu_state(PortState *s);

#endif
