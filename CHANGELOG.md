<!-- See COPYING.txt for license details. -->

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
