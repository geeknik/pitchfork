/* See COPYING.txt for license details. */

/*
 * m1_app_ui.c — UI/display umbrella for M1OS apps (m1_ui_call).
 *
 * Policy runs before the display, same discipline as m1_storage_call: the
 * focus lease (m1_ui_focus.c) is checked first, and only then do the kernel
 * hooks (m1_ui_hook_*, implemented in m1_app_api.c) touch the framebuffer.
 * This file is host-testable as-is: the only externals are the lease
 * registry (pure), FreeRTOS's current-task handle, and the hooks, which the
 * host tests substitute.
 */

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "m1_app_syscall.h"
#include "m1_ui_focus.h"

/*************************** C O D E ******************************************/

int32_t m1_ui_call(uint32_t op, const void *req, uint16_t req_len,
                   void *resp, uint16_t resp_len)
{
    void *me = (void *)xTaskGetCurrentTaskHandle();
    (void)resp;
    (void)resp_len;

    switch (op) {

    case M1_UI_OP_FOCUS_QUERY:
        return m1_ui_focus_held_by(me) ? 1 : 0;

    case M1_UI_OP_FLUSH:
        if (!m1_ui_focus_allowed(me))
            return M1_SYS_ERR_DENIED;
        return (int32_t)m1_ui_hook_flush();

    case M1_UI_OP_NOTIFY: {
        int hr = m1_call_hdr_ok(req, req_len, sizeof(m1_ui_notify_req_t));
        if (hr != 0)
            return hr;
        const m1_ui_notify_req_t *r = req;
        if (r->duration_ms == 0 || r->duration_ms > M1_UI_NOTIFY_MAX_MS)
            return M1_SYS_ERR_RANGE;
        if (memchr(r->msg, '\0', sizeof(r->msg)) == NULL)
            return M1_SYS_ERR_BAD_ARG;          /* not NUL-terminated */
        if (!m1_ui_focus_allowed(me))
            return M1_SYS_ERR_DENIED;
        m1_ui_hook_notify(r->msg, r->duration_ms);
        return 0;
    }

    default:
        return M1_SYS_ERR_BAD_OP;
    }
}
