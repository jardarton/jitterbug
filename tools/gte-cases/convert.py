#!/usr/bin/env python3
"""Converts ps1-tests' two GTE suites into jitterbug's case files.

    convert.py PS1_TESTS_DIR OUT_DIR

reads, from a checkout of https://github.com/JaCzekanski/ps1-tests,

    gte/test-all/tests.c                 -> OUT_DIR/test-all.txt
    gte-fuzz/gte_valid_0xc0ffee_50.log   -> OUT_DIR/gte-fuzz.txt

and checks, while reading, everything the conversion relies on (the case
counts and numbering, 64 registers in order, the command words). The format
is described in the header written at the top of each file (HEADER below)
and in third_party/ps1-tests/README.md. Output is a pure function of the
two input files: no dates, no paths.
"""
import hashlib
import re
import sys

NAMES = (
    "VXY0 VZ0 VXY1 VZ1 VXY2 VZ2 RGBC OTZ IR0 IR1 IR2 IR3 SXY0 SXY1 SXY2 SXYP "
    "SZ0 SZ1 SZ2 SZ3 RGB0 RGB1 RGB2 RES1 MAC0 MAC1 MAC2 MAC3 IRGB ORGB LZCS LZCR "
    "RT11RT12 RT13RT21 RT22RT23 RT31RT32 RT33 TRX TRY TRZ L11L12 L13L21 L22L23 "
    "L31L32 L33 RBK GBK BBK LR1LR2 LR3LG1 LG2LG3 LB1LB2 LB3 RFC GFC BFC OFX OFY "
    "H DQA DQB ZSF3 ZSF4 FLAG"
).split()
assert len(NAMES) == 64

COMMANDS = {
    0x01: "RTPS", 0x06: "NCLIP", 0x0C: "OP", 0x10: "DPCS", 0x11: "INTPL",
    0x12: "MVMVA", 0x13: "NCDS", 0x14: "CDP", 0x16: "NCDT", 0x1B: "NCCS",
    0x1C: "CC", 0x1E: "NCS", 0x20: "NCT", 0x28: "SQR", 0x29: "DCPL",
    0x2A: "DPCT", 0x2D: "AVSZ3", 0x2E: "AVSZ4", 0x30: "RTPT", 0x3D: "GPF",
    0x3E: "GPL", 0x3F: "NCCT",
}

HEADER = """\
# jitterbug GTE cases, format 1.
#
# Converted by tools/gte-cases/convert.py from ps1-tests (MIT, LICENSE
# here) at commit f727802fead11f1daa7549285548392ef87749cb; see README.md
# here. Every expected value was read from a console's GTE by the
# upstream test program. Do not edit: rerun tools/gte-cases/regen.sh.
#
# Lines (fields separated by one space, numbers in lowercase hex without
# 0x unless said otherwise):
#   source <path in ps1-tests> sha256 <sha256 of that file>
#   prelude clear|none
#       clear: before each case, write 0 to registers 0..63 in that order
#       (what test-all does); none: write nothing first (gte-fuzz).
#   inputs <decimal count>
#   cases <decimal count>
#   in <decimal k> <64 words>
#       input set k (0-based, in order): the values written to registers
#       0..63, data registers 0..31 (mtc2) then control registers 32..63
#       (ctc2), in that order. The order matters: writing SXYP (15) pushes
#       SXY0..2 written before it, IRGB (28) overwrites IR1..3, LZCS (30)
#       sets LZCR; ORGB (29) and LZCR (31) ignore writes.
#   case <decimal n> <command> <decimal k> [<reg>=<word> ...]
#       case n (the upstream test number), run on input set k: after the
#       prelude and the writes, <command> (the cop2 instruction's low 25
#       bits, 8 digits; "-" for none) executes once; then registers 0..63
#       are read (mfc2, cfc2). <reg> is a decimal register number 0..63;
#       each register listed reads <word>, every register not listed reads
#       back exactly the word input set k wrote to it.
"""


def fail(msg):
    sys.exit("convert.py: " + msg)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def command_word(cmd, sf, lm, tx, vx, mx):
    # gte-fuzz/gte.h, union Command: cmd bits 0-5, lm bit 10, translation
    # bits 13-14, vector 15-16, matrix 17-18, sf bit 19.
    return cmd | lm << 10 | tx << 13 | vx << 15 | mx << 17 | sf << 19


FIELDS = r"\(sf=(\d), lm=(\d), tx=(\d), vx=(\d), mx=(\d)\)"


def read_test_all(path):
    text = open(path).read()
    m = re.search(r"#define TEST_COUNT (\d+)", text)
    if not m:
        fail("test-all: no TEST_COUNT")
    count = int(m.group(1))
    body = text[text.index("const struct test_t tests[] = {"):]
    blocks = re.findall(r"\{ // test (\d+)\n(.*?)\n  \}\n\},?\n", body, re.S)
    if len(blocks) != count:
        fail("test-all: %d cases, TEST_COUNT %d" % (len(blocks), count))
    cases = []
    for i, (num, block) in enumerate(blocks):
        n = int(num)
        if n != i + 1:
            fail("test-all: case %d numbered %d" % (i + 1, n))
        m = re.match(
            r'  \.name = "([^"]*)",\n  \.input = \{ // Input registers\n(.*?)\n  \},\n'
            r"  \.opcode = 0x([0-9a-f]{8}), // ([^\n]*)\n"
            r"  \.output = \{ // Expected output\n(.*)$",
            block, re.S)
        if not m:
            fail("test-all: case %d: unexpected layout" % n)
        name, inp, op, opname, out = m.groups()
        words = []
        for part, what in ((inp, "input"), (out, "output")):
            lines = part.split("\n")
            if len(lines) != 64:
                fail("test-all: case %d: %d %s lines" % (n, len(lines), what))
            ws = []
            for r, line in enumerate(lines):
                lm = re.fullmatch(r"    0x([0-9a-f]{8}), // r\[(\d+)\]", line)
                if not lm or int(lm.group(2)) != r:
                    fail("test-all: case %d: %s line %d: %r" % (n, what, r, line))
                ws.append(int(lm.group(1), 16))
            words.append(ws)
        op = int(op, 16)
        if op == 0xFFFFFFFF:
            if name != "test %d" % n or opname != "No opcode":
                fail("test-all: case %d: name %r / %r without opcode" % (n, name, opname))
            cmd = None
        else:
            nm = re.fullmatch(r"test %d - GTE 0x([0-9a-f]{2}) " % n + FIELDS, name)
            if not nm:
                fail("test-all: case %d: name %r" % (n, name))
            f = [int(x, 16) for x in nm.groups()]
            if command_word(*f) != op or opname != "GTE %d" % f[0] or f[0] not in COMMANDS:
                fail("test-all: case %d: opcode 0x%08x against %r" % (n, op, name))
            cmd = op
        cases.append((n, words[0], cmd, words[1]))
    return cases


def read_fuzz(path):
    lines = open(path).read().split("\n")
    seed_line = "==== VALID CMD FUZZ (seed = 0x00c0ffee) ===="
    if lines[0] != seed_line or lines[-2:] != ["==== END ====", ""]:
        fail("gte-fuzz: unexpected first or last line")
    cases = []
    group = None
    i = 1
    end = len(lines) - 2
    while i < end:
        line = lines[i]
        m = re.fullmatch(r"-------------- GTE 0x([0-9a-f]{2}) (\S+) \(seed = 0x00c0ffee\)", line)
        if m:
            group = int(m.group(1), 16)
            expect = COMMANDS.get(group, "---")
            if m.group(2) != expect:
                fail("gte-fuzz: line %d: %r" % (i + 1, line))
            i += 1
            continue
        m = re.fullmatch(r"Test (\d+)", line)
        if not m or group is None:
            fail("gte-fuzz: line %d: %r" % (i + 1, line))
        n = int(m.group(1))
        if n != len(cases) + 1:
            fail("gte-fuzz: test %d out of order" % n)
        i += 1

        def regs(prefix):
            nonlocal i
            ws = []
            for r in range(64):
                rm = re.fullmatch(re.escape(prefix) + r" r\[(\d+)\] = 0x([0-9a-f]{8})", lines[i])
                if not rm or int(rm.group(1)) != r:
                    fail("gte-fuzz: line %d: %r" % (i + 1, lines[i]))
                ws.append(int(rm.group(2), 16))
                i += 1
            return ws

        inp = regs(">")
        cmd = None
        if group != 0x40:
            gm = re.fullmatch(r"GTE 0x([0-9a-f]{2}) (\S+) " + FIELDS, lines[i])
            if not gm or int(gm.group(1), 16) != group or gm.group(2) != COMMANDS[group]:
                fail("gte-fuzz: line %d: %r" % (i + 1, lines[i]))
            f = [int(gm.group(1), 16)] + [int(x) for x in gm.groups()[2:]]
            cmd = command_word(*f)
            i += 1
        out = regs("<")
        if lines[i] != "":
            fail("gte-fuzz: line %d: expected a blank line" % (i + 1))
        i += 1
        cases.append((n, inp, cmd, out))
    return cases


def write(path, source, source_sha, prelude, cases):
    inputs = []
    index = {}
    for _, inp, _, _ in cases:
        key = tuple(inp)
        if key not in index:
            index[key] = len(inputs)
            inputs.append(key)
    out = [HEADER]
    out.append("source %s sha256 %s\n" % (source, source_sha))
    out.append("prelude %s\n" % prelude)
    out.append("inputs %d\n" % len(inputs))
    out.append("cases %d\n" % len(cases))
    for k, inp in enumerate(inputs):
        out.append("in %d %s\n" % (k, " ".join("%08x" % w for w in inp)))
    for n, inp, cmd, res in cases:
        diff = ["%d=%08x" % (r, res[r]) for r in range(64) if res[r] != inp[r]]
        out.append("case %d %s %d%s\n" % (n, "-" if cmd is None else "%08x" % cmd,
                                         index[tuple(inp)],
                                         "".join(" " + d for d in diff)))
    text = "".join(out)
    if decode(text) != [(n, list(i), c, list(r)) for n, i, c, r in cases]:
        fail("%s: the written file does not decode to the cases read" % path)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def decode(text):
    """The cases a written file holds, as read_* returns them."""
    inputs = []
    cases = []
    for line in text.split("\n"):
        f = line.split(" ")
        if f[0] == "in":
            assert int(f[1]) == len(inputs)
            inputs.append([int(w, 16) for w in f[2:]])
        elif f[0] == "case":
            inp = inputs[int(f[3])]
            res = list(inp)
            for d in f[4:]:
                r, w = d.split("=")
                res[int(r)] = int(w, 16)
            cases.append((int(f[1]), list(inp), None if f[2] == "-" else int(f[2], 16), res))
    return cases


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    ta_rel = "gte/test-all/tests.c"
    fz_rel = "gte-fuzz/gte_valid_0xc0ffee_50.log"
    ta = read_test_all(src + "/" + ta_rel)
    fz = read_fuzz(src + "/" + fz_rel)
    write(dst + "/test-all.txt", ta_rel, sha256(src + "/" + ta_rel), "clear", ta)
    write(dst + "/gte-fuzz.txt", fz_rel, sha256(src + "/" + fz_rel), "none", fz)

    # How the suites relate, for the README: the same (inputs, command,
    # outputs) triples, in another order.
    def key(c):
        return (tuple(c[1]), -1 if c[2] is None else c[2], tuple(c[3]))

    same = sorted(map(key, ta)) == sorted(map(key, fz))
    for name, cs in (("test-all", ta), ("gte-fuzz", fz)):
        per = {}
        for c in cs:
            k = "none" if c[2] is None else COMMANDS[c[2] & 0x3F]
            per[k] = per.get(k, 0) + 1
        print("%s: %d cases, %d input sets; %s" % (
            name, len(cs), len({tuple(c[1]) for c in cs}),
            ", ".join("%s %d" % kv for kv in sorted(per.items()))))
    print("the two suites hold %s cases" % ("the same" if same else "different"))
    if not same:
        # Only test-all.txt is checked in, on the strength of this.
        fail("test-all and gte-fuzz no longer hold the same cases")


if __name__ == "__main__":
    main()
