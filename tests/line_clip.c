/*
 * line_clip: a line is clipped by the drawing area and by nothing else.
 * psx-spx clips every primitive to the drawing area (GP0(E3h), GP0(E4h)),
 * its edges included, and says lines are dithered whether flat or Gouraud
 * (Line Commands). So every pixel of a line that lies in the area is
 * drawn, whichever edge the line crosses or starts on, flat or Gouraud,
 * dithered or not, opaque or semi-transparent, and no pixel outside it.
 *
 * A rule differential fuzzing cannot check (docs/testing.md,
 * "What differential fuzzing cannot check"): psxstack dithers no line, and
 * leaves out a sloped line's pixels in the area's last column and row. The
 * expected pixels come from an exact evaluation of each line, chosen with
 * no exact half anywhere in its stepping (so that no tie rule enters), and
 * the whole neighbourhood of the area is checked. Strict C99 against
 * <jitterbug.h>.
 */
#include "api_test.h"

enum { AX0 = 100, AY0 = 100, AX1 = 163, AY1 = 147, MARGIN = 40 };
enum { BW = AX1 - AX0 + 1 + 2 * MARGIN, BH = AY1 - AY0 + 1 + 2 * MARGIN };

static int floor_div(int n, int d) /* d > 0 */
{
    int q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

/* Whether (x, y) is a pixel of the line a-b: one per step along the
   major axis, the minor coordinate the nearest integer to the exact one
   (there are no exact halves in these lines). */
static int on_line(int ax, int ay, int bx, int by, int x, int y)
{
    int dx = bx - ax, dy = by - ay, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady, i;

    for (i = 0; i <= steps; ++i) {
        int px, py;
        if (steps == 0) {
            px = ax;
            py = ay;
        } else if (adx >= ady) {
            px = ax + (dx < 0 ? -i : i);
            py = ay + floor_div(2 * i * dy + steps, 2 * steps);
        } else {
            py = ay + (dy < 0 ? -i : i);
            px = ax + floor_div(2 * i * dx + steps, 2 * steps);
        }
        if (px == x && py == y)
            return 1;
    }
    return 0;
}

static void one(jb_context *jb, const int *l, int gouraud, int dither, int semi)
{
    static uint16_t pix[BW * BH];
    uint32_t w[8], op = 0x40u | (gouraud ? 0x10u : 0) | (semi ? 0x02u : 0);
    int n = 0, x, y, bad = 0;
    char what[160];

    /* the neighbourhood cleared, the drawing state, the line */
    w[n++] = 0x02000000u;
    w[n++] = xy(AX0 - MARGIN, AY0 - MARGIN);
    w[n++] = xy(BW + 16, BH);
    gp0(jb, w, (size_t)n);
    n = 0;
    w[n++] = 0xe1000000u | (dither ? 0x200u : 0); /* blend mode 0 */
    w[n++] = 0xe3000000u | AX0 | AY0 << 10;
    w[n++] = 0xe4000000u | AX1 | AY1 << 10;
    w[n++] = 0xe5000000u;
    w[n++] = 0xe6000000u;
    gp0(jb, w, (size_t)n);
    n = 0;
    w[n++] = op << 24 | 0xf8f8f8u;
    w[n++] = xy(l[0], l[1]);
    if (gouraud)
        w[n++] = 0xe0e0e0u;
    w[n++] = xy(l[2], l[3]);
    gp0(jb, w, (size_t)n);
    read_rect(jb, AX0 - MARGIN, AY0 - MARGIN, BW, BH, pix);
    for (y = 0; y < BH && !bad; ++y)
        for (x = 0; x < BW && !bad; ++x) {
            int vx = AX0 - MARGIN + x, vy = AY0 - MARGIN + y;
            int want = vx >= AX0 && vx <= AX1 && vy >= AY0 && vy <= AY1 &&
                       on_line(l[0], l[1], l[2], l[3], vx, vy);
            int got = (pix[y * BW + x] & 0x7fff) != 0;
            if (got != want) {
                snprintf(what, sizeof what, "line (%d, %d)-(%d, %d)%s%s%s: pixel (%d, %d) %s",
                         l[0], l[1], l[2], l[3], gouraud ? " Gouraud" : "",
                         dither ? " dithered" : "", semi ? " semi-transparent" : "", vx, vy,
                         want ? "not drawn" : "drawn outside the line or the area");
                check(what, got, want);
                bad = 1;
            }
        }
    if (!bad)
        check("a line's pixels", 1, 1);
}

int main(void)
{
    /* Across each edge and starting on it: horizontal, vertical, at 45
       degrees, and 21 x 8 (8 i / 21 is never an exact half). */
    static const int lines[][4] = {
        {AX0 - 10, 120, AX0 + 30, 120},         {AX0, 121, AX0 + 20, 121},
        {AX1 - 20, 122, AX1 + 10, 122},         {AX1 + 10, 123, AX1, 123},
        {130, AY0 - 10, 130, AY0 + 20},         {131, AY0, 131, AY0 + 9},
        {132, AY1 + 8, 132, AY1 - 15},          {133, AY1 - 6, 133, AY1},
        {AX0 - 5, AY0 - 5, AX0 + 20, AY0 + 20}, {AX1 + 4, AY0 - 4, AX1 - 20, AY0 + 20},
        {AX0 - 4, AY1 + 4, AX0 + 20, AY1 - 20}, {AX1 + 5, AY1 + 5, AX1 - 20, AY1 - 20},
        {AX0 - 8, 110, AX0 + 13, 118},          {AX0 + 13, 130, AX0 - 8, 122},
        {AX1 - 13, 112, AX1 + 8, 120},          {AX1 + 8, 140, AX1 - 13, 132},
        {120, AY0 - 8, 128, AY0 + 13},          {150, AY1 + 8, 142, AY1 - 13},
        {AX0, 115, AX0 + 21, 123},              {AX1, 135, AX1 - 21, 127},
    };
    jb_config c;
    int s, k, gouraud, dither, semi;

    for (s = 0; s < 2; ++s) {
        jb_context *jb;
        memset(&c, 0, sizeof c);
        c.scale = s ? 2 : 1;
        c.threads = s ? 2 : 1;
        jb = must_create(&c);
        for (k = 0; k < (int)(sizeof lines / sizeof lines[0]); ++k)
            for (gouraud = 0; gouraud < 2; ++gouraud)
                for (dither = 0; dither < 2; ++dither)
                    for (semi = 0; semi < 2; ++semi)
                        one(jb, lines[k], gouraud, dither, semi);
        jb_destroy(jb);
    }
    return finish("line_clip");
}
