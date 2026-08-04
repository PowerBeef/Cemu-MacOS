# Rule: keep the status tracker current

`docs/status/index.html` is the fork's live record — every item attempted since the fork point and
what it measured. It is generated, committed, and **maintained as work happens, not reconstructed
afterwards.** This file is the standing obligation; `CLAUDE.md` points here and restates the
one-sentence version of it.

> This is a project-level `.claude/rules/` file and may not be auto-loaded the way `CLAUDE.md` is.
> That is why `CLAUDE.md` carries the obligation too. If you are reading this at all, follow it.

## The obligation

**When a work item lands, add an entry to `docs/status/ledger.json` naming its commits, then run
`python3 docs/status/build-status.py` and commit the regenerated `index.html` alongside it.**

That is the whole rule. The rest of this file is how to do it without making the record worse.

## A negative result is a first-class entry

Most of this fork's graphics work did not pan out, and *how* it failed is the valuable part. Pick the
status that is actually true:

| status | means |
|---|---|
| `landed` | shipped and measured |
| `refuted` | the hypothesis was tested against a control and was **false** |
| `cancelled` | a gate killed it **before** it was built |
| `reverted` | built, measured, removed — only the finding kept |
| `open` / `deferred` / `blocked` | roadmap; each must carry a `gate` saying what would justify starting |

Deleting a failed attempt from the record, or quietly filing it as `open`, is how the next person
pays for it a second time. Six attempts at the same 1.90 ms are on file for exactly this reason.

## Documentation and upkeep are work items

A commit that only touches docs still lands and still gets claimed. `area: "docs"` exists for exactly
this and already carries the README, the hardware reference and the documentation audits. The
tracker's *own* upkeep, meaning claiming commits, stamping measurements and correcting a wrong entry,
is claimed by `status-tracker`.

`unattributed` is for commits that genuinely are not work: a merge, a typo, a `.gitignore` line. It is
**not** an escape hatch for "this measured nothing." Most documentation commits measure nothing and
are still part of the record.

This is written down because the judgement was once made wrongly, in a session where six `area: docs`
items were already on file: a README rewrite was declared "not a work item" and left unclaimed. Look
for precedent in the ledger before deciding something is out of scope.

## Never type a number the repo already knows

The generator derives the commit list, the diffstat, every `testing/golden/baseline.tsv` row and the
telemetry counter totals **from the repo**. Typing any of those into the ledger reintroduces the
failure this page exists to prevent — `938a472` corrected fourteen hand-typed claims in one pass, and
the README said "107 counters" when there were 113.

If a fact lives in a file, cite the file. If you need a new derived fact on the page, teach
`build-status.py` to read it; do not paste it into `ledger.json`.

### The same rule now applies outward, to `README.md`

This discipline protected the ledger and left the most-read file in the repo with nothing at all,
which is exactly where the errors landed: **"107 counters" against a real 113, and "183%" against a
ledger that says 184** — the latter surviving three rewrites, because each one copied the previous.

`--verify` now requires every measurement in `README.md` to appear **verbatim in the ledger**. Not a
numeric comparison: correlating arbitrary prose to entries is guesswork, and a guessing linter is one
people learn to ignore. It asks a question with an exact answer — is this figure traceable to the
record at all? `WARN`, because prose legitimately carries context the ledger does not, and a check
that fails the build on a legitimate sentence gets switched off within a week.

**Only README.** Adding `CLAUDE.md` was tried and reverted: it carries per-counter tables and phase
breakdowns the ledger deliberately does not, since a verdict here is one line and a pointer. It
produced 35 warnings, every one working as intended.

The check has a **positive control**, because a linter that reports nothing is indistinguishable from
one that does not run. Against the README as of `584521b` it flags 8 figures, `183%` among them.
If you change the matcher, re-run that control.

### And a class the check cannot catch

A claim about *another project* rots when **they** change, and nothing here will ever notice. The
README described Cemu as reaching Metal through MoltenVK for four rewrites, long after upstream
merged the native Metal backend this fork is built on. So **name the version, date or commit you
checked, or do not make the claim.**

## A verdict is one line and a pointer

`docs/porting/00-master-plan.md` owns the reasoning. A ledger entry carries the *conclusion* plus a
`ref` to the section that argues it. Do not restate the argument — a second copy is a second thing to
keep in sync, and they will disagree.

Every measured claim gets `measured: { scene, at_commit, date, n }`. `at_commit` is what lets the
page show how far behind HEAD a measurement has fallen; without it a stale number looks fresh.

## Commands

```sh
python3 docs/status/build-status.py            # regenerate index.html
python3 docs/status/build-status.py --verify   # validate + report drift (what CI runs)
python3 docs/status/build-status.py --check    # local: did I forget to regenerate?
```

## What `--verify` enforces, and why the tiers differ

The question behind every severity is: **is this always a mistake, or is some lag legitimate?**

**Exit 2 — structurally broken.** Never correct. A hash that does not resolve, a duplicate `id`, a
`status` that is not legal for its array (`deferred` in `items[]` once hid a roadmap entry among
landed work), or a `ref` naming a file that does not exist.

**Exit 1 — drifted.** Also always a mistake, but it rots *silently*, which is why it fails CI rather
than being reported:

- **a `ref` naming a section that no longer exists.** Twelve of eighty-four refs had gone stale
  before this check existed, because `ref` was written as a paraphrase of a heading rather than as an
  anchor. **Write the ref so it still matches the heading**, or the check will tell you it does not.
- **a verdict quoting a measurement with no `measured` block.** `at_commit` is what lets the page
  show how far a number has drifted; an unstamped number cannot be checked at all. Roadmap entries
  are exempt — a gate *citing* a measurement made elsewhere should point at it, not restate it.
- **a roadmap entry with no `gate`.**

**Exit 0 — reported, tolerated.** Unclaimed commits (the tip is always unclaimed; that is
self-reference, not sloppiness) and measurements more than 40 commits behind HEAD. A number does not
become false because commits happened — it becomes *unverified*.

## Goals are derived, not asserted

`stages[]` carries the staged plan; items claim a stage with `"stage": "s3"`. The page computes how
much of each stage landed. **Do not hand-write a stage's completion** — that is exactly the surface
that drifted before, when the master plan said "Stage 3: complete" while CLAUDE.md still described
one of its defects as open. An item that belongs to no stage is fine and is listed as cross-cutting.

## Two things that are structural, not sloppiness

**A commit cannot name its own hash** in the ledger it contains, so the newest commit is always
unclaimed when CI sees it. `--verify` reports the tip separately from real drift for this reason. The
next ledger update claims it.

**`--check` cannot gate CI.** The page embeds `generated from <HEAD>` and `commits since fork: N`,
both of which change with the very commit carrying the HTML, so the committed page is always one
commit behind by construction. `--check` is a local pre-commit convenience only. This is why CI runs
`--verify` instead, and why it advises rather than blocks.
