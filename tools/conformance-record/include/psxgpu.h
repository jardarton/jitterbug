/*
 * The recorder's psxgpu.h: the primitive packets, structures and GPU calls
 * that the recorded ps1-tests programs use, and nothing else (the
 * clean-room shim of docs/testing.md, "How `tools/conformance-record` works").
 *
 * A packet is what the console's DMA walks: a tag word (the next packet's
 * address in bits 0-23, the number of words that follow in bits 24-31),
 * then the GP0 words, the command in the top byte of the first. The
 * programs fill packets by field name, so the structures below have the
 * names the programs use, in the order the GP0 command takes its
 * parameters (psx-spx, "GPU Render Polygon/Line/Rectangle Commands");
 * on a little-endian host, as on the console, each structure is those
 * words. The helper macros set fields; what each writes is psx-spx's
 * encoding of the command or attribute it names.
 */
#ifndef JBREC_PSXGPU_H
#define JBREC_PSXGPU_H

/* ---- Packet headers ---------------------------------------------------- */

/* Byte 3 of a packet is its length in words after the tag, byte 7 its GP0
   command (the top byte of the first GP0 word). */
#define JBREC_BYTE(p, i) (((unsigned char *)(p))[i])
#define setlen(p, n) (JBREC_BYTE(p, 3) = (unsigned char)(n))
#define setcode(p, c) (JBREC_BYTE(p, 7) = (unsigned char)(c))

/* A packet's length and command at once. */
#define JBREC_PACKET(p, n, c) (setlen(p, n), setcode(p, c))

/* Bit 1 of a polygon, line or rectangle command: semi-transparent. */
#define setSemiTrans(p, on) \
    (JBREC_BYTE(p, 7) = (unsigned char)((JBREC_BYTE(p, 7) & ~2u) | ((on) ? 2u : 0u)))

/* ---- Attributes (psx-spx, "Texpage Attribute", "Clut Attribute") ------- */

/* The texture page attribute, by field from bit 0: X base / 64 (4 bits),
   Y base / 256 (1 bit), the semi-transparency mode (2 bits), the colour
   depth, 0 4-bit, 1 8-bit, 2 15-bit (2 bits). */
#define JBREC_FIELD(v, width, at) (((unsigned)(v) & ((1u << (width)) - 1u)) << (at))
#define getTPage(depth, semi, x, y) \
    (JBREC_FIELD((depth), 2, 7) | JBREC_FIELD((semi), 2, 5) | JBREC_FIELD((y) / 256, 1, 4) | \
     JBREC_FIELD((x) / 64, 4, 0))
/* The CLUT attribute, from bit 0: X / 16 (6 bits), Y (9 bits). */
#define JBREC_CLUT(x, y) (JBREC_FIELD((y), 9, 6) | JBREC_FIELD((x) / 16, 6, 0))

#define setTPage(p, depth, semi, x, y) ((p)->tpage = getTPage(depth, semi, x, y))
#define setClut(p, x, y) ((p)->clut = JBREC_CLUT(x, y))

/* ---- Fields ------------------------------------------------------------ */

/* Vertex N's colour, position and texture coordinate. The arguments are
   assigned in order, left to right (a program may pass rand() for each). */
#define JBREC_RGB(p, N, R, G, B) ((p)->r##N = (R), (p)->g##N = (G), (p)->b##N = (B))
#define JBREC_XY(p, N, X, Y) ((p)->x##N = (X), (p)->y##N = (Y))
#define JBREC_UV(p, N, U, V) ((p)->u##N = (U), (p)->v##N = (V))

#define setRGB0(p, r, g, b) JBREC_RGB(p, 0, r, g, b)
#define setRGB1(p, r, g, b) JBREC_RGB(p, 1, r, g, b)
#define setRGB2(p, r, g, b) JBREC_RGB(p, 2, r, g, b)
#define setRGB3(p, r, g, b) JBREC_RGB(p, 3, r, g, b)

#define setXY0(p, x, y) JBREC_XY(p, 0, x, y)
#define setXY3(p, xa, ya, xb, yb, xc, yc) \
    (JBREC_XY(p, 0, xa, ya), JBREC_XY(p, 1, xb, yb), JBREC_XY(p, 2, xc, yc))
#define setXY4(p, xa, ya, xb, yb, xc, yc, xd, yd) \
    (setXY3(p, xa, ya, xb, yb, xc, yc), JBREC_XY(p, 3, xd, yd))
#define setWH(p, width, height) ((p)->w = (width), (p)->h = (height))

#define setUV0(p, u, v) JBREC_UV(p, 0, u, v)
#define setUV4(p, ua, va, ub, vb, uc, vc, ud, vd) \
    (JBREC_UV(p, 0, ua, va), JBREC_UV(p, 1, ub, vb), JBREC_UV(p, 2, uc, vc), JBREC_UV(p, 3, ud, vd))
/* A quad's corners from its top-left texel and size, in the order
   top-left, top-right, bottom-left, bottom-right. */
#define setUVWH(p, u, v, width, height) \
    setUV4(p, u, v, (u) + (width), v, u, (v) + (height), (u) + (width), (v) + (height))

/* ---- Packets: length and command (psx-spx's command numbers) ----------- */

#define setPolyF4(p) JBREC_PACKET(p, 5, 0x28)  /* flat quad */
#define setPolyFT4(p) JBREC_PACKET(p, 9, 0x2c) /* textured quad */
#define setPolyG3(p) JBREC_PACKET(p, 6, 0x30)  /* Gouraud triangle */
#define setPolyG4(p) JBREC_PACKET(p, 8, 0x38)  /* Gouraud quad */
#define setLineF2(p) JBREC_PACKET(p, 3, 0x40)  /* flat line */
#define setLineG2(p) JBREC_PACKET(p, 4, 0x50)  /* Gouraud line */
#define setTile(p) JBREC_PACKET(p, 3, 0x60)    /* rectangle of any size */
#define setSprt(p) JBREC_PACKET(p, 4, 0x64)    /* textured rectangle of any size */
#define setSprt8(p) JBREC_PACKET(p, 3, 0x74)   /* 8 x 8 textured rectangle */
#define setFill(p) JBREC_PACKET(p, 3, 0x02)    /* fill */

/* Polylines of four vertices, ending in a terminator word (psx-spx:
   5555_5555h is one); the Gouraud one also clears the high bytes of its
   later colour words. */
#define JBREC_END_OF_POLYLINE 0x55555555u
#define setLineF4(p) (JBREC_PACKET(p, 6, 0x4c), (p)->end = JBREC_END_OF_POLYLINE)
#define setLineG4(p) \
    (JBREC_PACKET(p, 9, 0x5c), (p)->pad1 = 0, (p)->pad2 = 0, (p)->pad3 = 0, \
     (p)->end = JBREC_END_OF_POLYLINE)

/* GP0(E1h), the draw mode: the texture page attribute's bits, dithering in
   bit 9, drawing to the displayed area allowed in bit 10. */
#define setDrawTPage(p, display, dither, tpage) \
    (setlen(p, 1), \
     (p)->code[0] = 0xe1000000u | ((unsigned)((tpage) | (dither) << 9 | (display) << 10) & 0xffffffu))
/* GP0(E6h), the mask bit setting: set it while drawing in bit 0, check it
   before drawing in bit 1. */
#define setDrawMask(p, set, check) \
    (setlen(p, 1), (p)->code[0] = 0xe6000000u | ((unsigned)((set) | (check) << 1) & 0xffffffu))

/* ---- Structures -------------------------------------------------------- */

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0, x1, y1, x2, y2, x3, y3;
} POLY_F4;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char u0, v0;
    unsigned short clut;
    short x1, y1;
    unsigned char u1, v1;
    unsigned short tpage;
    short x2, y2;
    unsigned char u2, v2;
    unsigned short pad0;
    short x3, y3;
    unsigned char u3, v3;
    unsigned short pad1;
} POLY_FT4;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char r1, g1, b1, pad1;
    short x1, y1;
    unsigned char r2, g2, b2, pad2;
    short x2, y2;
} POLY_G3;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char r1, g1, b1, pad1;
    short x1, y1;
    unsigned char r2, g2, b2, pad2;
    short x2, y2;
    unsigned char r3, g3, b3, pad3;
    short x3, y3;
} POLY_G4;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0, x1, y1;
} LINE_F2;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char r1, g1, b1, pad1;
    short x1, y1;
} LINE_G2;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0, x1, y1, x2, y2, x3, y3;
    unsigned int end;
} LINE_F4;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char r1, g1, b1, pad1;
    short x1, y1;
    unsigned char r2, g2, b2, pad2;
    short x2, y2;
    unsigned char r3, g3, b3, pad3;
    short x3, y3;
    unsigned int end;
} LINE_G4;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    short w, h;
} TILE;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char u0, v0;
    unsigned short clut;
    unsigned short w, h;
} SPRT;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    short x0, y0;
    unsigned char u0, v0;
    unsigned short clut;
} SPRT_8;

/* One GP0 word (DR_TPAGE also carries other one-word commands). */
typedef struct {
    unsigned int tag;
    unsigned int code[1];
} DR_TPAGE, DR_MASK;

typedef struct {
    unsigned int tag;
    unsigned char r0, g0, b0, code;
    unsigned short x0, y0;
    unsigned short w, h;
} FILL;

typedef struct {
    unsigned int tag;
    unsigned char p0, p1, p2, code;
    unsigned short x0, y0;
    unsigned short x1, y1;
    unsigned short w, h;
} VRAM2VRAM;

typedef struct {
    short x, y;
    short w, h;
} RECT;

/* The display: the area of VRAM shown, the picture's place on the screen
   (0 for the defaults), 24-bit colour, interlace. */
typedef struct {
    RECT disp;
    RECT screen;
    char isrgb24;
    char isinter;
    char reverse; /* not used */
} DISPENV;

/* Drawing: the drawing area and the offset from its corner; the draw
   mode's texture page bits, dithering, and drawing to the displayed area
   allowed; whether to fill the area with r0, g0, b0 when it is put; the
   texture window (not sent). */
typedef struct {
    RECT clip;
    short ofs[2];
    unsigned short tpage;
    unsigned char dtd;
    unsigned char dfe;
    unsigned char isbg;
    unsigned char r0, g0, b0;
    RECT tw;
} DRAWENV;

/* Where a TIM's parts are (GetTimInfo): its flags word, its image's
   rectangle and pixels, and its CLUT's (NULL when it has none). */
typedef struct {
    unsigned int mode;
    RECT *prect;
    unsigned int *paddr;
    RECT *crect;
    unsigned int *caddr;
} TIM_IMAGE;

#ifdef __cplusplus
extern "C" {
#endif

void ResetGraph(int mode);
void PutDispEnv(DISPENV *disp);
void PutDrawEnv(DRAWENV *draw);
void SetDispMask(int mask);
int VSync(int m);
int DrawSync(int m);
void LoadImage(RECT *rect, unsigned int *data);
void DrawPrim(void *pri);
int GetTimInfo(unsigned int *tim, TIM_IMAGE *timimg);
DISPENV *SetDefDispEnv(DISPENV *disp, int x, int y, int w, int h);
DRAWENV *SetDefDrawEnv(DRAWENV *draw, int x, int y, int w, int h);
void FntLoad(int x, int y);

#ifdef __cplusplus
}
#endif

#endif
