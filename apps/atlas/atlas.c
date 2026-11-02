/* atlas.c — Signal Atlas: unified passive inventory over WiFi + BLE.
 *
 * The thesis's flagship .m1app: scans the environment, sorts by signal
 * strength, displays a scrollable inventory, writes a summary to SD.
 * Built with -mlong-calls, uses only exported firmware symbols.
 */
#include "m1app-sdk.h"

#define MAX_DEVICES 30
#define SCAN_BUF_SIZE 1024
#define LINE_H 11
#define LIST_Y_START 16
#define LIST_ROWS 4

typedef struct {
    uint8_t domain;      /* 0=WiFi, 1=BLE */
    int8_t  rssi;
    char    name[24];    /* SSID or BLE name or MAC */
    uint8_t channel;     /* WiFi channel */
    char    detail[12];  /* "ch6 WPA2" or "type:1" */
} device_t;

static device_t devices[MAX_DEVICES];
static int dev_count = 0;
static int scroll_top = 0;
static uint8_t scan_buf[SCAN_BUF_SIZE];

static void insert_sorted(device_t *d)
{
    if (dev_count >= MAX_DEVICES)
        return;
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
    if (n < 2)
        return;
    uint16_t count = scan_buf[0] | (scan_buf[1] << 8);
    int off = 2;
    for (int i = 0; i < count && off < n && dev_count < MAX_DEVICES; i++) {
        /* entry: bssid[6], rssi:i8, channel:u8, authmode:u8, ssid_len:u8, ssid[] */
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
            snprintf(d.name, sizeof(d.name),
                     "%02X:%02X:%02X:%02X",
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
    if (n < 2)
        return;
    uint16_t count = scan_buf[0] | (scan_buf[1] << 8);
    int off = 2;
    for (int i = 0; i < count && off < n && dev_count < MAX_DEVICES; i++) {
        /* entry: addr[6], addr_type:u8, rssi:i8, name_len:u8, name[] */
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
            snprintf(d.name, sizeof(d.name),
                     "%02X:%02X:%02X:%02X",
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

    /* Show scanning indicator */
    u8g2_t *u = m1app_get_u8g2();
    u8g2_SetFont(u, u8g2_font_6x10_tr);
    u8g2_FirstPage(u);
    do {
        u8g2_DrawStr(u, 4, 20, "Scanning WiFi...");
    } while (m1app_display_flush());
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
    scan_wifi();

    u8g2_FirstPage(u);
    do {
        u8g2_DrawStr(u, 4, 20, "Scanning BLE...");
    } while (m1app_display_flush());
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
    scan_ble();
}

static void draw_list(void)
{
    u8g2_t *u = m1app_get_u8g2();
    u8g2_SetFont(u, u8g2_font_5x8_tr);

    u8g2_FirstPage(u);
    do {
        /* Header */
        u8g2_DrawStr(u, 2, 9, "Signal Atlas");
        int wifi_count = 0, ble_count = 0;
        for (int i = 0; i < dev_count; i++) {
            if (devices[i].domain == 0) wifi_count++;
            else ble_count++;
        }
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "W:%d B:%d", wifi_count, ble_count);
        int hw = u8g2_GetStrWidth(u, hdr);
        u8g2_DrawStr(u, 126 - hw, 9, hdr);
        u8g2_DrawHLine(u, 0, 11, 128);

        /* Device rows */
        for (int row = 0; row < LIST_ROWS; row++) {
            int idx = scroll_top + row;
            if (idx >= dev_count) break;
            device_t *d = &devices[idx];
            int y = LIST_Y_START + row * LINE_H + 8;

            char rssi_str[8];
            snprintf(rssi_str, sizeof(rssi_str), "%d", d->rssi);

            /* Domain marker */
            u8g2_DrawStr(u, 2, y, d->domain == 0 ? "W" : "B");

            /* Name (truncate to fit) */
            u8g2_DrawStr(u, 12, y, d->name);

            /* RSSI right-aligned */
            int rw = u8g2_GetStrWidth(u, rssi_str);
            u8g2_DrawStr(u, 126 - rw, y, rssi_str);
        }

        /* Bottom bar */
        u8g2_DrawHLine(u, 0, 53, 128);
        char footer[32];
        if (dev_count == 0)
            snprintf(footer, sizeof(footer), "none  OK:scan");
        else
            snprintf(footer, sizeof(footer), "%d/%d  OK:scan BK:exit",
                     scroll_top + 1, dev_count);
        u8g2_DrawStr(u, 2, 63, footer);

        /* Scroll indicator */
        if (dev_count > LIST_ROWS) {
            int bar_h = (LIST_ROWS * 40) / dev_count;
            int bar_y = LIST_Y_START + (scroll_top * (40 - bar_h)) /
                        (dev_count - LIST_ROWS);
            if (bar_y + bar_h > LIST_Y_START + 40)
                bar_y = LIST_Y_START + 40 - bar_h;
            u8g2_DrawVLine(u, 126, bar_y, bar_h);
        }
    } while (m1app_display_flush());
}

static void save_summary(void)
{
    if (dev_count == 0)
        return;

    /* Build summary text in chunks (m1_storage_call WRITE does atomic replace) */
    typedef struct {
        uint16_t size; uint8_t version; uint8_t flags;
        char path[64];
        uint16_t datalen;
        uint8_t data[256];
    } __attribute__((packed)) write_req_t;

    static write_req_t w;
    w.size = sizeof(w);
    w.version = 1;
    w.flags = 0;
    strcpy(w.path, "atlas_last.txt");

    int off = 0;
    off += snprintf((char*)&w.data[off], 80 - off,
                    "Signal Atlas scan\ndevices=%d\n", dev_count);
    for (int i = 0; i < dev_count && off < 240; i++) {
        off += snprintf((char*)&w.data[off], 250 - off,
                        "%s %s %ddBm %s\n",
                        devices[i].domain == 0 ? "W" : "B",
                        devices[i].name, devices[i].rssi,
                        devices[i].detail);
    }
    w.datalen = off;
    m1_storage_call(M1_STO_OP_WRITE, &w, sizeof(w), 0, 0);
}

int32_t app_main(void *param)
{
    (void)param;
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);

    /* Check brain is initialized */
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

    /* Main loop: display, handle input */
    while (1) {
        draw_list();
        m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);

        game_button_t btn = game_poll_button(500);
        switch (btn) {
        case GAME_BTN_OK:
            do_scan();
            break;
        case GAME_BTN_UP:
            if (scroll_top > 0) scroll_top--;
            break;
        case GAME_BTN_DOWN:
            if (scroll_top < dev_count - LIST_ROWS) scroll_top++;
            break;
        case GAME_BTN_BACK:
            save_summary();
            return 0;
        default:
            break;
        }
    }
    return 0;
}
