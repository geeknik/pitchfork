/* See COPYING.txt for license details. */

/*
 * m1_crash_core.h — pure logic of the crash recorder (host-testable).
 *
 * The device half (m1_crash.c) reads TAMP/RCC and writes the SD file; this
 * half formats the record and picks the next bounded slot so the policy is
 * testable without hardware:
 *
 *   - m1_crash_format(): renders one human-readable record (one field per
 *     line, stable key=value keys).
 *   - m1_crash_next_slot(): numbered-ring selection, 1..M1_CRASH_MAX_FILES.
 *     Fills the first free slot; when all are taken, wraps around to
 *     overwrite the oldest (slot == last_used % MAX + 1 via scanning), so a
 *     long-running device keeps the most recent records without unbounded
 *     growth or mass renames.
 */

#ifndef M1_CRASH_CORE_H
#define M1_CRASH_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M1_CRASH_MAX_FILES 32

typedef enum {
    M1_CRASH_KIND_NONE = 0,
    M1_CRASH_KIND_HARDFAULT,
    M1_CRASH_KIND_WATCHDOG,
    M1_CRASH_KIND_HUNG,        /* supervised app contained by the manager */
} m1_crash_kind_t;

typedef struct {
    m1_crash_kind_t kind;
    uint32_t pc, lr, cfsr, bfar, hfsr;   /* fault post-mortem (0 when absent) */
    uint32_t uptime_ms;                  /* tick at fault (0 when unknown) */
    uint32_t seq;                        /* consecutive faults w/o clean boot */
    uint32_t rsr;                        /* raw RCC reset-status flags */
    char     app[9];                     /* first 8 chars of running app, "" */
    char     fw[16];                     /* FW_BUILD_LABEL */
} m1_crash_info_t;

/* Render into buf (NUL-terminated); returns bytes written (may be 0 if cap
 * too small — call again with a bigger buffer; needs <= 512). */
size_t m1_crash_format(char *buf, size_t cap, const m1_crash_info_t *ci);

/* Bounded numbered-ring slot selection. exists(slot) reports whether
 * crash_<slot>.txt exists (device: f_stat; tests: RAM FS). last_used is the
 * previously written slot (0 = none). Returns the slot to write now. */
int m1_crash_next_slot(int last_used, bool (*exists)(int slot));

/* Consecutive-fault policy: >= this many faults without a clean boot in
 * between is treated as a crash loop. m1_crash_format marks it. */
#define M1_CRASH_LOOP_THRESHOLD 3

#endif /* M1_CRASH_CORE_H */
