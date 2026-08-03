/* See COPYING.txt for license details. */

/*
 * m1_rfid_api.c — composable 125 kHz LF-RFID primitive API for .m1app apps.
 *
 * Drives the existing lfrfid engine + T5577 driver DIRECTLY from the caller's
 * app task (the proven app-context pattern used by m1_nfc_api.c). The engine's
 * own decode task (lfrfid_rx_task_hdl) is parked (suspended) for the whole
 * session so the app is the single reader of the sample stream buffer — no
 * cross-task locking, one LF user at a time (built-in menus OR an app, never
 * both).
 *
 * Read: mirrors the lfrfidThread Q_EVENT_UI_LFRFID_START_READ bring-up +
 *       lfrfid_rxThread decode loop + Q_EVENT_UI_LFRFID_CARRIER_SWITCH cycling,
 *       inlined without the GUI queue.
 * Write / Emulate: mirror the Q_EVENT_UI_LFRFID_WRITE / _EMULATE handlers.
 */

#include <string.h>

#include "stm32h5xx_hal.h"
#include "main.h"            /* EN_EXT_5V_Pin, RFID_PULL_Pin, HAL, pins */
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"

#include "lfrfid.h"          /* engine globals, LFRFID_TAG_INFO, hal, t5577, protocol */
#include "m1_watchdog.h"     /* m1_wdt_reset */
#include "m1_rfid.h"
#include "m1_rfid_api.h"

/* Settle time after switching the carrier before sampling on the new mode. */
#ifndef LFRFID_CARRIER_STABILIZE_MS
#define LFRFID_CARRIER_STABILIZE_MS  50
#endif

/* ---- symbols owned by lfrfid.c that are global but not header-declared ---- */
extern TaskHandle_t lfrfid_rx_task_hdl;         /* the decode task we park */
extern void task_suspend_safe(TaskHandle_t xHandle);
extern void task_resume_safe(TaskHandle_t xHandle);

/* ===================================================================== */
/*  Lifecycle                                                            */
/* ===================================================================== */

/* EN_EXT_5V high + lfrfid_Init(). Park the engine decode task so the app owns
 * the sample stream. Returns false if alloc/task creation failed. */
bool m1_rfid_start(void)
{
    if (lfrfid_q_hdl) return true;              /* already initialised */

    /* lfrfid_Init() allocates ~31 KB (a 16 KB decode task, a 4 KB event task, a
     * ~9.6 KB encoder buffer, plus queue/timers/TCBs) and ASSERTS internally if
     * fewer than M1_LOW_FREE_HEAP_WARNING_SIZE (4 KB) remain after ANY single
     * alloc — so the true floor is ~35 KB free, not 30 KB. The old 30 KB guard
     * could pass and then assert-reboot mid-init. Require 40 KB for margin; from
     * an app (which already holds its own task stack on this heap) this bails
     * gracefully so the app shows "RFID init failed" instead of rebooting. */
    if (xPortGetFreeHeapSize() < 40000u)
        return false;

    lfrfid_Init();                              /* allocs + creates tasks/timers */
    HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port, EN_EXT_5V_Pin, GPIO_PIN_SET);

    if (!lfrfid_q_hdl || !lfrfid_rx_task_hdl || !lfrfid_program || !lfrfid_sb_hdl) {
        HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port, EN_EXT_5V_Pin, GPIO_PIN_RESET);
        return false;
    }

    /* Leave the engine's decode task running exactly as the built-in menu does —
     * m1_rfid_read drives everything through the lfrfidThread event pump, so the
     * decode happens in the engine's own 16 KB task, not the app's stack. */
    return true;
}

/* lfrfid_DeInit(); RFID_PULL low; EN_EXT_5V low. */
void m1_rfid_stop(void)
{
    /* Make sure no capture/carrier or emul HW is left running. */
    lfrfid_read_hw_deinit();                    /* guarded: no-op if idle */
    lfrfid_emul_hw_deinit();

    lfrfid_DeInit();

    HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port,  EN_EXT_5V_Pin,  GPIO_PIN_RESET);
}

/* ===================================================================== */
/*  Read (blocking, carrier auto-cycle)                                  */
/* ===================================================================== */

int m1_rfid_read(LFRFID_TAG_INFO *out, uint16_t timeout_ms)
{
    if (!out) return -1;
    if (!lfrfid_q_hdl) return -2;               /* not started */

    /* Delegate the ENTIRE read — capture ISR + decode + carrier auto-cycle — to
     * the engine's own tasks. Posting Q_EVENT_UI_LFRFID_START_READ makes the
     * lfrfidThread event pump resume the 16 KB decode task, init the field, and
     * start the read + carrier-switch timers (exactly like the built-in menu).
     * The decode is stack-heavy, so it MUST run in that task, not ours — doing it
     * in the app task overflows the app stack and reboots. We only trigger and
     * poll the shared result (the rx task writes lfrfid_tag_info.protocol on a
     * successful decode; lfrfid_tag_info_init sets it to 0xFF beforehand). */
    lfrfid_tag_info.protocol = (uint8_t)-1;     /* sentinel: nothing decoded yet */
    m1_app_send_q_message(lfrfid_q_hdl, Q_EVENT_UI_LFRFID_START_READ);

    uint32_t t0    = HAL_GetTick();
    int      found = 0;
    while ((HAL_GetTick() - t0) < timeout_ms) {
        m1_wdt_reset();
        if (lfrfid_tag_info.protocol != (uint8_t)-1) { found = 1; break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    /* Stop the read: the pump deinits the field, stops the timers, clears its
     * lock so a subsequent read can start. */
    m1_app_send_q_message(lfrfid_q_hdl, Q_EVENT_UI_LFRFID_STOP);
    vTaskDelay(pdMS_TO_TICKS(40));

    if (found) {
        lfrfid_GetTagInfo(out);
        return 1;
    }
    return 0;
}

/* ===================================================================== */
/*  Write (T5577)                                                        */
/* ===================================================================== */

/* Clone/erase: load the tag into the engine working buffer and run the
 * protocol's T5577 write path (mirror of Q_EVENT_UI_LFRFID_WRITE). */
bool m1_rfid_write_t5577(const LFRFID_TAG_INFO *tag)
{
    if (!tag || !lfrfid_q_hdl || !lfrfid_program) return false;
    if (tag->protocol >= LFRFIDProtocolMax)       return false;

    task_suspend_safe(lfrfid_rx_task_hdl);
    lfrfid_read_hw_deinit();                     /* guarded: free TIM3/TIM5 if a read was live */

    /* write.begin builds from lfrfid_tag_info; write.send reads lfrfid_program. */
    memcpy(&lfrfid_tag_info, tag, sizeof(LFRFID_TAG_INFO));

    /* Pre-charge the tag field cap (as the built-in retry path does). */
    HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_SET);
    HAL_Delay(52);

    lfrfid_state = LFRFID_STATE_WRITE;
    lfrfid_program->type = LFRFIDProgramTypeT5577;
    lfrfid_write_begin(lfrfid_tag_info.protocol, &lfrfid_tag_info, lfrfid_program);
    lfrfid_write_send(lfrfid_tag_info.protocol, NULL);   /* -> t5577_execute_write() */
    HAL_Delay(1000);

    HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_RESET);
    lfrfid_state = LFRFID_STATE_IDLE;
    return true;
}

/* Expert: raw ATA5577 block programming. t5577_execute_write() is fully
 * self-contained (drives RFID_PULL + the 125 kHz carrier itself). */
bool m1_rfid_write_t5577_blocks(const uint32_t *block_data, uint32_t max_blocks)
{
    if (!block_data || max_blocks == 0 || max_blocks > T5577_BLOCK_COUNT) return false;

    task_suspend_safe(lfrfid_rx_task_hdl);
    lfrfid_read_hw_deinit();                     /* guarded */

    LFRFIDProgram prog;
    memset(&prog, 0, sizeof(prog));
    prog.type = LFRFIDProgramTypeT5577;
    for (uint32_t i = 0; i < max_blocks; i++)
        prog.t5577.block_data[i] = block_data[i];
    prog.t5577.max_blocks = max_blocks;

    HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_SET);
    HAL_Delay(52);

    lfrfid_state = LFRFID_STATE_WRITE;
    t5577_execute_write(&prog, 0);
    HAL_Delay(20);

    HAL_GPIO_WritePin(RFID_PULL_GPIO_Port, RFID_PULL_Pin, GPIO_PIN_RESET);
    lfrfid_state = LFRFID_STATE_IDLE;
    return true;
}

/* ===================================================================== */
/*  Emulation                                                            */
/* ===================================================================== */

/* Mirror of Q_EVENT_UI_LFRFID_EMULATE. The protocol encoder_send() brings up
 * the emulation timer HW internally (lfrfid_emul_hw_init), so we do NOT call it
 * separately here (that would double-init TIM5). */
bool m1_rfid_emulate_start(const LFRFID_TAG_INFO *tag)
{
    if (!tag) return false;
    if (tag->protocol >= LFRFIDProtocolMax) return false;

    task_suspend_safe(lfrfid_rx_task_hdl);
    lfrfid_read_hw_deinit();                     /* guarded */

    memcpy(&lfrfid_tag_info, tag, sizeof(LFRFID_TAG_INFO));

    lfrfid_state = LFRFID_STATE_EMULATE;
    if (!lfrfid_encoder_begin(lfrfid_tag_info.protocol, &lfrfid_tag_info)) {
        lfrfid_state = LFRFID_STATE_IDLE;
        return false;
    }
    lfrfid_encoder_send(lfrfid_tag_info.protocol, NULL);   /* starts emul HW output */
    rfid_rxtx_is_taking_this_irq = 1;
    return true;
}

/* Mirror of Q_EVENT_UI_LFRFID_EMULATE_STOP. */
void m1_rfid_emulate_stop(void)
{
    lfrfid_state = LFRFID_STATE_IDLE;
    lfrfid_emul_hw_deinit();
}
