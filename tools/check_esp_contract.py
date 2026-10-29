#!/usr/bin/env python3
"""
check_esp_contract.py — wire-contract parity gate for the M1OS <-> m1-esp32-brain SPI link.

WHY THIS EXISTS
    m1_csrc/m1_esp_rpc.h is a namespace-renamed sed-copy of brain's canonical
    components/m1_rpc/include/m1_rpc.h (M1_RPC_* -> M1ESP_*, m1_rpc* -> m1esp*).
    The two firmwares are built in separate repos with no shared test, so a edit
    on either side silently desyncs the SPI wire contract until a device session
    dies at runtime. This script fails the build/CI the moment the copies drift.

WHAT IT CHECKS
    1. Every #define in either header exists on the other side with the same
       (macro-expanded) value: magic, version, MTU, opcodes, error codes, caps.
    2. The comment-stripped, namespace-normalized CODE text of both headers is
       identical (catches struct layout edits, enum renumbering, field reorder,
       added/removed params -- anything a constant diff alone would miss).

 Comments are stripped first so prose edits (em-dash reflows, path renames in
 doc lines) do not mask or fake drift. Exit codes: 0 = in sync, 1 = drift,
 2 = brain header not found (treated as skip when --optional).
"""

import argparse
import difflib
import os
import re
import sys

M1OS_HEADER_DEFAULT = os.path.join("m1_csrc", "m1_esp_rpc.h")
BRAIN_HEADER_DEFAULT = os.path.join(
    "..", "m1-esp32-brain", "components", "m1_rpc", "include", "m1_rpc.h")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def normalize_per_line(text):
    """Reverse the sed rename, line by line, whitespace-collapsed per line."""
    text = text.replace("M1ESP", "M1_RPC").replace("m1esp", "m1_rpc")
    return [re.sub(r"\s+", " ", ln).strip() for ln in text.splitlines() if ln.strip()]


def extract_defines(code):
    """name -> raw value for simple #define NAME VALUE lines (no continuation)."""
    defs = {}
    for m in re.finditer(r"#\s*define\s+(\w+)[ \t]+([^\n]+)", code):
        name, val = m.group(1), m.group(2).strip().rstrip("/*").strip()
        defs[name] = val
    return defs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--m1os", default=M1OS_HEADER_DEFAULT,
                    help="M1OS copy (default: %(default)s)")
    ap.add_argument("--brain", default=BRAIN_HEADER_DEFAULT,
                    help="brain canonical header (default: %(default)s; override"
                         " with env M1_BRAIN_HEADER)")
    ap.add_argument("--optional", action="store_true",
                    help="exit 0 with a notice when the brain header is absent")
    args = ap.parse_args()

    brain_path = os.environ.get("M1_BRAIN_HEADER", args.brain)
    if not os.path.isfile(args.m1os):
        print(f"error: M1OS header not found: {args.m1os}", file=sys.stderr)
        return 1
    if not os.path.isfile(brain_path):
        lvl = "notice" if args.optional else "error"
        print(f"{lvl}: brain header not found: {brain_path}\n"
              f"        Clone m1-esp32-brain next to M1OS or set M1_BRAIN_HEADER.",
              file=sys.stderr if not args.optional else sys.stdout)
        return 0 if args.optional else 2

    with open(args.m1os, encoding="utf-8", errors="replace") as f:
        m1os_code = normalize_per_line(strip_comments(f.read()))
    with open(brain_path, encoding="utf-8", errors="replace") as f:
        brain_code = normalize_per_line(strip_comments(f.read()))

    drift = False

    # --- Constant-level diff (precise, name-by-name) ---
    m1os_defs = extract_defines("\n".join(m1os_code))
    brain_defs = extract_defines("\n".join(brain_code))
    for name in sorted(set(m1os_defs) - set(brain_defs)):
        print(f"DRIFT: define {name} exists only in M1OS ({m1os_defs[name]})")
        drift = True
    for name in sorted(set(brain_defs) - set(m1os_defs)):
        print(f"DRIFT: define {name} exists only in brain ({brain_defs[name]})")
        drift = True
    for name in sorted(set(m1os_defs) & set(brain_defs)):
        if m1os_defs[name] != brain_defs[name]:
            print(f"DRIFT: define {name}: M1OS={m1os_defs[name]} brain={brain_defs[name]}")
            drift = True

    # --- Full code diff (catches structs/enums/typedefs) ---
    if m1os_code != brain_code:
        diff = list(difflib.unified_diff(
            brain_code, m1os_code,
            fromfile="brain/m1_rpc.h", tofile="M1OS/m1_esp_rpc.h", lineterm=""))
        print("\n".join(diff[:80]))
        if len(diff) > 80:
            print(f"... ({len(diff) - 80} more diff lines)")
        drift = True

    if drift:
        print("FAIL: m1_esp_rpc.h has drifted from brain's m1_rpc.h — the SPI wire"
              " contract is out of sync. Regenerate the copy (see header line 1)"
              " or reconcile the change on both sides.")
        return 1
    print("OK: m1_esp_rpc.h matches brain's m1_rpc.h (constants + code).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
