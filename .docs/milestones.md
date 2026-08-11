# Milestones

Milestone evidence is tracked here; implementation readiness alone does
not earn a field milestone.

## Daily driver — Sprint 42

- State: `PENDING`
- Implementation frontier: Sprint 42.5 (native language pack); Campaign 09
  is paused until its Definition of Done is green
- Reference machine: Intel Core i7-10870H, 32 GiB RAM, Linux
  7.1.5-arch1-2, `TERM=xterm-256color`; terminal emulator not yet recorded
- Evidence: [daily-driver dogfood log](dogfood-log.md)
- Remaining gate: at least 10 working days, 40 logged hours, 200,000 real
  keystrokes with the required live latency histogram, abnormal-exit and
  resume trials, self-hosting attribution for at least 60% of eligible changed
  lines from one complete post-Sprint-42 implementation sprint under the
  dogfood log's path/artifact rules (Sprint 42.5 is designated), and the other
  Sprint 42 field checks
- Revocation policy: B1–B10 in the dogfood log; once earned, any listed
  incident changes this state to `REVOKED` with a reason and issue id

Do not change this state to `EARNED` until every Sprint 42 dogfood checkbox
has a committed evidence link.
