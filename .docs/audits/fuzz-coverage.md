# Fuzz coverage ledger

Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`

No coverage-instrumented run has completed. Edge totals are populated only by
`make fuzz-cov`; zeros are not inferred for unmeasured targets.

| Date | Commit | Target | Iterations | Seed | New edges | Total edges | Corpus size | Findings |
|---|---|---|---:|---|---:|---:|---:|---|

## Pinned schedule

| Lane | Cadence | Budget | State |
|---|---|---|---|
| `fuzz` | every push | 200,000 iterations per target, seed 1 | existing |
| `fuzz-nightly` | nightly | 30 minutes per target, recorded date seed | pending Sprint 58 wiring |
| `soak` | continuous | 72 hours per tier-1 target, four seed streams | not started |
| `fuzz-cov-weekly` | weekly | four hours per target, monotonic edge counts | pending Sprint 58 wiring |
| `soak-rc` | each release candidate | 72 hours tier 1 + 12 hours tier 2 | release blocker |
