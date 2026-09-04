# Sprint 58 adversarial audit index

Audit opened: 2026-09-03  
Baseline commit: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`  
Baseline hosted run: `33714586788` (22 standard push jobs passed)  
Audit-control head at opening: `6272b0932aeccb00c880d014559ce5a790edf6f8`  
UCD version: 16.0.0

The product-code baseline is immutable for every front. Audit tooling,
reproducers, evidence, and the two Sprint 58 inbound obligations may advance
on `trunk`; a front always reproduces against the baseline commit above.

## Build matrix of record

| Target lane | Commit of record | Evidence |
|---|---|---|
| `x86_64-linux-gnu` | `41fef4166fe6bf127f36b8b9f6eb653a454a28c1` | hosted run `33714586788` |
| `arm64-linux` | `41fef4166fe6bf127f36b8b9f6eb653a454a28c1` | hosted run `33714586788` |
| `x86_64-linux-musl` | `41fef4166fe6bf127f36b8b9f6eb653a454a28c1` | hosted run `33714586788` |
| `arm64-macos` | `41fef4166fe6bf127f36b8b9f6eb653a454a28c1` | hosted run `33714586788` |

The baseline built the six shipping module sets: full
(`lsp ai fuss plugins`), minimal (`MODULES=""`), and the `lsp`, `ai`, `fuss`,
and `plugins` single-module profiles.

## Audit-harness confirmation

Hosted run `33815573832` passed at audit-control head `3e0edb57`, including
the F01 hard-XPASS suite under GCC, Clang, ASan/UBSan, arm64 Linux, arm64
macOS, musl, and the minimal `MODULES=""` profile. The three F01 failures
therefore have second-machine and cross-architecture confirmation; the
product-code baseline remains the immutable commit above.

## External tools at opening

| Tool | Version |
|---|---|
| git | 2.55.0 |
| clangd | Apple clangd 21.0.0 (`arm64-apple-darwin25.4.0`) |
| interactive shell | zsh 5.9 (`arm64-apple-darwin25.0`) |
| E-mode fixture shell | `/bin/sh` |

## Front ledger

`pending` means the front has not been handed to its reviewer. Counts remain
zero until its report closes; silence never counts as evidence.

| Front | File | Status | Raw | Deduped | Crit | High | Med | Low | Unverified |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| F01 unicode | `audit-01-unicode.md` | closed | 3 | 3 | 0 | 1 | 2 | 0 | 3 |
| F02 terminal | `audit-02-terminal.md` | awaiting CI | 0 | 0 | 0 | 0 | 0 | 0 | 3 |
| F03 text | `audit-03-text.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F04 modal | `audit-04-modal.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F05 execute | `audit-05-exec.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F06 regex | `audit-06-regex.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F07 UI/workspace | `audit-07-ui.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F08 Fletch | `audit-08-fletch.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F09 recorder | `audit-09-recorder.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F10 syntax | `audit-10-syntax.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F11 LSP | `audit-11-lsp.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F12 AI | `audit-12-ai.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F13 git/FUSS | `audit-13-git.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F14 plugins | `audit-14-plugins.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| F15 CI | `audit-15-ci.md` | pending | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

## Verdict

We are not ready to tag: Sprint 58 is open, only F01 of its fifteen fronts has
closed, the invariant sweep has not run, and no campaign-wide
absence-of-findings claim has been earned.
