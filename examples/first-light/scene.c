/*
 * first-light: the scene (scene.h), on the friendly level. C99, the public
 * API only, the library's maths only (jb_sinf, jb_cosf: the same bits on
 * every machine). The world is right-handed, y up: the camera stands at
 * the origin looking down -z, the cube 700 units away above a floor 200
 * below the eye. Angles in the scene's motion are in turns.
 */
#include "scene.h"

#include <string.h>

enum { TEX = 64, CUBE = 128, CUBE_Z = 700, FLOOR_Y = -200, ORBIT = 260, RING = 16 };
enum { STAR_ROW = 48, SEMI = 0x8000 }; /* the star texture: 32 x 32 and 16 x 16 */
#define TURN 6.2831853f                /* radians */
#define SN(turns) jb_sinf((turns) * TURN)
#define CS(turns) jb_cosf((turns) * TURN)
#define IABS(a) ((a) < 0 ? -(a) : (a))

/* A small integer hash, for the floor's speckle. */
static unsigned hash2(unsigned x, unsigned y)
{
    unsigned h = x * 73856093u ^ y * 19349663u;
    return (h ^ h >> 13) * 0x5bd1e995u >> 24;
}

static jb_result texture(fl_scene *s, jb_texture *t, int w, int h, int depth, const void *texels,
                         const uint16_t *palette, int repeat)
{
    jb_texture_desc d = {w, h, depth, texels, palette, repeat};

    return jb_texture_create(s->jb, &d, t);
}

/* The textures, drawn in code, placed in VRAM by the library. */
static jb_result make_textures(fl_scene *s)
{
    /* The floor's stone, light (1-4) and dark (8-11), and grout (15);
       0x8000 is opaque black (0x0000 would be transparent). */
    static const uint16_t floor_pal[16] = {
        0x8000, JB_RGB15(22, 21, 19), JB_RGB15(23, 22, 20), JB_RGB15(21, 20, 18),
        JB_RGB15(24, 23, 21), 0x8000, 0x8000, 0x8000, JB_RGB15(9, 10, 13),
        JB_RGB15(10, 11, 14), JB_RGB15(8, 9, 12), JB_RGB15(11, 12, 15), 0x8000, 0x8000, 0x8000,
        JB_RGB15(4, 4, 5)};
    uint8_t t8[TEX * TEX];
    uint16_t t15[TEX * TEX], pal[16];
    jb_result r;
    int u, v, i;

    /* Floor: a 4-bit 2 x 2 checker of speckled stone, repeating. */
    for (v = 0; v < TEX; ++v)
        for (u = 0; u < TEX; ++u)
            t8[v * TEX + u] = (uint8_t)((u & 31) == 0 || (v & 31) == 0 ? 15
                                        : (((u >> 5) ^ (v >> 5)) & 1 ? 8 : 1) +
                                              (int)(hash2((unsigned)u, (unsigned)v) & 3));
    if ((r = texture(s, &s->floor, TEX, TEX, JB_TEX_4BIT, t8, floor_pal, 1)) != JB_OK)
        return r;
    /* Plasma: 8-bit indices from four sine waves; its palette cycles. */
    for (v = 0; v < TEX; ++v)
        for (u = 0; u < TEX; ++u) {
            float sum = SN(u / 64.0f) + SN(v / 64.0f) + SN((u + v) / 128.0f) + SN((u - v) / 128.0f);
            t8[v * TEX + u] = (uint8_t)((sum + 4.0f) * 255.0f / 8.0f);
        }
    if ((r = texture(s, &s->plasma, TEX, TEX, JB_TEX_8BIT, t8, NULL, 0)) != JB_OK)
        return r;
    /* Emblem: 15-bit, a gold diamond on a teal gradient, bevelled. */
    for (v = 0; v < TEX; ++v)
        for (u = 0; u < TEX; ++u) {
            int du = IABS(2 * u - 63), dv = IABS(2 * v - 63), c = 31 - (du + dv) / 3;
            if (du >= 57 || dv >= 57)
                t15[v * TEX + u] = (du > dv ? u < 32 : v < 32) ? JB_RGB15(28, 28, 30)
                                                               : JB_RGB15(6, 6, 9);
            else
                t15[v * TEX + u] = du + dv < 44 ? JB_RGB15(c, c * 3 / 4, c / 4 + 1)
                                                : JB_RGB15(2, 8 + v / 8, 12 + v / 8);
        }
    if ((r = texture(s, &s->emblem, TEX, TEX, JB_TEX_15BIT, t15, NULL, 0)) != JB_OK)
        return r;
    /* Star: a four-pointed glow, 32 x 32 at u 0 and 16 x 16 at u 32. Index
       0 transparent, 1-14 a semi-transparent glow, 15 an opaque core. */
    memset(t8, 0, sizeof t8);
    for (i = 0; i < 2; ++i) {
        int size = i ? 16 : 32, reach = size * size;
        for (v = 0; v < size; ++v)
            for (u = 0; u < size; ++u) {
                int dx = 2 * u - size + 1, dy = 2 * v - size + 1, d2 = dx * dx + dy * dy, k = 0;
                if (d2 < reach / 25)
                    k = 15;
                else if ((IABS(dx) <= 1 || IABS(dy) <= 1) && d2 < reach)
                    k = 14 - d2 * 13 / reach;
                if (k < 15 && d2 < reach / 3 && 14 - d2 * 39 / reach > k)
                    k = 14 - d2 * 39 / reach;
                t8[v * STAR_ROW + i * 32 + u] = (uint8_t)k;
            }
    }
    pal[0] = 0;
    for (i = 1; i < 15; ++i)
        pal[i] = (uint16_t)(SEMI | JB_RGB15(31 * i / 14, 26 * i / 14, 12 * i / 14));
    pal[15] = JB_RGB15(31, 31, 28);
    return texture(s, &s->star, STAR_ROW, 32, JB_TEX_4BIT, t8, pal, 0);
}

/* The meshes. The cube's 24 vertices: each face's four corners, with the
   corner's own normal (out through it, so the faces shade like a smooth
   solid) and the face's texture corners; front and back the emblem. */
static void make_meshes(fl_scene *s)
{
    static const signed char corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    static const jb_gte_vec floor_pos[4] = {
        {-640, FLOOR_Y, -360}, {640, FLOOR_Y, -360}, {640, FLOOR_Y, -2200}, {-640, FLOOR_Y, -2200}};
    static const uint32_t floor_col[4] = {JB_RGB(160, 160, 160), JB_RGB(160, 160, 160),
                                          JB_RGB(40, 40, 48), JB_RGB(40, 40, 48)};
    static const uint8_t floor_uv[8] = {0, 255, 255, 255, 255, 0, 0, 0};
    static const jb_gte_vec shadow_pos[4] = {{-150, 0, 150}, {150, 0, 150}, {150, 0, -150},
                                             {-150, 0, -150}};
    static const jb_mesh_face quad = {{0, 1, 2, 3}, 4, 0};
    int f, k;

    for (f = 0; f < 6; ++f) {
        int sign = f & 1 ? -1 : 1, axis = f >> 1;
        /* u x v = the face's normal, so the corners go round anticlockwise. */
        int ua = (axis + (sign > 0 ? 1 : 2)) % 3, va = (axis + (sign > 0 ? 2 : 1)) % 3;
        for (k = 0; k < 4; ++k) {
            int i = f * 4 + k;
            int16_t p[3];
            p[axis] = (int16_t)(sign * CUBE);
            p[ua] = (int16_t)(corner[k][0] * CUBE);
            p[va] = (int16_t)(corner[k][1] * CUBE);
            s->cube_pos[i] = (jb_gte_vec){p[0], p[1], p[2]};
            s->cube_normal[i] = (jb_gte_vec){p[0] > 0 ? 2365 : -2365, /* 4096 / sqrt(3) */
                                             p[1] > 0 ? 2365 : -2365, p[2] > 0 ? 2365 : -2365};
            s->cube_uv[2 * i] = (uint8_t)(corner[k][0] < 0 ? 0 : TEX - 1);
            s->cube_uv[2 * i + 1] = (uint8_t)(corner[k][1] > 0 ? 0 : TEX - 1);
            s->cube_faces[f].v[k] = (uint16_t)i;
        }
        s->cube_faces[f].vertices = 4;
        s->cube_faces[f].material = axis == 2;
    }
    /* Texels as they are under a light of 1.0; the key light brightens them. */
    s->cube_mat[0] = (jb_material){&s->plasma, JB_NEUTRAL, JB_BLEND_NONE, 0, 0, 0};
    s->cube_mat[1] = (jb_material){&s->emblem, JB_NEUTRAL, JB_BLEND_NONE, 0, 0, 0};
    s->cube = (jb_mesh){24, s->cube_pos, s->cube_normal, NULL, s->cube_uv, 6, s->cube_faces,
                        2, s->cube_mat};
    /* The floor: one quad, unlit, the texture repeated four times each way,
       darker far off; the shadow: a square subtracted from it (B - F). */
    s->floor_mat = (jb_material){&s->floor, 0, JB_BLEND_NONE, 1, 0, 0};
    s->floor_mesh = (jb_mesh){4, floor_pos, NULL, floor_col, floor_uv, 1, &quad, 1, &s->floor_mat};
    s->shadow_mat = (jb_material){NULL, JB_RGB(72, 72, 56), JB_BLEND_SUB, 1, 0, 0};
    s->shadow = (jb_mesh){4, shadow_pos, NULL, NULL, NULL, 1, &quad, 1, &s->shadow_mat};
}

/* The panel, in the overlay, which draws in call order: a darkened box
   (B/2 + F/2) under a Gouraud border, a bar for the cube's turn, the
   title, and a light for each switch that is on. */
static void panel(fl_scene *s, unsigned frame)
{
    const uint32_t blue = JB_RGB(80, 120, 255), gold = JB_RGB(255, 200, 80);
    jb_vertex v[5] = {{6, 6, blue, 0, 0}, {137, 6, gold, 0, 0}, {137, 43, blue, 0, 0},
                      {6, 43, gold, 0, 0}, {6, 6, blue, 0, 0}};
    jb_polyline border = {1, JB_BLEND_NONE, 5, v};
    jb_rect r = {6, 6, 132, 38, JB_RGB(0, 0, 48), JB_BLEND_HALF, NULL, 0, 0, 0, 0, 0};
    char scale[3] = {(char)('0' + s->sw.scale % 10), 'x', 0};
    const char *label[7] = {scale, "V", "T", "D", "W", "G", "C"};
    const int on[7] = {s->sw.scale > 1, s->sw.precise, s->sw.perspective, s->sw.dither,
                       s->sw.widescreen, s->sw.geometry == JB_GEOMETRY_FLOAT, s->sw.clut_cache};
    int i, x = 10;

    jb_draw_rect(s->jb, &r);
    jb_draw_polyline(s->jb, &border);
    r.x = 10, r.y = 36, r.h = 3, r.blend = JB_BLEND_NONE, r.color = gold;
    r.w = (int)((frame * 4) & 1023u) * 124 / 1024;
    jb_draw_rect(s->jb, &r);
    jb_draw_text(s->jb, 10, 10, JB_RGB(255, 224, 160), "first-light");
    for (i = 0; i < 7; ++i) {
        jb_draw_text(s->jb, x, 23, on[i] ? JB_RGB(96, 255, 96) : JB_RGB(96, 72, 72), label[i]);
        x += 8 * (int)strlen(label[i]) + 8;
    }
}

jb_result fl_scene_draw(fl_scene *s, unsigned frame)
{
    float spin = frame * 4 / 1024.0f, a = frame * 6 / 1024.0f, cy = 40 - 20 * SN(frame / 128.0f);
    jb_vec3 centre = {0, cy, -CUBE_Z}, ground = {0, FLOOR_Y, -CUBE_Z}, one = {1, 1, 1};
    jb_vec3 tilt = {(frame * 2 + 96) / 1024.0f * TURN, spin * TURN, 0}, turn = {0, spin * TURN, 0};
    int i, w, glow = 104 + (int)(24 * SN(frame / 64.0f));
    int in_front = SN(a) < 0; /* the star in front of the cube */
    jb_billboard star = {&s->star, {ORBIT * CS(a), cy + 30 * SN(2 * a), -CUBE_Z - ORBIT * SN(a)},
                         80, 0, in_front ? 0 : 32, 0, in_front ? 32 : 16, in_front ? 32 : 16,
                         JB_RGB(glow, glow, glow), JB_BLEND_ADD, 0, 0};
    uint16_t pal[256];
    jb_mat4 m;

    if (jb_begin_frame(s->jb, JB_RGB(12, 10, 20)) != JB_OK || jb_frame_size(s->jb, &w, NULL))
        return jb_check(s->jb, NULL); /* this frame's failure, taken */
    for (i = 0; i < 256; ++i) { /* the plasma's colours cycle */
        float c = i / 256.0f + frame / 128.0f;
        pal[i] = JB_RGB15(10 + (int)(9 * SN(c)), 10 + (int)(9 * SN(c + 1 / 3.0f)),
                          10 + (int)(9 * SN(c + 2 / 3.0f)));
    }
    jb_palette_write(s->jb, &s->plasma, 0, 256, pal);

    /* Background: a Gouraud sky from night blue to a dusk glow (dithered),
       and twinkling stars. */
    jb_set_layer(s->jb, JB_LAYER_BACKGROUND);
    jb_poly sky = {4, 1, JB_BLEND_NONE, NULL, 0, /* as wide as the frame */
                   {{0, 0, JB_RGB(16, 20, 64), 0, 0}, {w, 0, JB_RGB(24, 16, 72), 0, 0},
                    {0, 150, JB_RGB(208, 112, 88), 0, 0}, {w, 150, JB_RGB(176, 96, 112), 0, 0}}};
    jb_draw_poly(s->jb, &sky);
    for (i = 0; i < 24; ++i) {
        int b = 128 + (int)(127 * SN(frame / 128.0f + i * 100 / 1024.0f));
        jb_rect dot = {(i * 97 + 13) % w, (i * 53 + 7) % 120, 1, 1, JB_RGB(b, b, b),
                       JB_BLEND_NONE, NULL, 0, 0, 0, 0, 0};
        jb_draw_rect(s->jb, &dot);
    }
    jb_set_layer(s->jb, JB_LAYER_OVERLAY);

    /* The scene, sorted by depth: the floor, the shadow turning with the
       cube, the lit cube, the ring (each line by its own depth, brighter
       nearer) and the star, which pass behind the cube and in front of it.
       The star's glow texels blend B + F, its core does not; near it shows
       the 32 x 32 image, far the 16 x 16 one. */
    jb_draw_mesh(s->jb, &s->floor_mesh, NULL);
    m = jb_mat4_trs(ground, turn, one);
    jb_draw_mesh(s->jb, &s->shadow, &m);
    m = jb_mat4_trs(centre, tilt, one);
    jb_draw_mesh(s->jb, &s->cube, &m);
    for (i = 0; i < RING; ++i) {
        float t0 = i / (float)RING, t1 = (i + 1) / (float)RING;
        int b0 = 155 - (int)(100 * SN(t0)), b1 = 155 - (int)(100 * SN(t1));
        jb_line3d l = {{ORBIT * CS(t0), cy, -CUBE_Z - ORBIT * SN(t0)},
                       {ORBIT * CS(t1), cy, -CUBE_Z - ORBIT * SN(t1)},
                       JB_RGB(b0 / 4, b0 / 3, b0 / 2), JB_RGB(b1 / 4, b1 / 3, b1 / 2), JB_BLEND_ADD, 0};
        jb_draw_line3d(s->jb, &l);
    }
    jb_draw_billboard(s->jb, &star);
    panel(s, frame);
    return jb_end_frame(s->jb);
}

jb_result fl_scene_set_switches(fl_scene *s, const fl_switches *sw)
{
    jb_result r;

    if ((r = jb_set_scale(s->jb, sw->scale)) != JB_OK ||
        (r = jb_set_widescreen(s->jb, sw->widescreen)) != JB_OK ||
        (r = jb_set_geometry(s->jb, sw->geometry)) != JB_OK)
        return jb_check(s->jb, NULL), r; /* reported here, not by the next frame */
    jb_set_precise_vertices(s->jb, sw->precise);
    jb_set_perspective_correct(s->jb, sw->perspective);
    jb_set_dither(s->jb, sw->dither);
    jb_set_clut_cache(s->jb, sw->clut_cache);
    s->sw = *sw;
    return JB_OK;
}

jb_result fl_scene_init(fl_scene *s, const fl_switches *sw, int threads)
{
    /* A warm key light from above, left and in front, 1.6 times full (the
       GTE has the headroom); a blue rim light from the right, behind; a
       dim blue ambient. The console version's projection distance, 256. */
    jb_light key = {{0.45f, -0.65f, -0.61f}, JB_RGB(255, 236, 204), 1.6f};
    jb_light rim = {{-0.8f, 0.2f, 0.56f}, JB_RGB(56, 96, 191), 0};
    jb_camera camera = {.projection = 256}; /* otherwise the default camera */
    jb_config c;
    jb_result r;

    memset(s, 0, sizeof *s);
    memset(&c, 0, sizeof c);
    c.scale = sw->scale, c.threads = threads, c.log = sw->log, c.log_user = sw->log_user;
    c.widescreen = 1; /* buffers with room for 16:9, switched at run time */
    if ((s->jb = jb_create(&c, &r)) == NULL)
        return r;
    if ((r = make_textures(s)) != JB_OK || (r = jb_set_camera(s->jb, &camera)) != JB_OK ||
        (r = jb_set_ambient(s->jb, JB_RGB(19, 19, 29))) != JB_OK ||
        (r = jb_set_light(s->jb, 0, &key)) != JB_OK || (r = jb_set_light(s->jb, 1, &rim)) != JB_OK ||
        (r = fl_scene_set_switches(s, sw)) != JB_OK) {
        fl_scene_free(s);
        return r;
    }
    make_meshes(s);
    return JB_OK;
}

const uint8_t *fl_scene_frame(fl_scene *s, int *w, int *h)
{
    return jb_frame(s->jb, w, h);
}

void fl_scene_free(fl_scene *s)
{
    jb_destroy(s->jb);
    s->jb = NULL;
}
