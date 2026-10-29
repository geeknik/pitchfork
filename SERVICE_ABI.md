# M1OS Service ABI — the umbrella dispatch contract

Status: active policy. Implementation: `m1_app_syscall.h` (contract),
`m1_sys_call` in `m1_app_api.c`, `m1_storage_call` in `m1_app_storage.c`.
App API level: 6 (`M1_APP_API_VERSION`).

## 1. Why umbrellas

Apps are relocatable ELFs whose undefined symbols resolve **by name at load
time** against the export table. Every directly exported symbol becomes
permanent ABI: it can never change signature, move, or be deleted without
breaking shipped apps, and it is callable by any app with no policy check.
The umbrella pattern spends **one export symbol per domain** instead:

```c
int32_t m1_<domain>_call(uint32_t op, const void *req, uint16_t req_len,
                         void *resp, uint16_t resp_len);
```

| Domain | Symbol | Status |
|---|---|---|
| system/info | `m1_sys_call(op, args)` | shipped (3 ops, value-return style) |
| storage | `m1_storage_call` | shipped (8 ops, request/response style) |
| ui/display | `m1_ui_call` | shipped (3 ops; first **leased** resource — focus-gated FLUSH, NOTIFY, FOCUS_QUERY) |
| radio, nfc, rfid, ir, system-control | `m1_radio_call`, … | planned — add only per this spec |

## 2. The rules (what makes this an ABI and not a convenience)

1. **Additive only.** Op codes are never renumbered, reused, or removed.
   Adding an op bumps `M1_APP_API_VERSION`; an app needing it declares that
   level in its `.m1meta`. The loader already rejects apps declaring a newer
   level than the kernel.
2. **Unknown op → `M1_SYS_ERR_BAD_OP`.** This is the `NOT_SUPPORTED`
   contract: an old kernel meets a new app's op and returns a clean negative
   result — never a crash, never a silent no-op.
3. **Every request and response starts with `m1_sto_hdr_t`**
   (`size`, `version`, `flags`):
   - The kernel accepts `req_len >=` the minimum it needs for the op; unknown
     trailing bytes are ignored, so fields can be appended without breaking
     old apps.
   - The kernel always writes `hdr.size = sizeof(full current struct)`, so a
     caller can detect a newer kernel's extra fields.
   - `version != 1` for a known op → `M1_SYS_ERR_BAD_ARG`. Unknown flag bits
     are refused, not ignored.
4. **Buffers are explicit.** `req_len`/`resp_len` are always validated; too
   small → `M1_SYS_ERR_RANGE`. Fixed caps (`M1_STO_IO_MAX`, …) replace
   unbounded transfers; apps loop.
5. **Return convention.** `>= 0` is op-specific success (documented per op);
   negative is an `M1_SYS_ERR_*` code. Domain implementations never log-only
   and never hide policy refusals inside generic failures — `DENIED` is
   distinct from `IO`.
6. **Policy runs before the peripheral.** Storage resolves and sandboxes the
   path before any `f_*` call. Future domains do the same: capability check →
   resource lease → operation. An ESP/radio umbrella must enforce TX policy
   at the STM32 side regardless of what the co-processor would allow.

## 3. Storage domain (reference implementation, API level 6)

Ops: `STAT`, `READ`, `WRITE` (atomic temp→sync→replace), `APPEND`
(sync-before-close), `MKDIR` (idempotent), `REMOVE`, `RENAME`, `LIST`
(bounded, `TRUNC` flag when `total > count`). Full struct/op definitions in
`m1_app_syscall.h`.

Path sandbox: relative paths resolve under `0:/AppData/` (created on demand);
absolute reads allowed only under `0:/AppData/`, `0:/apps/`, `0:/infrared/`,
`0:/System/`; all writes/removes/renames confined to `0:/AppData/`; `..`,
`.`, `\`, control characters, and oversized components rejected. The policy
is a pure function (`m1_storage_resolve_path`) so it is host-tested
(`m1_csrc/test/test_m1_app_storage.c`).

The raw FatFs exports (`f_open`, `f_unlink`, …) remain in the legacy table
for existing apps but are frozen: no new app should link them, and they are
first in line to move behind umbrellas once the app corpus is verified with
`nm -u` (the method used in commit `ff907db`).

## 3a. UI domain (API level 7) — the lease reference

The display is the proving ground for the thesis resource-lease model
(`m1_ui_focus.c`): one exclusive owner (the foreground app's task, granted at
app start by the manager and revoked at exit — focus never outlives the app),
or free (kernel UI). `FLUSH` and `NOTIFY` check the lease before touching the
framebuffer and return `M1_SYS_ERR_DENIED` for non-owners; `FOCUS_QUERY` lets
an app verify its own standing. The direct drawing primitives stay exported
and frozen for existing apps; their enforcement is deliberately deferred to
the next API level so shipped apps keep working. The lease registry is pure
token logic, fully host-tested (`m1_csrc/test/test_m1_ui.c`).

## 4. Legacy surface freeze

The 544-symbol direct table (commit `bf1f8e1`) is **closed**: the generator
may not add new names, and hand entries only gain umbrella forwarding or
frozen hot primitives (drawing, memory, button polling). Candidate removals
after corpus verification, in danger order: `m1_sdcard_format`,
`storage_format`, `m1_wdt_reset`, `power_off`/`power_reboot`/`m1_power_down`,
`SI446x_*` raw radio controls, `vUsb2SerTask`/`vSer2UsbTask`,
`wifi_cred_*`, `startup_config_write`, `subghz_scene_*`.

## 5. Verification requirements for a new domain

A domain PR must add, in the same change: op enum + structs in
`m1_app_syscall.h` (mirrored to the SDK), the implementation with policy
before peripheral access, host tests over the stub layer under ASan+UBSan
including refusal paths, and a fuzz target when the domain parses untrusted
bytes. See `m1_csrc/test/Makefile`.

## 6. Found security fix (record)

2026-10-29, first run of `test_m1_elf_loader.c`: `elf_apply_relocation`
applied `r_offset` from the app's ELF with no bounds check against the target
section — a crafted `.m1app` had an arbitrary heap read-modify-write in
privileged code; additionally, misaligned sites were C-level UB. Fixed by a
caller-side `r_offset + 4 <= section size` guard (non-fatal skip, like
unsupported reloc types), byte-wise site accesses, and a 2-byte-alignment
requirement for halfword-pair relocations. Regression tests in the suite;
2.3M-execution libFuzzer run clean afterwards.
