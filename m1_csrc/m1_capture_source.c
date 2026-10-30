/* See COPYING.txt for license details. */

/* m1_capture_source.c — see the header. Device side: FreeRTOS task pump,
 * FatFs sink, brain client pulls. The pure derivations at the bottom are
 * host-tested (test_m1_capsrc.c); everything that touches hardware lives
 * above them. Static buffers throughout — the boot-path stack rule applies
 * to any task context we do not control. */

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include "m1_log_debug.h"
#include "m1_esp_client.h"
#include "m1_capture_source.h"
#include "m1_capture_frame.h"
#include "m1_capture_pcap.h"
#include "m1_pcapng.h"

/*************************** D E F I N E S ************************************/

#define TAG "CAPSRC"

#define M1_CAPSRC_POLL_MS      50   /* pull cadence (brain ring absorbs bursts) */
#define M1_CAPSRC_MAX_MS    600000
#define M1_CAPSRC_DIR        "0:/wifi"

typedef struct {
    uint8_t  chan;
    int8_t   rssi;
    uint16_t flen;          /* copy of the brain's header for the record */
    uint8_t  frame[256];
} capsrc_frame_t;

typedef struct {
    bool        active;
    uint8_t     channel;
    uint32_t    deadline_ms;
    TaskHandle_t task;
    FIL         file;
    char        path[32];
    m1_capsrc_stats_t stats;
    uint32_t    frame_seq;
} capsrc_ctx_t;

static capsrc_ctx_t s_cap;

/* pcapng write buffer + one frame staging — static per the stack rule. */
static uint8_t           s_pcap_buf[1024];
static uint8_t           s_frame[256];
static m1_pcapng_writer_t s_writer;

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static void capsrc_task(void *param);
static bool capsrc_open_file(void);
static void capsrc_close_file(uint32_t brain_total, uint32_t brain_drop);

/*************************** C O D E ******************************************/

static bool capsrc_open_file(void)
{
    f_mkdir("0:");
    f_mkdir(M1_CAPSRC_DIR);
    for (uint32_t i = 1; i <= 999; i++) {
        snprintf(s_cap.path, sizeof(s_cap.path), "%s/mcap_%03lu.pcapng",
                 M1_CAPSRC_DIR, (unsigned long)i);
        FILINFO fi;
        if (f_stat(s_cap.path, &fi) == FR_NO_FILE) {
            FRESULT fr = f_open(&s_cap.file, s_cap.path,
                                FA_WRITE | FA_CREATE_NEW);
            return fr == FR_OK;
        }
    }
    return false;
}

static void capsrc_close_file(uint32_t brain_total, uint32_t brain_drop)
{
    m1_pcapng_stats_t isb = { 0 };
    isb.has_ifrecv = true; isb.isb_ifrecv = brain_total;
    isb.has_ifdrop = true; isb.isb_ifdrop = brain_drop;
    isb.has_usrdeliv = true; isb.isb_usrdeliv = 0;   /* per-session below */
    (void)m1_pcapng_write_isb(&s_writer, 1, 0, &isb);
    UINT written = 0;
    f_write(&s_cap.file, s_pcap_buf,
            s_writer.len, &written);
    f_sync(&s_cap.file);
    f_close(&s_cap.file);
}

static void capsrc_task(void *param)
{
    (void)param;
    m1_capframe_meta_t meta;

    while (s_cap.active && xTaskGetTickCount() < s_cap.deadline_ms) {
        int8_t rssi = 0;
        uint8_t chan = 0;
        uint16_t n = m1_esp_client_monitor_read(s_frame, sizeof(s_frame),
                                                &rssi, &chan);
        if (n > 0) {
            uint8_t klass; uint32_t ts;
            m1_capsrc_derive_meta(s_frame, n, chan, rssi,
                                  xTaskGetTickCount(), &klass, &ts);
            memset(&meta, 0, sizeof(meta));
            meta.version  = M1_CAPFRAME_VERSION;
            meta.pkt_class = klass;
            meta.primary_channel = chan;
            meta.rssi_dbm = rssi;
            meta.cap_len = n;
            meta.orig_len = n;
            meta.frame_seq = s_cap.frame_seq++;
            meta.ts_usec = ts;

            /* The stock consumer emits [radiotap][802.11] as an EPB. */
            static uint8_t scratch[M1_CAPTURE_PCAP_SCRATCH_MIN + 64];
            if (m1_capture_pcap_frame(&s_writer, 1, &meta, s_frame,
                                      scratch, sizeof(scratch)) == 0) {
                s_cap.stats.frames++;
                s_cap.stats.bytes += n;
            }

            if (s_writer.len > sizeof(s_pcap_buf) - 512) {
                UINT w = 0;              /* flush before the buffer fills */
                f_write(&s_cap.file, s_pcap_buf, s_writer.len, &w);
                f_sync(&s_cap.file);
                m1_pcapng_writer_init(&s_writer, s_pcap_buf, sizeof(s_pcap_buf));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(M1_CAPSRC_POLL_MS));
        }
    }
    m1_capsrc_stop();
    vTaskDelete(NULL);
}

int m1_capsrc_start(uint8_t channel, uint32_t duration_ms)
{
    if (s_cap.active)
        return -3;
    if (duration_ms == 0 || duration_ms > M1_CAPSRC_MAX_MS)
        duration_ms = M1_CAPSRC_MAX_MS;

    int rc = m1_esp_client_monitor_start(channel);
    if (rc != 0)
        return -1;

    if (!capsrc_open_file()) {
        (void)m1_esp_client_monitor_stop();
        return -2;
    }

    m1_pcapng_section_info_t shb = {
        .shb_hardware = "MonstaTek M1 / brain 1.5.0 (SPI)",
        .shb_os       = "Pitchfork",
        .shb_userappl = "Pitchfork WiFi Capture",
    };
    m1_pcapng_iface_info_t idb = {
        .if_name = "esp32c6-monitor",
        .tsresol = 6,               /* microseconds */
    };
    m1_pcapng_writer_init(&s_writer, s_pcap_buf, sizeof(s_pcap_buf));
    (void)m1_pcapng_write_shb(&s_writer, &shb);
    (void)m1_pcapng_write_idb(&s_writer, &idb);
    UINT w = 0;
    f_write(&s_cap.file, s_pcap_buf, s_writer.len, &w);
    f_sync(&s_cap.file);
    m1_pcapng_writer_init(&s_writer, s_pcap_buf, sizeof(s_pcap_buf));

    memset(&s_cap.stats, 0, sizeof(s_cap.stats));
    s_cap.stats.active = true;
    s_cap.channel = channel;
    s_cap.deadline_ms = xTaskGetTickCount() + duration_ms;
    s_cap.frame_seq = 0;
    s_cap.active = true;

    if (xTaskCreate(capsrc_task, "m1_capsrc", 512, NULL, 3,
                    &s_cap.task) != pdPASS) {
        s_cap.active = false;
        f_close(&s_cap.file);
        (void)m1_esp_client_monitor_stop();
        return -2;
    }
    M1_LOG_I(TAG, "capture start ch=%u dur=%lums -> %s",
             channel, (unsigned long)duration_ms, s_cap.path);
    return 0;
}

void m1_capsrc_stop(void)
{
    if (!s_cap.active) {
        /* No local session — but the brain may still be in monitor mode
         * after a failed/raced teardown (deadline-stop with monitor_stop
         * returning false left it stuck, refusing every later START).
         * STOP is the recovery path: clear the brain unconditionally. */
        if (!m1_esp_client_monitor_stop())
            M1_LOG_W(TAG, "brain monitor_stop refused while inactive (still busy?)");
        return;
    }
    s_cap.active = false;
    s_cap.stats.active = false;

    uint32_t total = 0, drop = 0; uint8_t buffered = 0;
    (void)m1_esp_client_monitor_stats(&total, &drop, &buffered);
    if (!m1_esp_client_monitor_stop())
        M1_LOG_W(TAG, "brain monitor_stop failed at session end - issue STOP again to clear");
    capsrc_close_file(total, drop);
    s_cap.stats.dropped = drop;
    M1_LOG_I(TAG, "capture stop: %lu frames, brain total=%lu drop=%lu -> %s",
             (unsigned long)s_cap.stats.frames,
             (unsigned long)total, (unsigned long)drop, s_cap.path);
}

void m1_capsrc_get_stats(m1_capsrc_stats_t *out)
{
    if (out)
        *out = s_cap.stats;
}

/* ---- pure derivations (host-tested) ---- */

void m1_capsrc_derive_meta(const uint8_t *frame, uint16_t len,
                           uint8_t channel, int8_t rssi, uint32_t now_ms,
                           uint8_t *pkt_class, uint32_t *ts_usec)
{
    (void)len; (void)channel; (void)rssi;
    /* Frame Control type bits: matches the brain's own classification. */
    *pkt_class = (frame[0] >> 2) & 0x3;   /* 0 mgmt, 1 ctrl, 2 data, 3 none */
    *ts_usec = (uint32_t)now_ms * 1000u;  /* tick-ms approximation */
}
