# M1OS — Architecture

**A modular app-runtime firmware for the Monstatek M1.**

M1OS turns the M1 (STM32H573 host + ESP32-C6 network brain) into an application
platform: you flash the kernel **once**, then run `.m1app` applications from the
SD card **forever** — no reflash loop for app development. It is a fork of
`m1-firmware`, paired with the **qMonstatek Studio** desktop tool, and it shares
the existing ESP32-C6 brain firmware unchanged.

---

## 1. Goals & non-goals

### Goals
- **Kill the reflash cycle for app development.** A dev compiles a `.m1app`; a
  user drops it on the SD card (or pushes it over USB/WiFi) and runs it — with no
  firmware flash.
- **A stable, versioned syscall ABI is the product contract.** Apps target an API
  level; the kernel guarantees it.
- **User-customizable menu.** Arrange apps into the menu positions you want, and
  nest them.
- **Frictionless distribution.** Devs ship apps; users install and run them
  instantly.

### Non-goals
- **Not a shipped product.** Units arrive on stock M1 FW `0.8.0.0`. M1OS is
  something an enthusiast/dev flashes onto their *own* unit once. There is no
  "M1OS device" being sold.
- **Not compliance-by-absence.** Offensive capability stays latent in the kernel
  and ESP; M1OS simply ships nothing offensive *pre-installed or pre-surfaced*.
  The user knowingly adds capability apps. (See §9.)
- **Not a network-stack rewrite.** The STM32 has no radio; the ESP32-C6 remains
  the network brain. Apps reach the network through the kernel's syscall → ESP
  RPC bridge. (See §9.)
- **Never a locked-down device.** No RDP-2 / TZEN / Secure Boot / Flash
  Encryption / eFuse-enforced signing. M1OS must always remain flashable,
  re-flashable, and roll-back-able. Any firmware signature is warn-only, never an
  enforced gate.

---

## 2. System topology

```
        USB / WiFi / BLE                      SPI (M1 master)
qMonstatek Studio <────────► STM32H573 (M1OS kernel) ◄────────► ESP32-C6 (brain)
                                    │                              WiFi / BLE /
                             SD card │                             802.15.4 / ESP-NOW
                          0:/apps/*.m1app
                          0:/menu/layout
                          0:/IR/ ... (data)
```

- **STM32H573 — the kernel.** Owns the display, input, SD, USB, the ESP SPI link,
  all timing-critical radio/NFC drivers, the ELF app loader, and the menu engine.
  SPI master.
- **ESP32-C6 — the network brain.** Native WiFi / BLE / 802.15.4 / ESP-NOW
  services over the binary RPC. **Shared, single line, additive protocol** — the
  same brain serves M1OS and the classic firmware. SPI slave.
- **SD card — the mutable layer.** Apps (`0:/apps/*.m1app`), the menu manifest,
  and data (IR DB, etc.). Required for customization; the kernel still boots
  without a card using a built-in default menu.
- **qMonstatek Studio — the companion.** Pushes apps to the SD card over RPC,
  designs the menu layout, and flashes the kernel.

---

## 3. Kernel / app boundary

The honest shape of "move everything into apps": timing-critical drivers **stay
resident in the kernel**; the *feature front-ends* become apps.

| Stays in the kernel (flash, resident)                          | Becomes an app (SD, transient)              |
|----------------------------------------------------------------|---------------------------------------------|
| Display (ST7567 / u8g2), input, buzzer, RGB LED                | "WiFi Deauth", "SubGHz Scanner", "NFC Read" |
| SD / FatFS, USB, the ESP SPI link + RPC                        | Any feature UI + orchestration + light logic|
| NFC CE loop / RFAL / ST25R3916; SubGHz SI4463 TX + scene engine | Games, tools, dev utilities                |
| RFID, IR TX, RNG / crypto                                      | Community / third-party apps                 |
| The ELF loader, menu engine, and app host                      |                                             |

**"Barebones" therefore means: the full kernel with every driver present, but a
near-empty menu and an empty `0:/apps/`.** Capability is latent; nothing offensive
is surfaced until the user installs an app.

---

## 4. The syscall ABI (the core contract)

Because the promise is "flash once," **the kernel's API surface is the contract**
— and adding a syscall later means a kernel reflash, the exact pain M1OS removes.
So the ABI is foundational, not an afterthought.

### Mechanism (inherited)
- A `.m1app` is an **ELF32 relocatable object** (`ET_REL`), loaded to RAM,
  relocated, and entered at a symbol named **`app_main(void*)`**.
- Apps call kernel functions as ordinary undefined external symbols, resolved at
  load time against a kernel export table.

### Changes for M1OS
1. **Grouped syscall dispatch.** Today the export table holds ~235 of a hard
   **256-symbol** cap, and resolution is **hash-only** (a collision would
   mis-resolve). M1OS exposes a *wide* surface without exploding the symbol count
   by routing each domain through an umbrella entry:
   - `m1_ui_call(op, args*)`, `m1_nfc_call(op, args*)`, `m1_subghz_call(...)`,
     `m1_rfid_call(...)`, `m1_ir_call(...)`, `m1_radio_call(...)` (ESP), etc.
   - Hot, simple primitives (display draw, `malloc`, input poll) stay as **direct
     symbols** for ergonomics and speed.
2. **String-compared resolution.** Resolve by hash **then** confirm with a string
   compare, eliminating the collision mis-resolution risk as the surface grows.
3. **Expose the full service surface at 1.0.** NFC read/write/emulate, SubGHz
   TX/scene, RFID T5577 write, IR, display, input, SD, and the complete ESP suite
   — so apps rarely need a syscall the kernel lacks. **Every gap is a reflash.**

### Versioning — the API level
- The per-app `.m1meta` manifest carries an **`api_version`** (API level). The
  loader already runs an app only if `kernel_level >= app_level`.
- **The API level is monotonic and additive-only.** Syscalls are never removed or
  renumbered. A kernel **MINOR** bump raises the API level; old apps keep running
  across kernel updates, and only apps that need a *new* syscall require a newer
  kernel.
- This is the OS model: apps declare a minimum API level; the kernel stays
  backward-compatible. It is what makes "flash once" hold for years.

---

## 5. App model

- **Format / path / manifest.** ELF32 `ET_REL` at `0:/apps/*.m1app`; metadata in
  an `.m1meta` section (`magic`, `api_version`, `stack_size`, `name[32]`). Missing
  `.m1meta` → a default manifest is synthesized.
- **One foreground app at a time.** Each app runs as its own FreeRTOS task and
  shares the system heap.
- **No sandbox — DECIDED.** Apps run privileged: same heap, same privilege level,
  direct kernel function pointers via the symbol table, no MPU regions. A full MPU
  sandbox is deliberately **not** built — it would force every kernel call through
  an `SVC` syscall gate with privilege transitions and MPU-carved memory (a
  microkernel), which is slower and fights the "broad, easy, fast API" goal. The
  trust model matches an open device: the user deliberately installs each app.
  Instead of a sandbox, ship three **soft mitigations** (all Phase 1):
  1. **Per-app heap tracking + teardown on kill** (the force-kill leak fix below).
  2. **Fault-to-menu recovery** — a faulting app task is killed and returns to the
     menu instead of rebooting the whole device.
  3. **Watchdog + app timeout** as a backstop for a wedged app.
  Real MPU-as-a-guard (kernel data marked read-only to the app task) stays
  documented as optional future hardening if untrusted app distribution ever
  becomes a goal. A sandbox is *not* device lockdown — it remains addable later.
- **Lifecycle.** Launch → `app_main` → **BACK force-kills** the app task.
  - **Required fix:** the force-kill (`vTaskDelete`) currently leaks whatever heap
    the app allocated — only the ELF sections are reclaimed. In an app ecosystem
    (many launches per session) that reproduces the C3.148-class slow heap-death.
    M1OS must add **per-app allocation tracking and teardown** on force-kill
    before distributing apps.
- **Limits to raise:** `APPS_MAX_FILES` (currently 16) and the syscall cap, per
  §4.

---

## 6. Customizable menu

### Today
The menu is a `const`, flash-resident, statically-sized tree
(`SUB_MENU_ITEMS_MAX = 14` wide, `SUB_MENU_LEVEL_MAX = 5` deep), selected by
compile flags. Nothing about the layout is read from storage.

### M1OS
1. **Tree in RAM, built at boot** from a manifest (drop `const`).
2. **Manifest on SD** (e.g. `0:/menu/layout`): an ordered list of top-level slots
   (**0–13** — the full 14), each entry one of:
   - `builtin` → a kept kernel function,
   - `app` → a `.m1app` launched by path,
   - `submenu` → recurses (≤ 5 deep, ≤ 14 wide).
   Each entry carries a label and icon reference.
3. **Boot loader** parses the manifest into an `S_M1_Menu_t` node pool (fixed pool
   or dynamic alloc). **Fallback to a built-in default layout** if the manifest is
   absent or invalid — a card-less unit is never bricked.
4. **Dispatch** stays function-pointer-based: `builtin` leaves call the kept
   kernel function; `app` leaves call an **app-launcher thunk** that loads the
   referenced `.m1app`.
5. **qMonstatek Studio** edits the manifest (drag-and-drop or numeric 0–13 +
   nesting) and pushes it to the SD card over the File RPC.

**Format — DECIDED: flat line-based text, versioned.** One entry per line, a
version header (`M1MENU v1`), an explicit depth column per entry for nesting, and
the fields above (type, ref, label, icon). Not JSON (forces a parser + heap onto
the MCU at boot) and not binary TLV (opaque, not hand-editable). Text is tiny to
parse with a hand-written level-stack, qM Studio emits it trivially, and a power
user can hand-edit it. **Validate strictly and fall back to the built-in default
layout on any parse error** — a malformed manifest must never leave the device
menu-less.

---

## 7. The developer loop (the win)

```
  compile .m1app            qM Studio uploads          launch on device
  (m1-sdk toolchain)  ───►  to 0:/apps over  ───►      from the menu
                            USB / WiFi                  (zero flash)
```

The round trip is seconds. The kernel is flashed **only** for ABI additions
(rare, and additive so old apps survive). This is the entire reason M1OS exists.

---

## 8. ESP32-C6 brain

- **Shared and single-line.** Both M1OS and the classic firmware flash the same
  brain releases. The RPC protocol advertises capability bits and a protocol
  version and grows **additively**.
- Apps never talk to the ESP directly — they call `m1_radio_call(...)` (and
  friends), and the kernel bridges to ESP RPC. The ESP needs no per-app awareness.
- **Only fork the ESP on a breaking protocol change** — which we avoid by keeping
  every change additive/capability-gated.

---

## 9. Versioning & repositories

| Component            | Repo                          | Versioning                                  |
|----------------------|-------------------------------|---------------------------------------------|
| **M1OS** (this)      | `bedge117/M1OS`               | `n.n.n.n` (see below); starts `1.0.0.0`     |
| **qMonstatek Studio**| `bedge117/qmonstatek-studio`  | semver from `1.0.0`; own updater feed        |
| ESP32-C6 brain       | `bedge117/m1-esp32-brain`     | unchanged, shared                            |

### M1OS `n.n.n.n` — tied to the ABI contract
| Digit  | Bumps when                                   | App impact                                   |
|--------|----------------------------------------------|----------------------------------------------|
| MAJOR  | ABI break (a syscall removed/changed)        | old apps may break — rare, avoided           |
| MINOR  | ABI addition (new syscalls, API level ++)    | old apps fine; new apps may need this kernel |
| PATCH  | kernel bug fix, no ABI change                | none                                         |
| BUILD  | incremental dev build                        | none                                         |

A dev reading `M1OS 1.3.x` knows exactly what app API level they can rely on.

**qMonstatek Studio** is a *distinct product* (own repo, updater feed, installer
`AppId`, and QSettings identity) so it installs alongside the classic app and
never collides on version or settings.

**Cross-repo shared fixes:** the new repos are seeded from the current history, so
a shared bug fix is a `git cherry-pick` across a sibling remote until the lines
diverge.

---

## 10. Phase plan

- **Phase 0 — Fork.** Create `M1OS` and `qmonstatek-studio` (history preserved);
  reset M1OS versioning to `n.n.n.n`; give the Studio app its new identity
  (`AppId`, QSettings name, updater repo).
- **Phase 1 — ABI foundation.** Grouped syscall dispatch + string resolution;
  expose the full service surface; lock the API-level additive discipline;
  **fix the force-kill heap leak.**
- **Phase 2 — Dev loop.** qM Studio "push app to SD + launch"; an RPC to
  enumerate installed apps and read their `.m1meta`.
- **Phase 3 — Customizable menu.** Tree → RAM; manifest + boot loader; qM Studio
  drag-and-drop / numeric + nesting editor.
- **Phase 4 — App catalog.** Browse / install apps from qM Studio once the ABI is
  stable.

Note the ordering: **the ABI and dev loop come before the customization UI**,
because they are what make the flash-once promise real.

---

## 11. Risks & open items

- **The syscall restructure is invasive** — it touches every app and the `m1-sdk`
  toolchain. Do it *before* distributing apps, while the blast radius is small.
- **SDK must track the ABI.** `m1-sdk` is versioned to the API level; its headers
  are the app-facing view of the syscall table.
- **Menu-manifest parser weight** on the MCU — pick a lightweight format.
- **Node-pool sizing** for the dynamic menu (fixed pool vs dynamic alloc).
- **No app isolation** — a buggy privileged app can corrupt the kernel. Accepted
  for the open/trusted model; revisit if apps come from untrusted sources.
- **`.m1app` force-kill leak** must land in Phase 1 or the ecosystem inherits the
  slow heap-death.

---

## 12. Relationship to the classic firmware

M1OS and `m1-firmware` are **two living products**, developed side by side:

- `m1-firmware` (`v0.8.0.0-C3.N`) — the fully-bundled firmware, unchanged.
- **M1OS** (`n.n.n.n`) — the modular app runtime.
- One shared ESP brain; one shared `m1-sdk` (versioned to the API level).

Neither can collide with the other: separate repos, separate release streams,
separate version schemes, separate desktop companions.
