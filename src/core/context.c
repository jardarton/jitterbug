/*
 * Contexts (context.h): creation, configuration and the calls that only
 * pass through to the backend.
 */
#include "core/context.h"

#include <string.h>

#include "soft/soft.h"

void jbi_config_default(jbi_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->scale = 1;
    cfg->dither = -1;
    cfg->threads = 1;
    cfg->persp_span = JBI_PERSP_SPAN;
    cfg->persp_span_1x = JBI_PERSP_SPAN_1X;
}

jb_context *jbi_context_create(const jbi_config *cfg)
{
    jbi_config defaults;
    jb_context *ctx;

    if (cfg == NULL) {
        jbi_config_default(&defaults);
        cfg = &defaults;
    }
    ctx = jbi_calloc(&cfg->alloc, 1, sizeof *ctx);
    if (ctx == NULL)
        return NULL;
    ctx->alloc = cfg->alloc;
    ctx->backend = jbi_soft_create(cfg, &ctx->scale);
    if (ctx->backend == NULL) {
        jbi_free(&cfg->alloc, ctx);
        return NULL;
    }
    ctx->texture_perspective = cfg->texture_perspective != 0;
    ctx->clut_cache = 1;
    jbi_set_precise_source(ctx, cfg->precise);
    return ctx;
}

void jbi_context_destroy(jb_context *ctx)
{
    jbi_allocator alloc;

    if (ctx == NULL)
        return;
    alloc = ctx->alloc;
    ctx->backend->destroy(ctx->backend);
    jbi_free(&alloc, ctx);
}

jbi_backend *jbi_context_backend(jb_context *ctx)
{
    return ctx->backend;
}

void jbi_flush(jb_context *ctx)
{
    ctx->backend->flush(ctx->backend);
}

int jbi_configure(jb_context *ctx, int scale, int dither, int threads)
{
    ctx->scale = ctx->backend->configure(ctx->backend, scale, dither, threads);
    return ctx->scale;
}

int jbi_threads(const jb_context *ctx)
{
    return ctx->backend->threads(ctx->backend);
}

int jbi_set_threads(jb_context *ctx, int threads)
{
    return ctx->backend->set_threads(ctx->backend, threads);
}

int jbi_scale(const jb_context *ctx)
{
    return ctx->scale;
}

void jbi_set_exact_1x(jb_context *ctx, int exact)
{
    ctx->backend->set_exact_1x(ctx->backend, exact);
}

void jbi_set_clut_cache(jb_context *ctx, int on)
{
    ctx->backend->set_clut_cache(ctx->backend, on != 0);
    ctx->clut_cache = on != 0;
}

int jbi_clut_cache(const jb_context *ctx)
{
    return ctx->clut_cache;
}

void jbi_clear_cache(jb_context *ctx)
{
    ctx->backend->clear_cache(ctx->backend);
}

void jbi_set_texture_mapping(jb_context *ctx, int perspective)
{
    ctx->backend->flush(ctx->backend);
    ctx->texture_perspective = perspective != 0;
}

int jbi_texture_mapping(const jb_context *ctx)
{
    return ctx->texture_perspective;
}

void jbi_set_precise_source(jb_context *ctx, const jbi_precise_source *src)
{
    ctx->has_precise = src != NULL;
    if (src != NULL)
        ctx->precise = *src;
    else
        memset(&ctx->precise, 0, sizeof ctx->precise);
}

/* Up to 1024N 15-bit pixels or 682N 24-bit pixels per row, 512N rows. */
size_t jbi_scanout_bytes(const jb_context *ctx)
{
    size_t n = (size_t)ctx->scale;

    return (size_t)JBI_VRAM_H * JBI_VRAM_W * 2 * n * n;
}

int jbi_scanout(jb_context *ctx, int x, int y, int w, int h, int rgb24, int enabled,
                void *pixels, int *width, int *height, int *stride)
{
    return ctx->backend->scanout(ctx->backend, x, y, w, h, rgb24, enabled, pixels, width, height,
                                 stride);
}
