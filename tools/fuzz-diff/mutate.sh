#!/bin/sh
# The check's self-test (docs/testing.md, "Self-tests and what
# `checks.fuzz-diff` runs"): applies one
# deliberate bug to a copy of the library's source, for a build that the
# strict and bounded modes must then fail. Never applied to the tree that
# ships; the flake's fuzz-diff check copies the tree first.
#   tools/fuzz-diff/mutate.sh ROOT NAME
# ROOT a copy of the repository; NAME one of:
#   line-clip   a dithered line loses its pixels in the drawing area's first
#               column (raster_line)
#   gouraud-tex a Gouraud textured triangle whose colours vary blends B - F
#               whatever its mode (raster_triangle)
#   uv-varies   a textured triangle whose texture coordinates vary blends
#               B - F whatever its mode (raster_triangle)
#   avg-up      blend mode 0 rounds up: (B + F + 1) / 2 (jbi_blend)
#   outside-right
#               a polygon clipped at the drawing area's right edge also
#               writes the pixel just right of it on its first row drawn
#               (tri_rows)
#   stray-above a rectangle clipped at the drawing area's top edge also
#               writes its first column's pixel in the row above the area
#               (raster_rect)
#   near-drop   a Gouraud textured triangle whose colours vary leaves out
#               every pixel whose 5-bit channels are each within one step
#               of the destination's, the colour bits not equal, the mask
#               bit not looked at (span_impl)
#   prelude-stray
#               a black fill of 1008 x 256 at (0,0), the first of the
#               prelude's four, also writes (1008,0), which the second
#               erases (jbi_fill)
#   read-last   the last word of every VRAM-to-CPU transfer has its lowest
#               bit flipped (GPUREAD)
#   mask-through
#               a Gouraud polygon whose colours vary ignores the mask check
#               (raster_triangle)
# Fails when the text it replaces is not found exactly once.
set -eu
root=$1
name=$2
sub() { # FILE OLD NEW
    n=$(grep -cF -- "$2" "$root/$1" || true)
    [ "$n" = 1 ] || { echo "mutate.sh: $name: '$2' found $n times in $1" >&2; exit 1; }
    awk -v old="$2" -v new="$3" '{ i = index($0, old); if (i) $0 = substr($0, 1, i - 1) new substr($0, i + length(old)); print }' \
        "$root/$1" > "$root/$1.mut"
    mv "$root/$1.mut" "$root/$1"
}
case $name in
line-clip)
    sub src/soft/raster.c "if (px < p->clip_x0 || px > p->clip_x1" \
        "if (px < p->clip_x0 + dither || px > p->clip_x1" ;;
gouraud-tex)
    sub src/soft/raster.c "    s.semi_mode = p->semi_mode;" \
        "    s.semi_mode = textured && gouraud ? 2 : p->semi_mode;" ;;
uv-varies)
    sub src/soft/raster.c "    s.semi_mode = p->semi_mode;" \
        "    s.semi_mode = textured && (v0->u != v1->u || v0->u != v2->u || v0->v != v1->v || v0->v != v2->v) ? 2 : p->semi_mode;" ;;
avg-up)
    sub src/soft/raster.h "        r = (br + r) >> 1;" "        r = (br + r + 1) >> 1;"
    sub src/soft/raster.h "        g = (bg + g) >> 1;" "        g = (bg + g + 1) >> 1;"
    sub src/soft/raster.h "        b = (bb + b) >> 1;" "        b = (bb + b + 1) >> 1;" ;;
outside-right)
    sub src/soft/raster.c "            xr = p->clip_x1 + 1;" \
        "            xr = p->clip_x1 + 1 + (y == t->y && p->clip_x1 < 1023);" ;;
stray-above)
    sub src/soft/raster.c "    if (y0 < p->clip_y0)" \
        "    if (y0 < p->clip_y0 && p->clip_y0 > 0 && y1 > p->clip_y0 && x0 < x1 && x0 <= p->clip_x1) VROW(vram, p->clip_y0 - 1)[x0] = (uint16_t)flat; if (y0 < p->clip_y0)" ;;
near-drop)
    sub src/soft/raster.c "        *dst = (uint16_t)(out | stp | s->set_mask);" \
        "        { int ndr = (int)(out & 31) - (int)(*dst & 31), ndg = (int)((out >> 5) & 31) - (int)((*dst >> 5) & 31), ndb = (int)((out >> 10) & 31) - (int)((*dst >> 10) & 31); if (depth && gouraud && (out & 0x7fffu) != (*dst & 0x7fffu) && ndr >= -1 && ndr <= 1 && ndg >= -1 && ndg <= 1 && ndb >= -1 && ndb <= 1) goto next; } *dst = (uint16_t)(out | stp | s->set_mask);" ;;
prelude-stray)
    sub src/core/gp0.c "    ctx->backend->fill(ctx->backend, fx, fy, fw, fh, color);" \
        "    ctx->backend->fill(ctx->backend, fx, fy, fw, fh, color); if (fx == 0 && fy == 0 && fw == 1008 && fh == 256 && rgb24 == 0) ctx->backend->fill(ctx->backend, 1008, 0, 1, 1, 0x1234);" ;;
read-last)
    sub src/api/stream.c "        a->latch = v;" \
        "        if (a->rd_next == a->rd_count) v ^= 1u; a->latch = v;" ;;
mask-through)
    sub src/soft/raster.c "    s.check_mask = p->check_mask;" \
        "    s.check_mask = gouraud ? 0 : p->check_mask;" ;;
*)
    echo "mutate.sh: unknown mutation $name" >&2
    exit 1 ;;
esac
