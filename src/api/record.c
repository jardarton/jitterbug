/*
 * Records and the ordering table (record.h), and the public jb_ot_* calls
 * (include/jitterbug.h, The ordering table).
 */
#include <string.h>

#include "api/record.h"
#include "api/texture.h"
#include "core/alloc.h"

uint32_t jbi_record_window(const jb_texture *t)
{
    int mx, my, ox, oy;

    if (t == NULL)
        return JBI_NO_WINDOW;
    jbi_texture_window(t, &mx, &my, &ox, &oy);
    return JBI_NO_WINDOW | (uint32_t)mx | (uint32_t)my << 5 | (uint32_t)ox << 10 |
           (uint32_t)oy << 15;
}

void jbi_record_draw(jb_context *jb, const jbi_record *r)
{
    jbi_subpixel sp;
    const jbi_subpixel *s = NULL;
    unsigned skip = 0;

    if (r->sub && r->passes != 0) {
        /* Which passes may draw this piece (record.h, passes). */
        int scaled = jbi_scale(jb) > 1;
        int precise = scaled && jb->api->precise_vertices;
        int in_1x = (r->passes & JBI_SPLIT_PASS_1X) != 0;
        int in_scaled = (r->passes & (precise ? JBI_SPLIT_PASS_PRECISE : JBI_SPLIT_PASS_1X)) != 0;

        if (!in_1x && (!scaled || !in_scaled))
            return;
        skip = (in_1x ? 0u : JBI_F_NO_1X) | (in_scaled ? 0u : JBI_F_NO_SCALED);
    }

    /* The window as set: the stream keeps the last GP0(E2h) as sent
       (api.h, env), which is what the drawing state follows. */
    if (r->window && (r->tw & 0xfffffu) != jb->api->env[0]) {
        uint32_t w = 0xe2000000u | (r->tw & 0xfffffu);

        jbi_api_command(jb, &w, 1);
    }
    if (r->sub) {
        sp = r->sp;
        sp.positions = jb->api->precise_vertices;
        sp.skip = skip;
        s = &sp;
    }
    switch (r->kind) {
    case JBI_REC_POLY: {
        jb_poly p = r->u.poly;

        p.texture = r->textured ? &r->tex : NULL;
        jbi_emit_poly(jb, &p, s);
        break;
    }
    case JBI_REC_LINE:
        jbi_emit_line(jb, &r->u.line, s);
        break;
    case JBI_REC_RECT: {
        jb_rect q = r->u.rect;

        q.texture = r->textured ? &r->tex : NULL;
        jbi_emit_rect(jb, &q);
        break;
    }
    case JBI_REC_STATE:
        jbi_api_command(jb, &r->u.word, 1);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* The table */

jb_ot *jbi_ot_create(jb_context *jb, int buckets, int max_records)
{
    jb_ot *ot = jbi_calloc(&jb->alloc, 1, sizeof *ot);

    if (ot == NULL)
        return NULL;
    ot->jb = jb;
    ot->buckets = buckets;
    ot->max_records = max_records;
    ot->head = jbi_calloc(&jb->alloc, (size_t)buckets, sizeof *ot->head);
    ot->tail = jbi_calloc(&jb->alloc, (size_t)buckets, sizeof *ot->tail);
    ot->rec = jbi_calloc(&jb->alloc, (size_t)max_records, sizeof *ot->rec);
    if (ot->head == NULL || ot->tail == NULL || ot->rec == NULL) {
        jbi_ot_destroy(ot);
        return NULL;
    }
    jbi_ot_clear(ot);
    return ot;
}

void jbi_ot_destroy(jb_ot *ot)
{
    jbi_allocator a;

    if (ot == NULL)
        return;
    a = ot->jb->alloc;
    jbi_free(&a, ot->head);
    jbi_free(&a, ot->tail);
    jbi_free(&a, ot->rec);
    jbi_free(&a, ot);
}

void jbi_ot_clear(jb_ot *ot)
{
    int i;

    for (i = 0; i < ot->buckets; ++i)
        ot->head[i] = ot->tail[i] = -1;
    ot->count = 0;
}

jbi_record *jbi_ot_add(jb_ot *ot, int bucket)
{
    int32_t k;
    jbi_record *r;

    if (ot->count >= ot->max_records)
        return NULL;
    k = ot->count++;
    r = &ot->rec[k];
    memset(r, 0, sizeof *r);
    r->next = -1;
    if (ot->tail[bucket] < 0)
        ot->head[bucket] = k;
    else
        ot->rec[ot->tail[bucket]].next = k;
    ot->tail[bucket] = k;
    return r;
}

void jbi_ot_draw(jb_context *jb, const jb_ot *ot)
{
    int b;

    for (b = ot->buckets - 1; b >= 0; --b) {
        int32_t k;

        for (k = ot->head[b]; k >= 0; k = ot->rec[k].next)
            jbi_record_draw(jb, &ot->rec[k]);
    }
}

/* The drawing area as e3 and e4, sent only where it is not that already
   (api.h, env: the words as last sent, without their command byte). */
static void set_area(jb_context *jb, uint32_t e3, uint32_t e4)
{
    uint32_t *env = jb->api->env;

    if ((e3 & 0xfffffu) != env[1])
        jbi_api_command(jb, &e3, 1);
    if ((e4 & 0xfffffu) != env[2])
        jbi_api_command(jb, &e4, 1);
}

void jbi_ot_draw_area(jb_context *jb, const jb_ot *ot, uint32_t e3, uint32_t e4)
{
    int b;

    for (b = ot->buckets - 1; b >= 0; --b) {
        int32_t k;

        for (k = ot->head[b]; k >= 0; k = ot->rec[k].next) {
            const jbi_record *r = &ot->rec[k];

            set_area(jb, r->area ? r->e3 : e3, r->area ? r->e4 : e4);
            jbi_record_draw(jb, r);
        }
    }
    set_area(jb, e3, e4);
}

/* ------------------------------------------------------------------------ */
/* The public calls */

enum { MAX_BUCKETS = 1 << 20, MAX_RECORDS = 1 << 24 };

jb_ot *jb_ot_create(jb_context *jb, int buckets, int max_records, jb_result *result)
{
    jb_ot *ot;
    jb_result r = JB_ERROR_INVALID_ARGUMENT;

    if (jb == NULL)
        goto done;
    if (buckets < 1 || buckets > MAX_BUCKETS) {
        r = jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_ot_create: buckets %d is outside 1..%d",
                         buckets, MAX_BUCKETS);
        goto done;
    }
    if (max_records < 1 || max_records > MAX_RECORDS) {
        r = jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                         "jb_ot_create: max_records %d is outside 1..%d", max_records, MAX_RECORDS);
        goto done;
    }
    ot = jbi_ot_create(jb, buckets, max_records);
    if (ot == NULL) {
        r = jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                         "jb_ot_create: no memory for %d buckets and %d records", buckets,
                         max_records);
        goto done;
    }
    if (result != NULL)
        *result = JB_OK;
    return ot;
done:
    if (result != NULL)
        *result = r;
    return NULL;
}

void jb_ot_destroy(jb_ot *ot)
{
    jbi_ot_destroy(ot);
}

jb_result jb_ot_clear(jb_ot *ot)
{
    if (ot == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    jbi_ot_clear(ot);
    return JB_OK;
}

/* A record in bucket, or NULL with the failure recorded (bucket out of
   range, the table full). */
static jbi_record *add(jb_ot *ot, const char *fn, int bucket, jb_result *r)
{
    jbi_record *rec;

    if (bucket < 0 || bucket >= ot->buckets) {
        *r = jbi_api_fail(ot->jb, JB_ERROR_INVALID_ARGUMENT, "%s: bucket %d is outside 0..%d", fn,
                          bucket, ot->buckets - 1);
        return NULL;
    }
    rec = jbi_ot_add(ot, bucket);
    if (rec == NULL)
        *r = jbi_api_fail(ot->jb, JB_ERROR_OUT_OF_MEMORY,
                          "%s: the table is full (%d records); make it with a larger max_records",
                          fn, ot->max_records);
    return rec;
}

static void copy_texture(jbi_record *rec, const jb_texture *t)
{
    if (t != NULL) {
        rec->textured = 1;
        rec->tex = *t;
    }
}

jb_result jb_ot_add_poly(jb_ot *ot, int bucket, const jb_poly *poly)
{
    jbi_record *rec;
    jb_result r;

    if (ot == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (poly == NULL)
        return jbi_api_fail(ot->jb, JB_ERROR_INVALID_ARGUMENT, "jb_ot_add_poly: poly is NULL");
    r = jbi_check_poly(ot->jb, "jb_ot_add_poly", poly, 1);
    if (r != JB_OK)
        return r;
    rec = add(ot, "jb_ot_add_poly", bucket, &r);
    if (rec == NULL)
        return r;
    rec->kind = JBI_REC_POLY;
    rec->u.poly = *poly;
    rec->u.poly.texture = NULL;
    copy_texture(rec, poly->texture);
    return JB_OK;
}

jb_result jb_ot_add_line(jb_ot *ot, int bucket, const jb_line *line)
{
    jbi_record *rec;
    jb_result r;

    if (ot == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (line == NULL)
        return jbi_api_fail(ot->jb, JB_ERROR_INVALID_ARGUMENT, "jb_ot_add_line: line is NULL");
    r = jbi_check_line(ot->jb, "jb_ot_add_line", line->gouraud, line->blend, line->v, 2, 1);
    if (r != JB_OK)
        return r;
    rec = add(ot, "jb_ot_add_line", bucket, &r);
    if (rec == NULL)
        return r;
    rec->kind = JBI_REC_LINE;
    rec->u.line = *line;
    return JB_OK;
}

jb_result jb_ot_add_polyline(jb_ot *ot, int bucket, const jb_polyline *line)
{
    jb_result r;
    size_t i;

    if (ot == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (line == NULL)
        return jbi_api_fail(ot->jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_ot_add_polyline: line is NULL");
    r = jbi_check_polyline(ot->jb, "jb_ot_add_polyline", line, 1);
    if (r != JB_OK)
        return r;
    if (bucket < 0 || bucket >= ot->buckets)
        return jbi_api_fail(ot->jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_ot_add_polyline: bucket %d is outside 0..%d", bucket,
                            ot->buckets - 1);
    if (line->count - 1 > (size_t)(ot->max_records - ot->count))
        return jbi_api_fail(ot->jb, JB_ERROR_OUT_OF_MEMORY,
                            "jb_ot_add_polyline: %zu segments do not fit in the table (%d of %d "
                            "records used); make it with a larger max_records",
                            line->count - 1, ot->count, ot->max_records);
    for (i = 0; i + 1 < line->count; ++i) {
        jbi_record *rec = jbi_ot_add(ot, bucket);

        rec->kind = JBI_REC_LINE;
        rec->u.line = jbi_polyline_segment(line, i);
    }
    return JB_OK;
}

jb_result jb_ot_add_rect(jb_ot *ot, int bucket, const jb_rect *rect)
{
    jbi_record *rec;
    jb_result r;

    if (ot == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (rect == NULL)
        return jbi_api_fail(ot->jb, JB_ERROR_INVALID_ARGUMENT, "jb_ot_add_rect: rect is NULL");
    r = jbi_check_rect(ot->jb, "jb_ot_add_rect", rect, 1);
    if (r != JB_OK)
        return r;
    rec = add(ot, "jb_ot_add_rect", bucket, &r);
    if (rec == NULL)
        return r;
    rec->kind = JBI_REC_RECT;
    rec->u.rect = *rect;
    rec->u.rect.texture = NULL;
    copy_texture(rec, rect->texture);
    return JB_OK;
}

jb_result jb_ot_draw(jb_context *jb, const jb_ot *ot)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (ot == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_ot_draw: ot is NULL");
    if (ot->jb != jb)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_ot_draw: the table was made with another context");
    jbi_ot_draw(jb, ot);
    return JB_OK;
}
