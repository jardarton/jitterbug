/*
 * The software backend (backend.h): the reference renderer. It owns both
 * VRAMs (1x and scaled), the primitive queue, the tile bookkeeping, the
 * worker pool and the scanout; see soft.c.
 *
 * jbi_soft_create is the factory the context uses. Everything after it
 * reaches into this backend's memory and exists for tests and tools only
 * (the trace replayer hashes and pokes VRAM, and replays calls the seed's
 * test made on these internals); the core never calls it, and a GPU
 * backend has no equivalent.
 */
#ifndef JBI_SOFT_SOFT_H
#define JBI_SOFT_SOFT_H

#include <stdint.h>

#include "core/backend.h"
#include "core/config.h"
#include "core/prim.h"

typedef struct jbi_soft jbi_soft;

/*
 * A software backend configured from cfg (scale, dither, threads,
 * exact_1x, persp_span, persp_span_1x, log), with both VRAMs zero; NULL
 * without memory. *scale gets the scale in effect.
 */
jbi_backend *jbi_soft_create(const jbi_config *cfg, int *scale);

/* ------------------------------------------------------------------------ */
/* Tests and tools only: the software backend's memory and internals. */

/* The backend as a software one, or NULL if it is another kind. */
jbi_soft *jbi_soft_from(jbi_backend *b);

/* The 1x VRAM, 1024 x 512 row-major, as it is: queued primitives are not
   finished and stale tiles not filled in. Writable. */
uint16_t *jbi_soft_vram_raw(jbi_soft *s);
/* The scaled VRAM, 1024N x 512N row-major, as it is, or NULL at 1x. */
uint16_t *jbi_soft_scaled_raw(jbi_soft *s);

/* The 1x VRAM after finishing queued primitives and filling in every
   stale tile (soft.c), as a program reading it would see it. */
uint16_t *jbi_soft_vram(jbi_soft *s);
/* After finishing queued primitives: the scaled VRAM at the internal
   resolution, 1024N x 512N, or the 1x VRAM at 1x. */
uint16_t *jbi_soft_hires_vram(jbi_soft *s, int *width, int *height);
/* After writing jbi_soft_vram directly: finishes queued primitives and
   copies the 1x VRAM into the scaled VRAM. */
void jbi_soft_vram_changed(jbi_soft *s);

/* Fills the scaled VRAM from the 1x VRAM (nothing at 1x). */
void jbi_soft_replicate(jbi_soft *s);
/* Stale 1x tiles in a rectangle, which may wrap (soft.c): mark them (when
   1x drawing is skipped), or fill them in from the scaled VRAM. */
void jbi_soft_lowres_mark(jbi_soft *s, int x, int y, int w, int h);
void jbi_soft_lowres_resolve(jbi_soft *s, int x, int y, int w, int h);

/* The perspective spans in use (jbi_config). */
void jbi_soft_persp_spans(const jbi_soft *s, int *span, int *span_1x);

/* The record the backend queues for a fill (backend.h, fill), for a test
   that every byte of it is determined by the arguments. */
void jbi_soft_fill_record(jbi_prim *p, int x, int y, int w, int h, uint16_t color);

#endif
