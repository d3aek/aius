---
title: Aius project scale
type: reference
scope: component
component: aius
status: current
tags: [aius, measurement, provenance]
updated: 2026-09-03
toc: true
---

# Aius project scale

This records the measured size of the Aius subtree and the method behind each
figure. Everything below was measured on 2026-08-05 against repository HEAD
`99eba92f5`, over the path `src/hardware/aius`. Where a count is known to be a
floor rather than a total, that is stated at the figure and again under
[limitations](#limitations).

## Measurement basis

- **Path** — every repository figure is scoped to `src/hardware/aius` unless the
  command says otherwise.
- **Commit pin** — HEAD `99eba92f5`. The repository is live; re-running any
  command later returns different numbers.
- **Line counts** — physical lines, including blanks and comments. No SLOC
  normalisation.
- **Commands** — run from the repository root, with `TZ=UTC` wherever a date or
  hour enters the result.

## Timeline

First subtree commit `8be0f3228`, 2026-07-15 23:44:32 UTC,
`docs(aius): establish capability and hardware plans`. Working activity starts
earlier than that: the first Codex session touching the subtree is
2026-07-13, so the git history understates the span by two days (see
[agent cost](#agent-cost)).

| Measure | Value |
|---|---|
| Commits touching the subtree | 683 |
| Span, first to last commit | 20.35 days |
| Calendar days with a commit | 21 of 22 |
| Commits 00:00–06:59 UTC | 180 (26.4%) |
| Days with both a pre-07:00 and a post-19:00 commit | 15 |
| Longest gap between consecutive commits | 59.8 h |
| Commit body text | 102,487 words |
| Mean commit body | 150 words |

Commits land in all 24 hours; 2026-07-16 alone spans all 24.
Busiest days by commit count: 07-16 (99), 07-24 (79), 07-25 (62), 07-22 (56).

```bash
git log --reverse --format='%h %ad %s' --date=iso-strict -- src/hardware/aius | head -1
git log --oneline -- src/hardware/aius | wc -l
TZ=UTC git log --format='%ad' --date=format:'%Y-%m-%d %H' -- src/hardware/aius | sort | uniq -c
git log --format='%b' -- src/hardware/aius | wc -w
```

## Code

First-party firmware C and C++ is 109,081 lines across 201 files.

| Area | Lines | Files |
|---|---|---|
| `firmware/test` | 56,357 | 61 |
| `firmware/lib` | 28,385 | 87 |
| `firmware/src` | 17,414 | 26 |
| `firmware/tools` | 3,325 | 14 |
| `evidence/**` | 3,600 | 13 |
| Total | 109,081 | 201 |

`firmware/lib` holds 26 library modules. Other first-party languages:

- **Python** — 30,400 lines across 62 files.
- **JavaScript** — 12,630 lines across 9 files.
- **Shell** — 2,673 lines across 19 files.

```bash
find firmware/src \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) | xargs wc -l | tail -1
```

The same form was run per area, and per extension for Python, JavaScript and
shell.

### Vendored code

A further 53,066 lines of C and C++ across 136 files sit under
`firmware/vendor`. **This is third-party code, not first-party authorship.**
It is excluded from every figure in the table above and from the churn rows
below that say "excluding vendored".

| Vendored component | Lines |
|---|---|
| `ffmpeg-eac3` | 31,829 |
| `dcadec-core` | 17,432 |
| `NativeEthernet` | 2,724 |
| `tweetnacl` | 1,081 |
| Total | 53,066 |

## Documentation

205,001 words across 29 markdown files.

| Location | Words | Files |
|---|---|---|
| `docs/**` | 197,411 | 25 |
| `firmware/README.md` | 5,443 | 1 |
| Root `README.md` + `CLAUDE.md` | 1,297 | 2 |
| `electronics/README.md` | 850 | 1 |
| Total | 205,001 | 29 |

```bash
find src/hardware/aius -name '*.md' -not -path '*/vendor/*' | xargs wc -w | tail -1
```

## Tests

1,163 test cases across 48 suite directories, counted by pattern match against
the source rather than by running the suites.

| Language | Cases | Pattern |
|---|---|---|
| C/C++ | 510 | `^\s*(static\s+)?void\s+test_\w+\s*\(` |
| Python | 358 | `^\s*def\s+test_\w+\s*\(` |
| JavaScript | 295 | vitest `it(` / `test(` |

The JavaScript cases sit inside 42 `describe(` blocks. Largest suites by case
count: `latency-runner` 114, `dsp` 80, `recovery-runner` 71, `ota` 59, `http`
53.

**1,163 is a floor.** 15 of the 48 suite directories return zero under these
patterns because they hold fixture and vector corpora rather than case
functions.

```bash
grep -rEc '^\s*(static\s+)?void\s+test_\w+\s*\(' firmware/test
grep -rEc '^\s*def\s+test_\w+\s*\(' src/hardware/aius --include='*.py'
grep -rEc '\b(it|test)\(' src/hardware/aius --include='*.test.js'
```

## Churn

Totals over the whole subtree history: 565,135 insertions, 185,366 deletions,
4,551 file-changes, touching 1,571 distinct files.

| Basis | Insertions |
|---|---|
| All subtree paths | 565,135 |
| Excluding vendored | 506,838 |
| Excluding vendored and `electronics/` | 364,417 |

**142,421 of the `electronics/` insertions are machine-generated.** They are
KiCad S-expressions emitted by [`generate.py`](../../electronics/generate.py) —
real committed output, but not hand-authored. The third row removes them.

```bash
git log --numstat --format='' -- src/hardware/aius | awk '{i+=$1; d+=$2; n++} END {print i, d, n}'
git log --name-only --format='' -- src/hardware/aius | sort -u | wc -l
```

## Defects

35 numbered defects at the time of measurement, contiguous 1–35: 29 closed and
6 remaining. 4 of the 6 remaining carry no closure language.

Three further defects, 36–38, were opened on 2026-08-05 from static analysis of
an 8 h bench log, putting the count at that measurement at 38. All three are
closed now. Two of them, 37 and 38, were fixed the same day with a host
regression and a linking target build, and are deployed and verified live on
image `3824579` (2026-09-01); 36, the symptom that led to them, closed the same
day when its watch re-ran clean. (Register is authoritative; snapshot updated
2026-09-03.)

The authoritative list is the §Open defects section of
[`aius-network-hardening.md`](../plans/aius-network-hardening.md);
[`aius-open-work-register.md`](../plans/aius-open-work-register.md) carries
per-defect gists. Closure state is inferred from markup — strikethrough and
closure wording — not read from a status field.

### Bench and verdict lines

540 dated bench or verdict lines across `docs/plans` and `docs/reference`,
spanning 17 distinct dates from 2026-07-18 to 2026-08-04. Densest dates: 07-28
(90 lines), 08-04 (84), 07-26 (69).

```bash
grep -rEn '(bench|verdict).*20[0-9]{2}-[0-9]{2}-[0-9]{2}' docs/plans docs/reference | wc -l
```

## Bench operations

| Measure | Value |
|---|---|
| Highest observed `boot_generation` | 346 |
| Instrumented bench observation | 36.14 h |
| Bench logs | 19 |
| Samples | 62,856 |
| Flash logs retained in scratch | 32 |
| Build logs retained in scratch | 66 |

The five longest single runs are 10.00 h (17,932 samples), 8.00 h (13,854
samples), 4.05 h, 3.06 h and 3.00 h.

`boot_generation` is a device counter held in retained memory. 346 is therefore
a lower bound on boots since that retained slot was last initialised, and it
undercounts across power loss.

Scratch is garbage-collected at 7 days, so the flash-log count and the
observation hours are floors for the project as a whole, not totals.

### Evidence and electronics

- **Evidence** — 45 Git LFS artifacts across 35 subdirectories, plus 20
  reviewable source snapshots across 10.
- **Electronics** — 5 KiCad sheets and 5 connectivity JSON specs declaring 76
  nets and 65 components.

```bash
find docs/reference/datasheets.md -type f | wc -l
jq '[.nets | length] | add' electronics/connectivity/*.json
```

## Agent cost

Two AI toolchains worked on this subtree: Claude Code and the OpenAI Codex CLI.
Both keep local session transcripts on this machine, so both are measured here
by the same rule — a session counts if the string `hardware/aius` appears
anywhere in its transcript. A session which merely read the path counts in
full, and one that touched the subtree without emitting the string is missed.

### Claude Code

| Measure | Value |
|---|---|
| Sessions referencing the subtree | 40 |
| API calls | 16,582 |
| Tokens excluding cache reads | 67,906,233 |
| — of which output | 12,544,977 |
| Total tokens including cache reads | 7,252,167,646 |
| Tool calls | 19,012 |
| Tool calls per session | median 375, max 1,667 |

Cache reads are 98.9% of the billed total. **The non-cache-read figure is the
meaningful measure of distinct token production**; the billed total measures
volume and overstates production by about two orders of magnitude if read as
production. Both are recorded so neither is mistaken for the other.

By API call: `claude-fable-5` 8,061, `claude-opus-5` 4,906, `claude-opus-4-8`
2,169, `claude-sonnet-5` 1,434, synthetic 12. These sum to the 16,582 total.

```bash
grep -rl 'hardware/aius' ~/.claude/projects/*/*.jsonl
```

### Codex CLI

Transcripts are `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`; 968 of 1,823
rollout threads reference the subtree. Codex records usage as `token_count`
events carrying a running `total_token_usage`. Its `input_tokens` is
cache-inclusive, unlike Claude's, so the two schemas are normalised before any
comparison below. Forked rollouts replay the parent's event log verbatim, so
figures are deduplicated by fork lineage; that dedup was validated at a 1.83%
false-collapse rate on single-file control lineages.

| Measure | Value |
|---|---|
| Rollout threads referencing the subtree | 968 (81 user-initiated, 887 sub-agent) |
| API calls | 144,325 |
| Uncached input tokens | 346,128,199 |
| Output tokens | 32,230,724 (12,578,041 reasoning) |
| Total tokens | 16,857,768,661 (97.9% cached input) |
| Tool calls | 136,815 |
| Patch applications | 10,982 (10,981 succeeded) |
| Human prompts typed | 1,183 (141,610 characters) |

Independent corroboration for the token figure: `state_5.sqlite` records
`threads.tokens_used` summing to 65,294,864,888 across the aius rollouts,
matching the raw pre-dedup cumulative sum from the transcripts.

By API call: `gpt-5.6-sol` 100,111, `gpt-5.6-luna` 25,738, `gpt-5.6-terra`
17,928, `gpt-5.4-mini` 443, `codex-auto-review` 105.

Codex records no cost, no request identifiers, no per-call model field (it is
inferred from the preceding `turn_context`), and no commit trailer.

### Combined

Only like-for-like categories are added. Session counts are **not** combinable:
a Codex thread includes each sub-agent separately, while a Claude transcript
bundles its sub-agents inline.

| Measure | Claude Code | Codex | Combined |
|---|---|---|---|
| API calls | 16,582 | 144,325 | 160,907 |
| Uncached input tokens | 67,906,233 | 346,128,199 | 414,034,432 |
| Output tokens | 12,544,977 | 32,230,724 | 44,775,701 |
| Total tokens processed | 7,252,167,646 | 16,857,768,661 | 24,109,936,307 |
| Tool invocations | 19,012 | 136,815 | 155,827 |
| Active hours | 94.79 | 258.04 | 303.41 |

Combined active hours are lower than the sum because 49.42 h had both
toolchains running concurrently and are counted once.

### Wall clock

Codex activity begins 2026-07-13, two days before the first commit, so the
project's working span starts earlier than its git history shows.

- **560.73 h** — calendar span across both toolchains, 2026-07-13 to
  2026-08-05. An outer bound containing all idle time.
- **303.41 h** — union of active periods, summing inter-event gaps under 600 s
  across the merged event stream of both toolchains, so concurrent sessions
  count once.

Per-session sums are higher (1,813.86 h span and 630.97 h active for Codex
alone) because up to nine sessions ran in parallel. Those inflated figures are
not used.

### Commit attribution

All 683 subtree commits carry the same git author, so author identity separates
nothing. 282 carry a `Co-Authored-By: Claude` trailer and 401 carry none; Codex
emits no trailer. Matching commit subjects against the commit-message files
composed inside each toolchain partitions them cleanly, with no subject
appearing in both: **Codex 319, Claude 330, unattributed 34**. This shows where
a message was composed, not which toolchain made the commit, and is
corroborative rather than conclusive.

## Limitations

1. **One machine.** Only Claude Code and Codex CLI sessions recorded on this
   machine are counted. Any work done through another interface, or on another
   machine, is absent from every agent-cost figure. True totals are higher by an
   unmeasured amount; no multiplier is estimated.
2. **Codex fork dedup is inferred**, from rollout lineage and cumulative-token
   identity rather than a recorded parent link. It was validated at a 1.83%
   false-collapse rate on single-file control lineages. Raw event counts that
   remain fork-inflated (agent messages, turns, task starts) are deliberately
   excluded rather than reported.
3. **The two token schemas differ.** Codex `input_tokens` is cache-inclusive
   and Claude's is not; combined figures normalise for this, but any comparison
   drawn from the vendors' own raw fields will not agree with the table here.
4. **Commit attribution rests on message composition**, not a provenance
   trailer. 34 commits (5%) resist attribution.
5. **Scratch is reaped at 7 days.** Flash-log counts and bench-observation hours
   are floors. Anything older than the retention window is gone.
6. **`boot_generation` 346 is a floor.** It counts boots since the retained
   memory slot was last initialised and is lost on power loss.
7. **Human bench time is not measured.** Cable swaps, probe placement,
   listening tests and power cycles appear in no figure here.
8. **Physical lines, not SLOC.** Line counts include blanks and comments and are
   not normalised.
9. **Session bucketing is a string match.** A session counts if `hardware/aius`
   appears anywhere in its transcript. That over-counts, when a passing mention
   pulls in an otherwise unrelated session, and under-counts, when subtree work
   never wrote the path. The same rule is applied to both toolchains.
10. **Test counts undercount.** 15 of the 48 suite directories return zero under
    the stated patterns, so 1,163 is a floor.
11. **Defect closure is inferred from markup**, not read from a status field. A
    defect closed without the expected wording reads as open.
12. **Both datasets are live.** Repository figures are pinned to HEAD
    `99eba92f5` as of 2026-08-05; the transcript stores grew during measurement,
    which is why the Claude API-call total is 16,582 rather than the 16,496
    recorded by an earlier pass the same day.
