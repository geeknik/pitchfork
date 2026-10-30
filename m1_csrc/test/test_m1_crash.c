/* test_m1_crash.c — host tests for the pure crash-record core
 * (m1_crash_core.c). Run: make -C m1_csrc/test test-crash
 *
 * Covers the record format (stable keys, crash-loop marker, truncation
 * contract) and the bounded numbered-ring slot policy against the RAM-FatFs
 * stub: first-free fill, no overwrite until the ring is full, then
 * round-robin after the last write.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"                 /* stub */
#include "m1_crash_core.h"

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

static bool slot_exists_fs(int slot)
{
    char path[40];
    snprintf(path, sizeof(path), "0:/System/crash/crash_%03d.txt", slot);
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

static void put_slot(int slot)
{
    char path[40];
    snprintf(path, sizeof(path), "0:/System/crash/crash_%03d.txt", slot);
    hostff_mkdir_raw("0:/System");
    hostff_mkdir_raw("0:/System/crash");
    hostff_put(path, (const uint8_t *)"x", 1);
}

static void test_format(void)
{
    char buf[512];
    m1_crash_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.kind = M1_CRASH_KIND_HARDFAULT;
    strcpy(ci.fw, "FW 1.1.83.0");
    ci.pc = 0x08012345; ci.lr = 0x08009ABC;
    ci.cfsr = 0x00008200; ci.bfar = 0x2000FF00; ci.hfsr = 0x40000000;
    ci.uptime_ms = 91000;
    ci.seq = 1;
    ci.rsr = 0x0000E000;
    strcpy(ci.app, "signalm");

    size_t n = m1_crash_format(buf, sizeof(buf), &ci);
    CHECK(n > 0);
    CHECK(strstr(buf, "kind=hardfault") != NULL);
    CHECK(strstr(buf, "fw=FW 1.1.83.0") != NULL);
    CHECK(strstr(buf, "pc=0x08012345") != NULL);
    CHECK(strstr(buf, "cfsr=0x00008200") != NULL);
    CHECK(strstr(buf, "uptime_ms=91000") != NULL);
    CHECK(strstr(buf, "seq=1\n") != NULL);            /* no CRASH_LOOP at 1 */
    CHECK(strstr(buf, "CRASH_LOOP") == NULL);
    CHECK(strstr(buf, "app=signalm") != NULL);

    ci.seq = M1_CRASH_LOOP_THRESHOLD;
    n = m1_crash_format(buf, sizeof(buf), &ci);
    CHECK(n > 0);
    CHECK(strstr(buf, "CRASH_LOOP") != NULL);

    ci.kind = M1_CRASH_KIND_WATCHDOG;
    ci.seq = 1;
    n = m1_crash_format(buf, sizeof(buf), &ci);
    CHECK(strstr(buf, "kind=watchdog") != NULL);

    ci.kind = M1_CRASH_KIND_HUNG;
    n = m1_crash_format(buf, sizeof(buf), &ci);
    CHECK(strstr(buf, "kind=hung") != NULL);
    CHECK(strstr(buf, "app=") != NULL);

    /* truncation contract: too-small buffer reports 0, never half a record */
    char tiny[16];
    CHECK(m1_crash_format(tiny, sizeof(tiny), &ci) == 0);
    CHECK(m1_crash_format(NULL, sizeof(buf), &ci) == 0);
    CHECK(m1_crash_format(buf, sizeof(buf), NULL) == 0);
}

static void test_slots(void)
{
    hostff_reset();

    /* empty ring: first free = 1, growing sequentially */
    CHECK(m1_crash_next_slot(0, slot_exists_fs) == 1);
    put_slot(1);
    CHECK(m1_crash_next_slot(1, slot_exists_fs) == 2);
    put_slot(2);

    /* partial ring keeps filling holes: 1..3 exist -> 4 */
    put_slot(3);
    CHECK(m1_crash_next_slot(3, slot_exists_fs) == 4);

    /* gap fills the first free slot */
    put_slot(5);
    CHECK(m1_crash_next_slot(5, slot_exists_fs) == 4);

    /* full ring: round-robin right after the last write */
    hostff_reset();
    for (int s = 1; s <= M1_CRASH_MAX_FILES; s++)
        put_slot(s);
    CHECK(m1_crash_next_slot(M1_CRASH_MAX_FILES, slot_exists_fs) == 1);
    CHECK(m1_crash_next_slot(7, slot_exists_fs) == 8);
    CHECK(m1_crash_next_slot(0, slot_exists_fs) == 1);
}

int main(void)
{
    test_format();
    test_slots();
    if (g_failures == 0)
        printf("test_m1_crash: ALL PASS\n");
    else
        printf("test_m1_crash: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
