#!/usr/bin/env python3
"""Replay the generated symbol table against a firmware image, the way the deck will.

mods/core/ep122_syms.h is what ships, and it is generated from ep122_syms.spec
against a reference build. The build never re-derives it -- the header is
committed -- so a deck running a firmware the reference was not derived from can
go unnoticed until a mod quietly does nothing. That is the one thing the spec's
own design cannot tell you, because it only ever speaks about the build the spec
was last re-anchored to.

Every row of the header carries its own description: a vtable's mangled class
name, a slot offset, or a masked instruction window with the digest it must
produce. So the header alone is enough to ask a binary where things are, with no
spec and no reference build. This does exactly what mods/resolve.c does at run
time, and reports what the deck would have:

    tools/check-header.py                          # build/EP122-deck
    tools/check-header.py -b path/to/EP122 -v path/to/EP122.RENESAS

Addresses move on every relink, so a symbol resolving somewhere other than the
address in the header's comment is the design working, not a fault -- it is the
header's comment that has gone stale. A symbol that does NOT resolve is the
fault: the mod needing it refuses, and says so in the mod log.

Exit status is 0 when every symbol resolves.
"""

import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.dirname(HERE)
HEADER = os.path.join(DECK, "mods", "core", "ep122_syms.h")
DEFAULT_DECK = os.path.join(DECK, "..", "..", "..", "..", "build", "EP122-deck")


def load_gen():
    """gen-syms.py owns the signature maths; this tool must not restate it."""
    spec = importlib.util.spec_from_file_location("gen_syms",
                                                 os.path.join(HERE, "gen-syms.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def table(text, tag):
    i = text.index("k_ep122_%s[]" % tag)
    return text[i:text.index("\n};", i)]


def parse_header(text):
    """The four tables, in the order the resolver walks them."""
    vt = [dict(name=m.group(1), cls=m.group(2),
               base=None if m.group(3) == "NULL" else m.group(3).strip('"'),
               ref=int(m.group(4), 16))
          for m in re.finditer(r'\{ EP122_([A-Z0-9_]+), "([^"]+)", (\S+) \},'
                               r'\s*/\* 0x([0-9a-f]+) \*/', table(text, "vt"))]
    sl = [dict(name=m.group(1), vt=m.group(2), off=int(m.group(3), 16),
               ref=int(m.group(4), 16))
          for m in re.finditer(r"\{ EP122_([A-Z0-9_]+), EP122_([A-Z0-9_]+), "
                               r"0x([0-9a-f]+) \},\s*/\* 0x([0-9a-f]+) \*/",
                               table(text, "slot"))]
    fn = [dict(name=m.group(1), scope=m.group(2), nth=int(m.group(3)),
               total=int(m.group(4)), span=int(m.group(5)), insns=int(m.group(6)),
               anchor=int(m.group(7)), akey=int(m.group(8), 16),
               digest=int(m.group(9), 16), ref=int(m.group(10), 16))
          for m in re.finditer(r"\{ EP122_([A-Z0-9_]+), (\S+), (\d+), (\d+), (\d+), "
                               r"(\d+), (\d+), 0x([0-9a-f]+), 0x([0-9a-f]+)ULL \},"
                               r"\s*/\* 0x([0-9a-f]+) \*/", table(text, "fn"))]
    dt = [dict(name=m.group(1), src=m.group(2), insn=int(m.group(3)),
               ref=int(m.group(4), 16))
          for m in re.finditer(r"\{ EP122_([A-Z0-9_]+), EP122_([A-Z0-9_]+), (\d+) \},"
                               r"\s*/\* 0x([0-9a-f]+) \*/", table(text, "data"))]
    ct = [dict(name=m.group(1), src=m.group(2), insn=int(m.group(3)),
               ref=int(m.group(4), 16))
          for m in re.finditer(r"\{ EP122_([A-Z0-9_]+), EP122_([A-Z0-9_]+), (\d+) \},"
                               r"\s*/\* 0x([0-9a-f]+) \*/", table(text, "call"))]
    # One row per processor, reference first: a symbol's first row is the one
    # this image is asked for, and a later row is the other processor's.
    seen = {}
    for r in fn:
        r["variant"] = r["name"] in seen
        seen[r["name"]] = True
    return vt, sl, fn, dt, ct


def sweep(gs, elf, rows):
    """Every address whose masked window produces each row's digest.

    One pass over .text, hashed on the anchor word, exactly as resolve.c does it.
    """
    by_key = {}
    for i, r in enumerate(rows):
        by_key.setdefault(r["akey"], []).append(i)
    hits = [[] for _ in rows]
    lo, hi = elf.text_off, elf.text_off + elf.text_sz
    va, data = elf.text_va, elf.data
    for off in range(lo, hi - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        cand = by_key.get(((w * 2654435761) & 0xFFFFFFFF) >> 16)
        if not cand:
            continue
        for i in cand:
            r = rows[i]
            start = off - r["anchor"] * 4
            if start < lo or start + r["insns"] * 4 - 4 > hi - 4:
                continue
            if len(hits[i]) < 33:
                if gs.window_digest(elf, va + (start - lo), r["insns"]) == r["digest"]:
                    hits[i].append(va + (start - lo))
    return hits


def bl_targets(elf, start, span):
    out = []
    for i in range(0, span, 4):
        w = elf.w32(start + i)
        if w is None:
            break
        if (w & 0xFC000000) == 0x94000000:
            imm = w & 0x03FFFFFF
            if imm >> 25:
                imm -= 1 << 26
            out.append(start + i + imm * 4)
    return out


def resolve_image(gs, elf, vt, sl, fn, dt, ct, rows, label):
    """resolve.c's phases, in its order: RTTI, slots, sweep, data, call."""
    sym = {}
    rtti = gs.Rtti(elf, [r["cls"] for r in vt])
    for r in vt:
        tis = rtti.find_class(r["cls"])
        if len(tis) != 1:
            continue
        top = 0
        if r["base"]:
            off = rtti.base_offset(tis[0], r["base"])
            if off is None:
                continue
            top = -off
        vts = [v for v, t in rtti.object_vtables(tis[0]) if t == top]
        if len(vts) == 1:
            sym[r["name"]] = vts[0]
    for r in sl:
        v = sym.get(r["vt"])
        if v:
            f = elf.q(v + r["off"])
            if f and elf.off(f) is not None:
                sym[r["name"]] = f

    hits = sweep(gs, elf, fn)
    for i, r in enumerate(fn):
        if r["variant"] or r["name"] in sym:
            continue                    # this processor's rows only, first answer wins
        n = hits[i]
        if len(n) > 32:
            continue
        if r["scope"] == "0xffff":
            if len(n) == 1:
                sym[r["name"]] = n[0]
            continue
        parent = sym.get(r["scope"].replace("EP122_", ""))
        if not parent:
            continue
        ordered = []
        for t in bl_targets(elf, parent, r["span"]):
            if t in n and t not in ordered:
                ordered.append(t)
        # The count is the guard: a firmware that adds or drops a sibling would
        # otherwise hand back whichever one moved into position.
        if len(ordered) == r["total"] and r["nth"] < len(ordered):
            sym[r["name"]] = ordered[r["nth"]]

    for r in dt:
        src = sym.get(r["src"].replace("EP122_", ""))
        got = gs.adrp_pair_target(elf, src, r["insn"])[0] if src else None
        if got and not sym.get(r["name"]):
            sym[r["name"]] = got
    for r in ct:
        src = sym.get(r["src"].replace("EP122_", ""))
        got = gs.bl_target(elf, src, r["insn"])[0] if src else None
        if got and not sym.get(r["name"]):
            sym[r["name"]] = got

    wanted, ref = [], {}
    for r in list(vt) + list(sl) + list(fn) + list(dt) + list(ct):
        name = r["name"]
        if name not in ref:
            if "variant" in r and r["variant"]:
                continue
            ref[name] = r["ref"]
            wanted.append(name)
    missing = [n for n in wanted if n not in sym]
    moved = [n for n in wanted if n in sym and sym[n] != ref[n]]
    print("%s: %d/%d symbols resolve, %d of them at an address the header's "
          "comment does not have" % (label, len(sym), len(wanted), len(moved)))
    if missing:
        print("  UNRESOLVED (a mod needing these refuses on this image):")
        for n in missing:
            print("    " + n)
    return missing


def main():
    args = sys.argv[1:]
    binary = os.environ.get("EP122") or os.path.normpath(DEFAULT_DECK)
    variant = os.environ.get("EP122_RENESAS")
    if "-b" in args:
        binary = args[args.index("-b") + 1]
    if "-v" in args:
        variant = args[args.index("-v") + 1]
    if "--help" in args or "-h" in args:
        return print(__doc__) or 0
    if not os.path.exists(binary):
        raise SystemExit("%s: missing. See gen-syms.py for where to get the deck's copy."
                         % binary)

    gs = load_gen()
    vt, sl, fn, dt, ct = parse_header(open(HEADER).read())
    bad = 0
    try:
        bad += bool(resolve_image(gs, gs.Elf(binary), vt, sl, fn, dt, ct, fn, binary))
    except SystemExit as e:
        # The vtable walk bootstraps from the image's own type_info layout, which
        # only carries over between builds of the same processor. Being handed the
        # other one is a wrong argument, not a finding.
        raise SystemExit("%s: %s" % (binary, e))

    if variant and os.path.exists(variant):
        # Only the signature rows can be checked here: the vtables and slots need
        # the RTTI walk above, which is bootstrapped from this image's layout.
        other = [dict(r, variant=False) for r in fn if r["variant"]]
        hits = sweep(gs, gs.Elf(variant), other)
        found = sum(1 for h in hits if h)
        print("%s: %d/%d rows for the other processor have their window here"
              % (variant, found, len(other)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
