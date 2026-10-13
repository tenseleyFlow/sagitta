# Sprint 8: File I/O — The No-Corruption Contract

## Prerequisites

- Sprint 7 — `TextBuf` (piece tree), `TextIter` chunk iterator (save path =
  iterate + write, zero copies), `yew_textbuf_from_bytes`.
- Sprint 2 — invalid-UTF-8 lossless escape policy (referenced, not applied
  at load: buffers store raw bytes; the policy governs rendering/motion).
- Sprint 0/1 — `Bytebuf`, `yew_log`, `YEW_BUG`, exit-code contract (3 =
  I/O error), unit harness, CI lanes.
- Binding: `00-decisions.md` Saving row (atomic always; fallback table;
  crash journal) and **Invariant 1 — this sprint lands it and it never
  leaves CI**. `01-architecture.md`: `src/text/file.c` owns this contract.
- Assumption beyond index entries: s00 landed an XDG-path helper
  (`yew_xdg_state_dir()`) alongside the log module; if not, land it here in
  `src/util/` — the journal needs `$XDG_STATE_HOME/yew/`.

## Goals

Land `src/text/file.c|h`: the load path (full read — no mmap; binary
detection; CRLF/BOM detect+preserve; size classes), the atomic save
decision table from `00-decisions.md` expanded to executable pseudocode
with every fallback (unwritable dir, symlink/hardlink preservation,
ownership/mode/xattr, EXDEV), the crash journal (append-only op log per
dirty buffer under XDG state — format, replay, lifecycle), and the kill -9
torture harness that makes Invariant 1 a CI gate forever. Statusline
indication of EOL/BOM is Sprint 15 (flags stored now); the interactive
recovery prompt is Sprint 14+ (replay API lands now, headless); journal
writes are wired to real edit transactions in Sprint 10 (this sprint
journals via a direct API and tests it headless).

## Deliverables

### 1. Load path — `src/text/file.h`, `src/text/file.c`

```c
typedef enum { YEW_EOL_LF, YEW_EOL_CRLF, YEW_EOL_MIXED } YewEol;

typedef struct FileMeta {
    YewEol eol;            /* dominant style; MIXED keeps dominant too   */
    u32    crlf_count, lf_count;
    bool   had_bom;        /* EF BB BF stripped at load, re-emitted      */
    bool   binary;         /* heuristic below                            */
    bool   had_invalid_utf8;
    bool   missing_final_nl;
    mode_t mode; uid_t uid; gid_t gid;
    nlink_t nlink; bool via_symlink; char *realpath;   /* arena-owned    */
    struct timespec mtime; u64 size_on_disk;
} FileMeta;

typedef enum { YEW_LOAD_OK, YEW_LOAD_ENOENT,      /* new file: empty buf */
               YEW_LOAD_EACCES, YEW_LOAD_EISDIR,
               YEW_LOAD_TOO_LARGE, YEW_LOAD_IO } YewLoadErr;

YewLoadErr yew_file_load(const char *path, TextBuf **out, FileMeta *meta);
```

Load = `open(O_RDONLY)`, `fstat`, one full read loop into a single malloc
(locked: **no mmap** — a file truncated under mmap is a SIGBUS), one
metadata sweep, `yew_textbuf_from_bytes` taking ownership. The sweep
computes everything in `FileMeta` in a single pass (memchr for NUL, LF, CR).

Size classes (log at load; budgets bite in Sprint 11):

| Class | Size | Behavior |
|---|---|---|
| empty | 0 | empty buffer; `eol` defaults LF (or config later) |
| normal | ≤ 256 MiB | plain path above |
| large | ≤ 2 GiB | same path; `yew_log(INFO)` the size; s11 gates timing |
| refused | > 2 GiB | `YEW_LOAD_TOO_LARGE` — hard error, revisit post-1.0 |

Binary heuristic: any NUL byte in the first 8 KiB (or whole file if
smaller) → `binary = true`. Binary buffers load byte-exact with EOL/BOM
handling disabled (no BOM strip, `eol` forced LF-semantics for line index
only). **Pitfall:** do not "helpfully" refuse binary files — byte-exact
round-trip is the contract; only transforms are disabled.

### 2. EOL and BOM policy (pinned)

- **Buffer holds file bytes verbatim, except the BOM.** CRLF is NOT
  normalized: `\r\n` stays in the buffer and round-trips by construction —
  the save path cannot re-corrupt what it never transformed. (Grapheme
  segmentation already treats CRLF as one cluster — GB3 — so Sprint 9
  motion never splits it.) `eol` in `FileMeta` tells *future edits* which
  EOL to insert; `yew_filemeta_eol_bytes(meta, &p, &n)` is the accessor.
- BOM (`EF BB BF` prefix): stripped at load, `had_bom = true`, re-emitted
  as the first bytes on save. Why strip: a BOM in the buffer breaks
  column-0 semantics everywhere for three bytes of noise.
- Detection: count `\r\n` vs bare `\n` in the sweep. `crlf > lf_bare` →
  CRLF; both nonzero → `YEW_EOL_MIXED` with the dominant style recorded.
- Invalid UTF-8: recorded (`had_invalid_utf8`), bytes preserved untouched;
  the s02 escape policy applies at render/motion time only. Never mangled,
  never U+FFFD-replaced in storage (invariant 2).

### 3. Atomic save — decision table → pseudocode

```c
typedef enum { YEW_SAVE_OK, YEW_SAVE_IO, YEW_SAVE_PERM,
               YEW_SAVE_CHANGED_ON_DISK } YewSaveErr;
YewSaveErr yew_file_save(const TextBuf *tb, const FileMeta *meta,
                         const char *path);
```

Decision table (expanded from `00-decisions.md`):

| # | Condition (checked in order) | Strategy |
|---|---|---|
| 1 | path is a symlink | resolve; run this table against the **target** (preserves the link itself) |
| 2 | target exists, `st_nlink > 1` | in-place + backup (rename would sever the other hard links) |
| 3 | target's dir not writable | in-place + backup |
| 4 | otherwise (incl. new file) | tmp + fsync + rename + dir-fsync |
| 5 | rename fails `EXDEV` | unlink tmp; in-place + backup (overlay/bind-mount oddity) |
| 6 | `fchown` on tmp fails and file has foreign owner | unlink tmp; in-place + backup (rename would steal ownership) |

Primary path, full pseudocode — implement exactly this order:

```
atomic_save(tb, meta, dst):
  dir  = dirname(dst); base = basename(dst)
  tmp  = dir "/.yew-" base "-" pid "-" counter    # same dir: same fs
  fd   = open(tmp, O_WRONLY|O_CREAT|O_EXCL, meta.exists ? meta.mode : 0666)
         # O_EXCL: never follow a planted symlink; EEXIST → bump counter
  if meta.had_bom: write EF BB BF
  for chunk in yew_textiter(tb): write_full(fd, chunk)   # EINTR-safe loop
  if fsync(fd) fails: goto fail                # BEFORE rename — the point
  if meta.exists:
      fchmod(fd, meta.mode)
      if fchown(fd, meta.uid, meta.gid) fails and (uid|gid != ours):
          close; unlink(tmp); return inplace_save(...)   # row 6
      copy_xattrs(dst, fd)                     # best effort, Linux only
  close(fd)
  if rename(tmp, dst) fails:
      if errno == EXDEV: unlink(tmp); return inplace_save(...)  # row 5
      goto fail
  dfd = open(dir, O_RDONLY|O_DIRECTORY); fsync(dfd); close(dfd)
  journal_discard(buffer)                      # clean save = no journal
  return OK
fail:
  unlink(tmp); return YEW_SAVE_IO              # dst untouched — invariant 1
```

In-place fallback (rows 2/3/5/6):

```
inplace_save(tb, meta, dst):
  bak = yew_xdg_state_dir() "/backup/" hash(realpath) ".bak"
  copy dst → bak byte-exact, fsync bak         # the escape hatch
  fd = open(dst, O_WRONLY);  write all;  ftruncate(fd, new_len)
       # write BEFORE truncate: a crash mid-way leaves old-len tail,
       # journal + bak recover; truncate-first guarantees data loss
  fsync(fd); close(fd)
  on any failure: restore dst from bak, return YEW_SAVE_IO
```

**Pitfalls:** `write` may be short even to regular files under signals —
loop. fsync of the *directory* is what makes rename durable on ext4/xfs —
skipping it passes every test until a power cut. Never `O_TRUNC` on the
real file in the atomic path. tmp name must be deterministic-prefix +
unique-suffix so crashed leftovers are identifiable (`.yew-*` swept with a
warning at next load of that dir, never auto-deleted silently).

Mtime conflict: `yew_file_save` stats dst first; if mtime/size differ from
`meta` (changed on disk since load) return `YEW_SAVE_CHANGED_ON_DISK` —
the UI decides later (Sprint 14+); headless callers get exit 3.

### 4. Crash journal — `src/text/journal.h`, `src/text/journal.c` (add to layout)

Per dirty buffer, an append-only op log at
`$XDG_STATE_HOME/yew/journal/<fnv64(realpath)>.sagj`.

Format (little-endian, versioned):

```
header : "SAGJ" u32 version=1
         u64 path_len, path bytes (realpath at load)
         u64 base_size, i64 base_mtime_s, u32 base_mtime_ns
record : u8  op        1=INS 2=DEL
         u64 off       byte offset (pre-op coordinates)
         u64 len
         len bytes     INS: inserted bytes; DEL: the REMOVED bytes
         u32 crc32     over the record from `op` through payload
```

```c
Journal *yew_journal_open(const char *realpath, const FileMeta *m);
void yew_journal_record(Journal *j, u8 op, u64 off, const u8 *b, u64 n);
void yew_journal_sync(Journal *j);        /* fsync; called on txn commit */
void yew_journal_discard(Journal *j);     /* unlink; after clean save    */
bool yew_journal_replay(const char *path, TextBuf *tb, FileMeta *m);
```

- DEL records carry the removed bytes so replay is also *reversible* —
  Sprint 10's undo tree deserialization reuses this exact record shape.
- Replay on open: if a journal exists for the file's realpath hash AND its
  header base_size/mtime match the file on disk, replay records until the
  first CRC mismatch or truncated tail (a crash mid-append is expected —
  the valid prefix is the recovery, the tail is discarded silently with a
  log line). Header mismatch → journal is stale (file changed outside a
  crashed session): renamed to `.sagj.stale`, never replayed, never
  deleted — the user's bytes are sacred even when confusing.
- fsync policy: `yew_journal_sync` on every transaction commit (Sprint 10
  wires transactions; until then the direct API is the test surface).
  CRC is a bespoke CRC-32 (one 256-entry table in `journal.c` — no zlib,
  invariant 7).

### 5. kill -9 torture harness — `tests/torture/` (new), Invariant 1's gate

Two attack layers, one checker:

**(a) Fault-injection shim** `tests/torture/faultshim.c` → `faultshim.so`
(LD_PRELOAD; built by the Makefile's test target, never linked into yew):
interposes `write`, `pwrite`, `fsync`, `fdatasync`, `rename`, `ftruncate`,
`close`. Behavior driven by env:

| Var | Meaning |
|---|---|
| `YEW_FAULT_AT=N` | after N intercepted calls succeed, the process `_exit(137)`s at the NEXT one — simulating kill -9 at a syscall boundary |
| `YEW_FAULT_SHORT=1` | additionally, `write` randomly (seeded) writes only half — exercises the EINTR/short-write loops |
| `YEW_FAULT_SEED=S` | determinism: same S → same fault schedule |

**(b) Driver** `tests/torture/kill9.c` (a real fork-based harness, not a
script): loop `i = 0,1,2,...`: fork; child sets `YEW_FAULT_AT=i` and execs
`build/yew-torture` (a tiny headless binary in `tests/torture/` that:
loads fixture, applies a scripted edit list journaling each, saves
atomically). Parent waits, then runs the checker. Loop ends when a run
completes with zero faults injected (i exceeded total syscalls). A second
mode kills the child from *outside* (`SIGKILL` after a random 0–20 ms
sleep, 500 iterations) to catch non-syscall-boundary states.

**(c) Checker** after every iteration, asserts Invariant 1 exactly:

1. The destination file is byte-identical to EITHER the pre-save content
   OR the post-save content — never truncated, never interleaved.
2. If it is the pre-save content, journal replay (header-matched)
   reproduces the post-edit buffer exactly.
3. No `.yew-*` tmp is ever counted as the file; leftovers are allowed.

Also covered: in-place fallback lane (fixture is a 2-link hardlink; after
every kill, EITHER dst is old, OR dst is new, OR dst is restorable from
the backup file — and the hardlink twin still shares the inode on
success).

### 6. Defer

- Statusline EOL/BOM/binary indication → Sprint 15 (flags ready in
  `FileMeta`).
- Interactive "recover from journal?" prompt → the first UI sprint that
  opens files (Sprint 14); `yew_journal_replay` is headless and complete
  now.
- Journal wiring to real edit transactions → Sprint 10 (`yew_journal_record`
  called from the undo-transaction commit path; until then only tests
  call it).
- Encoding conversion (latin-1 etc.) → never for 1.0 (UTF-8 only, locked).
- Watching for on-disk changes (inotify) → post-1.0; the mtime check at
  save is the 1.0 answer.

## Testing Strategy

- **Unit** (`tests/unit/test_file.c`): load fixtures — empty, no final
  newline, CRLF, mixed EOL (dominance), BOM, BOM+CRLF, NUL-at-byte-0,
  NUL-at-8191 vs 8193 (heuristic boundary), 3-byte file, ENOENT → empty
  buffer + OK-to-create; every fixture saves back byte-identical
  (`load → save → cmp`) — the round-trip table IS invariant 1's unit face.
- **Unit** (`tests/unit/test_journal.c`): record/replay round-trip;
  truncated-tail recovery (truncate journal at every byte length of a
  3-record file — replay never crashes, applies the valid prefix); CRC
  corruption at each record; stale-header rename path.
- **Save fallbacks** (`tests/unit/test_save_paths.c`): symlink preserved
  (link still a link, target updated atomically); hardlink pair shares
  inode after save; read-only dir → in-place + backup; mode/uid/gid
  preserved (mode always; chown path exercised via same-uid no-op +
  mocked failure through the shim).
- **Torture** (`make torture`, in CI): shim lane full sweep on the atomic
  path + 500-iteration external-SIGKILL lane + hardlink/in-place lane.
  CI runs the seeded quick set (~2 min); nightly runs 10× iterations.
- **Determinism**: same `YEW_FAULT_SEED` twice → identical fault schedule
  and identical checker verdicts.

## Definition of Done

1. `load → save → cmp` byte-identical for all ≥ 12 load fixtures,
   including BOM, CRLF, mixed, binary-with-NUL, no-final-newline.
2. Atomic path issues exactly: write(s), fsync(file), rename,
   fsync(dir) — order proven by the shim's intercept log in a unit test.
3. All 6 decision-table rows reachable and unit-tested; EXDEV and
   chown-failure rows exercised via the shim.
4. Torture sweep green: every `YEW_FAULT_AT` value from 0 to run-length,
   4 seeds, plus 500 external-SIGKILL iterations — checker invariant
   holds in 100% of iterations. `make torture` wired into CI.
5. Journal replay recovers the scripted edit list exactly in the torture
   checker whenever dst holds pre-save content.
6. Truncated-tail journal fuzz: replay at every truncation length of the
   3-record fixture — zero crashes, valid-prefix semantics.
7. Stale journal is renamed `.sagj.stale`, never applied, never deleted.
8. `yew_file_save` returns `YEW_SAVE_CHANGED_ON_DISK` when mtime/size
   moved under us; dst untouched in that case.
9. No mmap anywhere: `grep -rn 'mmap' src/` → nothing.
10. ASan/UBSan/valgrind lanes green including the torture binaries.
11. Exit code 3 on I/O failure paths from the headless torture binary.
12. ≥ 80 assertions across the three unit files.
