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

    # Repeated instruction words usually mean one broken handler, not N bugs.
    by_insn = Counter(f["insn"] for f in run["fails"])
    repeated = [(i, n) for i, n in by_insn.most_common() if n > 1]
    if repeated:
        print(f"\n  {len(repeated)} instruction word(s) failed more than once "
              f"(likely one defect each, not one per record):")
        for insn, n in repeated[:15]:
            print(f"    {n:>6}x  {insn:08X}  {family(insn)}")


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
