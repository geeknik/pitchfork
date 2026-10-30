<!-- See COPYING.txt for license details. -->

## [1.1.89] – 2026-10-30
### Added
- WiFi capture sessions over the SPI brain (`m1_capture_source`): pulls
  buffered promiscuous frames from the brain's monitor interface, wraps
  them through the ported stock codec chain (capframe → radiotap+EPB),
  and writes pcapng files to `0:/wifi/mcap_NNN.pcapng` with SHB/IDB/ISB
  (brain drop counters in the closing ISB). Session pump task, 50 ms
  cadence, static buffers throughout. Headless API ready for RPC/UI
  wiring (next).

## [1.1.90] – 2026-10-30
### Added
- RPC trigger surface for WiFi captures: `CAPTURE_START 0x70`
  (`[chan][dur_ms]`, chan 0 = hop, default 30 s), `CAPTURE_STOP 0x71`
  (idempotent), `CAPTURE_STATS 0x72/0x73`
  (active/frames/dropped/bytes). Captures are companion-triggerable
  over USB CDC; the pcapng lands on SD, retrievable via `FILE_READ`.

## [1.1.91] – 2026-10-30
### Fixed
- Stuck brain-monitor recovery (found live): if a session's deadline
  teardown lost the `monitor_stop` race with a busy ESP, the brain stayed
  in monitor mode and refused every later `CAPTURE_START` (`rc=-1`).
  `CAPTURE_STOP` now clears the brain unconditionally even with no local
  session, and failed monitor teardowns log a warning.

### Tooling (2026-10-30)
- `tools/m1ctl/` — in-repo headless companion CLI (stdlib Python):
  ping/info/capture start-stop-stats/pull/ls/doctor/selftest. Carries
  the full RPC wire notes (framing, sync-excluded CCITT, seq-matched
  streaming replies, lazy-ESP window, STOP-as-recovery) with an offline
  selftest enforcing them. Live-verified end to end: remote 12 s capture
  on ch6 → 105 frames → pcapng pulled to host with valid SHB.

## [1.1.92] – 2026-10-30
### Fixed
- Headless captures no longer depend on UI history: the capture engine
  now calls `m1_esp32_init()` itself (idempotent) so `CAPTURE_START`
  works on a fresh boot without opening a radio feature first.

### Foundation (2026-10-30)
- M1 Capture Envelope core (`m1_capture_envelope`): the thesis's
  canonical record — session ids, `/Sessions/<stamp>-<id>` directory
  naming (leap-year-correct civil-time conversion, libc-free),
  key=value envelope formatting (13 stable keys, additive-only,
  NULL-safe, truncation contract). Host-tested; device integration
  (WiFi captures write envelopes + crc32c artifact hashes at session
  end) follows next.

## [1.1.93] – 2026-10-30
### Added
- WiFi capture sessions close into the session model: session id minted,
  `/Sessions/s<tick>-<id>/captures/` created, the pcapng artifact hashed
  with CRC-32C, and a canonical Capture Envelope (`captures/1.m1env`)
  written — 13 stable keys incl. source domain, capture parameters,
  artifact reference, and integrity hash. Tick-stamp naming until the
  RTC lands; correlation is by session_id.

## [1.1.94] – 2026-10-30
### Fixed
- Envelope writer: correct CRC-32C semantics (M1_CRC32C_INIT seed +
  final XOR — stored hashes are now standard, host-verifiable), the
  missing `captures/` subdirectory creation, `0:/` path convention,
  and every mkdir/open result now logged instead of silently skipped
  (the live verification caught all three: no session dir appeared and
  the hash contract was wrong).

<!-- Changelog discipline: every FW_VERSION bump gets its entry in the same
     change (user directive 2026-10-30; see STATUS.md §7a). -->

## [1.1.88] – 2026-10-30
### Changed
- Sub-GHz TX boundary enforces the data-driven radio policy
  (`m1_radio_tx_allowed`, runtime region seeded from device config);
  fails closed with an on-screen notice. Raw `SI446x_*` exports frozen,
  retirement deferred to the 2.0 ABI decision.

## [1.1.87] – 2026-10-30
### Added
- `m1_radio_call` umbrella (app API level 9): `TX_ALLOWED`,
  `POWER_CLAMP`, `REGION_GET/SET` over the data-driven ISM band tables
  (`m1_radio_policy.c`, host-tested at band edges, fail-closed).

## [1.1.86] – 2026-10-30
### Added
- App supervision (API level 8): `HEARTBEAT` / `CANCEL_POLL` ops; 60 s
  silence → containment (suspend + lease/focus revoke + `hung` crash
  record + menu return; deliberately no force-delete).
### Fixed
- Runtime crash records (`m1_crash_record_runtime`) to the bounded ring.

## [1.1.85] – 2026-10-30
### Changed
- Boot mascot regenerated programmatically (first art was misaligned).

## [1.1.84] – 2026-10-30
### Fixed
- **Boot-loop incident:** the crash recorder's SD persist path put ~1.1 KB
  on the SD-init task's stack → usage STKOF → HardFault boot loop at
  461 ms (first hardware flash bricked boot; recovered over SWD, root
  cause read from the TAMP fault record). All boot-path buffers are now
  static. Full post-mortem in STATUS.md §7a item 11.

# Changelog

All notable changes to the M1 project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-02-05

### Added

- Initial open source release
- STM32H573VIT6 support (2MB Flash, 100LQFP)
- Hardware revision 2.x support
