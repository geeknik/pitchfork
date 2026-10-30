/* See COPYING.txt for license details. */

/* m1_radio_policy.c — data-driven Sub-GHz transmit policy. See the header;
 * tables only, no hardware access, no globals beyond the selected region. */

#include "m1_radio_policy.h"

typedef struct {
    uint32_t lo_hz;
    uint32_t hi_hz;
} radio_band_t;

typedef struct {
    const radio_band_t *bands;
    uint8_t             band_count;
    uint8_t             power_cap;
} radio_region_rules_t;

/* 315 MHz band is NA-only; 433 is global-with-variation; 868 EU; 902-928 NA. */
static const radio_band_t s_bands_na[] = {
    { 315000000, 316000000 },
    { 433050000, 434790000 },
    { 902000000, 928000000 },
};
static const radio_band_t s_bands_eu[] = {
    { 433050000, 434790000 },
    { 863000000, 870000000 },
};
static const radio_band_t s_bands_asia[] = {
    { 315000000, 316000000 },
    { 433050000, 434790000 },
    { 920000000, 925000000 },
};

static const radio_region_rules_t s_rules[M1_RADIO_REGION_COUNT] = {
    [M1_RADIO_REGION_NA]    = { s_bands_na,   3, M1_RADIO_POWER_CAP_NA },
    [M1_RADIO_REGION_EU]    = { s_bands_eu,   2, M1_RADIO_POWER_CAP_EU },
    [M1_RADIO_REGION_ASIA]  = { s_bands_asia, 3, M1_RADIO_POWER_CAP_ASIA },
};

static volatile m1_radio_region_t s_region = M1_RADIO_REGION_NA;

m1_radio_region_t m1_radio_band_region(uint32_t freq_hz)
{
    for (int r = 0; r < M1_RADIO_REGION_COUNT; r++) {
        for (int b = 0; b < s_rules[r].band_count; b++) {
            if (freq_hz >= s_rules[r].bands[b].lo_hz &&
                freq_hz <= s_rules[r].bands[b].hi_hz)
                return (m1_radio_region_t)r;
        }
    }
    return M1_RADIO_REGION_UNKNOWN;
}

bool m1_radio_tx_allowed(uint32_t freq_hz, m1_radio_region_t region)
{
    if ((unsigned)region >= M1_RADIO_REGION_COUNT)
        return false;                        /* unknown region: fail closed */
    for (int b = 0; b < s_rules[region].band_count; b++) {
        if (freq_hz >= s_rules[region].bands[b].lo_hz &&
            freq_hz <= s_rules[region].bands[b].hi_hz)
            return true;
    }
    return false;
}

uint8_t m1_radio_power_clamp(uint8_t power, m1_radio_region_t region)
{
    if ((unsigned)region >= M1_RADIO_REGION_COUNT)
        return 0;                            /* fail closed */
    if (power > s_rules[region].power_cap)
        return s_rules[region].power_cap;
    return power;
}

void              m1_radio_policy_set_region(m1_radio_region_t r) { s_region = r; }
m1_radio_region_t m1_radio_policy_get_region(void)                { return s_region; }
