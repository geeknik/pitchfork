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
#ifdef M1_HOST_TEST
void m1_esp32_init(void);            /* host: stubbed by the test */
#else
#include "m1_esp32_hal.h"            /* m1_esp32_init: bring the lazy link up */
#endif
#include "m1_capture_source.h"
#include "m1_capture_frame.h"
#include "m1_capture_pcap.h"
#include "m1_pcapng.h"
#include "m1_capture_envelope.h"
#include "m1_crc32c.h"

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
    uint8_t     env_status;   /* 0 none, 1 written, 2 mkdir-fail, 3 open-fail, 4 format-fail */
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
        vTaskDelay(pdMS_TO_TICKS(1));   /* ALWAYS yield: on a busy channel the
                                         * pull loop must never starve the
                                         * watchdog-supervised tasks (the
                                         * 1.1.93-95 mid-capture reboots). */
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

    /* The ESP link is lazy-initialized (menu paths bring it up on first
     * radio use). A headless capture must not depend on UI history: bring
     * the link up ourselves. m1_esp32_init() is guarded/idempotent, so this
     * is free when the link is already up. */
    m1_esp32_init();

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
    s_cap.env_status = 0;
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

/* Close the session with a canonical Capture Envelope: /Sessions tree,
 * crc32c integrity hash over the artifact, key=value record beside it.
 * Static buffers throughout (runs on the pump task's 2 KB stack). */
static void capsrc_write_envelope(void)
{
    static char dir[M1_ENV_DIR_MAX];
    static char epath[M1_ENV_DIR_MAX + 24];
    static char ebuf[1024];
    static uint8_t hashbuf[512];
    static FIL hf;
    char sid[M1_ENV_SESSION_ID_LEN];

    /* Wall clock awaits the RTC work (STATUS known-issue): sessions stamp
     * monotonically (tick) and correlate by session_id; the stamp format
     * upgrades to calendar time later without breaking readers. */
    m1_env_session_id(sid, xTaskGetTickCount(), s_cap.stats.frames);
    snprintf(dir, sizeof(dir), "0:/Sessions/s%lu-%s",
             (unsigned long)xTaskGetTickCount(), sid);
    FRESULT mr;
    mr = f_mkdir("0:/Sessions");
    if (mr != FR_OK && mr != FR_EXIST) {
        M1_LOG_E(TAG, "envelope: mkdir 0:/Sessions failed (%d)", (int)mr);
        s_cap.env_status = 2;
    }
    mr = f_mkdir(dir);
    if (mr != FR_OK && mr != FR_EXIST)
        M1_LOG_E(TAG, "envelope: mkdir %s failed (%d)", dir, (int)mr);
    char sub[M1_ENV_DIR_MAX + 12];
    snprintf(sub, sizeof(sub), "%s/captures", dir);
    mr = f_mkdir(sub);
    if (mr != FR_OK && mr != FR_EXIST)
        M1_LOG_E(TAG, "envelope: mkdir %s failed (%d)", sub, (int)mr);

    uint32_t crc = M1_CRC32C_INIT;
    if (f_open(&hf, s_cap.path, FA_READ) == FR_OK) {
        UINT got = 0;
        for (;;) {
            FRESULT fr = f_read(&hf, hashbuf, sizeof(hashbuf), &got);
            if (fr != FR_OK || got == 0)
                break;
            crc = m1_crc32c_update(crc, hashbuf, got);
        }
        f_close(&hf);
    }
    crc = m1_crc32c_final(crc);   /* standard CRC-32C, host-verifiable */

    char params[64];
    snprintf(params, sizeof(params), "chan:%u,frames:%lu",
             s_cap.channel, (unsigned long)s_cap.stats.frames);

    m1_envelope_t e;
    memset(&e, 0, sizeof(e));
    e.record_id = 1;
    e.session_id = sid;
    e.timestamp_monotonic_ms = xTaskGetTickCount();
    e.source_domain = "wifi";
    e.capture_parameters = params;
    e.raw_artifact_ref = s_cap.path;
    e.decoder_candidates = "radiotap";
    e.confidence_pm = 1000;         /* raw observation, uninterpreted */
    e.device_entity_id = "-";
    e.parent_record_id = -1;
    e.operator_annotation = "-";
    e.integrity_crc32c = crc;

    size_t n = m1_env_format(ebuf, sizeof(ebuf), &e);
    if (n == 0) {
        s_cap.env_status = 4;
        return;
    }
    snprintf(epath, sizeof(epath), "%s/captures/1.m1env", dir);
    static FIL ef;
    FRESULT fr2 = f_open(&ef, epath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr2 != FR_OK) {
        M1_LOG_E(TAG, "envelope: open %s failed (%d)", epath, (int)fr2);
        s_cap.env_status = 3;
        return;
    }
    {
        UINT w = 0;
        f_write(&ef, ebuf, n, &w);
        f_sync(&ef);
        f_close(&ef);
        M1_LOG_I(TAG, "envelope: %s (crc32c:%08lX)",
                 epath, (unsigned long)crc);
        s_cap.env_status = 1;
    }
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
    capsrc_write_envelope();
    M1_LOG_I(TAG, "capture stop: %lu frames, brain total=%lu drop=%lu -> %s",
             (unsigned long)s_cap.stats.frames,
             (unsigned long)total, (unsigned long)drop, s_cap.path);
}

void m1_capsrc_get_stats(m1_capsrc_stats_t *out)
{
    if (out) {
        *out = s_cap.stats;
        out->env_status = s_cap.env_status;
    }
}

bool m1_capsrc_active(void)
{
    return s_cap.active;
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
