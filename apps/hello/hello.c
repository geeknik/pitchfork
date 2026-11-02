/* hello.c — diagnostic first app: proves ELF loading + symbol resolution
 * + storage umbrella WITHOUT depending on the display. Writes a marker file
 * to the SD card at each stage. After running, check 0:/AppData/app_test.txt
 * via m1ctl pull to see exactly how far app_main got. */
#include "m1app-sdk.h"
#include <string.h>

const char app_name[] = "Hello Pitchfork";

/* storage request/response from m1_app_syscall.h */
typedef struct {
    uint16_t size;
    uint8_t version;
    uint8_t flags;
    char path[64];
    uint32_t offset;
    uint16_t length;
} __attribute__((packed)) sto_read_req_t;

typedef struct {
    uint16_t size;
    uint8_t version;
    uint8_t flags;
    uint16_t length;
    uint32_t fresult;
    uint8_t data[256];
} __attribute__((packed)) sto_read_resp_t;

typedef struct {
    uint16_t size;
    uint8_t version;
    uint8_t flags;
    char path[64];
    uint16_t datalen;
    uint8_t data[256];
} __attribute__((packed)) sto_write_req_t;

static int write_marker(const char *msg)
{
    static sto_write_req_t w;
    w.size = sizeof(w);
    w.version = 1;
    w.flags = 0;
    strcpy(w.path, "marker.txt");
    w.datalen = strlen(msg);
    memcpy(w.data, msg, w.datalen);
    return (int)m1_storage_call(2 /* WRITE */, &w, sizeof(w), 0, 0);
}

int32_t app_main(void *param)
{
    (void)param;

    /* Stage 1: app_main is running */
    write_marker("app_main entered\n");

    /* Stage 2: sys_call works */
    int32_t api = m1_sys_call(0 /* API_LEVEL */, 0);
    write_marker("sys_call works\n");

    /* Stage 3: display works */
    u8g2_t *u = &m1_u8g2;
    u8g2_SetFont(u, u8g2_font_6x10_tr);
    u8g2_FirstPage(u);
    do {
        u8g2_DrawStr(u, 2, 12, "Hello from .m1app!");
        u8g2_DrawStr(u, 2, 26, "API:");
        if (api > 0) u8g2_DrawStr(u, 40, 26, "OK");
        u8g2_DrawStr(u, 2, 40, "It works!");
    } while (u8g2_NextPage(u));

    write_marker("display drawn\n");

    /* Show for 5 seconds */
    m1app_delay(5000);

    write_marker("done\n");
    return 0;
}
