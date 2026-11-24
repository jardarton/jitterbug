#!/usr/bin/env python3
"""The converters' test inputs in tests/files/inputs, made from nothing but
the arithmetic below (docs/testing.md, "File, converter and loader tests"):
PNGs for jb-tex, OBJ, MTL and glTF for jb-mesh. Checked in; run again only
to change them (python3, standard library only), then convert them again
(the same section says how).

    python3 tests/files/make-inputs.py tests/files/inputs
"""
import base64
import json
import os
import struct
import sys
import zlib


def png(path, w, h, pixel):
    """An 8-bit RGBA PNG, pixel(x, y) -> (r, g, b, a)."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(bytes(pixel(x, y)))

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def noise(x, y, seed):
    """A small integer hash, 0..255."""
    n = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return (n ^ (n >> 16)) & 0xFF


def images(out):
    # Many colours: two gradients crossed, a band of semi-transparent
    # pixels, transparent corners. 8-bit and 4-bit quantisation, dithered
    # or not, and the alpha rules.
    def rocks(x, y):
        if (x < 3 and y < 3) or (x > 28 and y > 28):
            return (0, 0, 0, 0)
        r = (x * 8 + noise(x, y, 1) // 8) & 0xFF
        g = (y * 6 + 40) & 0xFF
        b = (128 + (x ^ y) * 3) & 0xFF
        a = 96 if 12 <= y < 16 else 255
        return (r, g, b, a)

    png(os.path.join(out, "rocks.png"), 32, 32, rocks)

    # Few colours (12, one of them opaque black): kept exactly at 4 bits.
    pal = [(0, 0, 0), (255, 255, 255), (200, 40, 40), (40, 200, 40), (40, 40, 200),
           (230, 200, 60), (120, 60, 160), (60, 160, 170), (90, 90, 90), (170, 120, 80),
           (250, 150, 200), (20, 90, 40)]

    def tiles(x, y):
        k = ((x // 4) * 7 + (y // 4) * 3 + ((x + y) // 6)) % len(pal)
        return pal[k] + (255,)

    png(os.path.join(out, "tiles.png"), 16, 16, tiles)

    # A smooth ramp: 15 bits with and without dithering.
    def sky(x, y):
        return (x * 3 + y, 64 + y * 6, 255 - x * 2, 255)

    png(os.path.join(out, "sky.png"), 64, 24, sky)

    # Water, and two more frames with the same shapes in other colours:
    # palette animation (--frame).
    for k in range(3):
        def frame(x, y, k=k):
            band = (((x + 2 * y) // 3) % 4 + k) % 4
            return (20 + band * 10, 60 + band * 25, 140 + band * 25, 255)
        png(os.path.join(out, "water%d.png" % k), 16, 16, frame)

    # For a repeat texture: 64 x 32, powers of two.
    def bricks(x, y):
        row = y // 8
        mortar = y % 8 == 7 or (x + (row % 2) * 8) % 16 == 15
        if mortar:
            return (200, 200, 190, 255)
        v = noise(x // 2, y // 2, 7) // 16
        return (150 + v, 60 + v, 40 + v // 2, 255)

    png(os.path.join(out, "bricks.png"), 64, 32, bricks)

    # Too wide for a texture (jb-tex refuses it).
    png(os.path.join(out, "wide.png"), 300, 2, lambda x, y: (x % 256, y * 100, 50, 255))


def obj(out):
    # A box of quads: two materials, one textured with u, v over the whole
    # face, one plain and half transparent; a five-sided cap above it (cut
    # into a fan); normals on every corner; negative indices on one face.
    v = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
         (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
    lines = ["# A box for jb-mesh's tests, made by make-inputs.py", "mtllib box.mtl", "o box"]
    for p in v:
        lines.append("v %g %g %g" % tuple(s * 40.0 for s in p))
    # cos and sin of 72 k degrees, written out, so the file needs no maths.
    cs = [(1.0, 0.0), (0.309017, 0.951057), (-0.809017, 0.587785),
          (-0.809017, -0.587785), (0.309017, -0.951057)]
    for c, s in cs:
        lines.append("v %.4f %.4f %.4f" % (c * 30.0, 55.0, s * 30.0))
    for uv in [(0, 0), (1, 0), (1, 1), (0, 1), (0.5, 0.5)]:
        lines.append("vt %g %g" % uv)
    for n in [(0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0), (0, -1, 0), (0, 1, 0)]:
        lines.append("vn %g %g %g" % n)
    lines.append("usemtl stone")
    # Anticlockwise from outside.
    lines.append("f 1/1/1 4/4/1 3/3/1 2/2/1")
    lines.append("f 5/1/2 6/2/2 7/3/2 8/4/2")
    lines.append("f 1/1/3 5/2/3 8/3/3 4/4/3")
    lines.append("f 2/1/4 3/4/4 7/3/4 6/2/4")
    lines.append("usemtl glass")
    lines.append("f 1//5 2//5 6//5 5//5")
    lines.append("f -10//6 -6//6 -7//6 -11//6")  # 4 8 7 3: the top
    lines.append("usemtl stone")
    lines.append("f 13/5/6 12/1/6 11/2/6 10/3/6 9/4/6")  # the cap, round from above
    with open(os.path.join(out, "box.obj"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out, "box.mtl"), "w") as f:
        f.write("# Materials for box.obj\n"
                "newmtl stone\nKd 1.0 1.0 1.0\nmap_Kd rocks.png\n\n"
                "newmtl glass\nKd 0.4 0.7 0.9\nd 0.5\n")

    # Vertex colours: a hexagon and a triangle, no normals, no texture
    # coordinates, no materials (the faces are "default").
    lines = ["# A coloured hexagon for jb-mesh's tests, made by make-inputs.py"]
    hexagon = [(50, 0), (25, 43.3), (-25, 43.3), (-50, 0), (-25, -43.3), (25, -43.3)]
    colours = [(1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 1, 1), (0, 0, 1), (1, 0, 1)]
    for (x, z), c in zip(hexagon, colours):
        lines.append("v %g 0 %g %g %g %g" % ((x, z) + c))
    lines.append("v 0 30 0 0.5 0.5 0.5")
    lines.append("f 1 6 5 4 3 2")
    lines.append("f 1 2 7")
    with open(os.path.join(out, "hexagon.obj"), "w") as f:
        f.write("\n".join(lines) + "\n")

    # A floor whose u, v run to 2 (a repeat texture tiles it twice; any
    # other is refused), with a CRLF line ending, a line joined by a
    # backslash, a comment after a face and numbers in exponent form.
    text = ("# A tiled floor for jb-mesh's tests, made by make-inputs.py\r\n"
            "v -6e1 0 -60\nv 60 0 -60\nv 60 0 6.0E+1\nv -60 \\\n 0 60\n"
            "vt 0 2\nvt 2 2\nvt 2 0\nvt 0 0\n"
            "f 1/1 4/4 3/3 2/2 # anticlockwise from above\n")
    with open(os.path.join(out, "tiled.obj"), "w", newline="") as f:
        f.write(text)

    # A strip whose u runs over four repeats from a hair below 0, as
    # exporters write it: within half a texel of the edge, so it starts in
    # the first repeat and spans exactly 256 texels of a 64-wide texture.
    with open(os.path.join(out, "edge.obj"), "w") as f:
        f.write("# A strip over four repeats, made by make-inputs.py\n"
                "v -100 0 -20\nv -100 0 20\nv 100 0 20\nv 100 0 -20\n"
                "vt -0.000001 1\nvt -0.000001 0\nvt 4 0\nvt 4 1\n"
                "f 1/1 2/2 3/3 4/4\n")

    # A face with an index past the vertices (jb-mesh refuses it).
    with open(os.path.join(out, "broken.obj"), "w") as f:
        f.write("# Broken on purpose, for jb-mesh's tests\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "f 1 2 9\n")


def gltf(out):
    """A pyramid in a node with translation, rotation and scale, and again
    under a mirroring node (negative scale: its triangles turn round) with a
    matrix of its own; its base a triangle strip; normals, TEXCOORD_0 and
    COLOR_0 (normalised unsigned bytes); materials named, double-sided,
    blended, unlit; and the same as .glb."""
    apex = (0.0, 1.0, 0.0)
    base = [(-1.0, 0.0, -1.0), (1.0, 0.0, -1.0), (1.0, 0.0, 1.0), (-1.0, 0.0, 1.0)]
    # Side triangles: each its own three vertices (flat normals).
    pos, nrm, uv, col, idx = [], [], [], [], []
    sides = [(3, 2), (2, 1), (1, 0), (0, 3)]
    side_normals = [(0, 0.4472136, 0.8944272), (0.8944272, 0.4472136, 0),
                    (0, 0.4472136, -0.8944272), (-0.8944272, 0.4472136, 0)]
    for (a, b), n in zip(sides, side_normals):
        for p, t in ((base[a], (0.0, 1.0)), (base[b], (1.0, 1.0)), (apex, (0.5, 0.0))):
            idx.append(len(pos))
            pos.append(p)
            nrm.append(n)
            uv.append(t)
            col.append((255, (200 - 40 * len(idx)) % 200, 40 * a, 255))
    # The base as a strip, anticlockwise seen from below.
    strip_pos = [base[0], base[1], base[3], base[2]]
    buf = bytearray()
    views, accessors = [], []

    def accessor(fmt, items, comp, typ, normalized=False, minmax=False, target=None):
        while len(buf) % 4:
            buf.append(0)
        off = len(buf)
        for it in items:
            buf.extend(struct.pack("<" + fmt, *(it if isinstance(it, tuple) else (it,))))
        view = {"buffer": 0, "byteOffset": off, "byteLength": len(buf) - off}
        if target:
            view["target"] = target
        views.append(view)
        a = {"bufferView": len(views) - 1, "componentType": comp, "count": len(items),
             "type": typ}
        if normalized:
            a["normalized"] = True
        if minmax:
            a["min"] = [min(it[k] for it in items) for k in range(3)]
            a["max"] = [max(it[k] for it in items) for k in range(3)]
        accessors.append(a)
        return len(accessors) - 1

    a_pos = accessor("fff", pos, 5126, "VEC3", minmax=True, target=34962)
    a_nrm = accessor("fff", nrm, 5126, "VEC3", target=34962)
    a_uv = accessor("ff", uv, 5126, "VEC2", target=34962)
    a_col = accessor("BBBB", col, 5121, "VEC4", normalized=True, target=34962)
    a_idx = accessor("H", idx, 5123, "SCALAR", target=34963)
    a_spos = accessor("fff", strip_pos, 5126, "VEC3", minmax=True, target=34962)
    doc = {
        "asset": {"version": "2.0", "generator": "jitterbug tests/files/make-inputs.py"},
        "extensionsUsed": ["KHR_materials_unlit"],
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"name": "pyramid", "mesh": 0, "translation": [10.0, -5.0, 2.5],
             "rotation": [0.0, 0.38268343, 0.0, 0.9238795], "scale": [30.0, 40.0, 30.0]},
            {"name": "mirror", "children": [2], "scale": [-1.0, 1.0, 1.0],
             "translation": [-60.0, 0.0, 0.0]},
            {"name": "inner", "mesh": 0,
             "matrix": [25.0, 0, 0, 0, 0, 25.0, 0, 0, 0, 0, 25.0, 0, 0, 10.0, 0, 1]},
        ],
        "meshes": [{"name": "pyramid", "primitives": [
            {"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv,
                            "COLOR_0": a_col}, "indices": a_idx, "material": 0, "mode": 4},
            {"attributes": {"POSITION": a_spos}, "material": 1, "mode": 5},
        ]}],
        "materials": [
            {"name": "sides", "doubleSided": True,
             "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.9, 0.8, 1.0],
                                      "baseColorTexture": {"index": 0}}},
            {"name": "base", "alphaMode": "BLEND",
             "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.25, 0.3, 0.5]},
             "extensions": {"KHR_materials_unlit": {}}},
        ],
        "textures": [{"source": 0}],
        "images": [{"uri": "bricks.png"}],
        "bufferViews": views,
        "accessors": accessors,
    }
    while len(buf) % 4:
        buf.append(0)
    doc["buffers"] = [{"byteLength": len(buf),
                       "uri": "data:application/octet-stream;base64," +
                              base64.b64encode(bytes(buf)).decode("ascii")}]
    with open(os.path.join(out, "pyramid.gltf"), "w") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    # The GLB: the same document with the buffer in its BIN chunk.
    doc["buffers"] = [{"byteLength": len(buf)}]
    js = json.dumps(doc, sort_keys=True).encode("utf-8")
    while len(js) % 4:
        js += b" "
    body = struct.pack("<II", len(js), 0x4E4F534A) + js
    body += struct.pack("<II", len(buf), 0x004E4942) + bytes(buf)
    with open(os.path.join(out, "pyramid.glb"), "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)


def bad_gltf(out):
    """A triangle whose COLOR_0 is a MAT4 (16 floats a vertex, more than a
    colour has room for), and one whose NORMAL is a VEC2: glTF that
    cgltf_validate passes and jb-mesh must refuse."""
    def triangle(name, attr, fmt, typ, item):
        buf = bytearray()
        for p in [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]:
            buf.extend(struct.pack("<fff", *p))
        off = len(buf)
        for _ in range(3):
            buf.extend(struct.pack("<" + fmt, *item))
        doc = {
            "asset": {"version": "2.0", "generator": "jitterbug tests/files/make-inputs.py"},
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0, attr: 1}}]}],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 0]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": typ}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": off},
                            {"buffer": 0, "byteOffset": off, "byteLength": len(buf) - off}],
            "buffers": [{"byteLength": len(buf),
                         "uri": "data:application/octet-stream;base64," +
                                base64.b64encode(bytes(buf)).decode("ascii")}],
        }
        with open(os.path.join(out, name), "w") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
            f.write("\n")

    triangle("badcolor.gltf", "COLOR_0", "f" * 16, "MAT4", (0.5,) * 16)
    triangle("badnormal.gltf", "NORMAL", "ff", "VEC2", (0.0, 1.0))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/files/inputs"
    os.makedirs(out, exist_ok=True)
    images(out)
    obj(out)
    gltf(out)
    bad_gltf(out)


if __name__ == "__main__":
    main()
