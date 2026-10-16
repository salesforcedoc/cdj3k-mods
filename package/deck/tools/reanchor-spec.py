#!/usr/bin/env python3
"""Re-anchor ep122_syms.spec's addresses to the binaries on the bench.

The spec's `addr=` and `ren=` are the REFERENCE BUILD's addresses, and they are
the one part of the spec that does not survive a firmware change. The generator
derives each signature from them and cannot tell a stale anchor from a fresh one
(a masked window at a stale address is still a unique window), and the build
never re-derives the header -- so a deck on a build the spec was not re-anchored
to is quiet until some dependent entry fails.

What does survive a relink is the generated header: its rows are masked windows
and digests, which are position-independent, so resolving them against an image
names the same code wherever it moved to. That is the oracle used here.

    tools/check-header.py -b build/ep122 -v build/ep122.g2m    # drift, or gone?
    tools/reanchor-spec.py                                     # what would move
    tools/reanchor-spec.py --write && tools/gen-syms.py        # re-anchor, regenerate

An entry whose descriptors are not found in the image it belongs to has no
oracle left -- the code itself changed, not just its address -- so it is listed
and left alone rather than guessed at. Exit status is non-zero when anything
could not be proven, or (without --write) when anything would move.
"""

import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.dirname(HERE)
SPEC = os.path.join(DECK, "mods", "core", "ep122_syms.spec")
HEADER = os.path.join(DECK, "mods", "core", "ep122_syms.h")
DEFAULT_DECK = os.path.join(DECK, "..", "..", "..", "..", "build", "EP122-deck")


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"),
                                                  os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gs = load("gen-syms")
ch = load("check-header")


def vtables_nokinds(elf, mangled):
    """Every vtable for a class, without the type_info flavour bootstrap.

    The bootstrap samples the reference build's own layout and does not carry
    over to the other processor's image -- which is exactly the image the Renesas
    slot values have to be read out of. It is only needed to tell a class's base
    layout apart, so a class that names no `base=` is reachable without it.
    """
    rtti = gs.Rtti(elf, [])
    tis = []
    for sva in elf.find_bytes(mangled.encode() + b"\0"):
        off = elf.off(sva)
        if off and 0x20 <= elf.data[off - 1] < 0x7F:      # a suffix of a longer name
            continue
        for p in elf.find_ptrs(sva):
            if rtti._looks_like_ti(p - 8):
                tis.append(p - 8)
    out = []
    for ti in sorted(set(tis)):
        for v, top in rtti.vtables(ti):
            if top == 0 and elf.is_text(elf.q(v)):
                out.append(v)
    return sorted(set(out))


def variant_slots(elf, vt, sl):
    """The other processor's slot values: whatever its own vtable slot holds."""
    cls_of = {r["name"]: r["cls"] for r in vt}
    vtable = {}
    out = {}
    for r in sl:
        cls = cls_of.get(r["vt"])
        if cls is None:
            continue
        if cls not in vtable:
            vts = vtables_nokinds(elf, cls)
            vtable[cls] = vts[0] if len(vts) == 1 else None
        v = vtable[cls]
        if v is None:
            continue
        val = elf.q(v + r["off"])
        if val and elf.off(val) is not None:
            out[r["name"]] = val
    return out


def variant_symbols(elf, vt, sl, fn, dt, ct):
    """The addresses the OTHER processor's rows name, on that processor's image.

    The signature rows come from the sweep alone; the vtables and slots come from
    the class name, never from the flavour walk, which is the deck image's.

    Returns the addresses, and the symbols whose Renesas row needed scoping that
    the spec does not carry yet -- a row can only be ambiguous here because it was
    emitted without ren_in, so it is the caller's to mirror from the primary row.
    """
    sym = dict(variant_slots(elf, vt, sl))
    primary_of = {}
    for r in fn:
        if not r["variant"]:
            primary_of.setdefault(r["name"], r)
    rows = [dict(r, variant=False) for r in fn if r["variant"]]
    hits = ch.sweep(gs, elf, rows)
    ambiguous = set()
    for i, r in enumerate(rows):
        n = hits[i]
        if r["name"] in sym:
            continue
        scope, span, nth, total = r["scope"], r["span"], r["nth"], r["total"]
        if len(n) != 1:
            # The row was emitted unscoped because the spec had no ren_in. The
            # primary row's scoping is the same rule, on this image: the twin
            # siblings differ only in the global they touch, and only one of them
            # is called from the enclosing function.
            primary = primary_of.get(r["name"])
            if primary is None or primary["scope"] == "0xffff":
                continue
            scope, span, nth = primary["scope"], primary["span"], primary["nth"]
            total = None            # the generator records what it observes
            ambiguous.add(r["name"])
        if scope == "0xffff":
            if len(n) == 1:
                sym[r["name"]] = n[0]
            continue
        parent = sym.get(scope.replace("EP122_", ""))
        if not parent:
            continue
        ordered = []
        for t in ch.bl_targets(elf, parent, span):
            if t in n and t not in ordered:
                ordered.append(t)
        if total is not None and len(ordered) != total:
            continue
        if nth < len(ordered):
            sym[r["name"]] = ordered[nth]
        else:
            ambiguous.discard(r["name"])
    for rows, pair in ((dt, gs.adrp_pair_target), (ct, gs.bl_target)):
        for r in rows:
            if not r["variant"]:
                continue
            src = sym.get(r["src"].replace("EP122_", ""))
            got = pair(elf, src, r["insn"])[0] if src else None
            if got and not sym.get(r["name"]):
                sym[r["name"]] = got
    return sym, ambiguous


def main():
    args = sys.argv[1:]
    write = "--write" in args
    deck = os.environ.get("EP122") or os.path.normpath(DEFAULT_DECK)
    ren = os.environ.get("EP122_RENESAS")
    if "-b" in args:
        deck = args[args.index("-b") + 1]
    if "-v" in args:
        ren = args[args.index("-v") + 1]
    if "-h" in args or "--help" in args:
        return print(__doc__) or 0
    if not os.path.exists(deck):
        raise SystemExit("%s: missing (the deck's own copy; see gen-syms.py)" % deck)
    if not ren or not os.path.exists(ren):
        raise SystemExit("$EP122_RENESAS is needed: every ren= entry is anchored to "
                         "that image (see gen-syms.py)")

    vt, sl, fn, dt, ct = ch.parse_header(open(HEADER).read())
    deck_elf, ren_elf = gs.Elf(deck), gs.Elf(ren)
    deck_sym, _, _ = ch.resolve_image(gs, deck_elf, vt, sl, fn, dt, ct)
    ren_sym, ren_needs_scope = variant_symbols(ren_elf, vt, sl, fn, dt, ct)

    # A slot is only asked for its Renesas value when something else scopes to it,
    # because that value is a parent address rather than an address of the slot.
    # That includes the scoping this very run is about to add: a variant row that
    # needs ren_in takes its parent's name from the primary row.
    needed = set()
    for line in open(SPEC):
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        parts = body.split()
        kv = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
        if "ren_in" in kv:
            needed.add(kv["ren_in"])
        if parts[0] in ("data", "call") and "ren" in kv:
            needed.add(kv["from"].replace("EP122_", ""))
        if parts[1] in ren_needs_scope and "in" in kv:
            needed.add(kv["in"])

    lines = open(SPEC).read().split("\n")
    moved, added, unproven = [], [], []

    def setfield(line, field, val):
        return re.sub(r"\b%s=0x[0-9a-f]+" % field, "%s=0x%x" % (field, val),
                      line, count=1)

    for n, line in enumerate(lines, 1):
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        parts = body.split()
        kind, name = parts[0], parts[1]
        kv = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
        new = line
        if kind == "func":
            a = deck_sym.get(name)
            if a is None:
                unproven.append("%s (addr)" % name)
                continue
            if int(kv["addr"], 0) != a:
                new = setfield(new, "addr", a)
            if "ren" in kv:
                b = ren_sym.get(name)
                if b is None:
                    unproven.append("%s (ren)" % name)
                    continue
                if int(kv["ren"], 0) != b:
                    new = setfield(new, "ren", b)
                if name in ren_needs_scope and "ren_in" not in kv:
                    new = new.rstrip() + " ren_in=%s ren_span=%s ren_nth=%s" % (
                        kv["in"], kv.get("span", "0x400"), kv.get("nth", "0"))
                    added.append(name + " (ren scope)")
        elif kind == "slot":
            if "ren" not in kv and name not in needed:
                continue
            if name not in ren_sym:
                unproven.append("%s (ren slot)" % name)
                continue
            if "ren" not in kv:
                new = new.rstrip() + " ren=0x%x" % ren_sym[name]
                added.append(name)
            elif int(kv["ren"], 0) != ren_sym[name]:
                new = setfield(new, "ren", ren_sym[name])
        elif kind in ("data", "call"):
            pair = gs.adrp_pair_target if kind == "data" else gs.bl_target
            src = kv["from"].replace("EP122_", "")
            for field, table, elf, insn in (("addr", deck_sym, deck_elf, "insn"),
                                            ("ren", ren_sym, ren_elf, "ren_insn")):
                if field == "ren" and "ren" not in kv:
                    continue
                parent = table.get(src)
                if not parent:
                    unproven.append("%s (%s parent %s)" % (name, field, src))
                    continue
                got, err = pair(elf, parent, int(kv[insn], 0))
                if got is None:
                    unproven.append("%s (%s %s): %s" % (name, field, insn, err))
                    continue
                if int(kv[field], 0) != got:
                    new = setfield(new, field, got)
        if new != line:
            moved.append((n, line.strip(), new.strip()))
            lines[n - 1] = new

    for n, old, new in moved:
        print("%d: %s\n     -> %s" % (n, old, new))
    if added:
        print("added ren= to slots something scopes to: %s" % ", ".join(sorted(added)))
    if unproven:
        print("\nno oracle for these -- the code itself changed, so they have to be "
              "found by hand rather than by moving the anchor:")
        for u in sorted(set(unproven)):
            print("  " + u)
    print("\n%d anchors would move" % len(moved))
    if not write:
        return 1 if (moved or unproven) else 0
    if moved:
        open(SPEC, "w").write("\n".join(lines))
        print("wrote %s -- now run tools/gen-syms.py" % SPEC)
    return 1 if unproven else 0


if __name__ == "__main__":
    sys.exit(main())
