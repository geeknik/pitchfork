/* hello.c — first Pitchfork .m1app, using only exported u8g2 APIs. */
#include "m1app-sdk.h"

const char app_name[] = "Hello Pitchfork";

static void draw_num(u8g2_t *u, int x, int y, int32_t v)
{
    char buf[12];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else {
        uint32_t uv = (v < 0) ? -(uint32_t)v : (uint32_t)v;
        while (uv > 0) { buf[--i] = '0' + (uv % 10); uv /= 10; }
        if (v < 0) buf[--i] = '-';
    }
    u8g2_DrawStr(u, x, y, &buf[i]);
}

int32_t app_main(void *param)
{
    (void)param;
    m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
    int32_t api = m1_sys_call(M1_SYS_OP_API_LEVEL, 0);
    int32_t heap = m1_sys_call(M1_SYS_OP_HEAP_FREE, 0);

    u8g2_t *u = &m1_u8g2;
    u8g2_SetFont(u, u8g2_font_6x10_tr);

    for (int frame = 0; frame < 6; frame++) {
        u8g2_FirstPage(u);
        do {
            u8g2_DrawStr(u, 2, 12, "Hello from .m1app!");
            u8g2_DrawStr(u, 2, 26, "API:");
            draw_num(u, 40, 26, api);
            u8g2_DrawStr(u, 2, 40, "Heap:");
            draw_num(u, 48, 40, heap / 1024);
            u8g2_DrawStr(u, 90, 40, "KB");
            u8g2_DrawStr(u, 2, 56, "BACK to exit");
        } while (u8g2_NextPage(u));

        m1app_delay(500);
        m1_sys_call(M1_SYS_OP_HEARTBEAT, 0);
    }
    return 0;
}
