#!/usr/bin/env python3
"""Decode TESSERA-CPUTEST output from a ppc750cl.s run.

Reads emulator stdout (with --forward-console-logging) on stdin or from a file
and turns the raw failure records into something readable, then into a summary
that can go straight into the ledger.

Decoding lives here rather than in the ROM on purpose: the auxiliary-data layout
varies by which checker caught the failure, so a wrong interpretation is likely
on the first pass. Getting it wrong here costs a re-run of this script; getting
it wrong in the ROM costs a rebuild and another emulator launch.

    ./report.py run.log
    ./report.py --compare recompiler.log interpreter.log

stdlib only, matching testing/telemetry-report.py.
"""

import argparse
import re
import sys
from collections import Counter

# TESSERA-CPUTEST markers emitted by main.c
RE_RESULT = re.compile(r"TESSERA-CPUTEST RESULT=(\S+)(?:\s+(?:failures|rc)=(-?\d+))?")
RE_FAIL = re.compile(
    r"FAIL (\d+) insn=([0-9A-Fa-f]{8}) addr=([0-9A-Fa-f]{8}) "
    r"aux=([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8}) "
    r"extra=([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8})"
)
RE_NOTE = re.compile(r"TESSERA-CPUTEST NOTE (\S+)=(\S+)")

# Primary opcode -> rough family. Only used to group failures for the summary;
# this is deliberately coarse, because the point is "which area is broken", not
# a disassembler. A real disassembly needs powerpc-eabi-objdump.
PRIMARY = {
    3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi",
    12: "addic", 13: "addic.", 14: "addi", 15: "addis", 16: "bc",
    17: "sc", 18: "b", 19: "cr-ops/bclr/bcctr", 20: "rlwimi",
    21: "rlwinm", 23: "rlwnm", 24: "ori", 25: "oris", 26: "xori",
    27: "xoris", 28: "andi.", 29: "andis.", 31: "integer-extended",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu",
    38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau",
    44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu",
    52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    56: "psq_l", 57: "psq_lu", 59: "float-single-extended",
    60: "psq_st", 61: "psq_stu", 63: "float-double-extended",
}
# Primary 4 is the paired-single group on Gekko/Broadway/Espresso -- the part of
# the ISA that no generic PowerPC suite covers and that this fork most needs.
PRIMARY[4] = "paired-single (ps_*)"


def family(insn: int) -> str:
    return PRIMARY.get((insn >> 26) & 0x3F, f"primary-{(insn >> 26) & 0x3F}")


def disassemble(words):
    """Map instruction words to mnemonics via powerpc-eabi-objdump.

    Grouping by primary opcode (below) says "floating point"; grouping by mnemonic
    says "the multiply-add family", which is the difference between a category and
    a lead. Derived rather than tabulated so it cannot go stale.

    Returns {} if the toolchain is not on PATH -- the rest of the report still works.
    """
    import shutil, subprocess, tempfile, os
    as_, objdump = shutil.which("powerpc-eabi-as"), shutil.which("powerpc-eabi-objdump")
    if not (as_ and objdump):
        return {}
    d = tempfile.mkdtemp()
    try:
        src, obj = os.path.join(d, "i.s"), os.path.join(d, "i.o")
        with open(src, "w") as f:
            f.write("\t.text\n" + "".join(f"\t.long 0x{w:08X}\n" for w in words))
        if subprocess.run([as_, "-m750cl", "-o", obj, src],
                          capture_output=True).returncode != 0:
            return {}
        out = subprocess.run([objdump, "-d", "-M750cl", obj],
                             capture_output=True, text=True).stdout
        mnem = []
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3 and parts[2].strip():
                mnem.append(parts[2].split()[0])
        return dict(zip(words, mnem)) if len(mnem) == len(words) else {}
    finally:
        import shutil as sh; sh.rmtree(d, ignore_errors=True)


def parse(path):
    text = sys.stdin.read() if path == "-" else open(path, encoding="utf-8", errors="replace").read()
    result, count, notes, fails = None, None, {}, []
    for line in text.splitlines():
        if m := RE_RESULT.search(line):
            result = m.group(1)
            count = int(m.group(2)) if m.group(2) is not None else None
        elif m := RE_NOTE.search(line):
            notes[m.group(1)] = m.group(2)
        elif m := RE_FAIL.search(line):
            g = m.groups()
            fails.append({
                "index": int(g[0]),
                "insn": int(g[1], 16),
                "addr": int(g[2], 16),
                "aux": [int(x, 16) for x in g[3:7]],
                "extra": [int(x, 16) for x in g[7:9]],
            })
    return {"result": result, "count": count, "notes": notes, "fails": fails}


def emit(run, label):
    print(f"=== {label}")
    if run["result"] is None:
        print("  no TESSERA-CPUTEST result line found -- did the ROM run, and was")
        print("  --forward-console-logging passed?")
        return
    print(f"  result   {run['result']}")
    if run["count"] is not None:
        print(f"  reported {run['count']}")
    print(f"  records  {len(run['fails'])}")
    for k, v in run["notes"].items():
        print(f"  note     {k}={v}")

    if run["result"] == "BOOTSTRAP-FAILED":
        print("\n  The suite could not bootstrap. One of the instructions it assumes")
        print("  correct is broken: beq/bne cr0, bl, blr, fcmpu, mflr, mtlr.")
        print("  Fix that before reading any other result.")
        return

    if not run["fails"]:
        return

    by_family = Counter(family(f["insn"]) for f in run["fails"])
    print("\n  failures by instruction family:")
    for fam, n in by_family.most_common():
        print(f"    {n:>6}  {fam}")

    words = sorted({f["insn"] for f in run["fails"]})
    mnem = disassemble(words)
    if mnem:
        by_mnem = Counter(mnem[f["insn"]] for f in run["fails"] if f["insn"] in mnem)
        print(f"\n  failures by mnemonic ({len(by_mnem)} distinct instructions):")
        for m, n in by_mnem.most_common(20):
            print(f"    {n:>6}  {m}")
        # The Espresso deviates from IEEE most visibly in the multiply-add family and
        # in single-precision rounding, so call that grouping out explicitly.
        madd = sum(n for m, n in by_mnem.items() if "madd" in m or "msub" in m)
        if madd:
            print(f"\n    {madd} of these are multiply-add/subtract forms -- the suite's own"
                  f"\n    changelog cites tests for 'fmadds/ps_madd is not rounded twice' and"
                  f"\n    'non-rounding of the product using single-precision inputs'.")
    else:
        print("\n  (mnemonic breakdown needs powerpc-eabi-as/objdump on PATH)")

    # Repeated instruction words usually mean one broken handler, not N bugs.
    by_insn = Counter(f["insn"] for f in run["fails"])
    repeated = [(i, n) for i, n in by_insn.most_common() if n > 1]
    if repeated:
        print(f"\n  {len(repeated)} instruction word(s) failed more than once "
              f"(likely one defect each, not one per record):")
        for insn, n in repeated[:15]:
            print(f"    {n:>6}x  {insn:08X}  {family(insn)}")

    # Got-FPSCR breakdown (check_fpu: aux[2]; check_ps: aux[3]). Values-only is
    # green, so residual full-suite fails are almost all FPSCR bookkeeping —
    # this is how you rank the next lever without re-running IGNORE builds.
    emit_fpscr(run, mnem if mnem else {})


# Host bit layout matches PEM / src FPSCR_* (bit = 1 << (31 - ppc_bit)).
_FPSCR_BITS = (
    (31, "FX"), (30, "FEX"), (29, "VX"), (28, "OX"), (27, "UX"),
    (26, "ZX"), (25, "XX"), (24, "VXSNAN"), (23, "VXISI"), (22, "VXIDI"),
    (21, "VXZDZ"), (20, "VXIMZ"), (19, "VXVC"), (18, "FR"), (17, "FI"),
    (10, "VXSOFT"), (9, "VXSQRT"), (8, "VXCVI"),
)
_FPRF_CLASSES = {
    0x00: "empty",
    0x02: "+zero",
    0x04: "+norm",
    0x05: "+inf",
    0x08: "-norm",
    0x09: "-inf",
    0x11: "qNaN",
    0x12: "-zero",
    0x14: "+denorm",
    0x18: "-denorm",
}


def got_fpscr(fail):
    """FPSCR recorded in a failure record (word 4 for FPU, word 5 for PS)."""
    if ((fail["insn"] >> 26) & 0x3F) == 4:
        return fail["aux"][3]
    return fail["aux"][2]


def emit_fpscr(run, mnem):
    fails = run["fails"]
    if not fails:
        return
    zero = nonzero = 0
    by_class = Counter()
    bit_hits = Counter()
    zero_by_mnem = Counter()
    for f in fails:
        fps = got_fpscr(f)
        fprf = (fps >> 12) & 0x1F
        by_class[_FPRF_CLASSES.get(fprf, f"fprf=0x{fprf:02X}")] += 1
        if fps == 0:
            zero += 1
            zero_by_mnem[mnem.get(f["insn"], family(f["insn"]))] += 1
        else:
            nonzero += 1
        for bit, name in _FPSCR_BITS:
            if fps & (1 << bit):
                bit_hits[name] += 1

    print(f"\n  got FPSCR: {zero} zero / {nonzero} nonzero "
          f"(zero = op left FPSCR untouched)")
    print("  FPRF class in got FPSCR:")
    for cls, n in by_class.most_common():
        print(f"    {n:>6}  {cls}")
    if bit_hits:
        print("  sticky / FI bits set in got FPSCR:")
        for name, n in bit_hits.most_common():
            print(f"    {n:>6}  {name}")
    if zero_by_mnem:
        print("  FPSCR-zero failures by mnemonic (top 15):")
        for m, n in zero_by_mnem.most_common(15):
            print(f"    {n:>6}  {m}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="emulator stdout log(s), or - for stdin")
    ap.add_argument("--compare", action="store_true",
                    help="treat the two logs as recompiler vs interpreter arms")
    args = ap.parse_args()

    runs = [(p, parse(p)) for p in args.logs]
    for path, run in runs:
        emit(run, path)
        print()

    if args.compare:
        if len(runs) != 2:
            sys.exit("--compare needs exactly two logs")
        (pa, a), (pb, b) = runs
        sa = {(f["insn"], f["addr"]) for f in a["fails"]}
        sb = {(f["insn"], f["addr"]) for f in b["fails"]}
        print("=== comparison")
        print(f"  both arms      {len(sa & sb)}")
        print(f"  only {pa:<10} {len(sa - sb)}")
        print(f"  only {pb:<10} {len(sb - sa)}")
        print()
        print("  Failures in BOTH arms are shared decode/semantics defects.")
        print("  Failures in only one arm localise to that execution path --")
        print("  a recompiler-only failure is an AArch64 backend defect.")
        # Exit non-zero only when the arms disagree; a shared failure is a real
        # bug too, but it is not what this mode is asserting.
        sys.exit(1 if (sa ^ sb) else 0)

    sys.exit(0 if all(r["result"] == "PASS" for _, r in runs) else 1)


if __name__ == "__main__":
    main()
