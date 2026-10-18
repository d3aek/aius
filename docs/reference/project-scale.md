---
title: Aius project scale
type: reference
scope: component
component: aius
status: current
tags: [aius, measurement, provenance]
updated: 2026-10-18
toc: true
---

# Aius project scale

This records the measured size of the Aius subtree and the method behind each
figure. Everything below is measured as of 2026-10-18 against HEAD `7b3d1d1`,
over the Aius submodule and its full history, except the [agent
cost](#agent-cost) figures, which are kept as of 2026-09-04 and not
re-measured. The subtree began as the upstream repository path `src/hardware/aius` and was
later extracted into this standalone submodule; the history was preserved
across the move, so every figure spans the first commit to now in one line, not
a snapshot plus a delta. The repository is live: re-running any command later
returns different numbers.

## Measurement basis

- **Path** — every figure is scoped to the Aius submodule (`git_modules/aius`
  from the repository root), full working tree and full history.
- **Commit pin**: HEAD `7b3d1d1`, 2026-10-18 (last commit 2026-10-17). The
  agent-cost figures stay pinned to `da68887`, 2026-09-04.
- **Line counts** — physical lines, including blanks and comments, no SLOC
  normalisation. Counted with `git grep -c` against the commit, which reads
  ~0.1% above a `wc -l` count (it counts a final newline-less line) but is
  applied identically everywhere.
- **Commands** — run from the repository root, with `TZ=UTC` wherever a date or
  hour enters the result.

## Timeline

First submodule commit `19978ed`, 2026-07-15, `docs(aius): establish capability
and hardware plans`. Working activity starts earlier: the first Codex session
touching the subtree is 2026-07-13, so the git history understates the span by
two days (see [agent cost](#agent-cost)).

| Measure | Value |
|---|---|
| Commits | 1,066 |
| Span, first to last commit | 94 days |
| Calendar days with a commit | 55 |
| Longest gap between consecutive commits | 286.7 h |
| Commit body text | 150,687 words |
| Mean commit body | 141 words |

Commits per day (bar = commit count), with that day's authored line churn
(insertions + deletions, excluding vendored code and machine-generated
electronics):

```text
date          commits                                authored lines
2026-07-15    2  #                                        926
2026-07-16   99  ##################################     56437
2026-07-17   37  ############                          101253
2026-07-18   16  #####                                 146691
2026-07-19    6  ##                                       172
2026-07-21   19  ######                                  6904
2026-07-22   56  ###################                     6798
2026-07-23   24  ########                                5436
2026-07-24   79  ###########################            43140
2026-07-25   62  #####################                  29766
2026-07-26   45  ###############                        21465
2026-07-27   38  #############                          16872
2026-07-28   34  ###########                            17665
2026-07-29   21  #######                                21524
2026-07-30   16  #####                                   8123
2026-07-31   43  ##############                         20611
2026-08-01   27  #########                              14173
2026-08-02   15  #####                                   3810
2026-08-03   28  #########                               1915
2026-08-04   14  ####                                    3604
2026-08-05   21  #######                                 3503
                 (223 h gap, 2026-08-06 to 2026-08-13, no commits)
2026-08-14    6  ##                                       234
2026-08-15    9  ###                                      439
2026-08-16   26  ########                                8108
2026-08-17    1  #                                         22
2026-08-18    2  #                                        838
2026-08-19    3  #                                        195
2026-08-22    5  #                                        794
2026-08-28    5  #                                       1788
2026-08-31    1  #                                         78
2026-09-01   18  ######                                  3080
2026-09-02   30  ##########                             10436
2026-09-03   41  ##############                         10888
2026-09-04   11  ###                                     3716
2026-09-06    4  #                                        540
2026-09-07    7  ##                                      3731
2026-09-09    2  #                                        289
2026-09-10    2  #                                        108
2026-09-11    5  #                                        294
2026-09-12   11  ###                                      317
                 (286 h gap, 2026-09-13 to 2026-09-23, no commits)
2026-09-24    1  #                                         24
                 (201 h gap, 2026-09-25 to 2026-10-02, no commits)
2026-10-03    4  #                                       6828
2026-10-04    7  ##                                       917
2026-10-05   10  ###                                     3207
2026-10-06   15  #####                                   7534
2026-10-07   37  ############                           22626
2026-10-09    1  #                                         54
2026-10-10    8  ##                                      9882
2026-10-11   20  ######                                 23411
2026-10-12   20  ######                                 14037
2026-10-13   15  #####                                   4611
2026-10-14    3  #                                        138
2026-10-15    2  #                                        264
2026-10-16   24  ########                                6232
2026-10-17    8  ##                                      1183
```

Activity is dense through late July (busiest day 2026-07-16, 99 commits),
pauses for the 223 h gap around the upstream repository-to-submodule move, then resumes at
a lower rate into September. September thins out to a handful of commits a day
with two further gaps (286 h and 201 h). October opens a second burst, 2026-10-03
to 2026-10-17, of 174 commits (busiest day 2026-10-07, 37 commits).

```bash
git log --date=short --format='%ad' | awk '{c[$0]++} END{for(x in c) print x,c[x]}' | sort
git log --numstat --format='' | awk 'NF>=3 && $1 ~ /^[0-9]+$/ {i+=$1;d+=$2} END{print i,d}'
```

## Code

First-party firmware C and C++ is 153,150 lines across 250 files.

| Area | Lines | Files |
|---|---|---|
| `firmware/test` | 80,995 | 84 |
| `firmware/lib` | 39,200 | 110 |
| `firmware/src` | 25,703 | 28 |
| `firmware/tools` | 3,435 | 14 |
| `firmware/chooser` | 217 | 1 |
| `evidence/**` | 3,600 | 13 |
| Total | 153,150 | 250 |

Other first-party languages:

- **Python**: 56,287 lines across 116 files.
- **JavaScript**: 16,333 lines across 12 files.
- **Shell**: 3,961 lines across 24 files.

```bash
git grep -cE '$' HEAD -- 'firmware/src/*.c' 'firmware/src/*.cpp' 'firmware/src/*.h' | awk -F: '{s+=$NF} END{print s, NR}'
```

The same form was run per area, and per extension for Python, JavaScript and
shell.

### Vendored code

A further 81,633 lines of C and C++ across 255 files sit under
`firmware/vendor`. **This is third-party code, not first-party authorship.** It
is excluded from every figure above and from the churn rows below that say
"excluding vendored".

| Vendored component | Lines |
|---|---|
| `ffmpeg-eac3` | 31,871 |
| `SdFat` | 27,963 |
| `dcadec-core` | 17,432 |
| `NativeEthernet` | 2,755 |
| `tweetnacl` | 1,081 |
| `SD` | 531 |
| Total | 81,633 |

## Documentation

429,559 words across 71 markdown files (excluding vendored).

```bash
git grep -h '$' HEAD -- '*.md' ':(exclude)firmware/vendor/*' | wc -w
```

## Tests

1,975 test cases, counted by pattern match against the source rather than by
running the suites.

| Language | Cases | Pattern |
|---|---|---|
| C/C++ | 751 | `^\s*(static\s+)?void\s+test_\w+\s*\(` |
| Python | 847 | `^\s*def\s+test_\w+\s*\(` |
| JavaScript | 377 | vitest `^\s*(it\|test)\(` |

**1,975 is a floor.** Fixture and vector corpora that hold no case functions
return zero under these patterns.

```bash
git grep -cP '^\s*(static\s+)?void\s+test_\w+\s*\(' HEAD -- 'firmware/test/*' | awk -F: '{s+=$NF} END{print s}'
```

## Churn

Totals over the whole submodule history: 898,244 insertions, 302,580 deletions,
6,552 file-changes, touching 2,015 distinct files.

| Basis | Insertions | Deletions |
|---|---|---|
| All paths | 898,244 | 302,580 |
| Excluding vendored | 813,985 | 301,627 |
| Excluding vendored and `electronics/` | 490,698 | 186,933 |

The `electronics/` insertions are dominated by machine-generated KiCad
S-expressions (the schematic sheets and the `aius-carrier.kicad_pcb` layout),
real committed output but not hand-authored. The third row removes them along
with vendored code.

```bash
git log --name-only --format='' | sort -u | wc -l
```

## Defects

41 numbered defects, contiguous 1–41: 34 closed and 7 open by markup (open:
4, 21, 23, 25, 27, 40, 41). Defects 40 and 41 were opened 2026-09-01. Twelve
design decisions, D1–D12, are recorded in
[`aius-outstanding-work-2026-09.md`](../plans/aius-outstanding-work-2026-09.md).

The authoritative list is the §Open defects section of
[`aius-network-hardening.md`](../plans/aius-network-hardening.md);
[`aius-open-work-register.md`](../plans/aius-open-work-register.md) carries
per-defect gists. Closure state is inferred from markup — strikethrough and
closure wording — not read from a status field.

## Bench operations

The device's retained `boot_generation` counter reads 29 on the deployed board
(read from `GET /api/status` on 2026-10-18);
the highest the register recorded is 346. It is a lower bound on boots since the
retained slot was last initialised, and undercounts across power loss and slot
reinitialisation.

Flash, build and bench-capture logs live in scratch under a 7-day GC, so there
is no cumulative count of runs — only a rolling recent window is ever retained.

### Evidence and electronics

- **Evidence**: 58 Git LFS artifacts across 39 subdirectories, plus 22
  reviewable source snapshots across 10.
- **Electronics**: 5 KiCad schematic sheets and 5 connectivity JSON specs
  declaring 67 nets and 64 components, plus the `aius-carrier.kicad_pcb` layout.

```bash
git -C git_modules/aius lfs ls-files | grep -c docs/reference/datasheets.md
jq '[.nets | length] | add' electronics/connectivity/*.json
```

## Agent cost

Two AI toolchains worked on this subtree: Claude Code and the OpenAI Codex CLI.
Both keep local session transcripts on this machine, measured by the same rule —
a session counts if its transcript names the subtree. Because the subtree moved
from `src/hardware/aius` to the submodule mid-history, that rule matches either
`hardware/aius` or `git_modules/aius`.

**Every figure in this section is as of 2026-09-04, not re-measured at the
2026-10-18 pin.** The session transcripts were not readable in the
re-measurement, and the per-figure counting commands (fork-lineage dedup, active
hour union) are not recorded here, so nothing could be reproduced. Activity
after 2026-09-04 is absent from them.

These figures are the sum of two measurement passes taken the same way: one
covering activity through 2026-08-05, one from 2026-08-05 to 2026-09-04. The
2026-08-05 boundary day sits in both, a negligible double-count.

### Claude Code

| Measure | Value |
|---|---|
| API calls | 54,727 |
| Tokens excluding cache reads | 245,697,962 |
| — of which output | 38,220,590 |
| Total tokens including cache reads | 18,005,782,973 |
| Tool calls | 66,878 |

Cache reads are 98.6% of the billed total. **The non-cache-read figure is the
meaningful measure of distinct token production**; the billed total measures
volume. By API call: `claude-opus-5` 17,592, `claude-opus-4-8` 15,878,
`claude-fable-5` 13,138, `claude-sonnet-5` 4,559, `claude-fable-5-1` 2,108,
`claude-opus-4-7` 1,131, `claude-haiku-4-5` 285, synthetic 36.

Claude's session unit changed across the history — older transcripts bundled
sub-agents inline, newer ones store them as separate files — so a single
end-to-end session count is not meaningful; the token and call totals are.

### Codex CLI

| Measure | Value |
|---|---|
| Rollout threads referencing the subtree | 1,027 |
| API calls | 184,884 |
| Uncached input tokens | 440,212,607 |
| Output tokens | 41,714,224 (16,304,643 reasoning) |
| Total tokens | 22,065,787,385 (~98% cached input) |
| Tool calls | 166,366 |
| Patch applications | 14,135 |
| Human prompts typed | 1,289 |

Codex `input_tokens` is cache-inclusive, unlike Claude's, so the two schemas are
normalised before any combined figure. Forked rollouts replay the parent's event
log, so figures are deduplicated by fork lineage. By API call: `gpt-5.6-sol`
140,660, `gpt-5.6-luna` 25,738, `gpt-5.6-terra` 17,928, `gpt-5.4-mini` 443,
`codex-auto-review` 105.

### Combined

Only like-for-like categories are added. Session and rollout counts are **not**
combinable across toolchains: a Codex thread includes each sub-agent separately,
while a Claude transcript bundles its sub-agents inline.

| Measure | Claude Code | Codex | Combined |
|---|---|---|---|
| API calls | 54,727 | 184,884 | 239,611 |
| Uncached input tokens | 245,697,962 | 440,212,607 | 685,910,569 |
| Output tokens | 38,220,590 | 41,714,224 | 79,934,814 |
| Total tokens processed | 18,005,782,973 | 22,065,787,385 | 40,071,570,358 |
| Tool invocations | 66,878 | 166,366 | 233,244 |
| Active hours | 204.16 | 288.10 | 442.29 |

Combined active hours are lower than the sum of the two columns because ~50 h
had both toolchains running concurrently and are counted once. The combined
active span runs 2026-07-13 (Codex, two days before the first commit) to
2026-09-04, and active hours are the union of active periods (inter-event gaps
under 600 s).

## Limitations

1. **One machine.** Only Claude Code and Codex CLI sessions recorded on this
   machine are counted. Work through another interface or on another machine is
   absent from every agent-cost figure; true totals are higher by an unmeasured
   amount.
2. **Agent cost is summed from two passes.** The cumulative figures add a
   through-2026-08-05 pass and a 2026-08-05-to-2026-09-04 pass measured the same
   way; the boundary day is a negligible double-count, and Claude's session unit
   changed between them (§Claude Code).
3. **Codex fork dedup is inferred**, from rollout lineage and cumulative-token
   identity rather than a recorded parent link.
4. **The two token schemas differ.** Codex `input_tokens` is cache-inclusive and
   Claude's is not; combined figures normalise for this, and will not match the
   vendors' own raw fields.
5. **Scratch is reaped at 7 days.** Flash, build and bench-capture logs are a
   rolling window, not a cumulative count; bench-observation hours are not
   recoverable across the retention window.
6. **`boot_generation` is a floor.** It counts boots since the retained memory
   slot was last initialised and is lost on power loss and slot reinitialisation.
7. **Human bench time is not measured.** Cable swaps, probe placement, listening
   tests and power cycles appear in no figure here.
8. **Physical lines, not SLOC.** Line counts include blanks and comments and are
   not normalised.
9. **Session bucketing is a string match**, now spanning two path strings
   (`hardware/aius` and `git_modules/aius`) across the migration. It over-counts
   when a passing mention pulls in an unrelated session and under-counts when
   subtree work never wrote either string.
10. **Test counts undercount.** Fixture-only suite directories return zero under
    the stated patterns, so 1,975 is a floor.
11. **Defect closure is inferred from markup**, not read from a status field.
12. **Both datasets are live.** Repository figures are pinned to HEAD `7b3d1d1`
    as of 2026-10-18 and agent-cost figures to `da68887` as of 2026-09-04;
    re-running any command later returns different numbers.
