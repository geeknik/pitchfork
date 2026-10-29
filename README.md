<!-- See COPYING.txt for license details. -->

# Pitchfork

**Offensive radio firmware for the MonstaTek M1** (STM32H573 host +
ESP32-C6 network brain).

> Formerly **M1OS** — same project, new name. (See [`STATUS.md`](STATUS.md) §2.)

A modular app runtime: flash the kernel **once**, then run `.m1app` applications from the SD card
**forever** — no reflash loop for app development. Devs compile an app, users
drop it on the card (or push it over USB/WiFi) and run it. The menu is
user-customizable: arrange apps into the positions you want and nest them.

> **This is a community project and is not affiliated with or endorsed by Monstatek.**

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full design.

## Status

**Active runtime, 1.1-series.** Current version **1.1.83.0**. Pitchfork (then M1OS) was seeded
from the fully-bundled `m1-firmware` at `v0.8.0.0-C3.148` (2026-07-28) and has
since diverged deliberately: it ships the `.m1app` ELF app runtime (API level
5, 544 exported symbols), user-customizable SD-driven menus, an RPC stack with
app install/list over USB and WiFi, and built-in features moved out to apps.

The authoritative statement of current state, repository relationships, and
the roadmap toward the session-oriented protocol-laboratory architecture is
[`STATUS.md`](STATUS.md) — it supersedes any older phase/version claims here
and in `ARCHITECTURE.md`, `CHANGELOG.md`, and `COMPATIBILITY.md`.

## Relationship to the classic firmware

Pitchfork and `m1-firmware` are **two living products**, developed side by side:

- [`m1-firmware`](https://github.com/bedge117/M1) (`v0.8.0.0-C3.N`) — the
  fully-bundled firmware, the pool M1OS draws from as apps. (Note: the stock
  MonstaTek line has moved on to 0.8.1.0 with fixes M1OS has not absorbed —
  see [`STATUS.md`](STATUS.md) §6 for the porting pool.)
- **Pitchfork** (`n.n.n.n`) — this modular app runtime, formerly M1OS.

They share one ESP32-C6 brain firmware and one app SDK
([`m1-sdk`](https://github.com/bedge117/m1-sdk)), and never collide: separate
repos, release streams, version schemes, and desktop companions. The desktop
companion for M1OS is **qMonstatek Studio**.

## Versioning — `n.n.n.n`, tied to the app ABI

| Digit  | Bumps when                                | App impact                                   |
|--------|-------------------------------------------|----------------------------------------------|
| MAJOR  | ABI break (a syscall removed/changed)     | old apps may break — rare, avoided           |
| MINOR  | ABI addition (new syscalls, API level ++) | old apps fine; new apps may need this kernel |
| PATCH  | kernel bug fix, no ABI change             | none                                         |
| BUILD  | incremental dev build                     | none                                         |

A dev reading `M1OS 1.3.x` knows exactly what app API level they can rely on.

## Hardware

- **MCU:** STM32H573VIT6 (Cortex-M33, 2 MB dual-bank flash)
- **Display:** 128x64 monochrome
- **WiFi/BT/802.15.4:** ESP32-C6 coprocessor (SPI link)
- **RF:** Si4463 sub-GHz transceiver
- **NFC:** ST25R3916 (13.56 MHz)
- **RFID:** 125 kHz ASK/PSK reader with T5577 write
- **IR:** receiver + transmitter
- **Storage:** microSD card (required for apps and menu customization)

## Open-device guarantee

M1OS is never locked down. No RDP-2, TZEN, Secure Boot, Flash Encryption, or
eFuse-enforced signing. The device must always remain flashable, re-flashable,
and roll-back-able. Any firmware signature is warn-only, never an enforced gate.

## License

GNU General Public License v3.0 — see [COPYING.txt](COPYING.txt).
