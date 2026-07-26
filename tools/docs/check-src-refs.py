#!/usr/bin/env python3
"""Verify every [SRC file:line] reference in docs/hardware/ still resolves.

The hardware reference is only useful if its `file:line` anchors are real. Code moves;
this catches anchors that have rotted before someone acts on a stale one.

    tools/docs/check-src-refs.py            # check docs/hardware/
    tools/docs/check-src-refs.py docs/x.md  # check specific files

Exit status is non-zero if any reference fails to resolve.

What it checks:
  - the referenced file exists (by path, or by unique basename under src/)
  - the referenced line number is within the file
It deliberately does NOT try to verify that the line still says what the doc claims --
that needs a human. A moved anchor usually shows up as an out-of-range line first.
"""
import os
import re
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# [SRC Const.h:7]  /  [SRC coreinit_Memory.cpp:11-56]  /  [SRC ../porting/x.md §4.1]
# Also picks up multi-reference forms like [SRC a.cpp:1, b.cpp:2].
REF = re.compile(r"\[SRC\s+([^\]]+)\]")
# Longest extensions first: alternation is ordered, so `h` before `hpp` would truncate
# "PPCInterpreterSPR.hpp" to "PPCInterpreterSPR.h" and report a bogus miss.
FILE_LINE = re.compile(r"([\w./\\-]+\.(?:cpp|hpp|mm|md|py|sh|txt|xml|h))(?::(\d+))?")


def build_basename_index():
    index = defaultdict(list)
    for root, dirs, files in os.walk(REPO):
        dirs[:] = [d for d in dirs
                   if d not in {".git", "build", "dependencies", "Roms", "bin",
                                "vcpkg_installed", "node_modules"}]
        for f in files:
            index[f].append(os.path.join(root, f))
    return index


def resolve(ref_path, doc_dir, index):
    """Return an absolute path, or None."""
    for cand in (os.path.join(doc_dir, ref_path), os.path.join(REPO, ref_path)):
        cand = os.path.normpath(cand)
        if os.path.isfile(cand):
            return cand
    hits = index.get(os.path.basename(ref_path), [])
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        # Prefer src/ when a basename is ambiguous.
        src_hits = [h for h in hits if os.sep + "src" + os.sep in h]
        if len(src_hits) == 1:
            return src_hits[0]
    return None


def line_count(path, cache={}):
    if path not in cache:
        try:
            with open(path, "rb") as fh:
                cache[path] = sum(1 for _ in fh)
        except OSError:
            cache[path] = 0
    return cache[path]


def main(argv):
    targets = argv[1:]
    if not targets:
        hw = os.path.join(REPO, "docs", "hardware")
        targets = sorted(os.path.join(hw, f) for f in os.listdir(hw) if f.endswith(".md"))

    index = build_basename_index()
    checked = failed = 0
    problems = []

    for doc in targets:
        doc_dir = os.path.dirname(os.path.abspath(doc))
        with open(doc, encoding="utf-8") as fh:
            for lineno, text in enumerate(fh, 1):
                for body in REF.findall(text):
                    for ref_path, ref_line in FILE_LINE.findall(body):
                        checked += 1
                        target = resolve(ref_path, doc_dir, index)
                        rel = os.path.relpath(doc, REPO)
                        if target is None:
                            failed += 1
                            problems.append(f"{rel}:{lineno}: no such file: {ref_path}")
                            continue
                        if ref_line:
                            n = int(ref_line)
                            total = line_count(target)
                            if n > total:
                                failed += 1
                                problems.append(
                                    f"{rel}:{lineno}: {ref_path}:{n} is past end of file "
                                    f"({total} lines)")

    for p in problems:
        print(p, file=sys.stderr)
    print(f"checked {checked} [SRC] references, {failed} unresolved")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
