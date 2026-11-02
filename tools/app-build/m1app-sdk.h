#ifndef M1APP_SDK_H
#define M1APP_SDK_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define M1APP_API_VERSION 9
int32_t m1_sys_call(uint32_t op, void *args);
int32_t m1_storage_call(uint32_t op, const void *req, uint16_t rl, void *resp, uint16_t rcl);
int32_t m1_ui_call(uint32_t op, const void *req, uint16_t rl, void *resp, uint16_t rcl);
int32_t m1_radio_call(uint32_t op, const void *req, uint16_t rl, void *resp, uint16_t rcl);
#define M1_SYS_OP_API_LEVEL 0
#define M1_SYS_OP_HEAP_FREE 1
#define M1_SYS_OP_UPTIME_MS 2
#define M1_SYS_OP_HEARTBEAT 3
#define M1_SYS_OP_CANCEL_POLL 4
#define M1_UI_OP_FOCUS_QUERY 0
#define M1_UI_OP_FLUSH 1
#define M1_UI_OP_NOTIFY 2
#define M1_STO_OP_STAT 0
#define M1_STO_OP_READ 1
#define M1_STO_OP_WRITE 2
#define M1_STO_OP_APPEND 3
#define M1_STO_OP_MKDIR 4
#define M1_STO_OP_REMOVE 5
#define M1_STO_OP_RENAME 6
#define M1_STO_OP_LIST 7
#define M1_RADIO_OP_TX_ALLOWED 0
#define M1_RADIO_OP_POWER_CLAMP 1
#define M1_RADIO_OP_REGION_GET 2
#define M1_RADIO_OP_REGION_SET 3
extern void *m1app_malloc(size_t n);
extern void m1app_free(void *p);
extern void m1app_delay(uint32_t ms);
extern uint32_t m1app_get_tick(void);
#include "u8g2.h"
extern u8g2_t *m1app_get_u8g2(void);  /* m1_u8g2 is NOT exported — use this getter */
extern uint8_t game_poll_button(void);
extern uint8_t m1app_display_flush(void);
extern void m1_u8g2_firstpage(void);
extern uint8_t m1_u8g2_nextpage(void);
int32_t app_main(void *param);
#endif
