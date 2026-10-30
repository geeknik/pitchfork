# m1ctl — headless control for the Pitchfork M1

Stdlib-only Python CLI speaking the device's USB-CDC RPC directly. This is
the companion tooling the project docs promise, developed in-repo (its
protocol notes are the reference; nothing here lives outside git).

    ./m1ctl ping                      # liveness
    ./m1ctl info                      # firmware + ESP link status
    ./m1ctl capture start --chan 6 --ms 15000
    ./m1ctl capture stats
    ./m1ctl capture stop              # also the stuck-monitor recovery path
    ./m1ctl pull /wifi/mcap_000.pcapng out.pcapng
    ./m1ctl ls /wifi
    ./m1ctl doctor                    # ping + info + recovery stop
    ./m1ctl selftest                  # offline: crc vector, framing, resync

## RPC wire notes (cracked 2026-10-30, enforced by selftest)

- Frame `[0xAA][CMD:1][SEQ:1][LEN:2 LE][PAYLOAD][CRC16:2 LE]`; CRC is
  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, non-reflected) over
  CMD+SEQ+LEN+PAYLOAD — **the sync byte is excluded**.
- Replies match by SEQ and may stream back-to-back (FILE_READ sends the
  whole file as sequential frames). The parser retains unconsumed tails —
  a trailing byte may be an incoming frame's first byte.
- The device boots its CDC in VCP/RPC mode (`M1_APP_RPC_ENABLE`); the
  serial CLI does not exist in these builds.
- The ESP link is lazy-initialized: the first radio command after boot can
  take seconds — `capture start` uses a 15 s window.
- `CAPTURE_STOP` clears a brain stuck in monitor mode even with no local
  session (1.1.91+); issue it when `capture start` returns rc=11.
- Error encoding for `capture start`: rc = 0 ok, 11 = radio failed,
  12 = storage failed, 13 = already active.

Command IDs live in `m1_csrc/m1_rpc.h`; the capture engine in
`m1_csrc/m1_capture_source.c`. Extend this tool in-repo — /tmp is not a
development environment.
