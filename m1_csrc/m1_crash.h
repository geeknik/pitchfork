/* See COPYING.txt for license details. */

/*
 * m1_crash.h — device-side crash recorder.
 *
 * Flow:
 *   1. m1_fault_capture() (stm32h5xx_it.c) stashes the fault post-mortem in
 *      TAMP BKP2R..BKP7R (existing) plus uptime + consecutive-fault count in
 *      BKP13R/BKP14R (new), then resets.
 *   2. m1_crash_boot_capture() runs at the very start of m1_wdt_init() —
 *      BEFORE m1_wdt_init reads and clears RCC->RSR — and snapshots the raw
 *      reset flags + the TAMP record into a pending buffer.
 *   3. After the first successful SD mount, m1_crash_persist_if_pending()
 *      writes one bounded record (0:/System/crash/crash_NNN.txt, ring of 32)
 *      and clears the fault magic so a plain remount never double-writes.
 *
 * Watchdog resets (no fault regs — the device just froze) are recorded too,
 * using the captured RSR flags: "it froze" becomes a concrete record with
 * uptime and reset cause.
 *
 * m1_crash_note_app() lets the app manager stash the running app's name into
 * BKP15R/BKP16R so the record names the likely culprit.
 */

#ifndef M1_CRASH_H
#define M1_CRASH_H

#include <stdbool.h>
#include <stdint.h>

void m1_crash_boot_capture(void);         /* before RSR is cleared, once */
void m1_crash_persist_if_pending(void);   /* after SD mounts; idempotent */

/* Pack the first 8 chars of the running app's name into TAMP BKP15/16R
 * ("" clears). Call on app start/exit; costs two register writes. */
void m1_crash_note_app(const char *app_name);

/* True when the pending buffer holds an unrecorded fault/watchdog event. */
bool m1_crash_pending(void);

/* Record a runtime-detected event (e.g. a contained hung app) into the same
 * bounded ring, independent of the boot-time capture. Any task may call it:
 * all buffers are static (boot-path stack rule). */
void m1_crash_record_runtime(const m1_crash_info_t *ci);

#endif /* M1_CRASH_H */
