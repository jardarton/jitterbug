/*
 * jb-mesh's scene (mesh.h): growing arrays of faces and materials, and the
 * number parser both readers use.
 */
#include "mesh.h"

#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int jbm_add_face(jbm_scene *s, const jbm_face *f)
{
    if (s->face_count == s->face_cap) {
        size_t cap = s->face_cap ? s->face_cap * 2 : 256;
        jbm_face *g = realloc(s->faces, cap * sizeof *g);
        if (g == NULL)
            return -1;
        s->faces = g;
        s->face_cap = cap;
    }
    s->faces[s->face_count++] = *f;
    return 0;
}

int jbm_add_material(jbm_scene *s, const char *name)
{
    int i;

    for (i = 0; i < s->material_count; i++)
        if (strcmp(s->materials[i].name, name) == 0)
            return i;
    if (strlen(name) >= sizeof s->materials[0].name)
        return -1;
    if (s->material_count == s->material_cap) {
        int cap = s->material_cap ? s->material_cap * 2 : 16;
        jbm_material *m = realloc(s->materials, (size_t)cap * sizeof *m);
        if (m == NULL)
            return -1;
        s->materials = m;
        s->material_cap = cap;
    }
    memset(&s->materials[i], 0, sizeof s->materials[i]);
    strcpy(s->materials[i].name, name);
    s->materials[i].color[0] = s->materials[i].color[1] = s->materials[i].color[2] = 1.0;
    return s->material_count++;
}

void jbm_scene_free(jbm_scene *s)
{
    free(s->faces);
    free(s->materials);
    memset(s, 0, sizeof *s);
}

int jbm_parse_number(const char *t, const char **end, double *out)
{
    /* 10^0 .. 10^22, each exact in a double. */
    static const double pow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                     1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                     1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    uint64_t mant = 0;
    int neg = 0, sig = 0, exp = 0, any = 0;
    double v;

    if (*t == '+' || *t == '-')
        neg = *t++ == '-';
    for (; *t >= '0' && *t <= '9'; t++, any = 1) {
        if (sig < 19) {
            if (mant != 0 || *t != '0') {
                mant = mant * 10 + (uint64_t)(*t - '0');
                sig++;
            }
        } else {
            exp++; /* a digit dropped */
        }
    }
    if (*t == '.') {
        t++;
        for (; *t >= '0' && *t <= '9'; t++, any = 1) {
            if (sig < 19) {
                if (mant != 0 || *t != '0') {
                    mant = mant * 10 + (uint64_t)(*t - '0');
                    sig++;
                }
                exp--;
            }
        }
    }
    if (!any)
        return -1;
    if (*t == 'e' || *t == 'E') {
        const char *e = t + 1;
        int eneg = 0, ev = 0, edigits = 0;
        if (*e == '+' || *e == '-')
            eneg = *e++ == '-';
        for (; *e >= '0' && *e <= '9'; e++, edigits++)
            ev = ev < 100000 ? ev * 10 + (*e - '0') : ev;
        if (edigits == 0)
            return -1;
        exp += eneg ? -ev : ev;
        t = e;
    }
    v = (double)mant;
    if (mant != 0) {
        if (exp > 400)
            return -1;
        if (exp < -400) {
            v = 0.0;
        } else {
            while (exp > 22) {
                v *= pow10[22];
                exp -= 22;
            }
            while (exp < -22) {
                v /= pow10[22];
                exp += 22;
            }
            v = exp >= 0 ? v * pow10[exp] : v / pow10[-exp];
        }
        if (v > DBL_MAX)
            return -1;
    }
    *out = neg ? -v : v;
    if (end != NULL)
        *end = t;
    return 0;
}
