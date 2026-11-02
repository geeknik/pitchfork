/* atlas.c — Signal Atlas v2: unified inventory + Capture Envelope sessions.
 *
 * Scans WiFi + BLE, displays results, writes observations as proper
 * Capture Envelopes to /Sessions/ for cross-radio correlation.
 * Uses raw FatFs (exported) for session dirs; storage umbrella for AppData.
 */
#include "m1app-sdk.h"
#include <string.h>

/* Raw FatFs (exported, not sandboxed) */
extern int f_mkdir(const char *path);
extern int f_open(void *fp, const char *path, unsigned char mode);
extern int f_close(void *fp);
extern int f_write(void *fp, const void *buf, unsigned int btw, unsigned int *bw);
extern int f_sync(void *fp);
extern unsigned int xTaskGetTickCount(void);

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

/* Write observations as Capture Envelopes to /Sessions/<id>/captures/ */
static void write_envelopes(void)
{
    if (dev_count == 0) return;

    uint32_t tick = xTaskGetTickCount();
    make_session_id(tick, dev_count);
    snprintf(session_dir, sizeof(session_dir),
             "0:/Sessions/a%lu-%s", (unsigned long)tick, session_id);

    f_mkdir("0:/Sessions");
    f_mkdir(session_dir);

    char cap_dir[56];
    snprintf(cap_dir, sizeof(cap_dir), "%s/captures", session_dir);
    f_mkdir(cap_dir);

    /* Write one envelope file per observation */
    for (int i = 0; i < dev_count; i++) {
        device_t *d = &devices[i];
        char path[80];
        snprintf(path, sizeof(path), "%s/captures/%03d.m1env",
                 session_dir, i + 1);

        /* Build envelope text (same key=value format as kernel m1_env_format) */
        char env[512];
        int n = snprintf(env, sizeof(env),
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
            i + 1, session_id,
            (unsigned long)tick,
            d->domain == 0 ? "wifi" : "ble",
            d->rssi, d->detail,
            d->domain == 0 ? "wifi" : "ble",
            d->name);
        if (n <= 0 || n >= (int)sizeof(env)) continue;

        /* Write via raw FatFs (exported; /Sessions is outside the sandbox) */
        static char fobj[64]; /* static FIL substitute — f_open needs a FIL struct */
        /* Use a minimal FIL: the actual FIL struct is larger, but we allocate
         * enough for FatFs to work. This is safe because FatFs uses the
         * struct internally and we only need it for the duration of the write. */
        static uint8_t fil_buf[600]; /* > sizeof(FIL) with LFN */
        unsigned int written = 0;
        int fr = f_open(fil_buf, path, 0x09 /* FA_WRITE | FA_CREATE_ALWAYS */);
        if (fr != 0) continue;
        f_write(fil_buf, env, n, &written);
        f_sync(fil_buf);
        f_close(fil_buf);
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

    /* Drain stale button events from during the scan (BACK pressed while
     * scanning gets queued; without this, the first game_poll_button()
     * returns BACK and exits immediately, then the menu sees more stale
     * events and relaunches the app). */
    while (game_poll_button(10) != GAME_BTN_NONE)
        m1app_delay(10);

    while (1) {
        draw_list();
        m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
        game_button_t btn = game_poll_button(500);
        switch (btn) {
        case GAME_BTN_OK:
            do_scan();
            write_envelopes();
            /* Drain events from during the rescan */
            while (game_poll_button(10) != GAME_BTN_NONE)
                m1app_delay(10);
            break;
        case GAME_BTN_UP:
            if (scroll_top > 0) scroll_top--;
            break;
        case GAME_BTN_DOWN:
            if (scroll_top < dev_count - LIST_ROWS) scroll_top++;
            break;
        case GAME_BTN_BACK:
            return 0;
        default:
            break;
        }
    }
    return 0;
}
