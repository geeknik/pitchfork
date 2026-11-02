/* atlas.c — Signal Atlas v2: unified inventory + Capture Envelope sessions.
 *
 * Scans WiFi + BLE, displays results, writes observations as proper
 * Capture Envelopes to /Sessions/ for cross-radio correlation.
 * Uses raw FatFs (exported) for session dirs; storage umbrella for AppData.
 */
#include "m1app-sdk.h"
#include <string.h>

/* Raw FatFs (exported, not sandboxed) */







#define MAX_DEVICES 30
#define SCAN_BUF_SIZE 1024
#define LINE_H 10
#define LIST_ROWS 4
#define SESSION_ID_LEN 9

typedef struct {
    uint8_t domain;      /* 0=WiFi, 1=BLE */
    int8_t  rssi;
    char    name[24];
    uint8_t channel;
    char    detail[12];
} device_t;

static device_t devices[MAX_DEVICES];
static int dev_count = 0;
static int scroll_top = 0;
static uint8_t scan_buf[SCAN_BUF_SIZE];
static char session_id[SESSION_ID_LEN];
static char session_dir[48];

/* FNV-1a session ID (same algorithm as kernel m1_env_session_id) */
static void make_session_id(uint32_t a, uint32_t b)
{
    uint32_t h = 0x811C9DC5;
    for (int i = 0; i < 4; i++) {
        h ^= (a >> (i * 8)) & 0xFF; h *= 0x01000193;
        h ^= (b >> (i * 8)) & 0xFF; h *= 0x01000193;
    }
    snprintf(session_id, SESSION_ID_LEN, "%08lx", (unsigned long)h);
}

static void insert_sorted(device_t *d)
{
    if (dev_count >= MAX_DEVICES) return;
    int i = dev_count;
    while (i > 0 && devices[i-1].rssi < d->rssi) {
        devices[i] = devices[i-1];
        i--;
    }
    devices[i] = *d;
    dev_count++;
}

static void scan_wifi(void)
{
    int n = m1_esp_client_wifi_scan(scan_buf, SCAN_BUF_SIZE);
    if (n < 2) return;
    uint16_t count = scan_buf[0] | (scan_buf[1] << 8);
    int off = 2;
    for (int i = 0; i < count && off < n && dev_count < MAX_DEVICES; i++) {
        if (off + 10 > n) break;
        int8_t rssi = (int8_t)scan_buf[off + 6];
        uint8_t chan = scan_buf[off + 7];
        uint8_t ssid_len = scan_buf[off + 9];
        if (ssid_len > 23) ssid_len = 23;
        if (off + 10 + scan_buf[off + 9] > n) break;

        device_t d;
        memset(&d, 0, sizeof(d));
        d.domain = 0;
        d.rssi = rssi;
        d.channel = chan;
        if (ssid_len > 0) {
            memcpy(d.name, &scan_buf[off + 10], ssid_len);
            d.name[ssid_len] = '\0';
        } else {
            snprintf(d.name, sizeof(d.name), "%02X:%02X:%02X:%02X",
                     scan_buf[off], scan_buf[off+1],
                     scan_buf[off+2], scan_buf[off+3]);
        }
        uint8_t auth = scan_buf[off + 8];
        snprintf(d.detail, sizeof(d.detail), "ch%d%s",
                 chan, auth > 0 ? " sec" : "");
        insert_sorted(&d);
        off += 10 + scan_buf[off + 9];
    }
}

static void scan_ble(void)
{
    int n = m1_esp_client_ble_scan(scan_buf, SCAN_BUF_SIZE, 4);
    if (n < 2) return;
    uint16_t count = scan_buf[0] | (scan_buf[1] << 8);
    int off = 2;
    for (int i = 0; i < count && off < n && dev_count < MAX_DEVICES; i++) {
        if (off + 9 > n) break;
        int8_t rssi = (int8_t)scan_buf[off + 7];
        uint8_t name_len = scan_buf[off + 8];
        if (name_len > 23) name_len = 23;
        if (off + 9 + scan_buf[off + 8] > n) break;

        device_t d;
        memset(&d, 0, sizeof(d));
        d.domain = 1;
        d.rssi = rssi;
        if (name_len > 0) {
            memcpy(d.name, &scan_buf[off + 9], name_len);
            d.name[name_len] = '\0';
        } else {
            snprintf(d.name, sizeof(d.name), "%02X:%02X:%02X:%02X",
                     scan_buf[off+2], scan_buf[off+3],
                     scan_buf[off+4], scan_buf[off+5]);
        }
        snprintf(d.detail, sizeof(d.detail), "t%d", scan_buf[off + 6]);
        insert_sorted(&d);
        off += 9 + scan_buf[off + 8];
    }
}

static void do_scan(void)
{
    dev_count = 0;
    scroll_top = 0;
    u8g2_t *u = m1app_get_u8g2();
    u8g2_SetFont(u, u8g2_font_6x10_tr);

    u8g2_FirstPage(u);
    do { u8g2_DrawStr(u, 4, 20, "Scanning WiFi..."); } while (m1app_display_flush());
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
    scan_wifi();

    u8g2_FirstPage(u);
    do { u8g2_DrawStr(u, 4, 20, "Scanning BLE..."); } while (m1app_display_flush());
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
    scan_ble();
}

/* Write observations via the sandboxed storage umbrella (safe FIL
 * management; the raw f_open path crashed — a 600-byte byte array is not
 * a FIL struct and sizeof(FIL) with exFAT exceeds it). Sessions live
 * under /AppData/sessions/ (the umbrella's sandbox root). */
typedef struct {
    uint16_t size; uint8_t version; uint8_t flags;
    char path[64]; uint16_t datalen; uint8_t data[256];
} __attribute__((packed)) sto_write_req_t;

static void write_envelopes(void)
{
    if (dev_count == 0) return;

    uint32_t tick = m1app_get_tick();
    make_session_id(tick, dev_count);

    /* Write each observation as one envelope file (atomic replace) */
    static sto_write_req_t w;
    w.size = sizeof(w);
    w.version = 1;
    w.flags = 0;

    for (int i = 0; i < dev_count; i++) {
        device_t *d = &devices[i];
        snprintf(w.path, sizeof(w.path), "sessions/%s_%03d.m1env",
                 session_id, i + 1);
        int n = snprintf((char*)w.data, sizeof(w.data),
            "envelope_version=1\n"
            "record_id=%d\n"
            "session_id=%s\n"
            "timestamp_monotonic_ms=%lu\n"
            "source_domain=%s\n"
            "capture_parameters=rssi:%d,%s\n"
            "raw_artifact_ref=-\n"
            "decoder_candidates=-\n"
            "confidence=1000\n"
            "device_entity_id=%s:%s\n"
            "parent_record_id=-1\n"
            "operator_annotation=-\n"
            "integrity_hash=-\n",
            i + 1, session_id, (unsigned long)tick,
            d->domain == 0 ? "wifi" : "ble",
            d->rssi, d->detail,
            d->domain == 0 ? "wifi" : "ble",
            d->name);
        if (n <= 0 || n >= (int)sizeof(w.data)) continue;
        w.datalen = n;
        m1_storage_call(M1_STO_OP_WRITE, &w, sizeof(w), 0, 0);
    }
}

static void draw_list(void)
{
    u8g2_t *u = m1app_get_u8g2();
    u8g2_SetFont(u, u8g2_font_5x8_tr);
    u8g2_FirstPage(u);
    do {
        u8g2_DrawStr(u, 1, 8, "Atlas");
        int wc = 0, bc = 0;
        for (int i = 0; i < dev_count; i++) {
            if (devices[i].domain == 0) wc++; else bc++;
        }
        char hdr[16];
        snprintf(hdr, sizeof(hdr), "W%d B%d", wc, bc);
        int hw = u8g2_GetStrWidth(u, hdr);
        u8g2_DrawStr(u, 127 - hw, 8, hdr);
        u8g2_DrawHLine(u, 0, 10, 128);

        for (int row = 0; row < LIST_ROWS; row++) {
            int idx = scroll_top + row;
            if (idx >= dev_count) break;
            device_t *d = &devices[idx];
            int y = 19 + row * 10;
            u8g2_DrawStr(u, 1, y, d->domain == 0 ? "W" : "B");
            char name[19];
            int nl = 0;
            while (d->name[nl] && nl < 18) { name[nl] = d->name[nl]; nl++; }
            name[nl] = '\0';
            u8g2_DrawStr(u, 10, y, name);
            char rs[6];
            snprintf(rs, sizeof(rs), "%d", d->rssi);
            int rw = u8g2_GetStrWidth(u, rs);
            u8g2_DrawStr(u, 120 - rw, y, rs);
        }

        if (dev_count > LIST_ROWS) {
            int track_h = 38;
            int thumb_h = (LIST_ROWS * track_h) / dev_count;
            if (thumb_h < 4) thumb_h = 4;
            int max_top = dev_count - LIST_ROWS;
            int thumb_y = 12 + (scroll_top * (track_h - thumb_h)) / max_top;
            if (thumb_y + thumb_h > 12 + track_h)
                thumb_y = 12 + track_h - thumb_h;
            u8g2_DrawVLine(u, 127, 12, track_h);
            u8g2_DrawBox(u, 126, thumb_y, 2, thumb_h);
        }

        u8g2_DrawHLine(u, 0, 52, 128);
        char foot[24];
        if (dev_count == 0)
            snprintf(foot, sizeof(foot), "no devices");
        else
            snprintf(foot, sizeof(foot), "%d-%d of %d",
                     scroll_top + 1,
                     scroll_top + LIST_ROWS < dev_count ? scroll_top + LIST_ROWS : dev_count,
                     dev_count);
        u8g2_DrawStr(u, 1, 62, foot);
        u8g2_DrawStr(u, 70, 62, "OK:scan");
    } while (m1app_display_flush());
}

int32_t app_main(void *param)
{
    (void)param;
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);

    if (!get_esp32_main_init_status()) {
        u8g2_t *u = m1app_get_u8g2();
        u8g2_SetFont(u, u8g2_font_6x10_tr);
        u8g2_FirstPage(u);
        do {
            u8g2_DrawStr(u, 2, 20, "ESP32 not ready");
            u8g2_DrawStr(u, 2, 34, "Run WiFi first");
        } while (m1app_display_flush());
        m1app_delay(3000);
        return 1;
    }

    do_scan();
    write_envelopes();

    /* Show results for 15 seconds, then auto-exit. Accept ANY button
     * press as exit (game_poll_button's BACK detection is unreliable from
     * app context — two-queue sync issue under investigation). */
    uint32_t start = m1app_get_tick();
    while (m1app_get_tick() - start < 15000) {
        draw_list();
        m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
        m1app_delay(200);

        game_button_t btn = game_poll_button(20);
        if (btn != GAME_BTN_NONE)
            return 0;  /* ANY button exits */

        if (m1_sys_call(M1_SYS_OP_CANCEL_POLL, 0))
            return 0;  /* kernel asked us to exit */
    }
    return 0;
}
