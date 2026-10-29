/* test_m1_esp_lease.c — host tests for the ESP-channel operation lease
 * (m1_esp_lease.c). Run: make -C m1_csrc/test test-lease
 *
 * Registry semantics under test: ownership binding (only the owner untracks),
 * re-start replacement, table-full refusal, revoke calling each stop exactly
 * once, and the re-entrancy guard (a stop that untracks itself must be a
 * no-op, not a recursion or a double-release).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"           /* stub */
#include "task.h"               /* stub */
#include "m1_esp_lease.h"

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

static int t_a, t_b;

/* The tests provide the stop-op hook (on device it is the typed dispatch in
 * m1_esp_client.c). It records which opcode it stopped; 0x0300's handler
 * re-enters untrack like the real client stop functions do. */
#define STOP_LOG_MAX 20
static int g_stops[STOP_LOG_MAX];
static int g_stop_calls;
static void stop_log(int which)
{
    if (g_stop_calls < STOP_LOG_MAX)
        g_stops[g_stop_calls] = which;
    g_stop_calls++;
}
void m1_esp_lease_stop_op(uint16_t op)
{
    if (op == 0x0300) {
        g_stop_calls++;
        m1_esp_lease_stopped_by(0x0300, &t_a);   /* safe no-op: already removed */
        return;
    }
    stop_log((int)op);
}

static void reset(void)
{
    /* drain everything: revoke under both owners until empty */
    m1_esp_lease_revoke(&t_a);
    m1_esp_lease_revoke(&t_b);
    g_stop_calls = 0;
    memset(g_stops, 0, sizeof(g_stops));
}

int main(void)
{
    /* --- registration + ownership --- */
    reset();
    CHECK(m1_esp_lease_count() == 0);
    CHECK(m1_esp_lease_started_by(0x0302, &t_a));
    CHECK(m1_esp_lease_has(0x0302));
    CHECK(m1_esp_lease_count() == 1);

    /* non-owner cannot untrack */
    CHECK(m1_esp_lease_stopped_by(0x0302, &t_b) == false);
    CHECK(m1_esp_lease_has(0x0302));
    /* owner can */
    CHECK(m1_esp_lease_stopped_by(0x0302, &t_a));
    CHECK(!m1_esp_lease_has(0x0302));

    /* NULL owner is refused (never a lease token) */
    CHECK(m1_esp_lease_started_by(0x0302, NULL) == false);

    /* --- re-start replaces the previous entry --- */
    CHECK(m1_esp_lease_started_by(0x0302, &t_a));
    CHECK(m1_esp_lease_started_by(0x0302, &t_b));   /* now B owns it */
    CHECK(m1_esp_lease_stopped_by(0x0302, &t_a) == false);     /* A does not */
    CHECK(m1_esp_lease_stopped_by(0x0302, &t_b));
    CHECK(m1_esp_lease_count() == 0);

    /* --- table-full refusal (safety: START callers must fail closed) --- */
    reset();
    uint16_t ops[16];
    for (int i = 0; i < 16; i++) {
        ops[i] = (uint16_t)(0x100 + i);
        CHECK(m1_esp_lease_started_by(ops[i], &t_a));
    }
    CHECK(m1_esp_lease_started_by(0x9999, &t_a) == false);
    CHECK(m1_esp_lease_count() == 16);
    reset();
    CHECK(m1_esp_lease_count() == 0);

    /* --- revoke stops exactly the owner's ops, once each --- */
    reset();
    CHECK(m1_esp_lease_started_by(0x01, &t_a));
    CHECK(m1_esp_lease_started_by(0x02, &t_a));
    CHECK(m1_esp_lease_started_by(0x03, &t_b));     /* B unaffected */
    CHECK(m1_esp_lease_revoke(&t_a) == 2);
    CHECK(g_stop_calls == 2);
    CHECK((g_stops[0] == 0x01 && g_stops[1] == 0x02) || (g_stops[0] == 0x02 && g_stops[1] == 0x01));
    CHECK(m1_esp_lease_has(0x03));                              /* B's op lives */
    CHECK(m1_esp_lease_revoke(&t_a) == 0);                      /* idempotent */
    CHECK(m1_esp_lease_revoke(&t_b) == 1);
    CHECK(m1_esp_lease_count() == 0);

    /* --- re-entrant stop is a safe no-op --- */
    reset();
    CHECK(m1_esp_lease_started_by(0x0300, &t_a));
    CHECK(m1_esp_lease_revoke(&t_a) == 1);
    CHECK(g_stop_calls == 1);
    CHECK(m1_esp_lease_count() == 0);

    if (g_failures == 0)
        printf("test_m1_esp_lease: ALL PASS\n");
    else
        printf("test_m1_esp_lease: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
