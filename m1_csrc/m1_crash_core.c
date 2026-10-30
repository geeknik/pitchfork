/* See COPYING.txt for license details. */

/* m1_crash_core.c — pure crash-record logic. See m1_crash_core.h. */

#include <stdio.h>
#include <string.h>
#include "m1_crash_core.h"

static const char *kind_str(m1_crash_kind_t k)
{
    switch (k) {
    case M1_CRASH_KIND_HARDFAULT: return "hardfault";
    case M1_CRASH_KIND_WATCHDOG:  return "watchdog";
    case M1_CRASH_KIND_HUNG:      return "hung";
    default:                      return "unknown";
    }
}

size_t m1_crash_format(char *buf, size_t cap, const m1_crash_info_t *ci)
{
    if (buf == NULL || cap == 0 || ci == NULL)
        return 0;

    int n = snprintf(buf, cap,
        "crash record\n"
        "kind=%s\n"
        "fw=%s\n"
        "pc=0x%08lX\n"
        "lr=0x%08lX\n"
        "cfsr=0x%08lX\n"
        "bfar=0x%08lX\n"
        "hfsr=0x%08lX\n"
        "uptime_ms=%lu\n"
        "seq=%lu%s\n"
        "rsr=0x%08lX\n"
        "app=%.8s\n",
        kind_str(ci->kind), ci->fw,
        (unsigned long)ci->pc, (unsigned long)ci->lr,
        (unsigned long)ci->cfsr, (unsigned long)ci->bfar,
        (unsigned long)ci->hfsr,
        (unsigned long)ci->uptime_ms,
        (unsigned long)ci->seq,
        (ci->seq >= M1_CRASH_LOOP_THRESHOLD) ? " CRASH_LOOP" : "",
        (unsigned long)ci->rsr, ci->app);

    if (n < 0)
        return 0;
    if ((size_t)n >= cap)
        return 0;                       /* truncated — caller retries larger */
    return (size_t)n;
}

int m1_crash_next_slot(int last_used, bool (*exists)(int slot))
{
    /* First free slot wins: a partially-filled ring never overwrites. */
    for (int s = 1; s <= M1_CRASH_MAX_FILES; s++) {
        if (!exists || !exists(s))
            return s;
    }
    /* Ring full: overwrite round-robin, starting right after the last write
     * so the oldest records go first. */
    int next = (last_used % M1_CRASH_MAX_FILES) + 1;
    return next;
}
