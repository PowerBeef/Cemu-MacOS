#!/usr/bin/env python3
"""Summarise or diff telemetry runs.

    testing/telemetry-report.py run.jsonl              # summarise one run
    testing/telemetry-report.py a.jsonl b.jsonl        # A/B two runs
    testing/telemetry-report.py run.jsonl --all        # do not split into phases
    testing/telemetry-report.py run.jsonl --phase=0    # pick a phase explicitly

A run is NOT one workload. `testing/drive-botw.sh` spends its first ~4,000 frames in the
title and save-select menus before the game is even loaded, and the menu is a completely
different thing to measure: 113 draws and 4.2 ms of GPU work per frame versus 3,516 draws
and 19.0 ms in the open world. Both are steady, and they sit at different vsync divisions
(33.27 ms vs 49.90 ms), so a median over the whole file lands wherever the phase *ratio*
happens to fall and describes no frame that actually occurred.

That is not hypothetical. Reporting whole-file medians produced "the frame is 33.27 ms and
work fits in one vsync period with 24% headroom" -- both menu figures -- and a "-6.2% GPU
busy" A/B result that was entirely the two runs having slightly different menu-to-gameplay
frame ratios. Restricted to gameplay the effect was zero.

So this splits a run into contiguous phases by draw count, prints them, and analyses the
LONGEST one unless told otherwise. It always says which phase it picked and how many
frames it set aside.

Why medians and percentiles rather than a mean: a mean hides the shape. Within the BotW
gameplay phase the median frame is 49.90 ms and p99 is 49.97 -- essentially no variance,
which is the signature of vsync quantisation rather than of variable work.

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


def phases(header, frames):
    """Split into contiguous runs of similar draw load.

    The split is on gpu.draw_calls against half the run's peak, which separates a menu
    (~113 draws) from gameplay (~3,516) with an enormous margin and is not sensitive to
    where exactly the threshold falls. Runs shorter than 100 frames are loading
    transitions and are folded into neither side -- they are reported, not analysed.
    """
    names = [c["n"] for c in header["counters"]]
    if "gpu.draw_calls" not in names or not frames:
        return [(0, len(frames) - 1)]
    di = names.index("gpu.draw_calls")
    draws = [f["v"][di] for f in frames]
    peak = sorted(draws)[int(len(draws) * 0.99)]
    if peak == 0:
        return [(0, len(frames) - 1)]
    heavy = [d > peak / 2 for d in draws]
    out, start = [], 0
    for k in range(1, len(heavy)):
        if heavy[k] != heavy[k - 1]:
            out.append((start, k - 1))
            start = k
    out.append((start, len(heavy) - 1))
    return out


def describe_phases(header, frames, chosen):
    names = [c["n"] for c in header["counters"]]
    di = names.index("gpu.draw_calls") if "gpu.draw_calls" in names else None
    print("\n    phases (a run is not one workload -- see the module docstring)")
    for k, (a, b) in enumerate(phases(header, frames)):
        sub = frames[a:b + 1]
        ms = statistics.median(f["ns"] / 1e6 for f in sub)
        dr = statistics.median(f["v"][di] for f in sub) if di is not None else 0
        mark = "  <-- analysed" if (a, b) == chosen else ""
        print(f"      [{k}] frames {a:6d}-{b:<6d} n={len(sub):6d}  "
              f"{ms:7.2f} ms = {1000/ms:6.2f} fps   {dr:7.0f} draws{mark}")


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


def select(header, frames, opts, quiet=False):
    """Pick the phase to analyse and say so. Never narrows silently."""
    if opts.get("all"):
        if not quiet:
            print("\n    --all: analysing every frame, phases NOT separated")
        return frames
    ph = phases(header, frames)
    want = opts.get("phase")
    chosen = ph[want] if want is not None and want < len(ph) else max(ph, key=lambda r: r[1] - r[0])
    if not quiet:
        describe_phases(header, frames, chosen)
        dropped = len(frames) - (chosen[1] - chosen[0] + 1)
        print(f"      ({dropped} frames in other phases are excluded below)")
    return frames[chosen[0]:chosen[1] + 1]


def report(path, skip, opts):
    header, frames, details = load(path)
    print(f"=== {path}")
    print(f"    build {header['build']}  label {header.get('label') or '-'}  "
          f"title {header['title']['name']} ({header['title']['id']})")
    cfg = header.get("config", {})
    if cfg:
        print("    " + "  ".join(f"{k}={v}" for k, v in cfg.items()))
    frames = select(header, frames, opts)
    ms = frame_ms(frames, skip)
    if not ms:
        print("    no frames after skip")
        return
    st = stats(ms)
    print(f"\n    frames {len(ms)} (first {skip} dropped as boot)")
    print(f"    frame ms   median {st['median']:7.2f}   p99 {st['p99']:7.2f}   max {st['max']:8.2f}")
    print(f"    fps        median {1000/st['median']:7.2f}   mean {1000/st['mean']:7.2f}"
          f"   1%low {1000/st['p99']:7.2f}")

    # Frame-time decomposition. The frame interval alone cannot distinguish "the work
    # takes 50ms" from "the work takes 34ms and we missed a 33.3ms vsync deadline, so the
    # pacer held the frame for a whole extra period". Those need opposite fixes, and the
    # difference is the single most decision-relevant number in a run.
    ser = series(header, frames, skip)
    waits = ["gpu.cp_idle_ns", "gpu.cp_fence_ns", "gpu.wait_flip_ns", "gpu.drawable_wait_ns"]
    have = [w for w in waits if w in ser and sum(ser[w]) > 0]
    if have:
        n = len(ms)
        wait_ms = [sum(ser[w][i] for w in have) / 1e6 for i in range(n)]
        work_ms = [max(0.0, ms[i] - wait_ms[i]) for i in range(n)]
        wk = stats(work_ms)
        print(f"\n    frame-time decomposition (median)")
        print(f"      frame                          {st['median']:8.2f} ms")
        for w in have:
            s_w = stats([v / 1e6 for v in ser[w]])
            print(f"      - {w:28s} {s_w['median']:8.2f} ms   p99 {s_w['p99']:8.2f} ms")
        gpu = stats([v / 1e6 for v in ser.get("gpu.busy_ns", [0])])
        print(f"      = work (frame minus waits)     {wk['median']:8.2f} ms   p99 {wk['p99']:8.2f} ms")
        if gpu and gpu["median"]:
            # NOT a subset of the line above: the GPU runs asynchronously, so its busy
            # time overlaps the Latte thread's waits rather than nesting inside its work.
            print(f"      (GPU busy, runs concurrently)  {gpu['median']:8.2f} ms")
        # How far from the next vsync division? Wii U vsync is 59.94Hz.
        vsync = 1000.0 / 59.94
        for mult in (1, 2, 3, 4):
            budget = vsync * mult
            if wk["median"] <= budget:
                head = (budget - wk["median"]) / budget * 100
                print(f"      work fits in {mult} vsync period(s) = {budget:.2f} ms "
                      f"with {head:.1f}% headroom")
                break
        else:
            print(f"      work exceeds 4 vsync periods ({vsync*4:.2f} ms)")

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


def diff(a, b, skip, opts):
    ha, fa, _ = load(a)
    hb, fb, _ = load(b)
    print(f"=== A {a}\n=== B {b}\n")
    # Phase-select BOTH sides. Two runs of the same script rarely spend the same number of
    # frames in the menu, and comparing whole-file medians turns that difference into a
    # counter delta that looks like a treatment effect and is not.
    na, nb = len(fa), len(fb)
    fa = select(ha, fa, opts, quiet=True)
    fb = select(hb, fb, opts, quiet=True)
    if not opts.get("all"):
        print(f"phase-selected: A {len(fa)}/{na} frames, B {len(fb)}/{nb} "
              f"(run with --all to compare every frame)\n")

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
    opts = {}
    for a in sys.argv[1:]:
        if a.startswith("--skip="):
            skip = int(a.split("=", 1)[1])
        elif a == "--all":
            opts["all"] = True
        elif a.startswith("--phase="):
            opts["phase"] = int(a.split("=", 1)[1])
    if len(args) == 1:
        report(args[0], skip, opts)
    elif len(args) == 2:
        diff(args[0], args[1], skip, opts)
    else:
        sys.exit(__doc__)
