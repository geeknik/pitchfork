/* test_m1_radio_policy.c — host tests for the data-driven Sub-GHz TX policy.
 * Run: make -C m1_csrc/test test-radio
 *
 * Verifies band membership per region (edges inclusive), fail-closed on
 * unknown regions and out-of-band frequencies, region attribution, power
 * clamping per region, and the runtime region selector. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "m1_radio_policy.h"
#include "m1_app_syscall.h"

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

int main(void)
{
    /* --- band membership, edges inclusive --- */
    CHECK(m1_radio_tx_allowed(315000000, M1_RADIO_REGION_NA));
    CHECK(m1_radio_tx_allowed(316000000, M1_RADIO_REGION_NA));   /* upper edge */
    CHECK(!m1_radio_tx_allowed(314999999, M1_RADIO_REGION_NA));
    CHECK(m1_radio_tx_allowed(433050000, M1_RADIO_REGION_EU));
    CHECK(m1_radio_tx_allowed(434790000, M1_RADIO_REGION_EU));
    CHECK(!m1_radio_tx_allowed(434790001, M1_RADIO_REGION_EU));
    CHECK(m1_radio_tx_allowed(868000000, M1_RADIO_REGION_EU));
    CHECK(m1_radio_tx_allowed(915000000, M1_RADIO_REGION_NA));
    CHECK(!m1_radio_tx_allowed(915000000, M1_RADIO_REGION_EU));  /* no 915 in EU */
    CHECK(m1_radio_tx_allowed(922500000, M1_RADIO_REGION_ASIA));
    CHECK(!m1_radio_tx_allowed(928500000, M1_RADIO_REGION_ASIA));/* 928.5 is NA-only */

    /* --- fail closed --- */
    CHECK(!m1_radio_tx_allowed(433920000, M1_RADIO_REGION_COUNT));
    CHECK(!m1_radio_tx_allowed(433920000, (m1_radio_region_t)77));
    CHECK(!m1_radio_tx_allowed(2400000000, M1_RADIO_REGION_NA));
    CHECK(!m1_radio_tx_allowed(0, M1_RADIO_REGION_NA));

    /* --- attribution --- */
    CHECK(m1_radio_band_region(315100000) == M1_RADIO_REGION_NA);
    CHECK(m1_radio_band_region(433920000) == M1_RADIO_REGION_NA); /* also EU+ASIA: first wins */
    CHECK(m1_radio_band_region(869000000) == M1_RADIO_REGION_EU);
    CHECK(m1_radio_band_region(123456789) == M1_RADIO_REGION_UNKNOWN);

    /* --- power clamps per region, fail closed --- */
    CHECK(m1_radio_power_clamp(127, M1_RADIO_REGION_NA) == M1_RADIO_POWER_CAP_NA);
    CHECK(m1_radio_power_clamp(50,  M1_RADIO_REGION_EU) == 50);    /* under cap: unchanged */
    CHECK(m1_radio_power_clamp(127, M1_RADIO_REGION_EU) == M1_RADIO_POWER_CAP_EU);
    CHECK(m1_radio_power_clamp(127, M1_RADIO_REGION_ASIA) == M1_RADIO_POWER_CAP_ASIA);
    CHECK(m1_radio_power_clamp(99,  (m1_radio_region_t)42) == 0);
    CHECK(m1_radio_power_clamp(0,   M1_RADIO_REGION_NA) == 0);

    /* --- runtime region selector --- */
    m1_radio_policy_set_region(M1_RADIO_REGION_EU);
    CHECK(m1_radio_policy_get_region() == M1_RADIO_REGION_EU);
    CHECK(!m1_radio_tx_allowed(915000000, m1_radio_policy_get_region()));
    m1_radio_policy_set_region(M1_RADIO_REGION_NA);
    CHECK(m1_radio_tx_allowed(915000000, m1_radio_policy_get_region()));

    /* --- m1_radio_call umbrella --- */
    {
        m1_radio_req_t r;
        memset(&r, 0, sizeof(r));
        r.hdr.size = sizeof(r);
        r.hdr.version = M1_CALL_LAYOUT_V1;

        m1_radio_policy_set_region(M1_RADIO_REGION_EU);
        r.freq_hz = 915000000;
        CHECK(m1_radio_call(M1_RADIO_OP_TX_ALLOWED, &r, sizeof(r), NULL, 0) == 0);
        r.freq_hz = 868000000;
        CHECK(m1_radio_call(M1_RADIO_OP_TX_ALLOWED, &r, sizeof(r), NULL, 0) == 1);

        r.power = 127;
        CHECK(m1_radio_call(M1_RADIO_OP_POWER_CLAMP, &r, sizeof(r), NULL, 0)
              == M1_RADIO_POWER_CAP_EU);

        CHECK(m1_radio_call(M1_RADIO_OP_REGION_GET, NULL, 0, NULL, 0) == M1_RADIO_REGION_EU);

        r.region = M1_RADIO_REGION_NA;
        CHECK(m1_radio_call(M1_RADIO_OP_REGION_SET, &r, sizeof(r), NULL, 0) == 0);
        CHECK(m1_radio_call(M1_RADIO_OP_REGION_GET, NULL, 0, NULL, 0) == M1_RADIO_REGION_NA);
        r.region = 77;
        CHECK(m1_radio_call(M1_RADIO_OP_REGION_SET, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_ARG);
        m1_radio_policy_set_region(M1_RADIO_REGION_NA);

        /* header validation + unknown op */
        r.region = M1_RADIO_REGION_EU;
        r.hdr.version = 2;
        CHECK(m1_radio_call(M1_RADIO_OP_REGION_SET, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_ARG);
        r.hdr.version = M1_CALL_LAYOUT_V1;
        CHECK(m1_radio_call(M1_RADIO_OP_REGION_SET, &r, 4, NULL, 0) == M1_SYS_ERR_BAD_ARG);
        CHECK(m1_radio_call(99, &r, sizeof(r), NULL, 0) == M1_SYS_ERR_BAD_OP);
    }

    if (g_failures == 0)
        printf("test_m1_radio_policy: ALL PASS\n");
    else
        printf("test_m1_radio_policy: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
