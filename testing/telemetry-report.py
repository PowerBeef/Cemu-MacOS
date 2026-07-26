#!/usr/bin/env python3
"""Summarise or diff telemetry runs.

    testing/telemetry-report.py run.jsonl              # summarise one run
    testing/telemetry-report.py a.jsonl b.jsonl        # A/B two runs

Why medians and percentiles rather than a mean: the window-title FPS this project has
been quoting all along is a mean, and a mean hides the thing you usually care about. On
BotW the median frame is 33.27 ms (30.06 fps, i.e. on target) while p99 is 49.9 ms -- a
tail of dropped frames drags the mean to 29.0. "Runs at 28.6 fps" and "hits 30 fps but
drops 1% of frames" call for completely different work.

Runs are aligned by counter *name*, never by index, so two files produced by different
builds with different counter sets still compare correctly; counters present in only one
run are reported rather than silently ignored.
"""
import json
import statistics
import sys


def load(path):
    header, frames, details = None, [], []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue  # a torn final line: the process was killed mid-write
            if rec.get("type") == "run":
                header = rec
            elif rec.get("t") == "f":
                frames.append(rec)
            elif rec.get("t") == "acc":
                details = rec.get("details", details)
            elif rec.get("type") == "summary":
                details = rec.get("accuracy_details", details)
    if header is None:
        sys.exit(f"{path}: no run header (was --telemetry passed?)")
    return header, frames, details


def series(header, frames, skip):
    """counter name -> list of per-frame values, boot frames dropped."""
    names = [c["n"] for c in header["counters"]]
    out = {n: [] for n in names}
    for rec in frames[skip:]:
        for i, v in enumerate(rec["v"]):
            if i < len(names):
                out[names[i]].append(v)
    return out


def stats(values):
    if not values:
        return None
    s = sorted(values)
    return {
        "median": statistics.median(s),
        "mean": sum(s) / len(s),
        "p1": s[len(s) // 100],
        "p99": s[min(len(s) - 1, len(s) * 99 // 100)],
        "max": s[-1],
        "total": sum(s),
    }


def frame_ms(frames, skip):
    return [r["ns"] / 1e6 for r in frames[skip:]]


def report(path, skip):
    header, frames, details = load(path)
    print(f"=== {path}")
    print(f"    build {header['build']}  label {header.get('label') or '-'}  "
          f"title {header['title']['name']} ({header['title']['id']})")
    cfg = header.get("config", {})
    if cfg:
        print("    " + "  ".join(f"{k}={v}" for k, v in cfg.items()))
    ms = frame_ms(frames, skip)
    if not ms:
        print("    no frames after skip")
        return
    st = stats(ms)
    print(f"\n    frames {len(ms)} (first {skip} dropped as boot)")
    print(f"    frame ms   median {st['median']:7.2f}   p99 {st['p99']:7.2f}   max {st['max']:8.2f}")
    print(f"    fps        median {1000/st['median']:7.2f}   mean {1000/st['mean']:7.2f}"
          f"   1%low {1000/st['p99']:7.2f}")

    print("\n    per-frame counters (median, non-zero only)")
    for name, vals in series(header, frames, skip).items():
        s = stats(vals)
        if not s or s["total"] == 0:
            continue
        unit = next((c["u"] for c in header["counters"] if c["n"] == name), "")
        if unit == "ns":
            print(f"      {name:34s} {s['median']/1e6:12.2f} ms   p99 {s['p99']/1e6:10.2f} ms")
        else:
            print(f"      {name:34s} {s['median']:12.1f} {unit:6s} p99 {s['p99']:10.1f}")

    if details:
        print("\n    accuracy signals fired:")
        for d in details:
            print(f"      {d['signal']:34s} {d['detail']}")


def diff(a, b, skip):
    ha, fa, _ = load(a)
    hb, fb, _ = load(b)
    print(f"=== A {a}\n=== B {b}\n")

    ca, cb = ha.get("config", {}), hb.get("config", {})
    changed = [(k, ca.get(k), cb.get(k)) for k in sorted(set(ca) | set(cb))
               if ca.get(k) != cb.get(k)]
    if changed:
        print("config differences:")
        for k, va, vb in changed:
            print(f"  {k:24s} {va} -> {vb}")
        print()
    else:
        print("config identical\n")

    ma, mb = stats(frame_ms(fa, skip)), stats(frame_ms(fb, skip))
    print(f"{'metric':36s} {'A':>14s} {'B':>14s} {'delta':>10s}")
    print("-" * 78)
    for label, va, vb in (("frame ms median", ma["median"], mb["median"]),
                          ("frame ms p99", ma["p99"], mb["p99"]),
                          ("fps median", 1000 / ma["median"], 1000 / mb["median"])):
        pct = (vb - va) / va * 100 if va else 0
        print(f"{label:36s} {va:14.2f} {vb:14.2f} {pct:+9.1f}%")

    sa, sb = series(ha, fa, skip), series(hb, fb, skip)
    only_a = sorted(set(sa) - set(sb))
    only_b = sorted(set(sb) - set(sa))
    for name in sorted(set(sa) & set(sb)):
        ra, rb = stats(sa[name]), stats(sb[name])
        if not ra or not rb or (ra["total"] == 0 and rb["total"] == 0):
            continue
        pct = (rb["median"] - ra["median"]) / ra["median"] * 100 if ra["median"] else float("nan")
        scale = 1e6 if name.endswith("_ns") else 1
        print(f"{name:36s} {ra['median']/scale:14.2f} {rb['median']/scale:14.2f} {pct:+9.1f}%")
    for name in only_a:
        print(f"{name:36s} {'present':>14s} {'ABSENT':>14s}")
    for name in only_b:
        print(f"{name:36s} {'ABSENT':>14s} {'present':>14s}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    skip = 60  # boot frames: the first is seconds long and would dominate every stat
    for a in sys.argv[1:]:
        if a.startswith("--skip="):
            skip = int(a.split("=", 1)[1])
    if len(args) == 1:
        report(args[0], skip)
    elif len(args) == 2:
        diff(args[0], args[1], skip)
    else:
        sys.exit(__doc__)
