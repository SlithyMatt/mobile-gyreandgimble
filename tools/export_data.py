#!/usr/bin/env python3
"""Build the game data for the mobile port from the X16 repository.

The X16 game (the `x16` submodule) is the single source of truth for the
graphics, levels, sounds and music. This script runs the X16 repo's own
build tools exactly as its Makefile does, then turns their output into C:

  1. Run tools/genrings.py, genthemes.py, gensprites.py, gensounds.py,
     genmusic.py, genchars.py and level.py into OUT/x16/, the same files
     the X16 build makes (RINGS.BIN, CELLS.BIN, SOUNDS.BIN and the .inc
     files the assembler includes).
  2. Assemble the .inc files with a small ca65 data assembler: each file
     becomes one byte blob, laid out from address 0 like the X16's RODATA,
     so lo/hi pointer tables (level_lo, tune_lo, ...) and .word pointers
     come out as offsets into that blob.
  3. Write OUT/gg_data.h and OUT/gg_data.c: every constant (NLEVELS,
     TH_STONE, SND_TURN, ...) as a #define, every label as a pointer into
     its blob, and the three .BIN files as arrays.

Usage: export_data.py X16_DIR OUT_DIR [PYTHON]
"""
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True     # leave the x16 submodule clean


def run_x16_tools(x16, out, python):
    """The X16 Makefile's tool invocations (see x16/Makefile)."""
    t = lambda name: os.path.join(x16, "tools", name)  # noqa: E731
    a = lambda *p: os.path.join(x16, "assets", *p)  # noqa: E731
    o = lambda name: os.path.join(out, name)  # noqa: E731
    levels = sorted(os.path.join(x16, "levels", f) for f in os.listdir(os.path.join(x16, "levels"))
                    if f.endswith(".txt"))
    cmds = [
        [t("genrings.py"), o("RINGS.BIN"), o("CELLS.BIN"), o("board.inc"), o("pieces.inc"), o("waypoints.inc")],
        [t("genthemes.py"), a("themes.txt"), o("themes.inc")],
        [t("gensprites.py"), a("tove.txt"), "tove", o("tove.inc")],
        [t("gensprites.py"), a("critters.txt"), "critters", o("critters.inc")],
        [t("gensounds.py"), o("SOUNDS.BIN"), o("sounds.inc"), o("wav")],
        [t("genmusic.py"), a("themes.txt"), a("music"), o("music.inc"), "fanfare"],
        [t("genchars.py"), a("stars.txt"), "star", o("stars.inc")],
        [t("level.py"), "build", o("level.inc")] + levels,
    ]
    for cmd in cmds:
        subprocess.run([python, "-B"] + cmd, check=True, cwd=x16, stdout=subprocess.DEVNULL)


# --- a ca65 subset: constants, labels, .byte and .word ----------------------

TOKEN = re.compile(r"\s*(\$[0-9A-Fa-f]+|%[01]+|\d+|[A-Za-z_][A-Za-z_0-9]*|[<>()+\-*/])")


class Inc:
    def __init__(self, name, path):
        self.name, self.path = name, path
        self.lines = []
        for n, raw in enumerate(open(path, encoding="utf-8"), 1):
            line = raw.split(";", 1)[0].strip()
            if line:
                self.lines.append((n, line))
        self.labels = {}        # label -> offset in the blob
        self.data = bytearray()


def tokens(expr, where):
    pos, out = 0, []
    expr = expr.strip()
    while pos < len(expr):
        m = TOKEN.match(expr, pos)
        if not m:
            sys.exit(f"{where}: can't parse {expr!r}")
        out.append(m.group(1))
        pos = m.end()
    return out


def evaluate(expr, syms, where):
    """Evaluate a ca65 expression: numbers, symbols, unary < > - (lo byte, hi
    byte, negate), binary + - * / and parentheses. Raises KeyError(symbol)
    for an undefined symbol."""
    toks = tokens(expr, where)
    pos = 0

    def peek():
        return toks[pos] if pos < len(toks) else None

    def take():
        nonlocal pos
        pos += 1
        return toks[pos - 1]

    def primary():
        tok = take() if peek() is not None else sys.exit(f"{where}: truncated {expr!r}")
        if tok == "(":
            v = add()
            if take() != ")":
                sys.exit(f"{where}: missing ) in {expr!r}")
            return v
        if tok.startswith("$"):
            return int(tok[1:], 16)
        if tok.startswith("%"):
            return int(tok[1:], 2)
        if tok[0].isdigit():
            return int(tok)
        if tok in syms:
            return syms[tok]
        raise KeyError(tok)

    def unary():
        if peek() == "<":
            take()
            return unary() & 0xFF
        if peek() == ">":
            take()
            return unary() >> 8 & 0xFF
        if peek() == "-":
            take()
            return -unary()
        return primary()

    def mul():
        v = unary()
        while peek() in ("*", "/"):
            v = v * unary() if take() == "*" else v // unary()
        return v

    def add():
        v = mul()
        while peek() in ("+", "-"):
            v = v + mul() if take() == "+" else v - mul()
        return v

    v = add()
    if pos != len(toks):
        sys.exit(f"{where}: trailing tokens in {expr!r}")
    return v


def split_operands(s):
    return [x.strip() for x in s.split(",")]


def assemble(incs):
    """Two passes over every file: lay out labels, then emit bytes."""
    consts = {}
    labels = {}             # label -> (inc, offset)
    for inc in incs:        # pass 1
        pc = 0
        for n, line in inc.lines:
            where = f"{inc.path}:{n}"
            m = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.+)", line)
            if m:
                consts[m.group(1)] = m.group(2)
                continue
            m = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*):\s*(.*)", line)
            if m:
                if m.group(1) in labels:
                    sys.exit(f"{where}: duplicate label {m.group(1)}")
                labels[m.group(1)] = (inc, pc)
                inc.labels[m.group(1)] = pc
                line = m.group(2)
                if not line:
                    continue
            m = re.fullmatch(r"\.(byte|word)\s+(.+)", line)
            if not m:
                sys.exit(f"{where}: unsupported line {line!r}")
            pc += len(split_operands(m.group(2))) * (1 if m.group(1) == "byte" else 2)
    syms = {}

    def resolve(name, stack=()):
        if name in syms:
            return syms[name]
        if name in stack:
            sys.exit(f"circular definition of {name}")
        expr = consts[name]
        while True:
            try:
                syms[name] = evaluate(expr, syms, name)
                return syms[name]
            except KeyError as e:
                dep = e.args[0]
                if dep in consts:
                    resolve(dep, stack + (name,))
                elif dep in labels:
                    syms[dep] = labels[dep][1]
                else:
                    sys.exit(f"{name}: unknown symbol {dep}")

    for name in consts:
        resolve(name)
    for name, (_, off) in labels.items():
        syms.setdefault(name, off)
    for inc in incs:        # pass 2
        for n, line in inc.lines:
            where = f"{inc.path}:{n}"
            if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*\s*=.*", line):
                continue
            m = re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*:\s*(.*)", line)
            if m:
                line = m.group(1)
                if not line:
                    continue
            m = re.fullmatch(r"\.(byte|word)\s+(.+)", line)
            for op in split_operands(m.group(2)):
                try:
                    v = evaluate(op, syms, where)
                except KeyError as e:
                    sys.exit(f"{where}: unknown symbol {e.args[0]}")
                if m.group(1) == "byte":
                    if not -128 <= v < 256:
                        sys.exit(f"{where}: {op} = {v} doesn't fit a byte")
                    inc.data.append(v & 0xFF)
                else:
                    if not -32768 <= v < 65536:
                        sys.exit(f"{where}: {op} = {v} doesn't fit a word")
                    inc.data += bytes((v & 0xFF, v >> 8 & 0xFF))
    return {k: syms[k] for k in consts}


# --- C output ---------------------------------------------------------------

def c_array(f, name, data):
    f.write(f"const uint8_t {name}[{max(1, len(data))}] = {{\n")
    for k in range(0, len(data), 32):
        f.write(",".join(str(b) for b in data[k:k + 32]) + ",\n")
    f.write("};\n\n")


def board_geometry(x16):
    """The board's radii, read from the X16 repo's genrings.py."""
    sys.path.insert(0, os.path.join(x16, "tools"))
    import genrings  # noqa: E402
    return {name: getattr(genrings, name) for name in
            ("CX", "CY", "RINGS", "CELLS", "BURROW_R", "RING_IN0", "RING_PITCH", "RING_W",
             "GAP_W", "RIM_OUT")}


INC_FILES = ("board", "pieces", "waypoints", "level", "themes", "tove", "critters",
             "sounds", "music", "stars")
BIN_FILES = ("RINGS", "CELLS", "SOUNDS")


def main():
    x16, out = sys.argv[1:3]
    python = sys.argv[3] if len(sys.argv) > 3 else sys.executable
    x16 = os.path.abspath(x16)
    raw = os.path.join(out, "x16")
    os.makedirs(raw, exist_ok=True)
    run_x16_tools(x16, raw, python)

    incs = [Inc(n, os.path.join(raw, n + ".inc")) for n in INC_FILES]
    consts = assemble(incs)
    geometry = board_geometry(x16)

    tmp_h, tmp_c = os.path.join(out, "gg_data.h.tmp"), os.path.join(out, "gg_data.c.tmp")
    with open(tmp_h, "w") as h:
        h.write("/* Generated by tools/export_data.py from the x16 submodule — do not edit. */\n")
        h.write("#ifndef GG_DATA_H\n#define GG_DATA_H\n\n#include <stdint.h>\n\n")
        h.write("/* Constants from the X16 build's .inc files */\n")
        for name, v in consts.items():
            h.write(f"#define {name} {v}\n")
        h.write("\n/* Board geometry from tools/genrings.py (for touch hit-testing) */\n")
        for name, v in geometry.items():
            h.write(f"#define GEO_{name} {v}\n")
        h.write("\n/* Each .inc file as a blob laid out from address 0; pointer tables in it\n"
                " * hold offsets into the same blob */\n")
        for inc in incs:
            h.write(f"extern const uint8_t gg_inc_{inc.name}[{max(1, len(inc.data))}];\n")
        h.write("\n/* Labels, as pointers into their blob (D_ + the X16 label) */\n")
        for inc in incs:
            for label, off in inc.labels.items():
                h.write(f"#define D_{label} (gg_inc_{inc.name} + {off})\n")
        h.write("\n/* The X16 build's binary files */\n")
        for name in BIN_FILES:
            size = os.path.getsize(os.path.join(raw, name + ".BIN"))
            h.write(f"#define GG_{name}_SIZE {size}\n"
                    f"extern const uint8_t gg_bin_{name.lower()}[{size}];\n")
        h.write("\n#endif\n")
    with open(tmp_c, "w") as c:
        c.write("/* Generated by tools/export_data.py from the x16 submodule — do not edit. */\n")
        c.write('#include "gg_data.h"\n\n')
        for inc in incs:
            c_array(c, f"gg_inc_{inc.name}", inc.data)
        for name in BIN_FILES:
            c_array(c, f"gg_bin_{name.lower()}", open(os.path.join(raw, name + ".BIN"), "rb").read())
    os.replace(tmp_h, os.path.join(out, "gg_data.h"))
    os.replace(tmp_c, os.path.join(out, "gg_data.c"))
    print(f"gg_data: {sum(len(i.data) for i in incs)} bytes of tables, "
          f"{len(consts)} constants, from {x16}")


if __name__ == "__main__":
    main()
