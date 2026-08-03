#!/usr/bin/env python3
"""
app_api_gen.py — generate the M1 app-API export surface from the firmware's
public subsystem headers, so every app-facing function is exported without
hand-maintaining the table in m1_app_api.c (and without it silently drifting
out of sync with the SDK header apps build against).

WHY THIS EXISTS
    Apps are relocatable ELFs whose undefined symbols are resolved BY NAME at
    load time against a table in the firmware (m1_app_api.c). That table was
    hand-written, so every new capability an app wanted meant editing the table
    and reflashing the M1. This generator makes the table (and the SDK header)
    a build product of a curated set of PUBLIC HEADERS instead.

SOURCE OF TRUTH
    PUBLIC_HEADERS below — the app-facing subsystem headers. Add a header here
    and all its public functions become callable from apps. Internal transport/
    bootloader/HAL headers are deliberately excluded.

OUTPUTS (only written when run without --dry-run)
    m1_csrc/m1_app_api_generated.inc   FW table entries: { "name", (void*)name },
    <sdk>/include/m1app_generated.h     matching `extern` prototypes for apps

PRESERVED, NOT TOUCHED
    The hand-written entries[] in m1_app_api.c keep the special cases the
    generator can't derive: name!=symbol aliases/wrappers (m1app_display_flush
    -> m1_u8g2_nextpage, badusb_type_string -> badusb_type_string_forced), the
    C-library shims, and the grouped-dispatch umbrellas. The generator skips any
    name already present there (dedup by name) and anything matching EXCLUDE.

FLASH NOTE
    Referencing a function in the table pins it against --gc-sections. Keep
    PUBLIC_HEADERS to genuine app-facing subsystems so the image doesn't retain
    dead internal code. Watch the FLASH % after a build.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
CSRC = os.path.join(ROOT, "m1_csrc")
API_C = os.path.join(CSRC, "m1_app_api.c")
GEN_INC = os.path.join(CSRC, "m1_app_api_generated.inc")
SDK_HDR_DEFAULT = os.path.normpath(
    os.path.join(HERE, "..", "..", "m1os-sdk", "include", "m1app_generated.h"))

# --- App-facing subsystem headers (the curated public API surface) -----------
# Add a header here to export its public functions to apps. NOT included on
# purpose: m1_rpc.h / m1_link.h / m1_esp32_hal.h (transport), m1_fw_update*.h /
# m1_fw_update_bl.h (bootloader), m1_menu.h (menu glue), HAL/CMSIS headers.
# "All app-facing headers": every header in m1_csrc/ is scanned, EXCEPT the
# non-API internals below (transport, bootloader, menu-render, HAL/board glue,
# task/ISR plumbing) — exporting those would only pin dead code / fail to link.
# The Sub-GHz and lfrfid trees are mostly per-protocol register/codec internals,
# so their app-facing API headers are added explicitly via EXTRA_HEADERS instead
# of scanning those dirs wholesale.
SCAN_DIRS = ["m1_csrc"]

# m1_csrc headers that are NOT app API (matched on basename).
HEADER_EXCLUDE_RE = re.compile(
    r"^(m1_rpc|m1_link|m1_esp_rpc|m1_esp32_hal|m1_esp32_fw_update|m1_qmon_relay|"
    r"m1_fw_update|m1_fw_update_bl|m1_menu|m1_gui|m1_app_manager|m1_elf_loader|"
    r"m1_app_api|m1_int_hdl|m1_tasks|m1_display_data|m1_core_config|m1_cli|"
    r"m1_cli_help|m1_at_|m1_esp_hosted_dummy|m1_fusb302|m1_bq2|logger|Res_String|"
    r"bit_util|m1_i2c$)"
)

# App-facing API headers outside m1_csrc (Sub-GHz / lfrfid), added explicitly.
EXTRA_HEADERS = [
    "Sub_Ghz/m1_sub_ghz_api.h",
    "lfrfid/lfrfid.h",
    "lfrfid/lfrfid_file.h",
]

# Never export a symbol whose name matches one of these (lifecycle / ISR / glue
# / internal). Apps get capabilities, not init/teardown or interrupt handlers.
EXCLUDE_RE = re.compile(
    r"(^menu_)|(^HAL_)|(^TIM_)|(^_)|"
    r"(_init$)|(_deinit$)|(_Init$)|(_DeInit$)|"
    r"(_IRQHandler$)|(_irqhandler$)|(_task$)|(_Task$)|(_isr$)|(_ISR$)|"
    r"(_Callback$)|(_callback$)|"
    r"(_gui_update$)|(_xkey_handler$)"          # menu-render / key glue, not app-safe
)

# Explicit one-off exclusions (symbols that don't link cleanly outside their
# build variant, or are unsafe from an app context). Add here as builds reveal.
# Declared in a public header but NOT defined/linkable in this build variant
# (transport, SPI-slave, cmd-handler, recovery, SD-manager internals). The build
# rejects these — they aren't real app-callable functions, so they can't be
# exported. Surfaced as "undefined reference" and added here.
EXCLUDE_NAMES = {
    "radio_patch_reinit",
    "cmd_handler_dispatch", "cmd_handler_log_pending", "cmd_handler_push_log",
    "fs_save_file_safe", "get_power_status", "m1_fb_popup",
    "m1_recovery_screen_show", "m1_sdm_close_file", "m1_sdm_close_files",
    "power_test_charger", "spi_slave_assert_data_ready",
    "spi_slave_deassert_data_ready", "spi_slave_get_handle",
    "spi_slave_notify_log_available", "spi_slave_process",
    "spi_slave_send_error", "spi_slave_send_response",
    "usart_get_rx_data_length", "usb_rxdata_process",
}

# Built-in feature handlers that are now delivered as APPS (primitive-based
# reimplementations) and were removed from their menus. Nothing else references
# them, so exporting was the only thing pinning their code — dropping them lets
# --gc-sections reclaim it. Verified against nm -u over every .m1app: no app
# calls these (the apps use the m1_nfc_* / lfrfid primitives instead). If a
# future app needs one, it becomes a thin-wrapper target and gets re-added.
EXCLUDE_NAMES |= {
    "nfc_detect_reader",   # -> Field Detector app
    "nfc_extra_actions",   # -> NFC Write / Unlock / Emulate / Fuzzer apps
    "nfc_add_manually",    # -> NFC Create app
    "nfc_tools",           # -> the individual NFC apps
    "rfid_125khz_utilities",  # -> RFID Erase / T5577 Info / Fuzzer / Brute apps
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


# Prototypes touching these firmware-internal types are exported (FW .inc) but
# left out of the app-safe SDK header, since apps don't have the type defs.
FW_TYPE_RE = re.compile(
    r"\bS_M1_\w+|\bLFRFID\w*|\bir_file_\w+|\bff_file_t\b|\bFIL\b|\bFILINFO\b|"
    r"\bu8g2_\w+|\bIRMP_DATA\b|\bir_universal_\w+|\brfid_\w+_t\b|"
    r"\bsubghz_\w+_t\b|\bS_RPC\w*|\bGPIO_\w+"
)

# A prototype statement, once whitespace-collapsed: <ret...> name ( args )
_PROTO = re.compile(
    r"^(?:extern\s+)?"
    r"[A-Za-z_][\w\s\*]*?[\s\*]"     # return type (ends in space or '*')
    r"([A-Za-z_]\w*)\s*"             # function name  (group 1)
    r"\(([^()]*)\)$"                 # ( args ) with no nested parens
)


def parse_header(path):
    """Return [(name, prototype)] for the public functions declared in a header.
    `prototype` is the cleaned, single-line `ret name(args)` (no trailing ;)."""
    raw = open(path, encoding="utf-8", errors="replace").read()
    raw = strip_comments(raw)
    raw = re.sub(r"^\s*#.*$", " ", raw, flags=re.M)   # drop preprocessor lines
    raw = re.sub(r"\s+", " ", raw)                     # collapse whitespace
    out = []
    for stmt in raw.split(";"):
        stmt = stmt.strip()
        if not stmt or stmt.startswith("typedef") or "{" in stmt:
            continue
        m = _PROTO.match(stmt)
        if not m:
            continue
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        out.append((name, stmt))
    return out


def existing_manual_names():
    """Names already listed by hand in m1_app_api.c entries[] (to dedup)."""
    txt = open(API_C, encoding="utf-8", errors="replace").read()
    return set(re.findall(r'\{\s*"([A-Za-z_]\w*)"\s*,', txt))


def write_if_changed(path, content, quiet):
    """Write content only if it differs from what's on disk. Keeps the file's
    mtime stable when nothing changed, so build integration doesn't force a
    recompile every build. Returns True if written."""
    try:
        if open(path, encoding="utf-8").read() == content:
            if not quiet:
                print(f"  unchanged: {os.path.basename(path)}")
            return False
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"  wrote {path}")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be exported; write nothing")
    ap.add_argument("--sdk-header", default=SDK_HDR_DEFAULT,
                    help="path to write the generated SDK extern header")
    ap.add_argument("--quiet", action="store_true",
                    help="only print on change/warning (for build integration)")
    args = ap.parse_args()

    # Build the header list: every .h in SCAN_DIRS minus non-API internals,
    # plus the explicit EXTRA_HEADERS.
    headers = []
    for d in SCAN_DIRS:
        dpath = os.path.join(ROOT, d)
        for fn in sorted(os.listdir(dpath)):
            if fn.endswith(".h") and not HEADER_EXCLUDE_RE.match(fn):
                headers.append(os.path.join(d, fn).replace("\\", "/"))
    headers += EXTRA_HEADERS

    manual = existing_manual_names()
    seen = set()
    exported = []          # (name, header, prototype) preserving discovery order
    skipped_excluded = []
    per_header = {}

    for hdr in headers:
        path = os.path.join(ROOT, hdr)
        if not os.path.isfile(path):
            print(f"  WARN: missing header {hdr}", file=sys.stderr)
            continue
        count = 0
        for name, proto in parse_header(path):
            if name in seen:
                continue
            seen.add(name)
            if name in manual:
                continue                       # already hand-exported
            if name in EXCLUDE_NAMES or EXCLUDE_RE.search(name):
                skipped_excluded.append(name)
                continue
            exported.append((name, hdr, proto))
            count += 1
        per_header[hdr] = count

    if not args.quiet:
        print("=== app_api_gen ===")
        print(f"  headers scanned       : {len(headers)}")
        print(f"  already hand-exported : {len(manual)}")
        for hdr in headers:
            if per_header.get(hdr):
                print(f"  {hdr:34s} +{per_header[hdr]}")
        print(f"  NEW auto-exported     : {len(exported)}")
        print(f"  excluded by rule      : {len(skipped_excluded)}")
        print(f"  => total table size    : {len(manual) + len(exported)}")

    if args.dry_run:
        print("\n  (dry run — nothing written)")
        print("  new symbols:")
        for name, hdr, _proto in exported:
            print(f"    {name}  [{hdr}]")
        return 0

    # --- FW table include (build content, write only if changed) -------------
    inc = ["/* AUTO-GENERATED by tools/app_api_gen.py — DO NOT EDIT.",
           " * Regenerate after changing a public header. Entries here are",
           " * merged into entries[] in m1_app_api.c. */"]
    inc += [f'    {{ "{name}", (void *){name} }},' for name, _h, _p in exported]
    write_if_changed(GEN_INC, "\n".join(inc) + "\n", args.quiet)

    # --- include list so m1_app_api.c has every scanned header's decls ---------
    used_hdrs = []
    for _n, h, _p in exported:
        if h not in used_hdrs:
            used_hdrs.append(h)
    incs = ["/* AUTO-GENERATED by tools/app_api_gen.py — DO NOT EDIT. */"]
    incs += [f'#include "{os.path.basename(h)}"' for h in used_hdrs]
    write_if_changed(os.path.join(CSRC, "m1_app_api_includes.inc"),
                     "\n".join(incs) + "\n", args.quiet)

    # --- SDK extern header (skipped gracefully if the SDK tree isn't present) -
    sdk_dir = os.path.dirname(args.sdk_header)
    if not os.path.isdir(sdk_dir):
        if not args.quiet:
            print(f"  SDK dir absent, skipping SDK header ({sdk_dir})")
        return 0

    hdr_lines = [
        "/* AUTO-GENERATED by tools/app_api_gen.py — DO NOT EDIT.",
        " * Every app-facing firmware symbol, exported by name. */",
        "#ifndef M1APP_GENERATED_H_", "#define M1APP_GENERATED_H_", "",
        "/* Every symbol below is exported by the firmware and callable from an",
        " * app. Functions whose signatures use firmware-internal types (S_M1_*,",
        " * LFRFID_*, ir_file_*, FatFS) are exported too but omitted here —",
        " * declare their prototype in your app to call them. */",
        "#include <stdint.h>", "#include <stdbool.h>", "#include <stddef.h>",
    ]
    cur, omitted = None, 0
    for name, hdr, proto in exported:
        if FW_TYPE_RE.search(proto):        # app doesn't have these types
            omitted += 1
            continue
        if hdr != cur:
            hdr_lines.append(f"\n/* --- {hdr} --- */")
            cur = hdr
        hdr_lines.append((proto if proto.startswith("extern ") else "extern " + proto) + ";")
    hdr_lines += ["", "#endif /* M1APP_GENERATED_H_ */"]
    write_if_changed(args.sdk_header, "\n".join(hdr_lines) + "\n", args.quiet)
    if not args.quiet:
        print(f"  ({omitted} of {len(exported)} omitted from SDK header: FW-type signatures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
