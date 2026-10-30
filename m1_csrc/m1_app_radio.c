/* See COPYING.txt for license details. */

/* m1_radio_call — radio-policy umbrella for M1OS apps. Thin validation over
 * the pure m1_radio_policy core (host-tested); no hardware access here, so
 * this file is host-testable as-is. TX enforcement lives at the kernel's
 * Sub-GHz boundary, not behind an app-callable op. */

#include <stdint.h>
#include "m1_app_syscall.h"
#include "m1_radio_policy.h"

int32_t m1_radio_call(uint32_t op, const void *req, uint16_t req_len,
                      void *resp, uint16_t resp_len)
{
    (void)resp;
    (void)resp_len;

    switch (op) {

    case M1_RADIO_OP_TX_ALLOWED: {
        int hr = m1_call_hdr_ok(req, req_len, sizeof(m1_radio_req_t));
        if (hr != 0)
            return hr;
        const m1_radio_req_t *r = req;
        return m1_radio_tx_allowed(r->freq_hz, m1_radio_policy_get_region()) ? 1 : 0;
    }

    case M1_RADIO_OP_POWER_CLAMP: {
        int hr = m1_call_hdr_ok(req, req_len, sizeof(m1_radio_req_t));
        if (hr != 0)
            return hr;
        const m1_radio_req_t *r = req;
        return (int32_t)m1_radio_power_clamp(r->power, m1_radio_policy_get_region());
    }

    case M1_RADIO_OP_REGION_GET:
        return (int32_t)m1_radio_policy_get_region();

    case M1_RADIO_OP_REGION_SET: {
        int hr = m1_call_hdr_ok(req, req_len, sizeof(m1_radio_req_t));
        if (hr != 0)
            return hr;
        const m1_radio_req_t *r = req;
        if (r->region >= M1_RADIO_REGION_COUNT)
            return M1_SYS_ERR_BAD_ARG;
        m1_radio_policy_set_region((m1_radio_region_t)r->region);
        return 0;
    }

    default:
        return M1_SYS_ERR_BAD_OP;
    }
}
