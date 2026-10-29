/* test_m1_ui.c — host tests for the UI umbrella (m1_app_ui.c) and the
 * display-focus lease registry (m1_ui_focus.c). Run:
 *   make -C m1_csrc/test test-ui
 *
 * The lease is pure pointer-token logic; the umbrella is compiled from the
 * REAL firmware source with the display hooks substituted by stubs below
 * (the real hooks live in m1_app_api.c next to u8g2). "Which task am I" is
 * steered through g_host_current_task.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"           /* stub */
#include "task.h"               /* stub */
#include "m1_app_syscall.h"
#include "m1_ui_focus.h"

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

/* ---- substituted display hooks ---- */
static int      g_flush_calls;
static uint8_t  g_flush_ret = 7;
static int      g_notify_calls;
static char     g_notify_msg[80];
static uint16_t g_notify_ms;

uint8_t m1_ui_hook_flush(void) { g_flush_calls++; return g_flush_ret; }
void m1_ui_hook_notify(const char *msg, uint16_t ms)
{
    g_notify_calls++;
    snprintf(g_notify_msg, sizeof(g_notify_msg), "%s", msg);
    g_notify_ms = ms;
}

static int task_a, task_b;      /* stable distinct tokens */

static void fill_hdr(m1_call_hdr_t *hdr, uint16_t size)
{
    memset(hdr, 0, size);
    hdr->size = size;
    hdr->version = M1_CALL_LAYOUT_V1;
}

static void test_focus_registry(void)
{
    CHECK(m1_ui_focus_is_free());
    CHECK(m1_ui_focus_allowed(&task_a));
    CHECK(m1_ui_focus_allowed(&task_b));

    CHECK(m1_ui_focus_try_grant(NULL) == false);   /* NULL is never a token */
    CHECK(m1_ui_focus_is_free());

    CHECK(m1_ui_focus_try_grant(&task_a));
    CHECK(!m1_ui_focus_is_free());
    CHECK(m1_ui_focus_held_by(&task_a));
    CHECK(!m1_ui_focus_held_by(&task_b));
    CHECK(m1_ui_focus_allowed(&task_a));
    CHECK(!m1_ui_focus_allowed(&task_b));          /* the lease excludes */

    CHECK(m1_ui_focus_try_grant(&task_b) == false); /* no silent stealing */
    CHECK(m1_ui_focus_held_by(&task_a));

    CHECK(m1_ui_focus_revoke(&task_b) == false);    /* only the owner revokes */
    CHECK(m1_ui_focus_held_by(&task_a));
    CHECK(m1_ui_focus_revoke(&task_a));
    CHECK(m1_ui_focus_is_free());
    CHECK(m1_ui_focus_revoke(&task_a) == false);    /* idempotent */
}

static void test_umbrella_gating(void)
{
    m1_ui_focus_revoke(g_host_current_task);

    /* Lease free: anyone may flush. */
    g_host_current_task = &task_b;
    g_flush_calls = 0;
    CHECK(m1_ui_call(M1_UI_OP_FLUSH, NULL, 0, NULL, 0) == 7);
    CHECK(g_flush_calls == 1);
    CHECK(m1_ui_call(M1_UI_OP_FOCUS_QUERY, NULL, 0, NULL, 0) == 0);

    /* Leased by A: A flushes and queries true; B is refused and the hook
     * must never run for B. */
    CHECK(m1_ui_focus_try_grant(&task_a));
    g_flush_calls = 0;
    g_host_current_task = &task_a;
    CHECK(m1_ui_call(M1_UI_OP_FLUSH, NULL, 0, NULL, 0) == 7);
    CHECK(g_flush_calls == 1);
    CHECK(m1_ui_call(M1_UI_OP_FOCUS_QUERY, NULL, 0, NULL, 0) == 1);

    g_host_current_task = &task_b;
    CHECK(m1_ui_call(M1_UI_OP_FLUSH, NULL, 0, NULL, 0) == M1_SYS_ERR_DENIED);
    CHECK(g_flush_calls == 1);                      /* hook untouched */
    CHECK(m1_ui_call(M1_UI_OP_FOCUS_QUERY, NULL, 0, NULL, 0) == 0);

    /* UNKNOWN op */
    CHECK(m1_ui_call(99, NULL, 0, NULL, 0) == M1_SYS_ERR_BAD_OP);

    CHECK(m1_ui_focus_revoke(&task_a));
}

static void test_notify(void)
{
    m1_ui_focus_revoke(g_host_current_task);
    g_host_current_task = &task_a;

    m1_ui_notify_req_t r;
    fill_hdr(&r.hdr, sizeof(r));

    /* Validation refusals. */
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, NULL, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_ARG);
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, 4, NULL, 0) == M1_SYS_ERR_BAD_ARG);
    fill_hdr(&r.hdr, sizeof(r)); r.hdr.version = 2;
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_ARG);
    fill_hdr(&r.hdr, sizeof(r)); r.hdr.flags = 0x01;
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_ARG);

    fill_hdr(&r.hdr, sizeof(r));
    strcpy(r.msg, "hello"); r.duration_ms = 0;
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_RANGE);
    r.duration_ms = M1_UI_NOTIFY_MAX_MS + 1;
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_RANGE);

    memset(r.msg, 'x', sizeof(r.msg));              /* no NUL anywhere */
    r.duration_ms = 100;
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_ARG);

    /* Lease held by someone else: DENIED before the hook. */
    CHECK(m1_ui_focus_try_grant(&task_b));
    strcpy(r.msg, "hi"); r.duration_ms = 500;
    g_notify_calls = 0;
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_DENIED);
    CHECK(g_notify_calls == 0);
    CHECK(m1_ui_focus_revoke(&task_b));

    /* Valid, from the owner. */
    CHECK(m1_ui_focus_try_grant(&task_a));
    CHECK(m1_ui_call(M1_UI_OP_NOTIFY, &r, sizeof(r), NULL, 0) == 0);
    CHECK(g_notify_calls == 1);
    CHECK(strcmp(g_notify_msg, "hi") == 0 && g_notify_ms == 500);
    CHECK(m1_ui_focus_revoke(&task_a));
}

int main(void)
{
    test_focus_registry();
    test_umbrella_gating();
    test_notify();
    if (g_failures == 0)
        printf("test_m1_ui: ALL PASS\n");
    else
        printf("test_m1_ui: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
