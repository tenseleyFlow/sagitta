# Embedded target runs

Sprint 57 keeps emulator evidence and hardware corroboration separate.  QEMU
is the required reproducible correctness/RSS gate.  A Raspberry Pi Zero 2 W
class run remains release evidence and is not inferred from emulation.

## QEMU x86_64 reference — 2026-08-31

- Host: Apple Silicon macOS, QEMU 11.1.1, TCG, `q35`, `qemu64`.
- Full kernel: Alpine `linux-virt-5.15.208-r0`; package SHA-256
  `842acc9e8e179b0fd3184949f1cd2e0c9cd6dec949f290a4b2e2cfd5cc6d7422`;
  `vmlinuz-virt` SHA-256
  `2722d06a528e7052415b19c4719ef0011247746f3986ce79601dc282c96bde27`.
- Full initramfs: 1,974,096 bytes.  The 4 MiB fixture lives on a fresh,
  deterministic 32 MiB ext2 work disk rather than consuming initramfs RAM.
- 64 MiB result: rows 1–11 pass; `VmHWM` peak 9,764,864 bytes; no yew OOM.
  Linux reported 38,180 KiB `MemTotal` and 18,668 KiB available immediately
  before the 4 MiB PTY row.
- Low-memory kernel: Tiny Core 8 `4.8.17-tinycore64`; published MD5
  `6b0e1446467f7a685ee0379d5486f067`; SHA-256
  `351b28092f2e2fc3c6c7e9e64605f5ff2cf9fa03c1830a0d40329098ffc5912e`.
- Low-memory initramfs: 636,623 bytes.
- 32 MiB result: row 12 cleanly refuses before loading the editor payload,
  naming the 48 MiB workload floor and the observed 21,188 KiB `MemTotal`;
  no yew OOM; clean poweroff.

The two distro kernels are deliberate.  Under the locked q35 geometry,
Alpine 6.6, Alpine 6.1, Alpine 5.15, and Tiny Core 9 through 16 cannot reach
PID 1 with a 32 MiB guest.  Tiny Core 8 is the newest tested distro x86_64
kernel that does.  This changes neither the 32 MiB guest ceiling nor the
64 MiB workload gate; it keeps row 12 capable of testing the named refusal
instead of accidentally testing kernel decompression failure.

## Raspberry Pi Zero 2 W class

Pending access to the designated 512 MiB arm64 hardware.  Do not mark the
hardware corroboration row complete until the exact binary, image, date,
wall time, and peak RSS are recorded here.
