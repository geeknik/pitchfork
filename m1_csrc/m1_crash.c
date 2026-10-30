/* See COPYING.txt for license details. */

/* m1_crash.c — device-side crash recorder. See m1_crash.h for the flow.
 * Pure formatting/slot logic lives in m1_crash_core.c (host-tested). */

/*************************** I N C L U D E S **********************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32h5xx_hal.h"
#include "ff.h"
#include "m1_log_debug.h"
#include "m1_fw_update_bl.h"      /* FW_VERSION_* */
#include "m1_crash.h"
#include "m1_crash_core.h"

/*************************** D E F I N E S ************************************/

#define TAG "CRASH"

#define M1_CRASH_MAGIC          0xFA017C0DU   /* set by m1_fault_capture */
#define M1_CRASH_DIR            "0:/System/crash"
#define M1_CRASH_SLOT_FMT       "0:/System/crash/crash_%03d.txt"
#define M1_CRASH_BUF_MAX        384   /* record fits in ~330 bytes; 384 gives
                                       * headroom without a bloated buffer */

/* TAMP layout: BKP2R..BKP7R = fault post-mortem (stm32h5xx_it.c);
 * BKP8R..BKP12R = SPI-link breadcrumbs / flash-stall markers;
 * BKP13R = tick at fault, BKP14R = consecutive-fault count,
 * BKP15R/BKP16R = running app name (8 chars packed LE). */

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static bool slot_exists(int slot);
static int  read_last_slot(void);

/*************************** D A T A *******************************************/

static struct {
    m1_crash_info_t info;
    bool            captured;   /* boot_capture ran */
    bool            persisted;  /* written (or nothing to write) */
} s_pending;

/*************************** C O D E ******************************************/

void m1_crash_boot_capture(void)
{
    if (s_pending.captured)
        return;
    s_pending.captured = true;

    memset(&s_pending.info, 0, sizeof(s_pending.info));
    snprintf(s_pending.info.fw, sizeof(s_pending.info.fw), "%d.%d.%d.%d",
             FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_BUILD, FW_VERSION_RC);

    /* Raw reset-status flags; m1_wdt_init clears them right after us. */
    s_pending.info.rsr = (uint32_t)RCC->RSR;

    /* Fault post-mortem, if the previous reset was a fault. */
    if (TAMP->BKP2R == M1_CRASH_MAGIC) {
        s_pending.info.kind   = M1_CRASH_KIND_HARDFAULT;
        s_pending.info.pc     = TAMP->BKP3R;
        s_pending.info.lr     = TAMP->BKP4R;
        s_pending.info.cfsr   = TAMP->BKP5R;
        s_pending.info.bfar   = TAMP->BKP6R;
        s_pending.info.hfsr   = TAMP->BKP7R;
        s_pending.info.uptime_ms = TAMP->BKP13R;
        s_pending.info.seq    = TAMP->BKP14R;
        uint32_t lo = TAMP->BKP15R, hi = TAMP->BKP16R;
        memcpy(s_pending.info.app, &lo, 4);
        memcpy(s_pending.info.app + 4, &hi, 4);
        s_pending.info.app[8] = '\0';
    } else if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) {
        /* No fault regs — the device froze until the watchdog bit. */
        s_pending.info.kind = M1_CRASH_KIND_WATCHDOG;
        s_pending.info.seq  = TAMP->BKP14R;
    }
}

bool m1_crash_pending(void)
{
    return s_pending.captured && !s_pending.persisted &&
           s_pending.info.kind != M1_CRASH_KIND_NONE;
}

void m1_crash_note_app(const char *app_name)
{
    uint32_t lo = 0, hi = 0;
    if (app_name != NULL) {
        const char *p = app_name;
        for (int i = 0; i < 4 && p[i]; i++)
            ((char *)&lo)[i] = p[i];
        for (int i = 0; i < 4 && p[4 + i]; i++)
            ((char *)&hi)[i] = p[4 + i];
    }
    PWR->DBPCR |= PWR_DBPCR_DBP;      /* same idiom as m1_fault_capture */
    TAMP->BKP15R = lo;
    TAMP->BKP16R = hi;
}

static bool slot_exists(int slot)
{
    static char path[40];
    static FILINFO fi;
    snprintf(path, sizeof(path), M1_CRASH_SLOT_FMT, slot);
    return f_stat(path, &fi) == FR_OK;
}

/* Newest record = highest existing slot number. */
static int read_last_slot(void)
{
    int last = 0;
    for (int s = 1; s <= M1_CRASH_MAX_FILES; s++)
        if (slot_exists(s))
            last = s;
    return last;
}

void m1_crash_persist_if_pending(void)
{
    if (!m1_crash_pending())
        return;

    /* BOOT-PATH STACK RULE: m1_sdcard_mount calls this on a small-stack init
     * task; the first cut used ~1.1KB of stack here (512B buf + FIL) and
     * stack-overflowed the device into a hardfault boot loop at 461ms. All
     * large locals below are static — .bss is cheap, boot-task stack is not. */
    static char buf[M1_CRASH_BUF_MAX];
    size_t len = m1_crash_format(buf, sizeof(buf), &s_pending.info);
    if (len == 0) {
        M1_LOG_E(TAG, "record did not fit %d bytes", M1_CRASH_BUF_MAX);
        s_pending.persisted = true;   /* do not retry forever on format bugs */
        return;
    }

    f_mkdir("0:/System");
    FRESULT fr = f_mkdir(M1_CRASH_DIR);
    if (fr != FR_OK && fr != FR_EXIST) {
        M1_LOG_E(TAG, "cannot create %s (fr=%d)", M1_CRASH_DIR, (int)fr);
        return;                        /* keep pending; next mount retries */
    }

    int slot = m1_crash_next_slot(read_last_slot(), slot_exists);
    static char path[40];
    snprintf(path, sizeof(path), M1_CRASH_SLOT_FMT, slot);

    static FIL f;      /* ~0.5KB with LFN enabled — never on this stack */
    fr = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        M1_LOG_E(TAG, "cannot open %s (fr=%d)", path, (int)fr);
        return;
    }
    UINT written = 0;
    fr = f_write(&f, buf, len, &written);
    if (fr == FR_OK)
        fr = f_sync(&f);
    f_close(&f);

    if (fr == FR_OK && written == len) {
        M1_LOG_W(TAG, "crash recorded: %s (kind=%d seq=%lu)",
                 path, (int)s_pending.info.kind,
                 (unsigned long)s_pending.info.seq);
        s_pending.persisted = true;

        /* Clear the fault magic + sequence so a later remount or a clean
         * reboot never re-records this event. */
        PWR->DBPCR |= PWR_DBPCR_DBP;
        if (TAMP->BKP2R == M1_CRASH_MAGIC)
            TAMP->BKP2R = 0;
        TAMP->BKP14R = 0;
    } else {
        M1_LOG_E(TAG, "write failed (fr=%d written=%lu/%lu)",
                 (int)fr, (unsigned long)written, (unsigned long)len);
        /* leave pending + magic intact: next mount retries once */
    }
}
