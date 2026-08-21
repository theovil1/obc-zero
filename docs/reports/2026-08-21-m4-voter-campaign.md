# Voter campaign, 1000 randomised corruptions

- **Date:** 2026-08-21
- **Commit:** `976c9f0-dirty`
- **Seed:** `1` — the campaign replays exactly from this alone
- **Runs:** 1000
- **Failures:** 0
- **Wall clock:** 14.7 minutes

## What each run asserts

One corrupted copy must leave a majority, be repaired, and change
nothing about the system's behaviour. Two must produce no verdict at
all, and the caller must fail safe rather than trust a survivor. A run
that injects nothing is a failure, not a pass.

## Coverage

| Dimension | Count |
|---|---:|
| first copy `a` | 320 |
| first copy `b` | 324 |
| first copy `c` | 356 |
| 1 copy corrupted | 481 |
| 2 copy corrupted | 519 |
| repairs performed | 481 |
| votes unresolved | 34254 |

## Failures

None.

That is what the voter is supposed to do, and it is worth saying
plainly what this does **not** show: the injections are single and
double bit flips in the stored value, applied before the window
opens. Corrupting a checksum word, corrupting mid-window, and
corrupting three copies at once are different faults and are not
covered here.
