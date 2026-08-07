#!/usr/bin/env python3
"""Generate the fork status tracker.

    docs/status/build-status.py            # regenerate docs/status/index.html
    docs/status/build-status.py --verify   # validate + report drift as Markdown (this is what CI runs)
    docs/status/build-status.py --check    # local pre-commit: did I forget to regenerate?
    docs/status/build-status.py -o /tmp/x.html

This repo's documentation has gone wrong the same way more than once: a number is typed
into a Markdown file, the code moves, and the number quietly becomes a lie. Commit 938a472
corrected fourteen of them in one pass. So the rule here is that **anything a file in the
repo already knows is read from that file, never typed into the ledger**:

    fork commit list, dates, subjects  <- git log
    files changed / insertions / deletions, per-area rollup
                                       <- git diff --numstat
    every scene capture ever recorded  <- testing/golden/baseline.tsv
    telemetry counter count and split  <- src/Cemu/Telemetry/TelemetryCounters.def

`ledger.json` holds only what no file can derive: a one-line verdict per work item, its
status, which commits it spans, and a pointer to the doc section that owns the reasoning.
It deliberately does NOT restate that reasoning -- the master plan stays the source of
truth, and a second copy of an argument is a second thing to keep in sync.

Two self-checks run on every generation, and they are deliberately NOT equally severe:

    1. every commit hash named in the ledger must resolve in git -- ABORTS. A hash that does
       not exist is a broken file, always the author's fault, and rendering it would publish
       a dead reference.
    2. every fork commit must be claimed by some entry, or be listed in `unattributed` --
       REPORTED, not fatal. It surfaces on the page as "Not in the ledger yet" and in
       --verify's output, so new work shows up loudly instead of being silently absent.

Why (2) cannot be fatal, and why --check cannot gate CI: both are self-reference.

    A ledger entry cannot name the hash of the commit that contains it, so the newest commit
    is ALWAYS unclaimed at the moment CI sees it. --verify therefore reports the tip commit
    separately from real drift.

    The page embeds `generated from <HEAD>` and `commits since fork: N`, and both change with
    the very commit that carries the HTML. So the committed page is always one commit behind
    by construction and a byte comparison can never pass in CI. --check is still useful
    locally, where HEAD has not moved between regenerating and running it.

Stdlib only, matching testing/telemetry-report.py.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
LEDGER_PATH = HERE / "ledger.json"
DEFAULT_OUT = HERE / "index.html"

BASELINE_TSV = REPO / "testing" / "golden" / "baseline.tsv"
COUNTERS_DEF = REPO / "src" / "Cemu" / "Telemetry" / "TelemetryCounters.def"

# Order matters: it is the order statuses appear in the filter bar and the legend.
STATUS_ORDER = ["landed", "refuted", "cancelled", "reverted", "open", "deferred", "blocked"]
AREA_ORDER = ["platform", "cpu", "graphics", "audio", "input", "tooling", "docs"]


class Abort(Exception):
    """A self-check failed. Emitting the page anyway would publish something wrong."""


# ----------------------------------------------------------------------------- sources


def git(*args: str) -> str:
    proc = subprocess.run(
        ["git", "-C", str(REPO), *args], capture_output=True, text=True
    )
    if proc.returncode != 0:
        raise Abort(f"git {' '.join(args)} failed:\n{proc.stderr.strip()}")
    return proc.stdout


def head_state() -> dict:
    # "dirty" means *the measurement data this page quotes* is uncommitted -- a page showing
    # numbers that are not in the repo. It deliberately excludes ledger.json, which is modified
    # BY DEFINITION at the moment you regenerate: including it fires the badge on every single
    # legitimate build, bakes the warning into every committed page, and trains the reader to
    # ignore it. It excludes source edits too, since those cannot change the page at all.
    inputs = [
        str(BASELINE_TSV.relative_to(REPO)),
        str(COUNTERS_DEF.relative_to(REPO)),
    ]
    return {
        "short": git("rev-parse", "--short", "HEAD").strip(),
        "date": git("log", "-1", "--format=%ad", "--date=short").strip(),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD").strip(),
        "dirty": bool(git("status", "--porcelain", "--", *inputs).strip()),
    }


def fork_commits(fork_point: str) -> list[dict]:
    """Every commit on this fork, oldest first. `index` is used for staleness math."""
    fmt = "%H\x1f%h\x1f%ad\x1f%s"
    out = git("log", "--reverse", f"--pretty={fmt}", "--date=short", f"{fork_point}..HEAD")
    commits = []
    for i, line in enumerate(out.splitlines()):
        if not line.strip():
            continue
        full, short, date, subject = line.split("\x1f")
        commits.append(
            {"index": i, "full": full, "short": short, "date": date, "subject": subject}
        )
    return commits


def divergence(fork_point: str) -> dict:
    """Line-level divergence from the fork point, rolled up by top-two path components.

    Binary files report '-' for both counts; they are counted as touched files but
    contribute no lines, which is why `files` is not the sum of anything below it.
    """
    out = git("diff", "--numstat", f"{fork_point}..HEAD")
    areas: dict[str, dict] = {}
    files = added = removed = 0
    for line in out.splitlines():
        if not line.strip():
            continue
        add_s, del_s, path = line.split("\t", 2)
        files += 1
        parts = path.split("/")
        key = "/".join(parts[:2]) if len(parts) > 1 else parts[0]
        area = areas.setdefault(key, {"path": key, "files": 0, "added": 0, "removed": 0})
        area["files"] += 1
        if add_s != "-":  # binary
            area["added"] += int(add_s)
            area["removed"] += int(del_s)
            added += int(add_s)
            removed += int(del_s)
    ranked = sorted(
        areas.values(), key=lambda a: a["added"] + a["removed"], reverse=True
    )
    return {"files": files, "added": added, "removed": removed, "areas": ranked}


def baselines() -> list[dict]:
    """testing/golden/baseline.tsv -- the committed record of every scene capture."""
    if not BASELINE_TSV.exists():
        return []
    cols = ["when", "commit", "scene", "fps", "backend", "rss", "threads", "cpu"]
    rows = []
    for line in BASELINE_TSV.read_text().splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        fields += [""] * (len(cols) - len(fields))
        rows.append(dict(zip(cols, fields[: len(cols)])))
    return rows


# Anchored at start-of-line: the .def's own header comment documents the macro shape as
# `TLM_COUNTER(id, "dotted.name", "unit", Area)`, which an unanchored pattern happily counts
# as a 114th counter. Caught by cross-checking against `grep -c '^TLM_COUNTER'`.
COUNTER_RE = re.compile(
    r'^TLM_COUNTER\(\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\w+)\s*\)',
    re.MULTILINE,
)


def counters() -> dict:
    """Counter total and per-area split, parsed from the X-macro .def.

    Regex rather than a line count because at least one declaration carries a trailing
    // comment, and a naive split on ',' would misread its area.
    """
    if not COUNTERS_DEF.exists():
        return {"total": 0, "by_area": []}
    text = COUNTERS_DEF.read_text()
    found = COUNTER_RE.findall(text)
    by_area = Counter(area for _, _, _, area in found)
    return {
        "total": len(found),
        "by_area": sorted(by_area.items(), key=lambda kv: kv[1], reverse=True),
    }


# ------------------------------------------------------------------------ self-checks


def check_hashes_resolve(ledger: dict) -> None:
    """A typo'd or rebased-away hash must abort, not render as a dead reference."""
    named: set[str] = set()
    for bucket in ("items", "refuted", "roadmap"):
        for entry in ledger.get(bucket, []):
            named.update(entry.get("commits", []))
    named.update(ledger.get("unattributed", []))
    named.add(ledger["fork_point"])

    bad = []
    for sha in sorted(named):
        proc = subprocess.run(
            ["git", "-C", str(REPO), "cat-file", "-e", f"{sha}^{{commit}}"],
            capture_output=True,
        )
        if proc.returncode != 0:
            bad.append(sha)
    if bad:
        raise Abort(
            "ledger.json names commits that do not resolve: " + ", ".join(bad)
        )


def check_coverage(ledger: dict, commits: list[dict]) -> list[dict]:
    """Return fork commits no item claims and `unattributed` does not excuse.

    Not fatal -- it is rendered on the page. Silently dropping new work is the failure
    mode worth designing against; stopping the build for it is not.
    """
    claimed: set[str] = set(ledger.get("unattributed", []))
    for bucket in ("items", "refuted", "roadmap"):
        for entry in ledger.get(bucket, []):
            claimed.update(entry.get("commits", []))
    # Ledger entries use short hashes; match on either form.
    claimed_full = set()
    for c in commits:
        if c["short"] in claimed or c["full"] in claimed:
            claimed_full.add(c["full"])
    return [c for c in commits if c["full"] not in claimed_full]


LEDGER_REL = "docs/status/ledger.json"

# What counts as "this commit did the work", as opposed to "this commit wrote it down".
#
# Prose is not the signal. A record commit in this repo routinely updates AGENTS.md, a porting
# doc or a testing README in the same breath as the ledger entry -- that IS the recording. Using
# any-file-outside-docs/status/ as the trigger fires on 60 of 71 correct entries, including all
# 55 back-filled by the tracker's own genesis commit, and a check that noisy gets switched off.
#
# Code and test artefacts are the signal: if the birth commit changed these, it did the work.
WORK_PREFIXES = ("src/", "testing/", "tools/")


def _is_work_file(path: str) -> bool:
    return path.startswith(WORK_PREFIXES) and not path.endswith(".md")


def entry_origins() -> dict[str, dict]:
    """id -> {commit, files} for the commit that first carried each entry.

    Derived by replaying `ledger.json`'s own history, so it costs one `git show` per commit
    that ever touched the file (33 as of writing, and it only grows with ledger edits).
    """
    origins: dict[str, dict] = {}
    seen: set[str] = set()
    for rev in git("log", "--format=%H", "--reverse", "--", LEDGER_REL).split():
        try:
            data = json.loads(git("show", f"{rev}:{LEDGER_REL}"))
        except (Abort, json.JSONDecodeError):
            continue  # a shape this version of the script cannot read is not an authoring error
        ids = [
            e["id"]
            for bucket in ("items", "refuted", "roadmap")
            for e in data.get(bucket, [])
            if isinstance(e, dict) and e.get("id")
        ]
        fresh = [i for i in ids if i not in seen]
        seen.update(ids)
        if not fresh:
            continue
        files = [f for f in git("show", "--name-only", "--format=", rev).splitlines() if f.strip()]
        for eid in fresh:
            origins.setdefault(eid, {"commit": rev, "files": files})
    return origins


def check_self_claim(ledger: dict) -> list[tuple[str, str, str]]:
    """Catch an entry that describes the commit it was born in but names that commit's parent.

    This is the hole the other checks cannot see. `check_coverage` finds a commit *nothing*
    claims; nothing finds a commit claimed by the *wrong* entry, because from its side it looks
    claimed. Five consecutive entries drifted that way before this existed, each naming the
    commit before the one it described, and `--verify` reported a clean ledger throughout.

    The mechanism is mechanical, which is what makes it checkable: writing an entry for work
    still sitting in the working tree and reaching for its hash with `git log -1` returns HEAD,
    which is the *previous* commit. The signature is an entry whose own birth commit did
    substantive work that the entry does not claim.

    Deliberately SUSPECT, not ERROR. A commit may legitimately do code work for item A while
    filing the entry for item B, and failing the build on that would get the check switched off. Roadmap entries
    are exempt: they are plans, they carry no `commits`, and the work in their birth commit
    belongs to some other entry.

    KNOWN BLIND SPOT, and it is structural rather than an oversight. An entry born in a commit
    that changed no code is invisible here, because a docs-only work commit -- a measurement
    write-up, say -- is byte-for-byte the same shape as a record commit. Two of the five drifted
    entries were of that kind and this check does not see them; only reading the content does.
    It catches the subset that names the wrong *code*, which is the subset that misleads someone
    reading the entry to find the change.

    Positive control, because a linter that reports nothing is indistinguishable from one that
    does not run: against the ledger as of `c33153f` it flags five -- `fbfetch-coordinate-sized`,
    `fpscr-rounding-mode`, `ps-madd-double-rounding`, `fp-regression-coverage` and
    `self-dep-reproducer` -- and against the corrected ledger, none. Re-run that pair if you
    change the matcher.

    The fifth was found by the check itself rather than by the audit that prompted it: `ab78ce7`
    created `testing/graphics-tests/run.sh`, the reproducer's runner, while filing that entry and
    claiming the commit for `doc-coverage-audit` instead.
    """
    out: list[tuple[str, str, str]] = []
    origins = entry_origins()
    for bucket in ("items", "refuted"):
        for e in ledger.get(bucket, []):
            eid = e.get("id")
            origin = origins.get(eid)
            if not origin:
                continue  # new in the working tree; it has no birth commit yet
            work = [f for f in origin["files"] if _is_work_file(f)]
            if not work:
                continue  # the healthy shape: work landed, a record commit writes it down
            full = origin["commit"]
            if any(full.startswith(c) for c in e.get("commits", [])):
                continue
            shown = ", ".join(f"`{f}`" for f in sorted(work)[:3])
            more = f" +{len(work) - 3} more" if len(work) > 3 else ""
            out.append((SUSPECT, eid, f"born in `{full[:7]}`, which also changed {shown}{more} — but "
                                   f"the entry does not claim it. Check it is not naming "
                                   f"`{full[:7]}`'s parent by mistake"))
    return out


# ---------------------------------------------------------------------------- render

esc = html.escape


# --------------------------------------------------------------------------------------
# The linter.
#
# The two original checks (hashes resolve, commits claimed) catch a ledger that is broken
# or behind. They do not catch a ledger that is *wrong*, which is the failure mode this
# project actually keeps hitting: an entry that still points at a renamed section, a number
# quoted with no provenance, a claim that contradicts the doc it cites.
#
# Severity is tiered on one question: is this ALWAYS a mistake, or is some lag legitimate?
#
#   FATAL    the file is structurally broken. Never correct. Aborts.
#   ERROR    always an authoring mistake, and silently rots if unchecked. Fails CI.
#   SUSPECT  probably wrong, but has a legitimate shape. Needs a human to look, not a build
#            failure. Kept apart from WARN because "check this attribution" and "this number
#            is old" call for different actions, and filing the first under "legitimate lag,
#            not wrong, re-check when convenient" is how it gets skipped.
#   WARN     lag that can be legitimate -- an ageing measurement, an unclaimed tip. Reported.
#
# Nothing here re-derives a fact the repo already owns; these check that what IS typed
# stays true.
# --------------------------------------------------------------------------------------

FATAL, ERROR, SUSPECT, WARN = "FATAL", "ERROR", "SUSPECT", "WARN"

# Statuses are not interchangeable between buckets. `deferred` in items[] once hid a
# roadmap entry among landed work for weeks.
ITEM_STATUSES = {"landed", "refuted", "cancelled", "reverted"}
ROADMAP_STATUSES = {"open", "deferred", "blocked"}

# A verdict quoting a duration, a rate or a percentage is a measurement, and a measurement
# without provenance is exactly the thing `measured.at_commit` exists to prevent.
MEASUREMENT_RE = re.compile(r"\b\d[\d,]*\.?\d*\s*(?:ms|fps|%|GB/s|MB|ns)\b")

# How far behind HEAD a measurement may drift before it is worth re-running. Not fatal:
# a number does not become false because commits happened, it becomes *unverified*.
STALE_COMMITS = 40


def resolve_ref_path(raw: str) -> Path | None:
    """Find the file a `ref` names, trying the places refs are actually written."""
    for base in (REPO, REPO / "docs", REPO / "docs" / "porting", REPO / "docs" / "hardware",
                 REPO / "docs" / "status", REPO / "docs" / "testing"):
        p = base / raw
        if p.is_file():
            return p
    return None


def ref_section_found(path: Path, section: str) -> bool:
    """Does `section` still name something in `path`?

    Deliberately lenient about *form* and strict about *existence*: refs are written as
    "4.2" or as a heading fragment, and either is fine so long as it still resolves. What
    is not fine is naming a heading that no longer exists, which is how 12 refs silently
    went stale before this check existed.
    """
    text = path.read_text(errors="replace")
    headings = re.findall(r"^#{1,6}\s*(.+)$", text, re.M)
    number = re.match(r"([\d.]+)", section)
    if number and any(h.strip().startswith(number.group(1)) for h in headings):
        return True
    probe = section.lower()[:35]
    return any(probe in h.lower() for h in headings) or probe in text.lower()


# README only, deliberately. It is the public summary and should make no measured claim that is
# not traceable to the record; that is where "107 counters" against a real 113 and "183%" against a
# ledger that says 184 both sat, across several rewrites, in the most-read file in the repo.
#
# AGENTS.md is NOT checked, and adding it was tried and reverted. It is the working notebook: it
# carries per-counter tables and phase breakdowns the ledger deliberately does not, because a
# verdict there is one line and a pointer. Requiring every figure in it to appear verbatim in the
# ledger produced 35 warnings, every one of them working as intended -- and a check that is always
# noisy is a check nobody reads.
PROSE_DOCS = ("README.md",)

# Deliberately narrow: only shapes that are almost always a measurement. Version numbers, years
# and sizes in build instructions must not trip this, or the check gets ignored.
MEASUREMENT_SHAPES = re.compile(
    r"""(?<![\w.])(
        \d+(?:\.\d+)?\s?%                                   |  # 184%, 42.1%
        \d+(?:\.\d+)?\s?(?:fps|FPS)                         |  # 30.06 fps
        \d+(?:\.\d+)?\s?(?:ms|ns|µs|us)\b                   |  # 35.25 ms, 454.5 ns
        \d{1,3}(?:,\d{3})+                                     # 52,330
    )""",
    re.VERBOSE,
)

# Figures that are structural rather than measured, or that name a quantity the reader can verify
# from the text around them. Keep this list short: every entry is a claim nobody will re-check.
MEASUREMENT_ALLOWLIST = {
    "100%",   # used rhetorically ("100% of the diff is ours")
    "0%",
}

CODE_BLOCK = re.compile(r"```.*?```", re.DOTALL)
CODE_SPAN = re.compile(r"`[^`]*`")


def check_prose_measurements(ledger_text: str) -> list[tuple[str, str, str]]:
    """Every measurement quoted in prose must appear verbatim in the ledger.

    Not a numeric comparison -- correlating arbitrary prose to ledger entries is guesswork, and a
    guessing linter is one people learn to ignore. This asks a question with an exact answer: is
    this figure traceable to the record at all? "183%" was not, because the record says 184.

    Reported as WARN, not ERROR: prose legitimately carries context the ledger does not, and a
    check that fails the build on a legitimate sentence would be turned off within a week.
    """
    out: list[tuple[str, str, str]] = []
    for name in PROSE_DOCS:
        path = REPO / name
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Commands and identifiers are not claims.
        text = CODE_BLOCK.sub(" ", text)
        text = CODE_SPAN.sub(" ", text)

        seen: set[str] = set()
        for m in MEASUREMENT_SHAPES.finditer(text):
            fig = m.group(1).strip()
            if fig in seen or fig in MEASUREMENT_ALLOWLIST:
                continue
            seen.add(fig)
            # Compare with the space normalised out: prose writes "35.25 ms", the ledger may
            # write "35.25ms", and a whitespace difference is not a drift.
            needle = fig.replace(" ", "")
            haystack = ledger_text.replace(" ", "")
            if needle not in haystack:
                out.append((WARN, name, f"`{fig}` is not in the ledger — either source it, or "
                                        f"drop it. This is how `183%` survived three rewrites "
                                        f"against a ledger that says 184."))
    return out


def lint(ledger: dict, commits: list[dict]) -> list[tuple[str, str, str]]:
    """Return [(severity, entry-id, message)]. Pure; callers decide what to do with it."""
    out: list[tuple[str, str, str]] = []
    buckets = {b: ledger.get(b, []) for b in ("items", "refuted", "roadmap")}
    by_full = {c["full"]: c for c in commits}
    short_to_full = {c["short"]: c["full"] for c in commits}

    seen_ids: dict[str, str] = {}
    for bucket, entries in buckets.items():
        for e in entries:
            eid = e.get("id", "<no id>")

            if eid in seen_ids:
                out.append((FATAL, eid, f"duplicate id (also in `{seen_ids[eid]}`)"))
            seen_ids[eid] = bucket

            allowed = ROADMAP_STATUSES if bucket == "roadmap" else ITEM_STATUSES
            if e.get("status") not in allowed:
                out.append((FATAL, eid, f'status `{e.get("status")}` is not valid in `{bucket}` '
                                        f'(expected one of {sorted(allowed)})'))

            raw_ref = e.get("ref", "")
            if not raw_ref:
                out.append((ERROR, eid, "no `ref` — every entry must point at the doc that owns "
                                        "its reasoning"))
            else:
                parts = re.split(r"\s+[—–-]{1,2}\s+", raw_ref, maxsplit=1)
                path = resolve_ref_path(parts[0].strip())
                if path is None:
                    out.append((FATAL, eid, f"`ref` names a file that does not exist: "
                                            f"`{parts[0].strip()}`"))
                elif len(parts) == 2 and path.suffix == ".md":
                    if not ref_section_found(path, parts[1].strip()):
                        out.append((ERROR, eid, f"`ref` names a section that no longer exists: "
                                                f'"{parts[1].strip()}" in `{path.name}`'))

            measured = e.get("measured")
            if measured is None:
                # Only items[] and refuted[] MAKE measurements. A roadmap gate quoting a
                # number is citing one made elsewhere, and restamping it there would create
                # the second copy this ledger exists to avoid.
                if bucket != "roadmap" and MEASUREMENT_RE.search(e.get("verdict", "")):
                    out.append((ERROR, eid, "verdict quotes a measurement but the entry has no "
                                            "`measured` block — an unstamped number cannot be "
                                            "checked for staleness"))
            else:
                for key in ("at_commit", "date"):
                    if key not in measured:
                        out.append((ERROR, eid, f"`measured` is missing `{key}`"))
                at = measured.get("at_commit")
                if at:
                    full = short_to_full.get(at, at)
                    if full in by_full:
                        behind = len(commits) - 1 - commits.index(by_full[full])
                        if behind > STALE_COMMITS:
                            out.append((WARN, eid, f"measured at `{at}`, now {behind} commits "
                                                   f"behind HEAD — re-run before quoting"))

            if bucket == "roadmap" and not e.get("gate"):
                out.append((ERROR, eid, "roadmap entry has no `gate` — every open item must say "
                                        "what would justify starting it"))

    for stage in ledger.get("stages", []):
        sid = stage.get("id", "<no id>")
        claimed = [e for e in buckets["items"] if e.get("stage") == sid]
        if not claimed and stage.get("status") == "complete":
            out.append((WARN, sid, "stage is marked complete but no item claims it — its status "
                                   "is asserted rather than derived"))

    out.extend(check_prose_measurements(json.dumps(ledger, ensure_ascii=False)))
    out.extend(check_self_claim(ledger))
    return out


def rich(text: str) -> str:
    """Inline `code` and **bold** only. Escaped first, so the markup cannot inject."""
    s = esc(text or "")
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
    return s


def num(n: int) -> str:
    return f"{n:,}"


def commit_chips(shas: list[str], by_short: dict[str, dict]) -> str:
    out = []
    for sha in shas:
        info = by_short.get(sha)
        title = esc(info["subject"]) if info else "not a commit on this fork"
        cls = "sha" if info else "sha sha-foreign"
        out.append(f'<span class="{cls}" title="{title}">{esc(sha)}</span>')
    return " ".join(out)


def staleness(entry: dict, commits: list[dict], by_short: dict[str, dict]) -> str:
    """How much has landed since this was measured. Visible decay beats a silent one."""
    m = entry.get("measured")
    if not m:
        return ""
    bits = []
    if m.get("scene"):
        bits.append(f'<span class="k">scene</span> {esc(m["scene"])}')
    if m.get("n"):
        bits.append(f'<span class="k">n</span> {esc(str(m["n"]))}')
    at = m.get("at_commit")
    if at and at in by_short:
        behind = len(commits) - 1 - by_short[at]["index"]
        label = "at HEAD" if behind == 0 else f"{behind} commit{'s' if behind != 1 else ''} ago"
        cls = "age" + (" age-stale" if behind >= 25 else "")
        bits.append(
            f'<span class="k">measured</span> <span class="{cls}">{esc(at)} &middot; {label}</span>'
        )
    elif m.get("date"):
        bits.append(f'<span class="k">measured</span> {esc(m["date"])}')
    return '<div class="meta">' + " ".join(bits) + "</div>"


def item_card(entry: dict, commits: list[dict], by_short: dict[str, dict]) -> str:
    status = entry.get("status", "open")
    area = entry.get("area", "")
    haystack = " ".join(
        [entry.get("title", ""), entry.get("verdict", ""), entry.get("note", ""),
         area, status, " ".join(entry.get("commits", []))]
    ).lower()
    parts = [
        f'<article class="card" id="{esc(entry["id"])}" data-status="{esc(status)}" '
        f'data-area="{esc(area)}" data-text="{esc(haystack)}">',
        '<header class="card-head">',
        f'<span class="pill pill-{esc(status)}">{esc(status)}</span>',
        f'<h3><a class="anchor" href="#{esc(entry["id"])}">{rich(entry["title"])}</a></h3>',
        f'<span class="area">{esc(area)}</span>',
        "</header>",
    ]
    if entry.get("verdict"):
        parts.append(f'<p class="verdict">{rich(entry["verdict"])}</p>')
    if entry.get("note"):
        parts.append(f'<p class="note">{rich(entry["note"])}</p>')
    if entry.get("gate"):
        parts.append(
            f'<p class="gate"><span class="k">gate</span> {rich(entry["gate"])}</p>'
        )
    parts.append(staleness(entry, commits, by_short))
    footer = []
    if entry.get("commits"):
        footer.append(commit_chips(entry["commits"], by_short))
    if entry.get("ref"):
        footer.append(f'<span class="ref">{rich(entry["ref"])}</span>')
    if footer:
        parts.append('<footer class="card-foot">' + "".join(footer) + "</footer>")
    parts.append("</article>")
    return "".join(parts)


def table(columns: list[str], rows: list[list], align_right: set[int] | None = None) -> str:
    align_right = align_right or set()
    head = "".join(
        f'<th class="{"r" if i in align_right else ""}">{rich(str(c))}</th>'
        for i, c in enumerate(columns)
    )
    body = []
    for row in rows:
        cells = "".join(
            f'<td class="{"r" if i in align_right else ""}">{rich(str(c))}</td>'
            for i, c in enumerate(row)
        )
        body.append(f"<tr>{cells}</tr>")
    return (
        '<div class="scroll"><table><thead><tr>'
        + head
        + "</tr></thead><tbody>"
        + "".join(body)
        + "</tbody></table></div>"
    )


def section(sid: str, title: str, lead: str, body: str) -> str:
    lead_html = f'<p class="lead">{rich(lead)}</p>' if lead else ""
    return (
        f'<section id="{esc(sid)}"><h2><a class="anchor" href="#{esc(sid)}">'
        f"{esc(title)}</a></h2>{lead_html}{body}</section>"
    )


def stages_body(ledger: dict) -> str:
    """Goals, with completion DERIVED from which items claim each stage.

    The point is that a stage's status is a computed fact, not a sentence someone has to
    remember to update. The master plan once said "Stage 3: complete" while AGENTS.md still
    described one of its defects as open; deriving it removes the surface that can disagree.

    Emits its own markup rather than going through table(), because table() routes cells
    through rich(), which escapes first by design -- that injection guard is worth keeping.
    """
    items = ledger.get("items", [])
    rows = []
    for st in ledger.get("stages", []):
        mine = [e for e in items if e.get("stage") == st["id"]]
        landed = sum(1 for e in mine if e["status"] == "landed")
        pct = round(100 * landed / len(mine)) if mine else 0
        bar = (f'<div class="bar"><span style="width:{pct}%"></span></div>' if mine else "—")
        count = f"{landed}/{len(mine)}" if mine else "—"
        rows.append(
            "<tr>"
            f'<td><strong>{esc(st["title"])}</strong><br>'
            f'<span class="muted">{rich(st["goal"])}</span></td>'
            f'<td class="r">{count}</td>'
            f"<td>{bar}</td>"
            "</tr>"
        )
    unassigned = [e for e in items if "stage" not in e]
    tail = ""
    if unassigned:
        ids = ", ".join(f"<code>{esc(e['id'])}</code>" for e in unassigned)
        tail = (f'<p class="lead">{len(unassigned)} item(s) are cross-cutting and claim no '
                f"stage: {ids}.</p>")
    return (
        '<div class="scroll"><table><thead><tr>'
        "<th>stage</th><th class=\"r\">landed</th><th></th>"
        "</tr></thead><tbody>" + "".join(rows) + "</tbody></table></div>" + tail
    )


def render(ledger: dict, data: dict) -> str:
    commits = data["commits"]
    by_short = {c["short"]: c for c in commits}
    div = data["divergence"]
    ctr = data["counters"]
    base = data["baselines"]
    head = data["head"]
    items = ledger.get("items", [])
    unattributed = data["unattributed"]

    status_counts = Counter(i.get("status", "open") for i in items)

    # ---- masthead + at a glance
    dirty_badge = (
        '<span class="warn-badge" title="baseline.tsv or the counter .def had uncommitted '
        'changes when this page was built, so it quotes numbers that are not in the repo">'
        "built from uncommitted measurements</span>"
        if head["dirty"]
        else ""
    )
    stats = [
        ("commits since fork", num(len(commits)), f'from {esc(ledger["fork_point"])}'),
        ("files diverged", num(div["files"]), f'+{num(div["added"])} / -{num(div["removed"])}'),
        ("telemetry counters", num(ctr["total"]),
         " &middot; ".join(f"{esc(a)} {n}" for a, n in ctr["by_area"])),
        ("scene captures", num(len(base)), "testing/golden/baseline.tsv"),
        ("ledger items", num(len(items)),
         " &middot; ".join(f"{esc(s)} {status_counts[s]}" for s in STATUS_ORDER if status_counts[s])),
    ]
    stat_html = "".join(
        f'<div class="stat"><div class="stat-n">{v}</div>'
        f'<div class="stat-l">{esc(label)}</div><div class="stat-s">{sub}</div></div>'
        for label, v, sub in stats
    )

    # ---- headline scene numbers, from the ledger's curated tables
    headline = ""
    if ledger.get("headline"):
        cards = "".join(
            f'<div class="hl"><div class="hl-n">{rich(h["value"])}</div>'
            f'<div class="hl-l">{rich(h["label"])}</div>'
            f'<div class="hl-s">{rich(h.get("note", ""))}</div></div>'
            for h in ledger["headline"]
        )
        headline = f'<div class="hl-grid">{cards}</div>'

    # ---- divergence
    narrative = "".join(
        f'<div class="dcol"><h3>{esc(group["title"])}</h3><ul>'
        + "".join(f"<li>{rich(x)}</li>" for x in group["entries"])
        + "</ul></div>"
        for group in ledger.get("divergence", [])
    )
    area_rows = [
        [a["path"], num(a["files"]), f'+{num(a["added"])}', f'-{num(a["removed"])}']
        for a in div["areas"][:18]
    ]
    div_body = (
        f'<div class="dgrid">{narrative}</div>'
        + '<h3 class="sub">Where the lines actually moved</h3>'
        + f'<p class="lead">Top 18 paths by churn, from <code>git diff --numstat '
        f'{esc(ledger["fork_point"])}..HEAD</code>. Translation catalogues dominate by line count '
        "and mean almost nothing; <code>src/Cafe</code> is where the deletions are.</p>"
        + table(["path", "files", "added", "removed"], area_rows, {1, 2, 3})
    )

    # ---- ledger
    areas_present = [a for a in AREA_ORDER if any(i.get("area") == a for i in items)]
    filters = (
        '<div class="filters">'
        '<input id="q" type="search" placeholder="Search titles, verdicts, commits…" '
        'autocomplete="off" spellcheck="false">'
        '<div class="chips" id="chips-status"><span class="chip-label">status</span>'
        + "".join(
            f'<button class="chip chip-{esc(s)}" data-kind="status" data-value="{esc(s)}">'
            f'{esc(s)} <span class="chip-n">{status_counts[s]}</span></button>'
            for s in STATUS_ORDER
            if status_counts[s]
        )
        + "</div>"
        '<div class="chips" id="chips-area"><span class="chip-label">area</span>'
        + "".join(
            f'<button class="chip" data-kind="area" data-value="{esc(a)}">{esc(a)}</button>'
            for a in areas_present
        )
        + "</div>"
        '<button id="reset" class="reset" hidden>clear</button>'
        "</div>"
    )
    ordered = sorted(
        items,
        key=lambda i: max(
            (by_short[s]["index"] for s in i.get("commits", []) if s in by_short),
            default=-1,
        ),
        reverse=True,
    )
    cards = "".join(item_card(i, commits, by_short) for i in ordered)
    ledger_body = filters + f'<div class="cards" id="cards">{cards}</div>' + (
        '<p class="empty" id="empty" hidden>Nothing matches that filter.</p>'
    )

    # ---- unattributed
    unattr = ""
    if unattributed:
        rows = [[c["short"], c["date"], c["subject"]] for c in unattributed]
        unattr = section(
            "unattributed",
            f"Not in the ledger yet ({len(unattributed)})",
            "These commits are on the fork but no ledger item claims them. Add an entry to "
            "`docs/status/ledger.json`, or list the hash under `unattributed` if it is "
            "genuinely not a work item.",
            '<div class="warnbox">' + table(["commit", "date", "subject"], rows) + "</div>",
        )

    # ---- measurements
    tables = "".join(
        section_table(t) for t in ledger.get("tables", [])
    )
    base_rows = [
        [r["when"][:10], r["commit"], r["scene"], r["fps"], r["rss"], r["threads"], r["cpu"]]
        for r in reversed(base)
    ]
    measure_body = tables + (
        '<h3 class="sub">Every scene capture on record</h3>'
        '<p class="lead">Verbatim from <code>testing/golden/baseline.tsv</code>, newest first. '
        "Written by <code>testing/capture-scene.sh</code>; the PNGs and traces beside it are "
        "gitignored. Rows before <code>c03dbbb</code> predate the finding that a BotW run is two "
        "workloads, so a single fps figure for those rows describes a blend.</p>"
        + table(
            ["date", "commit", "scene", "fps", "RSS", "threads", "CPU"],
            base_rows,
            {3, 4, 5, 6},
        )
    )

    # ---- refuted
    refuted_cards = "".join(
        item_card(r, commits, by_short) for r in ledger.get("refuted", [])
    )
    refuted_body = f'<div class="cards">{refuted_cards}</div>'

    # ---- traps
    traps = "".join(
        f'<article class="trap"><h3>{rich(t["title"])}</h3><p>{rich(t["body"])}</p></article>'
        for t in ledger.get("traps", [])
    )

    # ---- roadmap
    road_cards = "".join(
        item_card(r, commits, by_short) for r in ledger.get("roadmap", [])
    )

    nav = "".join(
        f'<a href="#{esc(sid)}">{esc(label)}</a>'
        for sid, label in [
            ("glance", "At a glance"),
            ("divergence", "vs. upstream"),
            ("ledger", "Work ledger"),
            ("measurements", "Measurements"),
            ("refuted", "Refuted"),
            ("traps", "Traps"),
            ("roadmap", "Roadmap"),
        ]
    )

    body = "".join(
        [
            section("glance", "At a glance", ledger.get("summary", ""),
                    headline + f'<div class="stats">{stat_html}</div>'),
            section(
                "stages",
                "Goals",
                "The staged plan, with completion **derived from the ledger** rather than "
                "asserted — a stage is as done as the items that claim it. Reasoning lives in "
                "`docs/porting/00-master-plan.md`; this is only the status.",
                stages_body(ledger),
            ),
            section(
                "divergence",
                "Divergence from upstream Cemu",
                f'Fork point is `{ledger["fork_point"]}` — the last upstream commit merged '
                "before the retarget began. Everything below is what happened after it.",
                div_body,
            ),
            section(
                "ledger",
                "Work ledger",
                "Every item attempted on this fork, including the ones that did not work. "
                "**`refuted` means the hypothesis was tested against a control and was false**; "
                "`cancelled` means a gate killed it before it was built; `reverted` means it was "
                "built, measured, and removed with only the finding kept. Those three are the "
                "majority of the graphics work and are the most expensive knowledge here to "
                "rediscover.",
                ledger_body,
            ),
            unattr,
            section(
                "measurements",
                "Measurements",
                "Numbers below are read from the repo or copied from the commit that measured "
                "them. Each carries the commit it was taken at, so staleness is visible rather "
                "than assumed.",
                measure_body,
            ),
            section(
                "refuted",
                "Refuted — do not re-raise",
                "Each of these was a plausible idea, was implemented or instrumented, and was "
                "killed by its own measurement. They are listed so the next person does not pay "
                "for them twice.",
                refuted_body,
            ),
            section(
                "traps",
                "Measurement traps",
                "Ways this project has already produced confident, wrong numbers.",
                f'<div class="traps">{traps}</div>',
            ),
            section(
                "roadmap",
                "Roadmap",
                "Open work, each with the condition that would justify starting it. A gate that "
                "has not been met is a reason not to begin, not a formality.",
                f'<div class="cards">{road_cards}</div>',
            ),
        ]
    )

    gen_note = (
        f'generated from <code>{esc(head["short"])}</code> on '
        f'<code>{esc(head["branch"])}</code>, {esc(head["date"])} {dirty_badge}'
    )

    # str.format is unusable here: CSS and JS are full of braces. Token replacement instead.
    fields = {
        "CSS": CSS,
        "JS": JS,
        "TITLE": esc(ledger.get("title", "TesseraEmu — status")),
        "TAGLINE": rich(ledger.get("tagline", "")),
        "NAV": nav,
        "GEN": gen_note,
        "BODY": body,
        "REGEN": esc("python3 docs/status/build-status.py"),
    }
    page = PAGE
    for key, value in fields.items():
        page = page.replace("<!--%" + key + "%-->", value)
    return page


def numeric_columns(columns: list[str], rows: list[list]) -> set[int]:
    """Right-align a column only when every cell in it is short.

    Blanket right-alignment looked fine on the counter tables and terrible on the
    accuracy-lever table, whose last two columns are sentences.
    """
    right = set()
    for i in range(1, len(columns)):
        cells = [str(r[i]) for r in rows if i < len(r)]
        if cells and all(len(re.sub(r"[*`]", "", c)) <= 16 for c in cells):
            right.add(i)
    return right


def section_table(t: dict) -> str:
    lead = f'<p class="lead">{rich(t["note"])}</p>' if t.get("note") else ""
    right = numeric_columns(t["columns"], t["rows"])
    return (
        f'<h3 class="sub" id="{esc(t["id"])}">{rich(t["title"])}</h3>'
        + lead
        + table(t["columns"], t["rows"], right)
    )


# ------------------------------------------------------------------------- page shell

CSS = """
:root{
  --bg:#fbfbfa; --panel:#fff; --ink:#1a1a19; --dim:#6b6b66; --line:#e3e2dd;
  --accent:#8a5a2b; --code-bg:#f2f1ec;
  --landed:#2f7d4f; --refuted:#b3701a; --cancelled:#6b6b66; --reverted:#7a4fa3;
  --open:#2b6cb0; --deferred:#5a6b7d; --blocked:#b3352b; --warn:#b3701a;
  --bar:#2f855a;
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme=light]){
    --bg:#141413; --panel:#1c1c1a; --ink:#e8e6e1; --dim:#9a978f; --line:#2f2e2b;
    --accent:#d4a373; --code-bg:#232320;
    --landed:#6cc08b; --refuted:#e0a458; --cancelled:#9a978f; --reverted:#b48ed6;
    --open:#7aaee0; --deferred:#93a5b8; --blocked:#e08b7f; --warn:#e0a458;
  }
}
:root[data-theme=dark]{
  --bg:#141413; --panel:#1c1c1a; --ink:#e8e6e1; --dim:#9a978f; --line:#2f2e2b;
  --accent:#d4a373; --code-bg:#232320;
  --landed:#6cc08b; --refuted:#e0a458; --cancelled:#9a978f; --reverted:#b48ed6;
  --open:#7aaee0; --deferred:#93a5b8; --blocked:#e08b7f; --warn:#e0a458;
}
*{box-sizing:border-box}
body{
  margin:0; background:var(--bg); color:var(--ink);
  font:15px/1.6 ui-sans-serif,-apple-system,"Helvetica Neue",Arial,sans-serif;
  -webkit-font-smoothing:antialiased;
}
code,.sha,.stat-n,.hl-n,td.r,th.r{
  font-family:ui-monospace,"SF Mono",Menlo,monospace;
}
code{background:var(--code-bg); padding:.1em .35em; border-radius:3px; font-size:.88em}
a{color:inherit}
.wrap{max-width:1080px; margin:0 auto; padding:0 24px 96px}

header.top{border-bottom:1px solid var(--line); background:var(--panel); margin-bottom:40px}
.top-in{max-width:1080px; margin:0 auto; padding:36px 24px 0}
h1{font-size:30px; letter-spacing:-.02em; margin:0 0 6px}
.tagline{color:var(--dim); margin:0 0 14px; max-width:70ch}
.gen{color:var(--dim); font-size:13px; margin:0 0 18px}
.bar{background:rgba(128,128,128,.22);border-radius:3px;height:8px;width:120px;overflow:hidden}
.bar span{display:block;height:100%;background:var(--bar)}
.warn-badge{
  background:var(--warn); color:var(--bg); border-radius:3px;
  padding:.1em .45em; font-size:11px; text-transform:uppercase; letter-spacing:.04em;
  margin-left:6px;
}
nav{display:flex; gap:18px; flex-wrap:wrap; padding-bottom:2px}
nav a{
  color:var(--dim); text-decoration:none; font-size:13px; padding:8px 0;
  border-bottom:2px solid transparent;
}
nav a:hover{color:var(--ink); border-bottom-color:var(--accent)}
#theme{
  position:absolute; top:36px; right:24px; background:none; border:1px solid var(--line);
  color:var(--dim); border-radius:5px; padding:5px 9px; cursor:pointer; font-size:12px;
}
.top-in{position:relative}

section{margin:0 0 56px; scroll-margin-top:16px}
h2{font-size:20px; letter-spacing:-.01em; margin:0 0 8px; padding-bottom:8px;
   border-bottom:1px solid var(--line)}
h3.sub{font-size:14px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim);
   margin:32px 0 8px}
.anchor{text-decoration:none}
.anchor:hover{color:var(--accent)}
.lead{color:var(--dim); max-width:78ch; margin:0 0 16px}

.hl-grid{display:grid; grid-template-columns:repeat(auto-fit,minmax(190px,1fr)); gap:12px;
  margin-bottom:18px}
.hl{background:var(--panel); border:1px solid var(--line); border-left:3px solid var(--accent);
  border-radius:6px; padding:14px 16px}
.hl-n{font-size:21px; font-weight:600; letter-spacing:-.02em}
.hl-l{font-size:13px; margin-top:2px}
.hl-s{font-size:12px; color:var(--dim); margin-top:4px}

.stats{display:grid; grid-template-columns:repeat(auto-fit,minmax(170px,1fr)); gap:12px}
.stat{background:var(--panel); border:1px solid var(--line); border-radius:6px; padding:14px 16px}
.stat-n{font-size:22px; font-weight:600; letter-spacing:-.02em}
.stat-l{font-size:13px; margin-top:2px}
.stat-s{font-size:11.5px; color:var(--dim); margin-top:4px; line-height:1.45}

.dgrid{display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr)); gap:20px}
.dcol h3{font-size:13px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim);
  margin:0 0 8px}
.dcol ul{margin:0; padding-left:18px}
.dcol li{margin-bottom:7px}

.filters{display:flex; flex-direction:column; gap:10px; margin-bottom:18px;
  padding:14px 16px; background:var(--panel); border:1px solid var(--line); border-radius:6px}
#q{width:100%; padding:9px 11px; border:1px solid var(--line); border-radius:5px;
  background:var(--bg); color:var(--ink); font:inherit; font-size:14px}
#q:focus{outline:2px solid var(--accent); outline-offset:-1px}
.chips{display:flex; gap:6px; flex-wrap:wrap; align-items:center}
.chip-label{font-size:11px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim);
  width:52px}
.chip{background:var(--bg); border:1px solid var(--line); color:var(--dim); cursor:pointer;
  border-radius:20px; padding:4px 11px; font:inherit; font-size:12.5px}
.chip:hover{color:var(--ink)}
.chip[aria-pressed=true]{background:var(--ink); color:var(--bg); border-color:var(--ink)}
.chip-n{opacity:.6; font-size:11px}
.reset{align-self:flex-start; background:none; border:none; color:var(--accent); cursor:pointer;
  font:inherit; font-size:12.5px; padding:0; text-decoration:underline}

.cards{display:flex; flex-direction:column; gap:10px}
.card{background:var(--panel); border:1px solid var(--line); border-radius:6px;
  padding:14px 16px; border-left:3px solid var(--cancelled)}
.card[data-status=landed]{border-left-color:var(--landed)}
.card[data-status=refuted]{border-left-color:var(--refuted)}
.card[data-status=reverted]{border-left-color:var(--reverted)}
.card[data-status=open]{border-left-color:var(--open)}
.card[data-status=deferred]{border-left-color:var(--deferred)}
.card[data-status=blocked]{border-left-color:var(--blocked)}
.card-head{display:flex; align-items:baseline; gap:10px; flex-wrap:wrap}
.card-head h3{font-size:15.5px; margin:0; font-weight:600; flex:1 1 320px; letter-spacing:-.01em}
.pill{font-size:10.5px; text-transform:uppercase; letter-spacing:.05em; border-radius:3px;
  padding:.2em .5em; color:var(--bg); background:var(--cancelled); white-space:nowrap}
.pill-landed{background:var(--landed)} .pill-refuted{background:var(--refuted)}
.pill-reverted{background:var(--reverted)} .pill-open{background:var(--open)}
.pill-deferred{background:var(--deferred)} .pill-blocked{background:var(--blocked)}
.area{font-size:11.5px; color:var(--dim); text-transform:uppercase; letter-spacing:.05em}
.verdict{margin:8px 0 0}
.note,.gate{margin:7px 0 0; color:var(--dim); font-size:13.5px}
.gate{border-left:2px solid var(--line); padding-left:10px}
.k{font-size:10.5px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim);
  margin-right:2px}
.meta{margin-top:8px; font-size:12px; color:var(--dim); display:flex; gap:14px; flex-wrap:wrap}
.age-stale{color:var(--warn)}
.card-foot{margin-top:10px; padding-top:9px; border-top:1px solid var(--line);
  display:flex; gap:8px; flex-wrap:wrap; align-items:center; font-size:12px}
.sha{background:var(--code-bg); border-radius:3px; padding:.15em .4em; font-size:11.5px;
  color:var(--dim); cursor:help}
.sha-foreign{border:1px dashed var(--line)}
.ref{color:var(--dim); font-size:12px; margin-left:auto}
.empty{color:var(--dim); padding:24px 0}

.traps{display:grid; grid-template-columns:repeat(auto-fit,minmax(300px,1fr)); gap:12px}
.trap{background:var(--panel); border:1px solid var(--line); border-radius:6px; padding:14px 16px}
.trap h3{font-size:14.5px; margin:0 0 6px}
.trap p{margin:0; color:var(--dim); font-size:13.5px}

.warnbox{border:1px solid var(--warn); border-radius:6px; padding:4px 12px; background:var(--panel)}

.scroll{overflow-x:auto; border:1px solid var(--line); border-radius:6px; background:var(--panel)}
table{border-collapse:collapse; width:100%; min-width:100%; font-size:13px}
th,td{text-align:left; padding:8px 12px; border-bottom:1px solid var(--line); vertical-align:top}
th{font-size:11px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim);
   font-weight:600}
tbody tr:last-child td{border-bottom:none}
tbody tr:hover{background:var(--code-bg)}
/* Only numeric columns are nowrap; prose columns must wrap or the page scrolls sideways. */
td.r,th.r{text-align:right; white-space:nowrap}

footer.end{color:var(--dim); font-size:12.5px; border-top:1px solid var(--line); padding-top:16px}
@media(max-width:640px){
  h1{font-size:24px} .wrap,.top-in{padding-left:16px; padding-right:16px}
  #theme{top:16px; right:16px} .ref{margin-left:0}
}
"""

JS = """
(function(){
  var root=document.documentElement, btn=document.getElementById('theme');
  try{var t=localStorage.getItem('tessera-theme'); if(t) root.setAttribute('data-theme',t);}catch(e){}
  btn.addEventListener('click',function(){
    var cur=root.getAttribute('data-theme');
    if(!cur) cur=matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light';
    var next=cur==='dark'?'light':'dark';
    root.setAttribute('data-theme',next);
    try{localStorage.setItem('tessera-theme',next);}catch(e){}
  });

  var q=document.getElementById('q'), reset=document.getElementById('reset');
  var cards=Array.prototype.slice.call(document.querySelectorAll('#cards .card'));
  var chips=Array.prototype.slice.call(document.querySelectorAll('.chip'));
  var empty=document.getElementById('empty');
  var active={status:new Set(),area:new Set()};

  function apply(){
    var term=(q.value||'').trim().toLowerCase();
    var shown=0;
    cards.forEach(function(c){
      var ok=true;
      if(active.status.size && !active.status.has(c.dataset.status)) ok=false;
      if(ok && active.area.size && !active.area.has(c.dataset.area)) ok=false;
      if(ok && term && c.dataset.text.indexOf(term)===-1) ok=false;
      c.hidden=!ok; if(ok) shown++;
    });
    empty.hidden=shown!==0;
    reset.hidden=!(term||active.status.size||active.area.size);
  }
  chips.forEach(function(ch){
    ch.setAttribute('aria-pressed','false');
    ch.addEventListener('click',function(){
      var set=active[ch.dataset.kind], v=ch.dataset.value;
      if(set.has(v)){set.delete(v); ch.setAttribute('aria-pressed','false');}
      else{set.add(v); ch.setAttribute('aria-pressed','true');}
      apply();
    });
  });
  q.addEventListener('input',apply);
  reset.addEventListener('click',function(){
    q.value=''; active.status.clear(); active.area.clear();
    chips.forEach(function(c){c.setAttribute('aria-pressed','false');});
    apply();
  });
})();
"""

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title><!--%TITLE%--></title>
<style><!--%CSS%--></style>
</head>
<body>
<header class="top"><div class="top-in">
<button id="theme" type="button">theme</button>
<h1><!--%TITLE%--></h1>
<p class="tagline"><!--%TAGLINE%--></p>
<p class="gen"><!--%GEN%--></p>
<nav><!--%NAV%--></nav>
</div></header>
<div class="wrap">
<!--%BODY%-->
<footer class="end">
Generated by <code>docs/status/build-status.py</code> from <code>docs/status/ledger.json</code>
plus git, <code>testing/golden/baseline.tsv</code> and
<code>src/Cemu/Telemetry/TelemetryCounters.def</code>. Regenerate with <code><!--%REGEN%--></code>.
Verdicts are summaries &mdash; <code>docs/porting/00-master-plan.md</code> owns the reasoning.
</footer>
</div>
<script><!--%JS%--></script>
</body>
</html>
"""


# ------------------------------------------------------------------------------- main


def collect() -> tuple[dict, dict]:
    """Load, validate, and gather every derived source. Raises Abort on anything malformed."""
    try:
        ledger = json.loads(LEDGER_PATH.read_text())
    except FileNotFoundError:
        raise Abort(f"{LEDGER_PATH} does not exist")
    except json.JSONDecodeError as e:
        raise Abort(f"{LEDGER_PATH} is not valid JSON: {e}")
    check_hashes_resolve(ledger)
    commits = fork_commits(ledger["fork_point"])
    if not commits:
        raise Abort(
            f'no commits between {ledger["fork_point"]} and HEAD -- is fork_point correct?'
        )
    data = {
        "head": head_state(),
        "commits": commits,
        "divergence": divergence(ledger["fork_point"]),
        "baselines": baselines(),
        "counters": counters(),
        "unattributed": check_coverage(ledger, commits),
    }
    return ledger, data


def build() -> str:
    ledger, data = collect()
    return render(ledger, data)


def verify_report(ledger: dict, data: dict) -> str:
    """Markdown drift report. Written for a GitHub job summary; readable in a terminal too.

    The tip commit is reported SEPARATELY and is not drift. A ledger entry cannot name the hash
    of the commit that contains it, so the newest commit is always unclaimed at the moment CI
    sees it. Lumping it in with real drift would make every single push report "1 unattributed"
    and the signal would be worth nothing.
    """
    commits = data["commits"]
    unattributed = data["unattributed"]
    tip = commits[-1]

    hashes = {
        sha
        for bucket in ("items", "refuted", "roadmap")
        for entry in ledger.get(bucket, [])
        for sha in entry.get("commits", [])
    }
    older = [c for c in unattributed if c["full"] != tip["full"]]
    tip_unclaimed = any(c["full"] == tip["full"] for c in unattributed)

    out = [
        "## Status tracker",
        "",
        f'`docs/status/ledger.json` — {len(ledger.get("items", []))} items, '
        f'{len(ledger.get("refuted", []))} refuted, {len(ledger.get("roadmap", []))} roadmap. '
        f"All {len(hashes)} commit hashes resolve.",
        "",
        f'{len(commits)} commits since the fork point `{ledger["fork_point"]}`.',
        "",
    ]

    if older:
        out += [
            f"### ⚠️ {len(older)} commit(s) landed without a ledger entry",
            "",
            "| commit | date | subject |",
            "| --- | --- | --- |",
        ]
        out += [f'| `{c["short"]}` | {c["date"]} | {c["subject"]} |' for c in older]
        out += [
            "",
            "Add an entry to `docs/status/ledger.json` naming these commits, or list a hash "
            "under `unattributed` if it is genuinely not a work item. Then regenerate with "
            "`python3 docs/status/build-status.py`.",
            "",
        ]
    else:
        out += ["Every commit older than the tip is accounted for. ✅", ""]

    if tip_unclaimed:
        out += [
            f'The tip commit `{tip["short"]}` is unclaimed, which is expected — a commit cannot '
            "name its own hash in the ledger it contains. The next ledger update should claim it.",
            "",
        ]

    findings = lint(ledger, commits)
    for sev, label, blurb in (
        (FATAL, "🛑 Structural errors — the ledger is malformed",
         "These are never correct. Fix before anything else."),
        (ERROR, "❌ Drift — an entry no longer matches the repo",
         "Always an authoring mistake, and it rots silently if unchecked. Fails CI."),
        (SUSPECT, "🔎 Suspect attribution — an entry may name the wrong commit",
         "Each of these was born in a commit that changed code it does not claim. That is the "
         "signature of reaching for a hash with `git log -1` while the work was still "
         "uncommitted, which returns the *previous* commit. Sometimes legitimate: one commit can "
         "do work for one item while filing another's entry. Check each."),
        (WARN, "⚠️ Ageing — legitimate lag, worth a look",
         "Not wrong, just unverified. Re-run or re-check when convenient."),
    ):
        rows = [f for f in findings if f[0] == sev]
        if not rows:
            continue
        out += [f"### {label} ({len(rows)})", "", blurb, "",
                "| entry | problem |", "| --- | --- |"]
        out += [f"| `{eid}` | {msg} |" for _, eid, msg in rows]
        out += [""]
    if not findings:
        out += ["No structural, drift or staleness findings. ✅", ""]
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    ap.add_argument(
        "--check",
        action="store_true",
        help="local pre-commit check: exit 1 if index.html does not match a fresh render at "
        "the CURRENT HEAD. Catches 'I edited the ledger and forgot to regenerate'. Cannot be "
        "used as a CI gate -- see --verify.",
    )
    ap.add_argument(
        "--verify",
        action="store_true",
        help="validate the ledger and report drift as Markdown; writes nothing. Exit 2 if the "
        "ledger is malformed, 1 if an entry has drifted out of sync with the repo, 0 "
        "otherwise (unclaimed commits and ageing measurements are reported, not fatal).",
    )
    args = ap.parse_args()

    if args.verify:
        try:
            ledger, data = collect()
        except Abort as e:
            print(f"build-status: {e}", file=sys.stderr)
            return 2
        print(verify_report(ledger, data))
        findings = lint(ledger, data["commits"])
        if any(f[0] == FATAL for f in findings):
            return 2
        # Drift fails CI; unclaimed commits and ageing measurements deliberately do not,
        # because both have legitimate causes (see the module docstring).
        return 1 if any(f[0] == ERROR for f in findings) else 0

    try:
        page = build()
    except Abort as e:
        print(f"build-status: {e}", file=sys.stderr)
        return 2

    if args.check:
        if not args.output.exists():
            print(f"build-status: {args.output} does not exist", file=sys.stderr)
            return 1
        if args.output.read_text() != page:
            print(
                f"build-status: {args.output} is stale -- run docs/status/build-status.py",
                file=sys.stderr,
            )
            return 1
        print(f"build-status: {args.output} is current")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(page)
    print(f"build-status: wrote {args.output} ({len(page):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
