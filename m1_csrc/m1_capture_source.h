/* See COPYING.txt for license details. */

/*
 * m1_capture_source.h — WiFi capture session over the SPI brain.
 *
 * Pitchfork's adapted data source for the ported stock capture stack: pulls
 * buffered promiscuous frames from m1_esp_client_monitor_read() (one frame
 * per call: [chan][rssi:i8][flen:2 LE][frame]), wraps each into a versioned
 * capframe record, and appends EPBs to a pcapng file on SD using the
 * golden-byte-tested stock writer. Replaces stock's UART4/GPDMA link layer
 * (deliberately not ported).
 *
 * v1 cadence: a pump task polls the brain every M1_CAPSRC_POLL_MS; the
 * brain's 24-frame ring absorbs bursts, drops are counted and reported via
 * m1_esp_client_monitor_stats() into the closing ISB. Session lifetime is
 * bounded by duration_ms; stop() is idempotent and safe from any task.
 */

#ifndef M1_CAPTURE_SOURCE_H
#define M1_CAPTURE_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t frames;      /* frames written this session   */
    uint32_t dropped;     /* brain-side drops (its counter) */
    uint16_t bytes;       /* payload bytes written          */
    bool     active;
    uint8_t  env_status;  /* 0 none, 1 written, 2 mkdir-fail, 3 open-fail, 4 format-fail */
} m1_capsrc_stats_t;

/* Start a session: monitor on `channel` (0 = hop 1-13), write for
 * duration_ms (bounded 1..600000). Returns 0 or a negative error:
 *  -1 radio busy/failed, -2 storage failed, -3 already active. */
int m1_capsrc_start(uint8_t channel, uint32_t duration_ms);

/* Stop the active session (idempotent). Closes the pcapng with an ISB. */
void m1_capsrc_stop(void);

void m1_capsrc_get_stats(m1_capsrc_stats_t *out);

/* True while a capture session is running — gates SD remount handling. */
bool m1_capsrc_active(void);

/* Pure helper (host-tested): derive capframe meta fields from a raw 802.11
 * frame's first bytes + capture context. */
void m1_capsrc_derive_meta(const uint8_t *frame, uint16_t len,
                           uint8_t channel, int8_t rssi, uint32_t now_ms,
                           uint8_t *pkt_class, uint32_t *ts_usec);

#endif /* M1_CAPTURE_SOURCE_H */
