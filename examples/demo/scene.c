/*
 * The demo: the scene (scene.h). C99, the public API only, and the
 * library's maths only (jb_sinf, jb_cosf, jb_tanf, jb_vec3_normalize: the
 * same bits on every machine); otherwise integers and float + - x /. The
 * world is right-handed with y up, sea level y = 0. Angles in the motion
 * are in turns.
 *
 * Each frame:
 *   - jb_begin_frame clears to the haze colour (s->clear);
 *   - every four frames the water takes the next of its file's palettes
 *     (jb_palette_write): the palette animation;
 *   - the sky is drawn at once, at the console level (jb_prim_poly): a
 *     dithered Gouraud gradient, the haze's colour from the horizon up past
 *     where the hills stand against it, then rings of cloud round the sky,
 *     placed by direction as the sun is, each ring's palette written into
 *     the same CLUT row by a VRAM transfer (jb_vram_load) just before it is
 *     drawn, with no cache flush; so with the CLUT cache on, as on the
 *     console, every ring shows the palette the first one loaded, and with
 *     it off each shows its own (the C key). Being drawn at once,
 *     it lies under everything the frame records;
 *   - the camera flies the path, but slows and turns to look at the pier
 *     and at the standing stones in two slow passages (make_view);
 *   - the scene is recorded: the sun, the seabed and the water (all biased
 *     into the farthest bucket of the frame's ordering table, in that call
 *     order, so that the land, which is never under the water when seen
 *     from above it, is drawn over them), then the land, the trees, the
 *     standing stones, the lighthouse and its beams, the glows, all sorted
 *     by depth. The terrain is every chunk in the frustum, all with one
 *     model matrix, textured and fogged towards each texture's own colour
 *     for the haze, its textures fading with the haze through palettes
 *     (terrain);
 *   - the panel goes into the overlay;
 *   - jb_end_frame draws the records and shows the frame.
 */
#include "scene.h"

#include <stdlib.h>
#include <string.h>

#define TURN 6.2831853f
#define SN(turns) jb_sinf((turns) * TURN)
#define CS(turns) jb_cosf((turns) * TURN)

enum {
    PROJECTION = 200, /* H, the projection plane's distance in pixels */
    Z_NEAR = 24,
    Z_FAR = 4000,
    FOG_START = 1200,
    FOG_END = 3800,
    TREE_RANGE = 2600,  /* trees, stones and the lighthouse are drawn within this */
    GLOW_RANGE = 1100,  /* and fireflies within this */
    CLOUD_BANDS = 5,
    SKY_STOPS = 6
};
#define FAR_BIAS 100000.0f /* world units: into the farthest bucket */
/* World units: the pier's posts sorted behind every face of its deck (576
   long, sorted by each quad's average depth, so a post nearer than a
   quad's middle sorted in front of it and was drawn over the planks),
   still in front of the sea's farthest bucket. The eye is always above
   the deck (make_view), which so always covers the posts under it. */
#define POST_BIAS 1000.0f
#define HAZE JB_RGB(168, 108, 124) /* dusk: a dull rose, no brighter than the sky above it */

/* The sun: where it is (a direction towards it) and its light. */
static const jb_vec3 sun_dir = {0.7911f, 0.1197f, -0.5998f}; /* 6.9 degrees up */

/*
 * The swell: DM_WAVES waves travelling across the sea, each m, n times
 * round the map in x and z (so each repeats with the map, 8192 units, and
 * the sea is the same on every copy of it: a function of world position
 * and the frame alone, so the seams between chunks stay closed), a period
 * in frames that divides DM_LOOP (so the flight loops without a seam),
 * and an amplitude. Long and low: 600 to 1100 units from crest to crest,
 * 6.5 units high all told, set 6.5 below sea level so that the crests come
 * to the beach line at the most and the land, raised to it at a coast,
 * covers the sea there.
 */
static const struct wave {
    int m, n, period;
    float amp;
} waves[DM_WAVES] = {{7, 3, 346, 3.0f}, {-4, 9, 692, 2.0f}, {11, -6, 173, 1.5f}};
#define SEA_MEAN (-6.5f)

int dm_key(int key, dm_switches *sw, int *paused, unsigned *frame)
{
    switch (key) {
    case '1':
    case '2':
    case '3':
    case '4': sw->scale = key - '0'; break;
    case 'v': sw->precise = !sw->precise; break;
    case 't': sw->perspective = !sw->perspective; break;
    case 'd': sw->dither = !sw->dither; break;
    case 'c': sw->clut_cache = !sw->clut_cache; break;
    case 'w': sw->widescreen = !sw->widescreen; break;
    case 'g':
        sw->geometry = sw->geometry == JB_GEOMETRY_GTE ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE;
        break;
    case 'f': sw->fog = !sw->fog; break;
    case 'h': sw->panel = !sw->panel; break;
    case 'p': *paused = !*paused; break;
    case 'n':
        if (*paused)
            ++*frame;
        break;
    default: return 0;
    }
    return 1;
}

static float fabs_(float x)
{
    return x < 0 ? -x : x;
}

/* x in 0..m, any float within int range. */
static float wrapf(float x, float m)
{
    return x - m * (float)dm_floor(x / m);
}

/* The angle of (x, z), from +z towards +x, in turns 0..1. A polynomial for
   atan on 0..1 (good to about 1e-5 radians), float + - x / only. */
static float turns_of(float x, float z)
{
    float ax = fabs_(x), az = fabs_(z), r, r2, a;

    if (ax == 0 && az == 0)
        return 0;
    r = ax <= az ? ax / az : az / ax;
    r2 = r * r;
    a = r * (0.9998660f + r2 * (-0.3302995f + r2 * (0.1801410f + r2 * (-0.0851330f + r2 * 0.0208351f))));
    if (ax > az)
        a = TURN / 4 - a;
    if (z < 0)
        a = TURN / 2 - a;
    if (x < 0)
        a = -a;
    return wrapf(a / TURN, 1.0f);
}

/* The haze (on or off as the switch says), towards color: HAZE for what
   the haze hides (a textured face's own colour for it: haze_for), black
   for what adds light, which must fade to nothing with distance rather
   than towards the haze's colour. */
static void fog(dm_scene *s, uint32_t color)
{
    jb_set_fog(s->jb, color, FOG_START, s->sw.fog ? FOG_END : 0);
}

/* A 4-bit texture's mean texel, JB_RGB, read from VRAM through its CLUT,
   clear texels (0) left out; JB_NEUTRAL for any other. */
static uint32_t texture_mean(jb_context *jb, const jb_texture *t)
{
    uint16_t clut[16], row[64];
    unsigned long sum[3] = {0, 0, 0}, n = 0;
    int x, y, c, mean[3];

    if (t->depth != JB_TEX_4BIT || t->u % 4 != 0 ||
        jb_vram_read(jb, t->clut_x, t->clut_y, 16, 1, clut) != JB_OK)
        return JB_NEUTRAL;
    for (y = 0; y < t->height; ++y) {
        if (jb_vram_read(jb, t->page_x + t->u / 4, t->page_y + t->v + y, (t->width + 3) / 4, 1, row) != JB_OK)
            return JB_NEUTRAL;
        for (x = 0; x < t->width; ++x) {
            uint16_t px = clut[row[x / 4] >> (4 * (x % 4)) & 15];
            if (px == 0)
                continue;
            for (c = 0; c < 3; ++c)
                sum[c] += (unsigned long)(px >> (5 * c) & 31);
            ++n;
        }
    }
    for (c = 0; c < 3; ++c)
        mean[c] = n == 0 ? 128 : (int)(sum[c] * 8 / n); /* 5-bit, times 8 */
    return JB_RGB(mean[0], mean[1], mean[2]);
}

/*
 * The fog colour that brings a texture of this mean texel to HAZE. The
 * depth cue moves a face's vertex colour towards the fog colour, and the
 * GPU multiplies each texel by that colour over 128, so a textured face
 * fogged all the way shows its texels times the fog colour over 128, not
 * the fog colour itself: fogged towards HAZE, the bright sand would stand
 * out against the sky as bright salmon. HAZE x 128 over the mean brings
 * the mean to HAZE. Each channel is at most 255, so a texture too dark for
 * that (the grass's blue) would fog darker than HAZE; and the texels' own
 * contrast is never fogged away. The fade palettes mend both (make_fades).
 */
static uint32_t haze_for(uint32_t mean)
{
    int c, out[3];

    for (c = 0; c < 3; ++c) {
        int m = (int)(mean >> (8 * c) & 255), h = (int)(HAZE >> (8 * c) & 255);
        out[c] = m == 0 ? 255 : h * 128 / m;
        out[c] = out[c] > 255 ? 255 : out[c];
    }
    return JB_RGB(out[0], out[1], out[2]);
}

/* The colour, 15-bit, that a texture fogged towards fog_to (haze_for) must
   have to come to HAZE: HAZE x 128 over fog_to, channel by channel, as
   5-bit values; where fog_to is not clamped, the texture's mean. */
static uint16_t fade_target(uint32_t fog_to)
{
    int c, v[3];

    for (c = 0; c < 3; ++c) {
        int f = (int)(fog_to >> (8 * c) & 255), h = (int)(HAZE >> (8 * c) & 255);
        int t = f == 0 ? 255 : h * 128 / f; /* 8-bit */
        t = (t + 4) / 8;
        v[c] = t > 31 ? 31 : t;
    }
    return (uint16_t)JB_RGB15(v[0], v[1], v[2]);
}

/* Palette in faded level k of DM_FADES - 1 of the way to the colour to,
   into out: each entry's channels moved that far, rounded, its
   semi-transparency bit kept, a clear entry (0) left clear. */
static void fade_palette(const uint16_t *in, uint16_t to, int k, uint16_t *out)
{
    int i, c;

    for (i = 0; i < 16; ++i) {
        int v[3];
        if (in[i] == 0) {
            out[i] = 0;
            continue;
        }
        for (c = 0; c < 3; ++c) {
            int a = in[i] >> (5 * c) & 31, b = to >> (5 * c) & 31, num = (b - a) * k, den = DM_FADES - 1;
            v[c] = a + (num >= 0 ? (2 * num + den) / (2 * den) : -((-2 * num + den) / (2 * den)));
        }
        out[i] = (uint16_t)((in[i] & 0x8000u) | JB_RGB15(v[0], v[1], v[2]));
        if (out[i] == 0)
            out[i] = 0x8000u; /* black that shows, not clear */
    }
}

/*
 * The fade levels. The haze fogs a texel towards its texture's colour for
 * it (haze_for), multiplied by the texel, so it never takes away the
 * texture's own pattern, and a texture too dark for that colour (the
 * grass) never reaches the haze. So, as the haze thickens with distance,
 * each terrain texture is drawn through palettes faded from its own
 * colours to the one its fog colour brings to HAZE exactly (fade_target):
 * at the last level the texture is that one colour, and fogged all the way
 * it is HAZE. Level k of DM_FADES is a copy of the texture whose CLUT is a
 * palette k / (DM_FADES - 1) of the way there, each made as a 1 x 1
 * texture (fade_clut) to hold it: the texels are the texture's own, so
 * nothing but the colours changes from one level to the next. The water's
 * are written again with each frame of its palette animation
 * (dm_scene_draw). The chunks choose their level by the haze where they
 * are nearest (fade_level).
 */
static jb_result make_fades(dm_scene *s)
{
    static const int tex[4] = {DM_TEX_SAND, DM_TEX_GRASS, DM_TEX_ROCK, DM_TEX_WATER};
    jb_result r;
    int t, k, i;

    for (t = 0; t < 4; ++t) {
        const jb_texture *base = &s->assets.tex[tex[t]];
        uint16_t pal[16], faded[16];
        if ((r = jb_vram_read(s->jb, base->clut_x, base->clut_y, 16, 1, pal)) != JB_OK)
            return r;
        s->fade_to[t] = fade_target(s->haze_for[tex[t]]);
        s->fade_tex[0][t] = *base;
        for (k = 1; k < DM_FADES; ++k) {
            jb_texture_desc d;
            static const uint8_t texel = 0;
            memset(&d, 0, sizeof d);
            fade_palette(pal, s->fade_to[t], k, faded);
            d.width = d.height = 1, d.depth = JB_TEX_4BIT, d.texels = &texel, d.palette = faded;
            if ((r = jb_texture_create(s->jb, &d, &s->fade_clut[k][t])) != JB_OK)
                return r;
            s->fade_tex[k][t] = *base;
            s->fade_tex[k][t].clut_x = s->fade_clut[k][t].clut_x;
            s->fade_tex[k][t].clut_y = s->fade_clut[k][t].clut_y;
        }
    }
    for (k = 0; k < DM_FADES; ++k) {
        for (i = 0; i < DM_LAND_MATERIALS; ++i) {
            s->land_fade[k][i] = s->land_mat[i];
            s->land_fade[k][i].texture = &s->fade_tex[k][i == DM_SAND ? 0 : i == DM_GRASS ? 1 : 2];
        }
        s->bed_fade[k] = s->bed_mat, s->bed_fade[k].texture = &s->fade_tex[k][0];
        s->water_fade[k] = s->water_mat, s->water_fade[k].texture = &s->fade_tex[k][3];
    }
    return JB_OK;
}

/* Turns: frame / period, from frame modulo period, so that each motion
   repeats at its own period and none jumps when a counter wraps. */
static float phase(unsigned frame, unsigned period)
{
    return (float)(frame % period) / (float)period;
}

/* The sea's height at world (x, z) this frame (s->sea_phase), DM_SEA_DEPTH
   below sea level to 0, and its slope (gx, gz: rise a unit along x, z). */
static float sea_height(const dm_scene *s, float x, float z, float *gx, float *gz)
{
    float h = SEA_MEAN, sx = 0, sz = 0;
    int i;

    for (i = 0; i < DM_WAVES; ++i) {
        float a = ((float)waves[i].m * x + (float)waves[i].n * z) / DM_WORLD - s->sea_phase[i];
        float slope = waves[i].amp * TURN * CS(a) / DM_WORLD;
        h += waves[i].amp * SN(a);
        sx += slope * (float)waves[i].m, sz += slope * (float)waves[i].n;
    }
    *gx = sx, *gz = sz;
    return h;
}

/* The camera's path over the repeating map: x runs once across the map
   while z swings, both periodic in DM_PATH units (x up to the map's
   period), so the flight loops without a seam. */
static void path(float t, float *x, float *z)
{
    float a = t / DM_PATH;

    *x = DM_WORLD * a + 300 * SN(2 * a);
    *z = 1700 * SN(a) + 500 * SN(3 * a + 0.2f);
}

/* The offset that brings world x (or z) to the copy of the map nearest the
   eye's e. */
static float nearest(float x, float e)
{
    return wrapf(x - e + DM_WORLD / 2, DM_WORLD) - DM_WORLD / 2;
}

/* Where the camera is and looks, for frame t of the loop. */
typedef struct view {
    jb_vec3 eye, fwd;  /* fwd: unit, along the view */
    jb_vec3 right, up; /* the camera's others, unit: fwd x +y, right x fwd */
    float half_w, half_h; /* the frame's half-width and half-height over H */
    float fx, fz;      /* the heading, unit, in x-z */
    float pitch;       /* turns below the horizon */
    float yaw;         /* the heading in turns, from +z towards +x */
} view;

/* The camera's axes from its heading h (unit, in x-z) and vw->pitch. */
static void aim(view *vw, jb_vec3 h)
{
    vw->fwd.x = h.x * CS(vw->pitch), vw->fwd.y = -SN(vw->pitch), vw->fwd.z = h.z * CS(vw->pitch);
    vw->right.x = -h.z, vw->right.y = 0, vw->right.z = h.x;
    vw->up.x = h.x * SN(vw->pitch), vw->up.y = CS(vw->pitch), vw->up.z = h.z * SN(vw->pitch);
    vw->half_w = vw->half_h = 0; /* the frame's, set once it is begun */
}

/*
 * Where the camera is and looks, for frame `frame`. The frame becomes a
 * unit of the path: one a frame, but in a slow passage, which takes
 * DM_SLOW_A units fewer than its DM_SLOW_W frames along a smoothstep (so
 * the speed falls from one to 1 - 1.5 DM_SLOW_A / DM_SLOW_W, about a
 * sixth, at its middle and back, with no jump). Through a passage the
 * camera also turns towards its sight, comes down towards it and looks
 * down at it, by w, rising from 0 to 1 at the middle and back (sin^2).
 * A fixed camera (dm_scene.shot), when there is one, is the view instead.
 */
static view make_view(const dm_scene *s, unsigned frame)
{
    float t = (float)(frame % DM_LOOP), x, z, x0, z0, x1, z1, g, sum = 0, peak = 0, alt, w = 0;
    float pitch;
    const dm_slow *slow = NULL;
    jb_vec3 h;
    view vw;
    int k;

    if (s->shot != NULL) {
        /* A fixed camera (dm_scene.shot). */
        h.x = SN(s->shot->yaw), h.y = 0, h.z = CS(s->shot->yaw);
        vw.eye = s->shot->eye;
        vw.fx = h.x, vw.fz = h.z, vw.yaw = s->shot->yaw, vw.pitch = s->shot->pitch;
        aim(&vw, h);
        return vw;
    }
    for (k = 0; k < DM_SLOWS; ++k) {
        float u = ((float)(frame % DM_LOOP) - (float)s->slow[k].start) / DM_SLOW_W;
        if (u >= 1) {
            t -= DM_SLOW_A;
        } else if (u > 0) {
            t -= DM_SLOW_A * u * u * (3 - 2 * u);
            w = SN(u / 2) * SN(u / 2);
            slow = &s->slow[k];
        }
    }
    path(t, &x, &z);
    path(t - 1, &x0, &z0);
    path(t + 1, &x1, &z1);
    h.x = x1 - x0, h.y = 0, h.z = z1 - z0;
    h = jb_vec3_normalize(h);
    if (slow != NULL) {
        /* Turned towards the sight. */
        jb_vec3 to = {nearest(slow->at.x, x), 0, nearest(slow->at.z, z)}, d;
        d = jb_vec3_normalize(to);
        h.x = (1 - w) * h.x + w * d.x, h.z = (1 - w) * h.z + w * d.z;
        h = jb_vec3_normalize(h);
    }
    vw.fx = h.x, vw.fz = h.z;
    vw.yaw = turns_of(h.x, h.z);
    /* Height: above the ground ahead, half its average and half its peak
       over the next 120 frames, and a slow rise and fall. */
    for (k = 0; k < 12; ++k) {
        float px, pz;
        path(t + 10.0f * (float)k, &px, &pz);
        g = dm_ground(s->terrain, px, pz);
        g = g < 0 ? 0 : g;
        sum += g;
        peak = g > peak ? g : peak;
    }
    alt = sum / 48.0f + peak * 0.75f + 300.0f + 80.0f * SN(3.0f * t / DM_PATH);
    g = dm_ground(s->terrain, x, z);
    g = g < 0 ? 0 : g; /* over the sea, its surface, not the seabed */
    if (slow != NULL)
        alt -= w * slow->lower;
    if (alt < g + 250.0f - 130.0f * w)
        alt = g + 250.0f - 130.0f * w; /* so at least 120 above the ground or sea: above the pier's deck */
    vw.eye.x = wrapf(x, DM_WORLD), vw.eye.y = alt, vw.eye.z = wrapf(z, DM_WORLD);
    pitch = 0.042f + 0.012f * SN(2.0f * t / DM_PATH + 0.3f);
    if (slow != NULL) {
        /* Looking down at the sight: the angle below the horizon, 0.02 to
           0.12 turns. */
        jb_vec3 to = {nearest(slow->at.x, x), 0, nearest(slow->at.z, z)};
        jb_vec3 d = jb_vec3_normalize(to);
        float across = to.x * d.x + to.z * d.z, drop = alt - slow->at.y, down;
        down = drop > 0 && across > 0 ? turns_of(drop, across) : 0.02f;
        down = down < 0.02f ? 0.02f : down > 0.12f ? 0.12f : down;
        pitch = (1 - w) * pitch + w * down;
    }
    vw.pitch = pitch;
    aim(&vw, h);
    return vw;
}

/* Whether a sphere at offset (rx, ry, rz) from the eye, radius r, may be
   in view: within range of the eye across the ground, between the near
   and far planes, and inside the frustum's four sides, the camera's pitch
   and all (the ground at the bottom of the frame is seen at a wider angle
   to the heading than the frame's half-width). Never too tight: a side's
   plane is passed by r (1 + half) for r times the length of its
   unnormalised normal, sqrt(1 + half^2). */
static int in_view(const view *vw, float rx, float ry, float rz, float r, float range)
{
    float x = rx * vw->right.x + rz * vw->right.z;
    float y = rx * vw->up.x + ry * vw->up.y + rz * vw->up.z;
    float z = rx * vw->fwd.x + ry * vw->fwd.y + rz * vw->fwd.z;

    if (rx * rx + rz * rz > (range + r) * (range + r) || z < Z_NEAR - r || z > Z_FAR + r)
        return 0;
    return fabs_(x) <= z * vw->half_w + r * (1 + vw->half_w) &&
           fabs_(y) <= z * vw->half_h + r * (1 + vw->half_h);
}

/* --- The sky, at the console level ----------------------------------- */

/* The screen row of a direction e turns above the horizon: e + pitch
   above the view's axis, the camera looking pitch below the horizon. */
static float sky_y(const view *vw, int h, float e)
{
    return (float)h / 2 - PROJECTION * jb_tanf((vw->pitch + e) * TURN);
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int k, int n)
{
    int c[3], i;

    for (i = 0; i < 3; ++i) {
        int x = (int)(a >> (8 * i) & 255u), y = (int)(b >> (8 * i) & 255u);
        c[i] = x + (y - x) * k / n;
    }
    return JB_RGB(c[0], c[1], c[2]);
}

/* How far either side of the heading the clouds are drawn, in turns:
   past the frame's corners at every pitch and in widescreen. */
#define CLOUD_REACH 0.19f

/*
 * Where on the screen (1x pixels, the frame w x h) the direction a turns
 * round from the heading (towards +x, as turns_of counts) and e turns up
 * from the horizon is: its coordinates along the camera's right, up and
 * forward (pitch below the horizon), over H. 0 for a direction not well in
 * front of the eye.
 */
static int cloud_vertex(const view *vw, int w, int h, float a, float e, int *x, int *y)
{
    float ce = CS(e), se = SN(e), cp = CS(vw->pitch), sp = SN(vw->pitch);
    float right = -ce * SN(a), up = ce * CS(a) * sp + se * cp, ahead = ce * CS(a) * cp - se * sp;
    float fx, fy;

    if (!(ahead > 0.1f))
        return 0;
    fx = (float)w / 2 + PROJECTION * right / ahead;
    fy = (float)h / 2 - PROJECTION * up / ahead;
    if (fx < -1000 || fx > 1000 || fy < -1000 || fy > 1000)
        return 0;
    *x = dm_floor(fx + 0.5f), *y = dm_floor(fy + 0.5f);
    return 1;
}

static void sky(dm_scene *s, const view *vw, unsigned frame, int w, int h)
{
    /* Stops of the gradient: turns above the horizon and colours. The
       haze's own colour from the horizon up past where the hills' tops
       stand against the sky, so that hazed land melts into the sky behind
       it; then dusk to night. */
    static const float stop_e[SKY_STOPS] = {0.0f, 0.010f, 0.020f, 0.032f, 0.046f, 0.07f};
    static const uint32_t stop_c[SKY_STOPS] = {HAZE, HAZE, JB_RGB(150, 92, 128),
                                               JB_RGB(96, 66, 120), JB_RGB(48, 42, 100), JB_RGB(22, 26, 72)};
    /* The cloud rings: turns above the horizon (bottom to top) and the
       colour each ring's palette is tinted with (5-bit), gold to violet in
       small steps. */
    static const float band_e[CLOUD_BANDS + 1] = {0.006f, 0.011f, 0.017f, 0.024f, 0.033f, 0.045f};
    static const int band_tint[CLOUD_BANDS][3] = {
        {31, 23, 16}, {29, 21, 17}, {27, 20, 19}, {25, 19, 20}, {23, 18, 21}};
    const jb_texture *cloud = &s->assets.tex[DM_TEX_CLOUD];
    jb_poly p;
    int i, k;

    memset(&p, 0, sizeof p);
    p.vertices = 4, p.gouraud = 1;
    /* Each strip between two stops (and above the last, its colour to the
       frame's top), cut to the frame with its colours interpolated there:
       Gouraud, so dithered. Below the horizon the clear colour shows. */
    for (i = 0; i < SKY_STOPS; ++i) {
        float yb = sky_y(vw, h, stop_e[i]), yt = i < SKY_STOPS - 1 ? sky_y(vw, h, stop_e[i + 1]) : -1e9f;
        float b = yb > (float)h ? (float)h : yb, t = yt < 0 ? 0 : yt;
        int bottom = (int)b + 1, top = (int)t, n = 1024;
        uint32_t cb, ct;
        if (top >= bottom || b <= 0)
            continue;
        if (i < SKY_STOPS - 1) {
            cb = lerp_rgb(stop_c[i], stop_c[i + 1], (int)((yb - b) / (yb - yt) * (float)n), n);
            ct = lerp_rgb(stop_c[i], stop_c[i + 1], (int)((yb - t) / (yb - yt) * (float)n), n);
        } else {
            cb = ct = stop_c[SKY_STOPS - 1];
        }
        p.v[0] = (jb_vertex){0, top, ct, 0, 0};
        p.v[1] = (jb_vertex){w, top, ct, 0, 0};
        p.v[2] = (jb_vertex){0, bottom, cb, 0, 0};
        p.v[3] = (jb_vertex){w, bottom, cb, 0, 0};
        jb_prim_poly(s->jb, &p);
    }
    /*
     * The clouds: five rings of one sheet of cloud round the sky, from the
     * horizon up (band_e: elevations in turns), drawn as a grid of quads
     * in direction, columns 8 texels of the sheet wide in azimuth, a row a
     * ring, each vertex placed on the screen where its direction is
     * (cloud_vertex, as the camera sees the sun): so turning and pitching
     * move the clouds exactly as they move the sun. u goes 512 texels
     * round the sky (the sheet twice) and drifts with one slow wind, a
     * sheet's width a loop; v follows elevation, the rings' rows of the
     * sheet one after another (rows 60 - 12 k down to 48 - 12 k for ring
     * k), so the sheet is whole across them. Each ring's palette is the
     * cloud's densities tinted with its light, gold low to violet high in
     * small steps, written over the cloud's CLUT just before the ring is
     * drawn, with no cache flush (the C key, README).
     */
    p.gouraud = 0, p.blend = JB_BLEND_HALF, p.texture = cloud;
    {
        float wind = phase(frame, DM_LOOP) * 256.0f;
        int first = dm_floor(((vw->yaw - CLOUD_REACH) * 512.0f + wind) / 8);
        int last = dm_floor(((vw->yaw + CLOUD_REACH) * 512.0f + wind) / 8);
        for (k = 0; k < CLOUD_BANDS; ++k) {
            uint16_t pal[16];
            int c, j;
            pal[0] = 0;
            for (j = 1; j < 16; ++j) {
                uint16_t col = s->assets.cloud[j];
                pal[j] = (uint16_t)((col & 0x8000u) |
                                    JB_RGB15((col & 31) * band_tint[k][0] / 31,
                                             (col >> 5 & 31) * band_tint[k][1] / 31,
                                             (col >> 10 & 31) * band_tint[k][2] / 31));
            }
            jb_vram_load(s->jb, cloud->clut_x, cloud->clut_y, 16, 1, pal);
            for (c = first; c <= last; ++c) {
                float a0 = ((float)(8 * c) - wind) / 512.0f - vw->yaw, a1 = a0 + 8.0f / 512.0f;
                int u0 = dm_wrap(8 * c, 256), u1 = u0 + 8 > 255 ? 255 : u0 + 8, v0 = 48 - 12 * k, ok = 1;
                int xs[4], ys[4], q;
                ok &= cloud_vertex(vw, w, h, a0, band_e[k + 1], &xs[0], &ys[0]);
                ok &= cloud_vertex(vw, w, h, a1, band_e[k + 1], &xs[1], &ys[1]);
                ok &= cloud_vertex(vw, w, h, a0, band_e[k], &xs[2], &ys[2]);
                ok &= cloud_vertex(vw, w, h, a1, band_e[k], &xs[3], &ys[3]);
                /* Left out only when all four corners are beyond one side of
                   the frame (a quad can reach in with two of them out). */
                if (!ok || (xs[0] < 0 && xs[1] < 0 && xs[2] < 0 && xs[3] < 0) ||
                    (xs[0] > w && xs[1] > w && xs[2] > w && xs[3] > w) ||
                    (ys[0] > h && ys[1] > h && ys[2] > h && ys[3] > h) ||
                    (ys[0] < 0 && ys[1] < 0 && ys[2] < 0 && ys[3] < 0))
                    continue;
                for (q = 0; q < 4; ++q) {
                    p.v[q].x = xs[q], p.v[q].y = ys[q], p.v[q].color = JB_NEUTRAL;
                    p.v[q].u = cloud->u + (q & 1 ? u1 : u0);
                    p.v[q].v = cloud->v + v0 + (q & 2 ? 12 : 0);
                }
                jb_prim_poly(s->jb, &p);
            }
        }
    }
}

/* --- The panel ------------------------------------------------------- */

static void panel(dm_scene *s, int paused)
{
    const uint32_t on = JB_RGB(120, 255, 120), off = JB_RGB(200, 128, 128), title = JB_RGB(255, 224, 150);
    const dm_switches *sw = &s->sw;
    char scale[8] = "1-4:1x";
    struct item {
        const char *text;
        int on;
    } items[11];
    static const int row_end[3] = {4, 8, 11}; /* items a row */
    jb_rect box;
    int i, x = 6, y = 6, row = 0;

    scale[4] = (char)('0' + sw->scale % 10);
    items[0] = (struct item){scale, sw->scale > 1};
    items[1] = (struct item){"V:verts", sw->precise};
    items[2] = (struct item){"T:persp", sw->perspective};
    items[3] = (struct item){"D:dith", sw->dither};
    items[4] = (struct item){"C:clut", sw->clut_cache};
    items[5] = (struct item){"W:wide", sw->widescreen};
    items[6] = (struct item){sw->geometry == JB_GEOMETRY_FLOAT ? "G:float" : "G:GTE", 1};
    items[7] = (struct item){"F:fog", sw->fog};
    items[8] = (struct item){paused ? "P:paused" : "P:pause", paused};
    items[9] = (struct item){"H:hide", 1};
    items[10] = (struct item){"jitterbug", 2};
    memset(&box, 0, sizeof box);
    /* A darkened box (B - F) under the text. */
    box.x = 3, box.y = 3, box.w = 30 * 8 + 6, box.h = 3 * 9 + 5, box.color = JB_RGB(176, 176, 150);
    box.blend = JB_BLEND_SUB;
    jb_draw_rect(s->jb, &box);
    for (i = 0; i < 11; ++i) {
        if (i == row_end[row])
            x = 6, y += 9, ++row;
        if (i == 10)
            x = 6 + 21 * 8; /* the name, right-aligned in the box */
        jb_draw_text(s->jb, x, y, items[i].on == 2 ? title : items[i].on ? on : off, items[i].text);
        x += 8 * (int)strlen(items[i].text) + 8;
    }
}

/* --- The scene ------------------------------------------------------- */

/* Farthest first; equal distances by place, so that the order is total
   and every C library's qsort gives the same one. */
static int far_first(const void *a, const void *b)
{
    const dm_visible *p = a, *q = b;

    if (p->dist2 != q->dist2)
        return p->dist2 < q->dist2 ? 1 : -1;
    if (p->cz != q->cz)
        return p->cz < q->cz ? -1 : 1;
    return p->cx < q->cx ? -1 : p->cx > q->cx ? 1 : 0;
}

static void draw_mesh(dm_scene *s, const jb_mesh *mesh, const jb_mat4 *m, float bias, uint32_t tint)
{
    jb_mesh_draw d;

    d.mesh = mesh, d.model = m, d.depth_bias = bias, d.tint = tint;
    jb_draw_mesh_ex(s->jb, &d);
}

static void glow(dm_scene *s, jb_vec3 at, float size, uint32_t tint, float bias)
{
    jb_billboard b;

    memset(&b, 0, sizeof b);
    b.texture = &s->assets.tex[DM_TEX_GLOW];
    b.position = at, b.width = size, b.tint = tint, b.blend = JB_BLEND_ADD, b.depth_bias = bias;
    jb_draw_billboard(s->jb, &b);
}

static const dm_chunk *chunk_at(const dm_scene *s, int cx, int cz)
{
    return &s->terrain->chunk[dm_wrap(cz, DM_CHUNKS) * DM_CHUNKS + dm_wrap(cx, DM_CHUNKS)];
}

/* A chunk's mesh, drawn with the terrain's one model matrix m: its
   positions moved by the chunk's place from the origin (dx, dz, world
   units) into s->pos, which is drawn and then free again. */
static void draw_chunk_mesh(dm_scene *s, const jb_mesh *mesh, int dx, int dz, const jb_mat4 *m, float bias)
{
    jb_mesh moved = *mesh;
    int k;

    for (k = 0; k < mesh->vertex_count; ++k) {
        s->pos[k] = mesh->positions[k];
        s->pos[k].x = (int16_t)(s->pos[k].x + dx);
        s->pos[k].z = (int16_t)(s->pos[k].z + dz);
    }
    moved.positions = s->pos;
    draw_mesh(s, &moved, m, bias, 0);
}

/*
 * A visible chunk's sea, with materials (its fade level's), fogged towards
 * fog_to: its vertices raised and lowered by the swell (sea_height, at
 * their world positions, rounded to whole units), and coloured by it,
 * Gouraud: lighter where a slope faces the sun and on the crests, warmer
 * there, darker on the slopes turned away and in the troughs, so that
 * bands of light roll across the sea with the waves.
 */
static void draw_sea(dm_scene *s, const dm_chunk *ch, const dm_visible *v, int ox, int oz,
                     const jb_mat4 *m, const jb_material *materials, uint32_t fog_to)
{
    /* the sun's direction across the sea, unit */
    static const float sx = 0.7969f, sz = -0.6042f;
    jb_mesh sea = ch->water;
    int k, dx = (v->cx - ox) * DM_CHUNK_SIZE, dz = (v->cz - oz) * DM_CHUNK_SIZE;

    for (k = 0; k < sea.vertex_count; ++k) {
        float x = (float)(v->cx * DM_CHUNK_SIZE + ch->water_pos[k].x);
        float z = (float)(v->cz * DM_CHUNK_SIZE + ch->water_pos[k].z), gx, gz;
        float h = sea_height(s, x, z, &gx, &gz);
        /* facing: the slope down towards the sun, up to about 0.05 */
        int lit = (int)(-(gx * sx + gz * sz) * 1600.0f + (h - SEA_MEAN) * 4.0f);
        int b = 128 + lit, r = b + (lit > 0 ? lit / 3 : 0), bl = b - (lit > 0 ? lit / 4 : 0);
        s->pos[k].x = (int16_t)(ch->water_pos[k].x + dx);
        s->pos[k].y = (int16_t)dm_floor(h + 0.5f);
        s->pos[k].z = (int16_t)(ch->water_pos[k].z + dz);
        r = r < 0 ? 0 : r > 255 ? 255 : r, b = b < 0 ? 0 : b > 255 ? 255 : b;
        bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
        s->sea_color[k] = JB_RGB(r, b, bl);
    }
    sea.positions = s->pos, sea.colors = s->sea_color, sea.materials = materials;
    fog(s, fog_to);
    draw_mesh(s, &sea, m, FAR_BIAS, 0);
}

/* One of a visible chunk's meshes, with materials (as many as the mesh's:
   those of its fade level), fogged towards fog_to (its texture's colour
   for the haze: haze_for). */
static void draw_part(dm_scene *s, const jb_mesh *mesh, const dm_visible *v, int ox, int oz,
                      const jb_mat4 *m, float bias, const jb_material *materials, uint32_t fog_to)
{
    jb_mesh part = *mesh;

    part.materials = materials;
    fog(s, fog_to);
    draw_chunk_mesh(s, &part, (v->cx - ox) * DM_CHUNK_SIZE, (v->cz - oz) * DM_CHUNK_SIZE, m, bias);
}

/*
 * A chunk's fade level: the haze's amount where the chunk is nearest the
 * eye (its bounds' least depth along the view, at a corner of the box),
 * as the depth cue has it ((1/start - 1/z) / (1/start - 1/end)), on
 * DM_FADES - 1 steps, rounded down so that no part of it is faded more
 * than the haze there fades it. 0 with the haze off: nothing fades.
 */
static int fade_level(const dm_scene *s, const view *vw, int cx, int cz, int lo, int hi)
{
    float near = 1e30f, f;
    int c, level;

    if (!s->sw.fog)
        return 0;
    for (c = 0; c < 8; ++c) {
        float x = (float)((cx + (c & 1)) * DM_CHUNK_SIZE) - vw->eye.x;
        float y = (float)(c & 2 ? hi : lo) - vw->eye.y;
        float z = (float)((cz + (c >> 2 & 1)) * DM_CHUNK_SIZE) - vw->eye.z;
        float d = x * vw->fwd.x + y * vw->fwd.y + z * vw->fwd.z;
        near = d < near ? d : near;
    }
    if (near <= FOG_START)
        f = 0;
    else
        f = (1.0f / FOG_START - 1.0f / near) / (1.0f / FOG_START - 1.0f / FOG_END);
    f = f > 1 ? 1 : f;
    level = (int)(f * (DM_FADES - 1)) + s->fade_bias;
    return level > DM_FADES - 1 ? DM_FADES - 1 : level;
}

/*
 * The terrain: every chunk that may be in view (its bounds, land, seabed
 * and water, in the frustum), all placed by one model matrix, a
 * translation to the corner of the chunk the eye is over, each chunk's
 * positions offset from there (draw_chunk_mesh). So a vertex on a seam
 * between two chunks (land, seabed or water) is one and the same input to
 * the GTE from either: with a matrix of each chunk's own, its fixed-point
 * rounding differed by a unit from one side to the other, and a crack a
 * pixel wide opened along the seam.
 */
static void terrain(dm_scene *s, const view *vw)
{
    float reach = DM_CHUNK_SIZE * 0.75f;
    int n = 0, cx, cz, i, k, x0, x1, z0, z1, ox, oz;
    jb_mat4 m;

    x0 = dm_floor((vw->eye.x - Z_FAR - reach) / DM_CHUNK_SIZE);
    x1 = dm_floor((vw->eye.x + Z_FAR + reach) / DM_CHUNK_SIZE);
    z0 = dm_floor((vw->eye.z - Z_FAR - reach) / DM_CHUNK_SIZE);
    z1 = dm_floor((vw->eye.z + Z_FAR + reach) / DM_CHUNK_SIZE);
    for (cz = z0; cz <= z1; ++cz)
        for (cx = x0; cx <= x1 && n < DM_MAX_VISIBLE; ++cx) {
            const dm_chunk *ch = chunk_at(s, cx, cz);
            int lo = ch->lo < 0 ? ch->lo : 0, hi = ch->hi > 0 ? ch->hi : 0;
            float rx = (float)(cx * DM_CHUNK_SIZE + DM_CHUNK_SIZE / 2) - vw->eye.x;
            float rz = (float)(cz * DM_CHUNK_SIZE + DM_CHUNK_SIZE / 2) - vw->eye.z;
            float ry = (float)(lo + hi) / 2 - vw->eye.y;
            /* Its bounds' radius, at most half the box's diagonal: 363
               across (256 root 2) and half its height. */
            float r = 363.0f + (float)(hi - lo) / 2;
            if (in_view(vw, rx, ry, rz, r, Z_FAR)) {
                s->visible[n].cx = cx, s->visible[n].cz = cz, s->visible[n].dist2 = rx * rx + rz * rz;
                s->visible[n].fade = fade_level(s, vw, cx, cz, lo, hi);
                ++n;
            }
        }
    qsort(s->visible, (size_t)n, sizeof s->visible[0], far_first);
    ox = dm_floor(vw->eye.x / DM_CHUNK_SIZE), oz = dm_floor(vw->eye.z / DM_CHUNK_SIZE);
    m = jb_mat4_translate((float)(ox * DM_CHUNK_SIZE), 0, (float)(oz * DM_CHUNK_SIZE));
    /* The seabed, then the water, into the farthest bucket in call order,
       farthest chunk first; then the land, sorted by depth, a material at
       a time. */
    for (i = 0; i < n && !(s->gaps & DM_GAPS_WATER); ++i)
        if (chunk_at(s, s->visible[i].cx, s->visible[i].cz)->bed.face_count > 0)
            draw_part(s, &chunk_at(s, s->visible[i].cx, s->visible[i].cz)->bed, &s->visible[i], ox, oz, &m,
                      FAR_BIAS, &s->bed_fade[s->visible[i].fade], s->haze_for[DM_TEX_SAND]);
    for (i = 0; i < n; ++i)
        if (chunk_at(s, s->visible[i].cx, s->visible[i].cz)->has_water) {
            jb_material sea = s->water_fade[s->visible[i].fade];
            if (s->gaps & DM_GAPS_WATER) /* the gap check's sea layer: the sea alone, opaque */
                sea.blend = JB_BLEND_NONE;
            draw_sea(s, chunk_at(s, s->visible[i].cx, s->visible[i].cz), &s->visible[i], ox, oz, &m, &sea,
                     s->haze_for[DM_TEX_WATER]);
        }
    for (k = 0; k < DM_LAND_MATERIALS && !(s->gaps & DM_GAPS_WATER); ++k)
        for (i = 0; i < n; ++i) {
            const dm_chunk *ch = chunk_at(s, s->visible[i].cx, s->visible[i].cz);
            jb_mesh part = ch->land;
            part.faces = ch->land_faces + ch->land_first[k];
            part.face_count = ch->land_first[k + 1] - ch->land_first[k];
            if (part.face_count > 0)
                draw_part(s, &part, &s->visible[i], ox, oz, &m, 0, s->land_fade[s->visible[i].fade],
                          s->haze_for[s->land_mat[k].texture - s->assets.tex]);
        }
    fog(s, HAZE);
}

static uint32_t mix(uint32_t h);

/*
 * The sun's glitter on the sea: small glints, added, along the path from
 * the eye towards the sun across the water, where a flat sea mirrors it.
 * The path is cut into cells of the world 24 units square, and a cell
 * glints in a frame when an integer hash of its place and of the loop's
 * frame over three says so (one in two), brighter or dimmer, so
 * the glints flicker where they are as the eye moves along; the path
 * widens with distance, and only cells over the sea glint, at its height
 * there. Fogged to black, as every glow is.
 */
static void glitter(dm_scene *s, const view *vw, unsigned frame)
{
    static const float sx = 0.7969f, sz = -0.6042f; /* towards the sun, unit */
    uint32_t tick = (frame % DM_LOOP) / 3u;
    int d, j;

    for (d = 160; d <= 2400; d += 40)
        for (j = -1; j <= 1; ++j) {
            float half = 12.0f + 0.05f * (float)d, gx, gz;
            float x = vw->eye.x + sx * (float)d - sz * half * (float)j;
            float z = vw->eye.z + sz * (float)d + sx * half * (float)j;
            int cx = dm_floor(x / 24), cz = dm_floor(z / 24);
            uint32_t hs = mix((uint32_t)dm_wrap(cx, DM_WORLD / 24) * 7919u ^
                              mix((uint32_t)dm_wrap(cz, DM_WORLD / 24) + tick * 2654435761u));
            jb_vec3 at;
            int b;
            if ((hs & 1u) != 0 || dm_ground(s->terrain, x, z) > -4)
                continue;
            at.x = (float)(cx * 24) + (float)(hs >> 8 & 15u) + 4;
            at.z = (float)(cz * 24) + (float)(hs >> 12 & 15u) + 4;
            at.y = sea_height(s, at.x, at.z, &gx, &gz) + 1;
            b = 110 + (int)(hs >> 16 & 127u);
            glow(s, at, 12.0f + 0.02f * (float)d, JB_RGB(b, b * 7 / 8, b * 5 / 8), 0);
        }
}

static void things(dm_scene *s, const view *vw, unsigned frame)
{
    const dm_place *lh = &s->lighthouse;
    float lx = nearest(lh->x, vw->eye.x), lz = nearest(lh->z, vw->eye.z);
    jb_vec3 one = {1, 1, 1}, tower = {vw->eye.x + lx, lh->y, vw->eye.z + lz};
    jb_vec3 lamp = {tower.x, tower.y + 249, tower.z};
    /* The tower and its beams, 760 long, round the lamp. */
    int lighthouse = in_view(vw, lx, lamp.y - vw->eye.y, lz, 900, TREE_RANGE + 900);
    jb_mat4 m;
    int i;

    /* The trees. */
    for (i = 0; i < s->trees; ++i) {
        const dm_place *p = &s->tree[i];
        float rx = nearest(p->x, vw->eye.x), rz = nearest(p->z, vw->eye.z);
        jb_vec3 at = {vw->eye.x + rx, p->y, vw->eye.z + rz}, turn = {0, p->turn * TURN, 0};
        jb_vec3 size = {p->size, p->size, p->size};
        /* A tree is 172 high and 92 across, times its size. */
        if (!in_view(vw, rx, p->y + 86 * p->size - vw->eye.y, rz, 100 * p->size, TREE_RANGE))
            continue;
        m = jb_mat4_trs(at, turn, size);
        draw_mesh(s, &s->assets.tree, &m, 0, 0);
    }
    /* The standing stones. */
    for (i = 0; i < DM_STONES; ++i) {
        const dm_place *p = &s->stone[i];
        float rx = nearest(p->x, vw->eye.x), rz = nearest(p->z, vw->eye.z);
        jb_vec3 at = {vw->eye.x + rx, p->y, vw->eye.z + rz}, turn = {0.05f, p->turn * TURN, 0};
        /* A stone is 98 high and 30 across. */
        if (!in_view(vw, rx, p->y + 49 - vw->eye.y, rz, 60, TREE_RANGE))
            continue;
        m = jb_mat4_trs(at, turn, one);
        fog(s, s->haze_for[DM_TEX_ROCK]);
        draw_mesh(s, &s->assets.stone, &m, 0, 0);
        fog(s, HAZE);
    }
    /* The pier, textured and fogged towards the planks' colour for the
       haze: its middle, 288 out, and 300 round it. */
    {
        const dm_place *p = &s->pier;
        float rx = nearest(p->x + 288 * SN(p->turn), vw->eye.x), rz = nearest(p->z + 288 * CS(p->turn), vw->eye.z);
        jb_vec3 at = {vw->eye.x + rx - 288 * SN(p->turn), p->y, vw->eye.z + rz - 288 * CS(p->turn)};
        jb_vec3 turn = {0, (p->turn + 0.5f) * TURN, 0};
        if (in_view(vw, rx, p->y - vw->eye.y, rz, 300, TREE_RANGE + 300)) {
            /* The deck (its first faces, the textured ones), then the
               posts. */
            jb_mesh deck = s->assets.pier, posts = s->assets.pier;
            deck.face_count = 0;
            while (deck.face_count < s->assets.pier.face_count &&
                   s->assets.pier.materials[s->assets.pier.faces[deck.face_count].material].texture != NULL)
                ++deck.face_count;
            posts.faces += deck.face_count, posts.face_count -= deck.face_count;
            m = jb_mat4_trs(at, turn, one);
            fog(s, s->haze_for[DM_TEX_PLANKS]);
            if (!(s->hide & DM_HIDE_DECK))
                draw_mesh(s, &deck, &m, 0, 0);
            if (!(s->hide & DM_HIDE_POSTS) && posts.face_count > 0)
                draw_mesh(s, &posts, &m, POST_BIAS, 0);
            fog(s, HAZE);
        }
    }
    /* The lighthouse. */
    if (lighthouse) {
        m = jb_mat4_translate(tower.x, tower.y, tower.z);
        draw_mesh(s, &s->assets.tower, &m, 0, 0);
    }

    /* What glows, added, fogged to black. */
    fog(s, 0);
    glitter(s, vw, frame);
    /* Fireflies about every fifth near tree. */
    for (i = 0; i < s->trees; i += 5) {
        const dm_place *p = &s->tree[i];
        float rx = nearest(p->x, vw->eye.x), rz = nearest(p->z, vw->eye.z);
        float a = phase(frame, 240) + p->turn, b = 140 + 60 * SN(phase(frame, 37) + p->turn);
        jb_vec3 fly;
        if (!in_view(vw, rx, p->y + 70 - vw->eye.y, rz, 100, TREE_RANGE) ||
            rx * rx + rz * rz >= (float)GLOW_RANGE * GLOW_RANGE)
            continue;
        fly.x = vw->eye.x + rx + 52 * SN(a);
        fly.y = p->y + 40 + 30 * SN(phase(frame, 90) + p->turn * 3);
        fly.z = vw->eye.z + rz + 52 * CS(a);
        glow(s, fly, 16, JB_RGB((int)b, (int)b, (int)(b / 3)), 0);
    }
    /* The lighthouse's two beams, turning once in 300 frames, and its lamp,
       which flares as a beam sweeps past the eye. */
    if (lighthouse) {
        float spin = phase(frame, 300), ex, ez, face, b;
        jb_vec3 to_eye;
        for (i = 0; i < 2; ++i) {
            jb_vec3 r = {0, spin * TURN + (float)i * TURN / 2, 0};
            m = jb_mat4_trs(lamp, r, one);
            draw_mesh(s, &s->assets.beam, &m, 0, 0);
        }
        /* A beam points along -z turned by spin: (-sin, -cos). */
        ex = -SN(spin), ez = -CS(spin);
        to_eye.x = -lx, to_eye.y = 0, to_eye.z = -lz;
        to_eye = jb_vec3_normalize(to_eye);
        face = fabs_(ex * to_eye.x + ez * to_eye.z);
        face = face * face * face * face;
        b = 120 + 135 * face;
        glow(s, lamp, 60 + 90 * face, JB_RGB((int)b, (int)(b * 0.9f), (int)(b * 0.6f)), 0);
    }
    fog(s, HAZE);
}

jb_result dm_scene_draw(dm_scene *s, unsigned frame, int paused)
{
    const dm_assets *a = &s->assets;
    view vw = make_view(s, frame);
    jb_camera cam;
    jb_vec3 sun;
    int k;
    int w, h;

    if (jb_begin_frame(s->jb, s->clear) != JB_OK || jb_frame_size(s->jb, &w, &h) != JB_OK)
        return jb_check(s->jb, NULL); /* this frame's failure, taken */
    for (k = 0; k < DM_WAVES; ++k)
        s->sea_phase[k] = phase(frame, (unsigned)waves[k].period);
    /* The palette animation: the water's file holds a palette for each
       phase its ripples turn through, one a frame every four. */
    jb_palette_write(s->jb, &a->tex[DM_TEX_WATER], 0, 16,
                     a->water + 16 * (frame / 4u % (unsigned)a->water_frames));
    /* and its fade levels' palettes, the same colours faded */
    for (k = 1; k < DM_FADES; ++k) {
        uint16_t pal[16];
        fade_palette(a->water + 16 * (frame / 4u % (unsigned)a->water_frames), s->fade_to[3], k, pal);
        jb_palette_write(s->jb, &s->fade_clut[k][3], 0, 16, pal);
    }

    memset(&cam, 0, sizeof cam);
    cam.eye = vw.eye;
    cam.target.x = vw.eye.x + 256 * vw.fwd.x;
    cam.target.y = vw.eye.y + 256 * vw.fwd.y;
    cam.target.z = vw.eye.z + 256 * vw.fwd.z;
    cam.z_near = Z_NEAR, cam.z_far = Z_FAR, cam.projection = PROJECTION;
    jb_set_camera(s->jb, &cam);
    fog(s, HAZE);
    /* The view, for dm_scene_covered. */
    s->eye = vw.eye, s->fwd = vw.fwd, s->right = vw.right, s->up = vw.up;

    if (!(s->gaps & DM_GAPS_NO_SKY))
        sky(s, &vw, frame, w, h);

    /* The sun, far off and first into the farthest bucket: a wide glow and
       a bright core, added. */
    sun.x = vw.eye.x + 1300 * sun_dir.x, sun.y = vw.eye.y + 1300 * sun_dir.y;
    sun.z = vw.eye.z + 1300 * sun_dir.z;
    glow(s, sun, 520, JB_RGB(200, 120, 70), FAR_BIAS);
    glow(s, sun, 110, JB_RGB(255, 240, 200), FAR_BIAS);
    vw.half_w = (float)w / 2 / PROJECTION, vw.half_h = (float)h / 2 / PROJECTION;
    terrain(s, &vw);
    if (!(s->gaps & DM_GAPS_WATER))
        things(s, &vw, frame);
    if (s->sw.panel)
        panel(s, paused);
    return jb_end_frame(s->jb);
}

jb_result dm_scene_set_switches(dm_scene *s, const dm_switches *sw)
{
    jb_result r;

    if ((r = jb_set_scale(s->jb, sw->scale)) != JB_OK ||
        (r = jb_set_widescreen(s->jb, sw->widescreen)) != JB_OK ||
        (r = jb_set_geometry(s->jb, sw->geometry)) != JB_OK) {
        /* Reported here, not by the next frame; the old switches kept. */
        jb_check(s->jb, NULL);
        jb_set_scale(s->jb, s->sw.scale);
        jb_set_widescreen(s->jb, s->sw.widescreen);
        jb_set_geometry(s->jb, s->sw.geometry);
        jb_check(s->jb, NULL);
        return r;
    }
    jb_set_precise_vertices(s->jb, sw->precise);
    jb_set_perspective_correct(s->jb, sw->perspective);
    jb_set_dither(s->jb, sw->dither);
    jb_set_clut_cache(s->jb, sw->clut_cache);
    s->sw = *sw;
    return JB_OK;
}

/* An integer hash, for placing things. */
static uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

/* The steepest rise from vertex (i, j) to a neighbour. */
static int slope(const dm_terrain *t, int i, int j)
{
    int h = dm_vertex_height(t, i, j), d = 0, k;
    static const int n[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

    for (k = 0; k < 4; ++k) {
        int e = dm_vertex_height(t, i + n[k][0], j + n[k][1]) - h;
        e = e < 0 ? -e : e;
        d = e > d ? e : d;
    }
    return d;
}

/* The squared distance on the repeating map between (x, z) and (px, pz). */
static float map_dist2(float x, float z, float px, float pz)
{
    float dx = nearest(x, px), dz = nearest(z, pz);

    return dx * dx + dz * dz;
}

/* The path's unit, 0..DM_PATH, where it passes nearest (x, z), and in *d2
   the squared distance there (across the ground). */
static float nearest_tau(float x, float z, float *d2)
{
    float best = 1e30f, tau = 0, px, pz, d;
    int k, at = 0;

    for (k = 0; k < DM_PATH; k += 8) {
        path((float)k, &px, &pz);
        if ((d = map_dist2(x, z, px, pz)) < best)
            best = d, at = k;
    }
    for (k = at - 8; k <= at + 8; ++k) {
        path((float)k, &px, &pz);
        if ((d = map_dist2(x, z, px, pz)) <= best)
            best = d, tau = (float)dm_wrap(k, DM_PATH);
    }
    if (d2 != NULL)
        *d2 = best;
    return tau;
}

/*
 * The pier: from a low beach (a vertex 1 to 30 up) straight out to sea,
 * on one of 16 headings, where its whole length (576, and 36 either side)
 * is over water but its first 64, which may rest on the beach (no higher
 * than 30); the deck 8 above the highest ground under it. Of those, the
 * one whose middle the camera passes nearest to 260 units (beside it,
 * close enough to see the planks bend), away from the stones' passage and
 * from the loop's ends (so each slow passage fits in the loop whole): the
 * sight of the slow passage s->slow[1].
 */
static void place_pier(dm_scene *s)
{
    const dm_terrain *t = s->terrain;
    float best = 1e30f;
    int i, j, k, side, d;

    for (j = 0; j < DM_MAP; ++j)
        for (i = 0; i < DM_MAP; ++i) {
            int hgt = dm_vertex_height(t, i, j);
            if (hgt < 1 || hgt > 30)
                continue;
            for (k = 0; k < 16; ++k) {
                float a = (float)k / 16, dx = SN(a), dz = CS(a), x = (float)(i * DM_CELL), z = (float)(j * DM_CELL);
                float d2, tau, gap, score, top = 0;
                int ok = 1;
                for (d = 0; d <= 576 && ok; d += 32)
                    for (side = -1; side <= 1 && ok; ++side) {
                        float g = dm_ground(t, x + dx * (float)d + dz * 36.0f * (float)side,
                                            z + dz * (float)d - dx * 36.0f * (float)side);
                        ok = d >= 64 ? g < -2 : g <= 30;
                        top = g > top ? g : top;
                    }
                if (!ok)
                    continue;
                tau = nearest_tau(x + dx * 288, z + dz * 288, &d2);
                gap = tau - s->slow[0].tau;
                gap = gap < 0 ? -gap : gap;
                gap = gap > DM_PATH / 2 ? DM_PATH - gap : gap;
                if (tau < DM_SLOW_W || tau > DM_PATH - DM_SLOW_W || gap < 700 || d2 < 120.0f * 120.0f)
                    continue;
                score = d2 - 260.0f * 260.0f;
                score = score < 0 ? -score : score;
                if (score < best) {
                    best = score;
                    s->pier.x = x, s->pier.y = top + 8, s->pier.z = z, s->pier.turn = a;
                    s->slow[1].tau = tau;
                }
            }
        }
    s->slow[1].at.x = s->pier.x + 288 * SN(s->pier.turn);
    s->slow[1].at.y = s->pier.y;
    s->slow[1].at.z = s->pier.z + 288 * CS(s->pier.turn);
    s->slow[1].lower = 150;
}

/* The slow passages in the order they come round the loop, and the frame
   each starts at: its middle where the camera passes its sight. */
static void slow_starts(dm_scene *s)
{
    int k;

    if (s->slow[1].tau < s->slow[0].tau) {
        dm_slow first = s->slow[1];
        s->slow[1] = s->slow[0], s->slow[0] = first;
    }
    for (k = 0; k < DM_SLOWS; ++k)
        s->slow[k].start = (unsigned)((int)s->slow[k].tau + k * DM_SLOW_A + DM_SLOW_A / 2 - DM_SLOW_W / 2);
}

/* Where the trees, the stones and the lighthouse stand: all from the
   heightmap and a hash, so the same on every machine. */
static void place(dm_scene *s)
{
    const dm_terrain *t = s->terrain;
    float best = 1e30f, px, pz, sx, sz;
    int i, j, k, bi = 0, bj = 0, top = -1;

    s->trees = 0;
    for (j = 0; j < DM_MAP; ++j)
        for (i = 0; i < DM_MAP; ++i) {
            uint32_t hs = mix((uint32_t)(j * DM_MAP + i) * 3u + 1u);
            /* Groves: a block of 8 x 8 cells in three is wooded. */
            int grove = mix((uint32_t)(j / 8 * DM_MAP + i / 8) + 5u) % 3u == 0;
            int hgt = dm_vertex_height(t, i, j);
            float x, z;
            if (t->material[j * DM_MAP + i] != DM_GRASS || hs % (grove ? 4u : 40u) != 0 || hgt < 40 ||
                hgt > 520 ||
                slope(t, i, j) > 48 || s->trees == DM_MAX_TREES)
                continue;
            x = (float)(i * DM_CELL + 16 + (int)(hs >> 8 & 31u));
            z = (float)(j * DM_CELL + 16 + (int)(hs >> 13 & 31u));
            s->tree[s->trees].x = x, s->tree[s->trees].z = z;
            s->tree[s->trees].y = dm_ground(t, x, z) - 4;
            s->tree[s->trees].turn = (float)(hs >> 18 & 255u) / 256.0f;
            s->tree[s->trees].size = 0.7f + (float)(hs >> 26 & 63u) / 100.0f;
            ++s->trees;
        }
    /* The lighthouse: on a headland near the first part of the flight
       but not under it, a vertex 40-220 up with deep water within three
       cells, at least 700 units from where the camera passes. */
    path(DM_PATH * 0.10f, &px, &pz);
    for (j = 0; j < DM_MAP; ++j)
        for (i = 0; i < DM_MAP; ++i) {
            int hgt = dm_vertex_height(t, i, j), deep = 0, di, dj;
            float d, x = (float)(i * DM_CELL), z = (float)(j * DM_CELL);
            if (hgt < 40 || hgt > 220 || slope(t, i, j) > 60)
                continue;
            for (dj = -3; dj <= 3; ++dj)
                for (di = -3; di <= 3; ++di)
                    deep |= dm_vertex_height(t, i + di, j + dj) < -30;
            d = map_dist2(x, z, px, pz);
            if (!deep || d >= best)
                continue;
            for (k = 0; k < DM_PATH; k += 16) {
                float qx, qz;
                path((float)k, &qx, &qz);
                if (map_dist2(x, z, qx, qz) < 700.0f * 700.0f)
                    break;
            }
            if (k == DM_PATH)
                best = d, bi = i, bj = j;
        }
    s->lighthouse.x = (float)(bi * DM_CELL), s->lighthouse.z = (float)(bj * DM_CELL);
    s->lighthouse.y = (float)dm_vertex_height(t, bi, bj) - 6;
    /* The stones: on the highest grassy, gentle ground near the middle of
       the flight, in a ring. */
    path(DM_PATH * 0.55f, &px, &pz);
    for (j = 0; j < DM_MAP; ++j)
        for (i = 0; i < DM_MAP; ++i) {
            int hgt = dm_vertex_height(t, i, j);
            if (t->material[j * DM_MAP + i] == DM_GRASS && slope(t, i, j) < 24 && hgt > top &&
                map_dist2((float)(i * DM_CELL), (float)(j * DM_CELL), px, pz) < 1500.0f * 1500.0f)
                top = hgt, bi = i, bj = j;
        }
    sx = (float)(bi * DM_CELL), sz = (float)(bj * DM_CELL);
    for (k = 0; k < DM_STONES; ++k) {
        float a = (float)k / DM_STONES;
        dm_place *p = &s->stone[k];
        p->x = sx + 96 * SN(a), p->z = sz + 96 * CS(a);
        p->y = dm_ground(t, p->x, p->z) - 6;
        p->turn = a + 0.25f, p->size = 1;
    }
    s->slow[0].at.x = sx, s->slow[0].at.y = dm_ground(t, sx, sz) + 40, s->slow[0].at.z = sz;
    s->slow[0].lower = 60;
    s->slow[0].tau = nearest_tau(sx, sz, NULL);
    place_pier(s);
    slow_starts(s);
    /* No tree in the ring or at the lighthouse. */
    for (k = 0; k < s->trees;) {
        if (map_dist2(s->tree[k].x, s->tree[k].z, sx, sz) < 160.0f * 160.0f ||
            map_dist2(s->tree[k].x, s->tree[k].z, s->lighthouse.x, s->lighthouse.z) < 120.0f * 120.0f)
            s->tree[k] = s->tree[--s->trees];
        else
            ++k;
    }
}

jb_result dm_scene_init(dm_scene *s, const dm_switches *sw, int threads)
{
    /* The low sun, warm, from its direction; a cool light from the sky
       above; a dim violet ambient. */
    jb_light sun = {{-0.7911f, -0.1197f, 0.5998f}, JB_RGB(255, 196, 140), 1.35f};
    jb_light skylight = {{0.2f, -1.0f, 0.1f}, JB_RGB(70, 80, 130), 0};
    jb_config c;
    jb_result r;
    int i;

    memset(s, 0, sizeof *s);
    s->clear = HAZE;
    memset(&c, 0, sizeof c);
    c.scale = sw->scale, c.threads = threads, c.log = sw->log, c.log_user = sw->log_user;
    c.widescreen = 1; /* buffers with room for 16:9, switched at run time */
    c.max_primitives = 32768;
    c.ot_size = 2048;
    if ((s->terrain = malloc(sizeof *s->terrain)) == NULL)
        return JB_ERROR_OUT_OF_MEMORY;
    if ((s->jb = jb_create(&c, &r)) == NULL) {
        free(s->terrain);
        s->terrain = NULL;
        return r;
    }
    if ((r = dm_assets_load(s->jb, &s->assets)) != JB_OK || (r = jb_set_ambient(s->jb, JB_RGB(64, 56, 92))) != JB_OK ||
        (r = jb_set_light(s->jb, 0, &sun)) != JB_OK || (r = jb_set_light(s->jb, 1, &skylight)) != JB_OK ||
        (r = dm_scene_set_switches(s, sw)) != JB_OK) {
        dm_scene_free(s);
        return r;
    }
    /* The terrain's materials: lit, textured, their colour the vertices';
       the water unlit and blended half over the seabed. */
    for (i = 0; i < DM_LAND_MATERIALS; ++i)
        s->land_mat[i] = (jb_material){&s->assets.tex[i == DM_SAND ? DM_TEX_SAND : i == DM_GRASS ? DM_TEX_GRASS : DM_TEX_ROCK],
                                       0, JB_BLEND_NONE, 0, 0, 0};
    s->bed_mat = (jb_material){&s->assets.tex[DM_TEX_SAND], 0, JB_BLEND_NONE, 0, 0, 0};
    s->water_mat = (jb_material){&s->assets.tex[DM_TEX_WATER], 0, JB_BLEND_HALF, 1, 0, 0};
    for (i = 0; i < DM_TEXTURES; ++i) {
        s->mean[i] = texture_mean(s->jb, &s->assets.tex[i]);
        s->haze_for[i] = haze_for(s->mean[i]);
    }
    if ((r = make_fades(s)) != JB_OK) {
        dm_scene_free(s);
        return r;
    }
    dm_terrain_generate(s->terrain);
    dm_terrain_uv(s->land_uv, s->water_uv);
    dm_terrain_build(s->terrain, s->land_mat, &s->bed_mat, &s->water_mat, s->land_uv, s->water_uv);
    place(s);
    return JB_OK;
}

const uint8_t *dm_scene_frame(dm_scene *s, int *w, int *h)
{
    return jb_frame(s->jb, w, h);
}

/* Whether the ray through (sx, sy) (1x pixels from the frame's centre)
   meets the meshes' surface within DM_COVERED: dm_scene_covered. */
static int ray_hits(const dm_scene *s, float sx, float sy)
{
    float dx, dy, dz, t, lim = DM_COVERED, across2;

    /* The direction with a unit component along the view, so t is the
       depth. */
    dx = s->fwd.x + sx / PROJECTION * s->right.x - sy / PROJECTION * s->up.x;
    dy = s->fwd.y + sx / PROJECTION * s->right.y - sy / PROJECTION * s->up.y;
    dz = s->fwd.z + sx / PROJECTION * s->right.z - sy / PROJECTION * s->up.z;
    across2 = dx * dx + dz * dz;
    if (s->gaps & DM_GAPS_WATER) {
        /* The gap check's sea layer, the sea alone: where the ray meets
           it, somewhere between sea level and the swell's troughs
           (DM_SEA_DEPTH below), in a chunk that has sea, at both ends of
           that and a pixel and a half round them (the footprint there,
           t / H across and that over the sine of the ray's angle to the
           sea along it), where a corner of the sea's edge, rounded to a
           pixel, may be. */
        jb_vec3 d = {dx, dy, dz}, u;
        float x, z, m;
        int c, e;
        if (!(dy < 0))
            return 0;
        u = jb_vec3_normalize(d);
        for (e = 0; e < 2; ++e) {
            if ((t = (s->eye.y + (float)(e * DM_SEA_DEPTH)) / -dy) >= lim || t * t * across2 >= lim * lim)
                return 0;
            m = 1.5f * t / PROJECTION * (dx * u.x + dy * u.y + dz * u.z) / -dy;
            x = s->eye.x + t * dx, z = s->eye.z + t * dz;
            for (c = 0; c < 5; ++c) {
                float ox = c == 0 ? 0 : c & 1 ? m : -m, oz = c == 0 ? 0 : c & 2 ? m : -m;
                if (!chunk_at(s, dm_floor((x + ox) / DM_CHUNK_SIZE), dm_floor((z + oz) / DM_CHUNK_SIZE))->has_water)
                    return 0;
            }
        }
        return 1;
    }
    /* Steps of DM_COVERED_STEP near the surface, longer above it: a fifth
       of the height above it, which the ray cannot close in one step (the
       ground rises at most about 2 a unit across, the ray across at most
       about 1.6 a unit of depth). */
    for (t = Z_NEAR; t < lim && t * t * across2 < lim * lim;) {
        float above = s->eye.y + t * dy - dm_surface(s->terrain, s->eye.x + t * dx, s->eye.z + t * dz);
        /* Under it by a pixel and a half at this depth (t / H world units
           a 1x pixel), so that a ray grazing a far hill's top, which the
           drawn hill, its corners rounded to pixels, may miss, is not
           taken as one that must meet it. */
        if (above < -1.5f * t / PROJECTION)
            return 1;
        t += above / 5 > DM_COVERED_STEP ? above / 5 : DM_COVERED_STEP;
    }
    return 0;
}

int dm_scene_covered(const dm_scene *s, int x, int y)
{
    float cx, cy;
    int w, h, i, j;

    if (jb_frame_size(s->jb, &w, &h) != JB_OK)
        return 0;
    cx = ((float)x + 0.5f) / (float)s->sw.scale - (float)w / 2;
    cy = ((float)y + 0.5f) / (float)s->sw.scale - (float)h / 2;
    if (!ray_hits(s, cx, cy))
        return 0;
    for (j = -1; j <= 1; ++j)
        for (i = -1; i <= 1; ++i)
            if ((i != 0 || j != 0) && !ray_hits(s, cx + (float)i, cy + (float)j))
                return 0;
    return 1;
}

void dm_scene_free(dm_scene *s)
{
    jb_destroy(s->jb);
    s->jb = NULL;
    free(s->terrain);
    s->terrain = NULL;
}
