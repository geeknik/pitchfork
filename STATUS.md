# M1OS Status — Source of Truth

Audit date: 2026-10-29. This document is the authoritative statement of what
M1OS **is** today, how the surrounding repositories relate to it, and where it
stands against the M1OS product thesis (session-oriented protocol laboratory:
observe / identify / interact / automate / develop / resume).

Until the documents below are individually refreshed, this file supersedes
their stale claims:

| Document | Stale claim | Reality |
|---|---|---|
| `README.md` "Status" | "Phase 0 — fork … byte-identical … no firmware behavior has changed" | M1OS is 1.1.80.0 with an app runtime, RPC stack, and customized menus |
| `ARCHITECTURE.md` §4/§5 | "~235 of a hard 256-symbol cap"; phase plan mostly future | 544 exported symbols (cap 1024); app-list RPC and menu customization already shipped |
| `CHANGELOG.md` | Stops at `[1.0.0] - 2026-02-05` | 1.1 series through 1.1.80.0 unrecorded (168 commits total) |
| `COMPATIBILITY.md` | Matrix tops out at stock `v0.8.0.4`, "last updated 2026-07-24" | Stock line shipped 0.8.1.0 (2026-10-02) + two fixes; matrix needs new rows |

---

## 1. Current state in one paragraph

M1OS is a full-source fork of the **C3 community firmware at
`v0.8.0.0-C3.148`** (fork commit `8cecf41`, 2026-07-28), re-versioned onto its
own `n.n.n.n` scheme, now at **1.1.81.0** (`m1_csrc/m1_fw_update_bl.h`; audited
baseline: 1.1.80.0 at `82123ea`, 2026-08-03, with the §7a tranche landed on
top of it). It replaces the classic "everything compiled in"
model with a flash-once kernel plus `.m1app` ELF applications loaded from
`0:/apps`, a user-customizable menu driven by SD manifests, and an RPC stack
(`m1_csrc/m1_rpc.c`, 3,957 lines) that supports app install/list over USB and
WiFi. Built-in features that became apps were deliberately gutted (e.g.
`m1_nfc.c` went from 8,424 lines in stock to 3,013 here). Development on this
line froze 2026-08-03 and resumed 2026-10-29 (§7a); the stock MonstaTek line
shipped 0.8.1.0 and two bug fixes in the interim that are **not** present
here.

## 2. Repository topology — three repos, two lineages

| Repo | Lineage | Version | Role |
|---|---|---|---|
| `geeknik/M1OS` (this repo) | bedge117 C3 community line, forked at C3.148 | **1.1.80.0** | App-runtime kernel (target of the M1OS thesis) |
| `geeknik/M1` | MonstaTek stock line (shares only the Feb-2026 initial-upload root with M1OS) | **0.8.1.0** (2026-10-02) + fixes `664ec65`, `2af1f8a` | Fully-bundled firmware; owns the capture stack, M1CP protocol, host-test suite, release machinery |
| `MonstaTek-Esp32-Core` (mtkcore) | MonstaTek ESP line | **0.8.1.0**, ESP-IDF 6.0.1 | ESP32-C6 "shared service core": canonical schema, 113 opcodes / 8 services, radio arbiter, 3 transports |
| `geeknik/m1-esp32-brain` | bedge117 clean-room ESP app (design lineage: M1-T-800 framing + hapaxx11 negotiation) | **1.5.0** (2026-08-03, dormant since) | The ESP firmware M1OS actually pairs with today: 0x4D31 SPI binary protocol, 67 dispatched opcodes, offensive WiFi/BLE/ZB suite |

Facts that follow from this:

- M1OS and stock M1 diverged **structurally**. A merge is infeasible; the
  relationship must be targeted porting in both directions.
- M1OS has **none of stock's October work**: no `m1_lcd_wake_restart_timer()`
  (the `664ec65` NFC/RFID backlight fix), no `2af1f8a` TX-Restricted restyle,
  no 0.8.1.0 release content.
- M1OS has **zero host tests** (no `test*` directory exists) and no CI. Stock
  M1 carries 42 host-test files plus `tools/m1cp_host_test` (8 binaries) and
  `tests/m1pkg` fixtures; mtkcore carries 87 host-test files / 90 CMake
  targets with ASan+UBSan.
- **ESP pairing is split and now fully audited** (decision framework in §9).
  M1OS pairs with bedge117's `m1-esp32-brain` 1.5.0 over **SPI** (STM32
  master / ESP slave, 512-byte full-duplex MTU, `0x4D31` framing, CRC-16-CCITT,
  fragmentation with reassembly, handshake-GPIO flow control; protocol version
  `0x01`). `m1_csrc/m1_esp_rpc.h` is a namespace-renamed sed-copy of brain's
  header — wire-identical by construction, but with no parallel tests to catch
  drift. Stock M1 0.8.1.0 pairs with mtkcore 0.8.1.0 over **UART4**. Both links
  exist on the same board; the two READMEs were each right for their own
  pairing. mtkcore additionally implements a **native SPI v1 transport** (its
  canonical binary interface), unused by stock — the likely convergence path
  for M1OS (§9).
- Absent locally and still unauditable: `m1-sdk` (the SDK header consumer of
  the ABI generator) and qMonstatek Studio (desktop companion).

## 3. The app runtime — verified inventory

The foundation is real and better than hobby-grade. Preserve it; do not
rewrite.

- **Enumeration** (`m1_csrc/m1_app_manager.c`, 917 lines): scans `0:/apps` for
  `*.m1app`, 64-app cap, cached snapshot with dirty flag + generation counter,
  last-good snapshot preserved on scan failure, 1,000 ms bounded index lock.
  One RPC and menu consumers share the snapshot.
- **Manifest** (`.m1meta` ELF section): `magic "M1AP"`, `api_version`,
  `stack_size` (words), `name[32]`, `category[16]`, `version[16]` — leniently
  parsed so old 40-byte manifests still load
  (`m1_csrc/m1_elf_loader.h:110-117`). **Missing:** app id, capability mask,
  heap limit, watchdog budget (the thesis manifest is a superset).
- **ELF loader** (`m1_csrc/m1_elf_loader.c`, 1,019 lines): ET_REL ARM ELF
  only, ≤16 sections, GNU-hash + strcmp-confirmed symbol resolution, `app_main`
  entry with Thumb bit forced, API level gate (rejects `api_version > 5`),
  full unwind on load failure. **Hazard:** unsupported relocation types are
  skipped with a warning, non-fatally (`:566-571`, `:824-830`) — a malformed
  app can load with silently unresolved relocations.
- **Tasks**: stack clamped 256–16,384 words (floor 512 at launch), priority
  deliberately equal to the subfunc handler so apps cannot starve the UI task.
- **Tracked heap** (`m1_csrc/m1_app_api.c:201-268`): `pvPortMalloc`/
  `vPortFree` are rerouted through tracked versions; `m1_app_tracked_free_all()`
  reclaims everything after app exit. This is the right primitive for per-app
  heap accounting (thesis stabilization item 5) — extend it, don't replace it.
- **No app validation at all**: no signature, hash, or CRC on `.m1app` files.
  Any ELF dropped on the card runs privileged. `ARCHITECTURE.md` §5 documents
  this as a decided trade-off for local development; it is not acceptable for
  a public catalog, as the thesis states.

## 4. The ABI — the central contradiction, quantified

- Apps resolve firmware functions **by name against a 544-entry table**
  (315 hand-listed in `m1_csrc/m1_app_api.c` + 229 generated into
  `m1_csrc/m1_app_api_generated.inc`; cap `API_MAX_SYMBOLS` 1024) and call
  them as plain C function pointers. No dispatcher, no capability check, no
  broker.
- The generator (`tools/app_api_gen.py`) regex-scans public headers and
  filters by reachability (`tools/app_api_alive.txt`, 2,874 alive names from
  `nm` on an empty-mode build). Commit `bf1f8e1` ("M1OS 1.1.78") took the
  table 659 → 544 by dropping dead code. Auto-generation is good; the
  **selection criterion is the problem** — "reachable and in a header"
  ≠ "safe to freeze as ABI".
- Examples of live exports that no app should ever be able to call directly:
  `m1_sdcard_format`, `storage_format` (destroy all data); `m1_wdt_reset`,
  `power_off`, `power_reboot`, `m1_power_down`; `SI446x_Set_Frequency`,
  `SI446x_Set_Tx_Power`, `SI446x_Start_Tx_CW` (unrestricted RF);
  `vUsb2SerTask`/`vSer2UsbTask` (FreeRTOS task bodies as ordinary calls);
  `usart_rxupdate_head_pointer`, `CDC_Signal_Next_Tx` (USB/UART plumbing);
  `wifi_cred_load_all`/`wifi_cred_save`/`wifi_cred_delete` (stored
  credentials); `settings_save_to_sd`, `startup_config_write`;
  `subghz_scene_*` internals; `assert_failed`.
- The dispatch umbrella exists in embryo: `m1_sys_call(op, args)` with exactly
  three read-only ops (`API_LEVEL`, `HEAP_FREE`, `UPTIME_MS`) and a documented
  additive-only discipline (`m1_csrc/m1_app_syscall.h`). The thesis
  `m1_ui_call` / `m1_radio_call` / `m1_storage_call` / `m1_system_call` set is
  the natural completion of a pattern already in the tree — barely started,
  not contradicted.

Migration guidance (compatibility-preserving): keep the `.m1app`/`.m1meta`
format, `app_main`, the API-level gate, the tracked allocator, and
name-resolved linkage; freeze a small list of hot primitives (drawing, memory,
button polling) as direct symbols; route dangerous domains through versioned
umbrellas whose request/response structs begin with `size`/`version`/`flags`;
have the generator emit umbrella forwarding instead of raw internals. Unknown
ops return `NOT_SUPPORTED`.

## 5. Lifecycle and reliability gaps (verified in source)

1. **No force-kill.** The parent-side `vTaskDelete()` was removed (it raced
   app cleanup — comment at `m1_app_manager.c:750-753`), and the wait loop
   has no timeout: an app that never returns blocks the menu task forever.
2. **App tasks are not watchdog-supervised.** The report registry watches
   BUTTONS / USB2SER / QMON_RELAY / RPC tasks only (`m1_watchdog.c:146-150`).
   A wedged app neither trips the watchdog nor recovers.
3. **Fault handling is whole-device reset**: `HardFault_Handler` stashes a
   post-mortem into TAMP backup registers and resets
   (`Core/Src/stm32h5xx_it.c:124-165`). Better than stock (whose naked
   HardFault path hangs on `bkpt` and never reaches its reporter — verified in
   `geeknik/M1`), but there is no crash-loop counter, no quarantine, and no
   SD-persisted crash record.
4. **No persistent reset-reason record**; TAMP carries only the last fault.
5. **Zero tests, zero CI** (see §2).

## 6. What stock M1 0.8.1.0 has that M1OS lacks (the porting pool)

Priorities for the thesis, roughly ordered:

1. **The capture stack** (`m1_capture_{frame,link,pcap,transport,scan_parse}`,
   `m1_pcapng`, `m1_radiotap`): a versioned, CRC-32C/COBS-protected, credit-
   flow-controlled capture pipeline from ESP32-C6 promiscuous mode to SD
   pcapng — host-tested with golden-byte tests. This is the chassis for the
   thesis capture envelope and Signal Atlas. It never existed in the M1OS
   lineage.
2. **Host-test infrastructure** (42 files + runner recipes): the thesis
   verification pipeline ("host-build every parser that consumes untrusted
   bytes") starts from this, not from zero.
3. **Release machinery**: reproducible builds (`SOURCE_DATE_EPOCH`,
   `-ffile-prefix-map`, byte-identical rebuild verification), post-build
   symbol-guard scripts, release manifest.
4. **M1CP manager protocol** (USB-CDC framed protocol with upload/verify/
   abort, busy arbitration, cooperative cancellation) — the closest thing to
   the thesis companion transport; compare with M1OS's own RPC before
   standardizing on one.
5. **ESP version accounting** (`m1_esp_version` — installed ESP version
   persisted in RTC backup registers) if the ESP pairing moves to mtkcore.
6. **Fixes**: `664ec65` (backlight timeout), `2af1f8a` (TX-restricted UX),
   plus 0.8.1.0 content — each needs a hand port; the NFC code they touch was
   gutted here, so applicability varies.

Conversely, M1OS holds things stock lacks and will need if the lines converge:
the app runtime, menu engine, RPC stack, and tracked allocator.

## 7. First-sprint gap map (thesis items → status)

| # | Thesis deliverable | Status | Where it stands |
|---|---|---|---|
| 1 | Source-of-truth status document | **Done** | This file; README now links it |
| 2 | Tag known-working STM32/ESP/SDK/Studio combos | **Partial** | §8 matrix is the software side; brain `v1.5.0` tagged; needs one hardware-verified row per pairing |
| 3 | Persistent fault + reset-reason records | **Partial** | M1OS: TAMP fault stash + reset only. Need SD `/System/crash` records, full RCC CSR snapshot, crash-loop counter |
| 4 | Task/stack/heap/mutex/service telemetry | **Partial** | Tracked per-app heap exists (extend to peak/quota); stock has a CLI heap/stack snapshot (`m1_mtest 5`) worth porting; no periodic health table, no mutex-wait instrumentation |
| 5 | Exclusive SD ownership firmware vs MSC | **Audit needed** | Stock has the `usbmsc_sd_enable` gate but a verified race (card-detect task can remount during an active MSC session); M1OS's diverged `m1_sdcard.c` must be audited against the same race |
| 6 | Fuzz ELF loader + manifest parser on host | **Done (first tranche)** | `m1_csrc/test/`: storage-umbrella + ELF-loader suites under ASan/UBSan, libFuzzer harness, CI workflow. Found and fixed a real heap OOB (§ below) |
| 7 | Define service-dispatch ABI; stop direct exports | **In progress** | Spec in `SERVICE_ABI.md`; `m1_storage_call` shipped (API level 6, sandboxed, atomic writes); legacy 544-symbol surface declared closed |
| 8 | Resource leases (ESP, Sub-GHz, NFC, IR, USB, display focus) | **Partial** | Display-focus lease shipped (m1_ui_focus + m1_ui_call, API level 7): grant at app start, idempotent revoke at exit, gated FLUSH/NOTIFY. Radio/NFC/IR/USB/SD leases remain; mtkcore's arbiter is the model |
| 9 | Capture envelope + session directory schema | **Chassis exists in stock** | Stock capture stack (WiFi-only, `session_id` hardcoded `0xCA07`, no cross-radio envelope). Define the envelope, then port the chassis |
| 10 | Signal Atlas reference app | **Blocked** | Depends on 7+8+9 landing; it becomes the first `.m1app` proving the platform |

## 7a. Landed 2026-10-29 (first implementation tranche)

1. **Wire-contract parity gate** (`tools/check_esp_contract.py`, wired into
   CMake + CI): first run caught real drift — brain renamed the capability
   defines to `M1_CAP_*` and added `M1_CAP_SOFTAP` (bit 23), which the sed-copy
   predated. `m1_esp_rpc.h` regenerated from brain's canonical header
   (no M1OS code referenced the old names); parity now enforced on every build.
2. **Brain pairing protected**: `tools/build_factory.sh` (refuses to build
   unpinned), `tools/IDF_PIN`, `PROVENANCE.md`, README build section, tag
   `v1.5.0` on `e515182` in the brain repo.
3. **Storage umbrella shipped** (`m1_storage_call`, API level 6): sandboxed
   paths, versioned `size/version/flags` request headers, atomic
   temp→sync→replace writes, idempotent mkdir, bounded list with truncation
   flag; spec + freeze policy in `SERVICE_ABI.md`. The path policy is a pure
   host-tested function.
4. **Host-test infrastructure** (`m1_csrc/test/`): shared RAM-FatFs stub so
   the REAL `m1_app_storage.c` and `m1_elf_loader.c` run on host under
   ASan+UBSan; ~50 storage assertions, 20 ELF-loader robustness cases, and a
   libFuzzer harness (2.3M executions clean after the fix below).
5. **Security fix in the ELF loader** (found by the new suite on its first
   run): relocation sites were applied with no bounds check on `r_offset` — a
   crafted `.m1app` had an arbitrary heap read-modify-write in privileged
   code, and misaligned sites were C-level UB. Fixed: caller-side
   `r_offset + 4 <= section size` guard (non-fatal skip), byte-wise site
   accesses, halfword-alignment requirement; regression tests added
   (`SERVICE_ABI.md` §6).
6. **CI** (`.github/workflows/host-tests.yml`): host suites + fuzz smoke,
   strict ESP parity (brain checked out alongside), generator determinism.
7. **Brain provenance closed** (brain repo commit `57e6ae4`): the upstream
   v1.5.0 release binary was captured — md5 matches its shipped `.md5`,
   sha256 recorded, and the app descriptor reveals **ESP-IDF v5.1.2-dirty**
   (built 2026-08-01). `tools/IDF_PIN` is set to v5.1.2 with the -dirty
   caveat; a hash-verified clean rebuild remains the last step. The locally
   attached M1 runs stock 0.8.x firmware (verified over USB CDC), so a live
   M1OS↔brain pairing check waits for the M1OS flash.
8. **UI umbrella + display-focus lease** (app API level 7, 1.1.82):
   `m1_ui_call` (FOCUS_QUERY / focus-gated FLUSH / NOTIFY) and the pure
   `m1_ui_focus` lease registry, granted/revoked by the app manager around
   every app run. First proof of the thesis resource-lease model. Host suite
   now defaults to Homebrew clang, whose LLVM runtime supports
   LeakSanitizer on darwin — local runs leak-gate like Linux CI (Apple
   clang does not).

## 8. Compatibility matrix (software-verified rows)

| STM32 firmware | ESP32 firmware | Link | Verified? |
|---|---|---|---|
| M1OS 1.1.80.0 | m1-esp32-brain **1.5.0** (`CMakeLists.txt:9`; wire semver in `main/main.c:160`) | SPI master/slave, 512 B framed MTU, protocol `0x4D31` v1 | **Wire-compatible** — sed-copied contract header, ~60 of 67 opcodes in active use. **Build not reproducible**: ESP-IDF unpinned, `sdkconfig` uncommitted, build scripts gitignored — tag a known-good 1.5.0 binary |
| Stock M1 0.8.1.0 | mtkcore 0.8.1.0 (build_id `0.8.1.0`, epoch 2026-10-01; `main/CMakeLists.txt:41-48`) | Factory UART4 ASCII + COBS capture channel | Version pairing consistent (same-day releases); STM32 persists ESP version in RTC DR2/DR3 |
| M1OS 1.1.80.0 | mtkcore 0.8.1.0 | mtkcore's native SPI v1 transport (candidate) | Not built; requires porting `m1_esp_rpc` onto mtkcore's 113-opcode surface and pin/protocol verification against brain's SPI wiring |

Toolchains: M1OS builds with CMake+Ninja+`arm-none-eabi-gcc`, but
`CMakePresets.json` hardcodes Windows compiler paths and the toolchain file
takes whatever `arm-none-eabi-gcc` is on PATH — **no version pin** (stock
documents GNU Arm 14.2.Rel1 / STM32CubeIDE 1.17 / Cube FW_H5 1.3.0 and
verifies byte-identical rebuilds). mtkcore pins ESP-IDF 6.0.1 with
`CONFIG_APP_REPRODUCIBLE_BUILD=y`.

Hardware notes: the ESP "SPI link" claim in this README is **correct for the
M1OS+brain pairing** (resolved — it conflicted only with stock's UART4
pairing). Still to verify: the display spec (stock documents ST7586s ERC240160
240×160; this README says "128x64 monochrome" — same physical device, one is
wrong), and whether mtkcore's native SPI v1 pin map matches brain's
(SCLK=GPIO7, MOSI=12, MISO=13, CS=15, HANDSHAKE=14; mind mtkcore's documented
GPIO6 polarity trap between image variants) before assuming the convergence
path is flash-only.

Brain's audit verdict in brief (full detail in §9): the SPI protocol is real
and versioned, but brain is a flat 9.3-kloc command handler — no arbiter, no
cancellation, a dead event channel, an advisory-only capability bitmap that
M1OS never reads, zero policy gating on deauth/beacon/raw-TX/802.15.4-inject,
zero tests, and no way to bit-rebuild it from the repo. Capture on brain is a
pull-based 24×256 B monitor ring plus a one-shot 4 KB EAPOL pcap buffer — not
a streaming chassis. Its BLE depth is where M1OS ships features today (HID
keyboard, spam, beacons, NUS direct) but it has **no GATT client at all**;
mtkcore is the mirror image — full GATT client plus streaming capture, but its
HID/spam/beacon families are schema-defined and capability-DISABLED. The two
ESP lines have complementary coverage; mtkcore already reserves the opcodes
brain's unique features need.

## 9. Decisions required before Runtime v2

1. **ESP pairing** — audited 2026-10-29. *Verdict: keep brain 1.5.0 as the
   working pair short-term; converge on mtkcore long-term.* Rationale: brain
   is the only firmware M1OS speaks to today (~60 of its 67 opcodes in active
   use; menu, apps, relay, and screen push all talk `M1ESP_*`), so ripping it
   out now buys nothing. But brain is architecturally flat (no arbiter, no
   cancellation, dead event channel, advisory capabilities, zero policy
   gating, zero tests, not bit-rebuildable, dormant since 2026-08-03), while
   mtkcore already implements exactly the broker-adjacent pattern this thesis
   needs on the STM32 side — arbiter with conflict classes, capability
   manifest with genuinely DISABLED families, versioned binary transport,
   87 ASan host tests, pinned reproducible builds. Convergence path: port
   `m1_esp_rpc` onto mtkcore's **native SPI v1 transport** (not factory UART —
   the M1OS wiring is SPI), then enable mtkcore's schema-defined-but-disabled
   HID/spam/beacon families to regain brain's unique features; brain's missing
   GATT client comes free (mtkcore has a full one). Verify pin/protocol
   compatibility first (§8 hardware notes). Regardless of pairing, TX policy
   must be enforced on the STM32 side by the broker — neither ESP line can be
   trusted to police itself.
2. **Porting direction with stock 0.8.1.0** — *Recommendation:* treat stock as
   upstream for reliability and the capture stack; port its host-test suite
   first (item 6), then the capture chassis (item 9). Hand-port `664ec65` /
   `2af1f8a` now; they are small.
3. **ABI freeze policy** — *Recommendation:* freeze the 544-symbol table as
   "legacy surface, additive-only, no new entries" today, and route all new
   app access through umbrellas (§4). This costs nothing now and stops the
   surface growing while the broker is designed.
4. **Changelog/compat refresh ownership** — CHANGELOG needs 1.1.x entries and
   COMPATIBILITY.md needs stock-0.8.1.0 rows; both are mechanical once the
   ESP decision (1) is made.

## 10. Immediate next actions (ordered)

1. Preserve the working pair: tag a known-good brain 1.5.0 binary (the repo
   cannot rebuild it bit-identically — ESP-IDF unpinned, `sdkconfig`
   uncommitted), commit a pinned `sdkconfig` + build script, and add a
   wire-contract parity check so the sed-copied `m1_esp_rpc.h` cannot drift
   from brain's header silently.
2. Port stock's host-test infrastructure into M1OS CI and add ELF-loader +
   manifest fuzz targets (sprint 6; prerequisites for everything else).
3. Hand-port `664ec65` and `2af1f8a` from stock; audit 0.8.1.0 content for
   applicability.
4. Write the umbrella ABI specification (`size`/`version`/`flags` request
   headers, op numbering per domain, `NOT_SUPPORTED` contract) and implement
   `m1_storage_call` first — smallest domain, highest danger-reduction
   (`m1_sdcard_format` et al. leave the direct surface).
5. App lifecycle hardening: launch timeout + resurrected force-kill that does
   not race cleanup (cooperative cancel first, kill only as last resort),
   app-task watchdog registration, app crash counter with quarantine.
6. Port the capture chassis from stock; define the M1 Capture Envelope
   (generalizing stock's `m1_capframe_meta_t` pattern) and the
   `/Sessions/YYYY-MM-DDTHHMMSSZ-<id>/` directory schema. Note: this lands on
   the mtkcore path (its COBS capture channel is the streaming chassis);
   brain's monitor ring is not suitable.
7. Refresh `CHANGELOG.md`, `COMPATIBILITY.md`, `ARCHITECTURE.md` §4/§5
   numbers; resolve the display-spec README discrepancy with hardware; verify
   mtkcore native-SPI pin compatibility as the §9.1 convergence precondition.
