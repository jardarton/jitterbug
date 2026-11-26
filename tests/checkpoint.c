/*
 * Roll back immediate and queued drawing, transfers, and 1x resolves.
 * Check that configuration changes are rejected during a checkpoint.
 */
#include <stdio.h>
#include <stdlib.h>

#include "core/context.h"
#include "soft/soft.h"

enum { RED = 0x001f, GREEN = 0x03e0, BLUE = 0x7c00 };

static void check(int scale, const char *label, int got, int want)
{
    if (got == want)
        return;
    fprintf(stderr, "%dx %s: got %d, want %d\n", scale, label, got, want);
    exit(1);
}

static uint16_t pixel(jb_context *ctx, int x, int y)
{
    uint16_t value;

    jbi_store(ctx, x, y, 1, 1, &value);
    return value;
}

static void run(int scale)
{
    const uint32_t original[] = {0xe3000000u, 0xe407ffffu, 0xe5000001u,
                                 0x68ff0000u, 96u | 96u << 16};
    const uint32_t temporary[] = {0xe5000004u, 0x6800ff00u, 32u | 32u << 16};
    const uint16_t upload[] = {GREEN, BLUE};
    jbi_config cfg;
    jb_context *ctx;
    const uint16_t *raw;
    uint16_t resolved;

    jbi_config_default(&cfg);
    cfg.scale = scale;
    ctx = jbi_context_create(&cfg);
    check(scale, "context creation", ctx != NULL, 1);
    raw = jbi_soft_vram_raw(jbi_soft_from(jbi_context_backend(ctx)));
    jbi_fill(ctx, 32, 32, 16, 1, 0xffu);
    jbi_gp0(ctx, original, sizeof original / sizeof original[0]);
    check(scale, "begin", jbi_checkpoint_begin(ctx), JBI_CHECKPOINT_OK);
    check(scale, "nested begin", jbi_checkpoint_begin(ctx), JBI_CHECKPOINT_ACTIVE);
    check(scale, "scale guard", jbi_configure(ctx, scale + 1, 0, 2), scale);
    check(scale, "thread guard", jbi_set_threads(ctx, 2), 0);
    jbi_set_clut_cache(ctx, 0);
    check(scale, "palette mode guard", jbi_clut_cache(ctx), 1);
    jbi_set_texture_mapping(ctx, 1);
    check(scale, "texture mapping guard", jbi_texture_mapping(ctx), 0);
    jbi_set_exact_1x(ctx, 1);

    jbi_gp0(ctx, temporary, sizeof temporary / sizeof temporary[0]);
    check(scale, "temporary draw", pixel(ctx, 36, 32), GREEN);
    check(scale, "1x rasterization guard", raw[32 * JBI_VRAM_W + 36],
          scale > 1 ? RED : GREEN);
    jbi_store_1x(ctx, 97, 96, 1, 1, &resolved);
    check(scale, "earlier draw pending when checkpoint began", resolved, BLUE);
    jbi_fill(ctx, 64, 64, 16, 1, 0xffu);
    jbi_load(ctx, JBI_VRAM_W - 1, 511, 2, 1, upload);
    jbi_copy(ctx, JBI_VRAM_W - 1, 511, 120, 120, 2, 1);
    check(scale, "temporary fill", pixel(ctx, 64, 64), RED);
    check(scale, "temporary wrapped upload", pixel(ctx, 0, 511), BLUE);
    check(scale, "temporary copy", pixel(ctx, 121, 120), BLUE);

    jbi_checkpoint_rollback(ctx);
    check(scale, "drawing offset restored", ctx->draw.off_x, 1);
    check(scale, "original pixel restored", pixel(ctx, 36, 32), RED);
    check(scale, "fill restored", pixel(ctx, 64, 64), 0);
    check(scale, "wrapped upload restored", pixel(ctx, 0, 511), 0);
    check(scale, "copy restored", pixel(ctx, 121, 120), 0);
    check(scale, "raw 1x before resolve", raw[96 * JBI_VRAM_W + 97], scale > 1 ? 0 : BLUE);
    jbi_store_1x(ctx, 97, 96, 1, 1, &resolved);
    check(scale, "stale tile restored", resolved, BLUE);

    check(scale, "scale change after rollback", jbi_configure(ctx, scale + 1, 0, 1), scale + 1);
    check(scale, "begin after scale change", jbi_checkpoint_begin(ctx), JBI_CHECKPOINT_OK);
    jbi_checkpoint_rollback(ctx);
    jbi_context_destroy(ctx);
}

int main(void)
{
    run(1);
    run(3);
    return 0;
}
