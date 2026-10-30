/* See COPYING.txt for license details. */

/*
 * m1_radio_policy.h — data-driven Sub-GHz transmit policy.
 *
 * Replaces the compile-time ISM-region byte: regions and their bands are
 * DATA (tables below), selected at runtime, and every TX path checks
 * m1_radio_tx_allowed() before touching the SI4463. Bands are hobby-ISM
 * reference ranges for passive/low-power use — operators remain responsible
 * for their jurisdiction's actual rules (power, duty, modulation).
 *
 * Pure logic, no hardware: fully host-tested (m1_csrc/test/).
 */

#ifndef M1_RADIO_POLICY_H
#define M1_RADIO_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    M1_RADIO_REGION_NA = 0,     /* FCC / North America        */
    M1_RADIO_REGION_EU,         /* ETSI / Europe              */
    M1_RADIO_REGION_ASIA,
    M1_RADIO_REGION_COUNT,
    M1_RADIO_REGION_UNKNOWN = 0xFF,
} m1_radio_region_t;

/* Max PA setting the policy allows per region (conservative defaults;
 * the caller clamps to this before applying). */
#define M1_RADIO_POWER_CAP_NA    100
#define M1_RADIO_POWER_CAP_EU    60    /* ETSI ERP limits are strictest  */
#define M1_RADIO_POWER_CAP_ASIA  80

/* Returns the region's index whose band contains freq_hz (Hz), or
 * M1_RADIO_REGION_UNKNOWN if no band matches. */
m1_radio_region_t m1_radio_band_region(uint32_t freq_hz);

/* True if freq_hz is inside an allowed band of `region`. */
bool m1_radio_tx_allowed(uint32_t freq_hz, m1_radio_region_t region);

/* Clamp a PA level (0..127) to the region's cap; returns the capped value. */
uint8_t m1_radio_power_clamp(uint8_t power, m1_radio_region_t region);

/* Run-time region selection used by the umbrella (persisted by settings). */
void              m1_radio_policy_set_region(m1_radio_region_t region);
m1_radio_region_t m1_radio_policy_get_region(void);

#endif /* M1_RADIO_POLICY_H */
