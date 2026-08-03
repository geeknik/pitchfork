/*
 * m1_nfc_api.c — composable NFC primitive API for .m1app apps.
 *
 * Thin, stable verbs over the existing RFAL / ST25R3916 driver so apps can build
 * their own NFC tools (read / clone / emulate) instead of only launching the
 * built-in. Drives the reader DIRECTLY from the caller's task (the proven
 * app-context pattern used by m1_field_detect) — the nfc_worker_task stays idle
 * while an app owns the field, so no cross-task locking is needed (one NFC user
 * at a time: either the built-in worker OR an app, never both).
 *
 * Lifecycle: m1_nfc_field_on(true) -> poll/transceive/read... -> field_on(false).
 */

#include <string.h>

#include "stm32h5xx_hal.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

#include "rfal_nfc.h"
#include "rfal_rf.h"
#include "rfal_t2t.h"
#include "st25r3916.h"
#include "st25r3916_com.h"
#include "nfc_conf.h"        /* USR_INT_LINE / USR_INT_LINE_NUM */
#include "nfc_poller.h"      /* nfc_poller_pwd_auth */
#include "mfc_crypto1.h"     /* mfc_auth / mfc_read_block_crypto */
#include "nfc_driver.h"      /* Emu_SetNfcA, nfc_worker_q_hdl, NFC roles */
#include "nfc_listener.h"    /* Emu_SetPersona, ListenerRequestStop, EmuPersona_t */
#include "common/nfc_ctx.h"  /* nfc_ctx_set_dump, g_nfc_dump_buf, g_nfc_valid_bits */
#include "m1_lib.h"          /* m1_app_send_q_message */
#include "m1_watchdog.h"
#include "m1_nfc.h"

extern EXTI_HandleTypeDef USR_INT_LINE;
extern void st25r3916Isr(void);

static bool    s_field_on  = false;
static bool    s_activated = false;
static uint8_t s_uid[10];
static uint8_t s_uid_len   = 0;

static uint8_t map_tech(rfalNfcDevType t)
{
    switch (t) {
    case RFAL_NFC_LISTEN_TYPE_NFCA:   return M1_NFC_TECH_A;
    case RFAL_NFC_LISTEN_TYPE_NFCB:   return M1_NFC_TECH_B;
    case RFAL_NFC_LISTEN_TYPE_NFCF:   return M1_NFC_TECH_F;
    case RFAL_NFC_LISTEN_TYPE_NFCV:   return M1_NFC_TECH_V;
    case RFAL_NFC_LISTEN_TYPE_ST25TB: return M1_NFC_TECH_ST25TB;
    default:                          return M1_NFC_TECH_UNKNOWN;
    }
}

/* Power the reader field + bring up the ST25R3916 (on), or tear it down (off).
 * Mirrors m1_field_detect_start/stop exactly (proven app-context bring-up). */
bool m1_nfc_field_on(bool on)
{
    if (on) {
        if (s_field_on) return true;

        HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port,  EN_EXT_5V_Pin,  GPIO_PIN_SET);
        HAL_GPIO_WritePin(EN_EXT_3V3_GPIO_Port, EN_EXT_3V3_Pin, GPIO_PIN_SET);
        HAL_Delay(50);

        USR_INT_LINE.Line = USR_INT_LINE_NUM;
        USR_INT_LINE.RisingCallback = st25r3916Isr;
        HAL_EXTI_GetHandle(&USR_INT_LINE, USR_INT_LINE.Line);
        HAL_EXTI_RegisterCallback(&USR_INT_LINE, HAL_EXTI_COMMON_CB_ID,
                                  USR_INT_LINE.RisingCallback);

        bool ok = false;
        for (int i = 0; i < 5; i++) {
            if (rfalNfcInitialize() == RFAL_ERR_NONE) { ok = true; break; }
            HAL_Delay(20);
        }
        if (!ok) {
            HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port, EN_EXT_5V_Pin, GPIO_PIN_RESET);
            return false;
        }
        s_field_on  = true;
        s_activated = false;
        s_uid_len   = 0;
        return true;
    }

    if (!s_field_on) return true;
    rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
    st25r3916Deinitialize();
    HAL_GPIO_WritePin(EN_EXT_5V_GPIO_Port, EN_EXT_5V_Pin, GPIO_PIN_RESET);
    s_field_on  = false;
    s_activated = false;
    s_uid_len   = 0;
    return true;
}

/* Poll for a tag until timeout. Returns 1 = found (fills *out and leaves the
 * target activated for transceive/read), 0 = none, <0 = error. */
int m1_nfc_poll(m1_nfc_target_t *out, uint16_t timeout_ms)
{
    if (!s_field_on || !out) return -1;

    rfalNfcDiscoverParam disc;
    rfalNfcDefaultDiscParams(&disc);
    disc.devLimit      = 1U;
    disc.totalDuration = 1000U;
    disc.notifyCb      = NULL;           /* we poll rfalNfcWorker() directly */
    disc.techs2Find    = RFAL_NFC_TECH_NONE;
#if RFAL_FEATURE_NFCA
    disc.techs2Find |= RFAL_NFC_POLL_TECH_A;
#endif
#if RFAL_FEATURE_NFCB
    disc.techs2Find |= RFAL_NFC_POLL_TECH_B;
#endif
#if RFAL_FEATURE_NFCF
    disc.techs2Find |= RFAL_NFC_POLL_TECH_F;
#endif
#if RFAL_FEATURE_NFCV
    disc.techs2Find |= RFAL_NFC_POLL_TECH_V;
#endif
#if RFAL_FEATURE_ST25TB
    disc.techs2Find |= RFAL_NFC_POLL_TECH_ST25TB;
#endif

    rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
    s_activated = false;
    if (rfalNfcDiscover(&disc) != RFAL_ERR_NONE) return -2;

    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms) {
        rfalNfcWorker();
        if (rfalNfcIsDevActivated(rfalNfcGetState())) {
            rfalNfcDevice *dev = NULL;
            if (rfalNfcGetActiveDevice(&dev) == RFAL_ERR_NONE && dev) {
                memset(out, 0, sizeof(*out));
                out->tech = map_tech(dev->type);
                uint8_t ul = (dev->nfcidLen > sizeof(out->uid)) ? (uint8_t)sizeof(out->uid)
                                                                : dev->nfcidLen;
                memcpy(out->uid, dev->nfcid, ul);
                out->uid_len = ul;
                if (dev->type == RFAL_NFC_LISTEN_TYPE_NFCA) {
                    out->sak     = dev->dev.nfca.selRes.sak;
                    out->atqa[0] = dev->dev.nfca.sensRes.anticollisionInfo;
                    out->atqa[1] = dev->dev.nfca.sensRes.platformInfo;
                }
                memcpy(s_uid, out->uid, ul);
                s_uid_len   = ul;
                s_activated = true;
                return 1;
            }
        }
        m1_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return 0;
}

/* Send a raw command frame to the activated target, get the response. Returns
 * the response length in bytes (>=0), or <0 on error. The universal primitive:
 * APDUs, NTAG commands, magic-card frames, anything the target speaks. */
int m1_nfc_transceive(const uint8_t *tx, uint16_t txlen,
                      uint8_t *rx, uint16_t rxcap, uint32_t timeout_ms)
{
    if (!s_field_on || !tx || !rx) return -1;
    uint16_t act = 0;
    uint32_t ms  = timeout_ms ? timeout_ms : 20U;
    if (ms > 1000U) ms = 1000U;
    ReturnCode e = rfalTransceiveBlockingTxRx((uint8_t *)tx, txlen, rx, rxcap, &act,
                                              RFAL_TXRX_FLAGS_DEFAULT,
                                              rfalConvMsTo1fc(ms));
    if (e != RFAL_ERR_NONE) return -100 - (int)e;
    return (int)act;
}

/* Read one 4-byte NTAG / Type-2 page. Returns 4 on success, <0 on error. */
int m1_nfc_ntag_read_page(uint8_t page, uint8_t out[4])
{
    if (!s_field_on || !out) return -1;
    uint8_t  rx[16];
    uint16_t rcv = 0;
    if (rfalT2TPollerRead(page, rx, sizeof(rx), &rcv) != RFAL_ERR_NONE || rcv < 4)
        return -2;
    memcpy(out, rx, 4);
    return 4;
}

/* Write one 4-byte NTAG / Type-2 page. Works on writable NTAG/UL pages and on
 * magic (Gen1a/Gen2) T2T cards' page 0-2 for UID writes. Returns true on OK. */
bool m1_nfc_ntag_write_page(uint8_t page, const uint8_t data[4])
{
    if (!s_field_on || !data) return false;
    return rfalT2TPollerWrite(page, data) == RFAL_ERR_NONE;
}

/* NTAG PWD_AUTH (0x1B). Returns true + fills pack_out[2] on success. */
bool m1_nfc_ntag_pwd_auth(const uint8_t pwd[4], uint8_t pack_out[2])
{
    if (!s_field_on || !pwd) return false;
    uint8_t pack[2] = { 0 };
    ReturnCode e = nfc_poller_pwd_auth(pwd, pack);
    if (pack_out) memcpy(pack_out, pack, 2);
    return e == RFAL_ERR_NONE;
}

/* MIFARE Classic: authenticate `block`'s sector (keytype 0 = Key A, 1 = Key B)
 * with the 6-byte key, then read the 16-byte block. Uses the software Crypto-1
 * against the currently-activated NFC-A target. Returns true on success. */
bool m1_nfc_mifare_read_block(uint8_t block, uint8_t keytype,
                              const uint8_t key[6], uint8_t out[16])
{
    if (!s_field_on || !s_activated || !key || !out || s_uid_len < 4) return false;
    /* 4-byte UID cards use bytes[0..3]; 7-byte UID cards use bytes[3..6]. */
    const uint8_t *uid4 = (s_uid_len == 7) ? &s_uid[3] : &s_uid[0];
    crypto1_state_t st;
    uint8_t cmd = keytype ? MFC_AUTH_CMD_B : MFC_AUTH_CMD_A;
    if (!mfc_auth(&st, uid4, block, cmd, key)) return false;
    return mfc_read_block_crypto(&st, block, out);
}

/* ===================================================================== */
/*  Emulation (card emulation). Runs on the shared nfc_worker task via the
 *  listener role — the app sets up identity + optional dump, starts it, then
 *  stops on exit. Releases any direct-mode field first (one NFC user at a time). */
/* ===================================================================== */

static bool s_emulating = false;

/* Emulate a simple NFC-A tag identity (persona RAW = use the given ATQA/SAK).
 * uid_len 4 or 7. Good for UID cloning (access fobs). */
bool m1_nfc_emulate_uid(const uint8_t *uid, uint8_t uid_len,
                        uint8_t sak, uint8_t atqa0, uint8_t atqa1)
{
    if (!uid || (uid_len != 4 && uid_len != 7)) return false;
    if (s_field_on) m1_nfc_field_on(false);
    menu_nfc_init();                       /* ensure the worker task + queue exist */
    if (!nfc_worker_q_hdl) return false;
    Emu_SetPersona(EMU_PERSONA_RAW);
    Emu_SetNfcA(uid, uid_len, atqa0, atqa1, sak);
    m1_app_send_q_message(nfc_worker_q_hdl, Q_EVENT_NFC_START_EMULATE);
    s_emulating = true;
    return true;
}

/* Emulate a full NTAG / Type-2 dump (e.g. an amiibo). `dump` = raw 4-byte pages;
 * the UID is taken from pages 0-1. Persona T2T serves reader READ commands from
 * the dump. `len` must be <= NFC_DUMP_BUF_SIZE. */
bool m1_nfc_emulate_ntag(const uint8_t *dump, uint16_t len)
{
    if (!dump || len < 16 || len > NFC_DUMP_BUF_SIZE) return false;
    if (s_field_on) m1_nfc_field_on(false);
    menu_nfc_init();
    if (!nfc_worker_q_hdl) return false;

    memset(g_nfc_dump_buf, 0x00, NFC_DUMP_BUF_SIZE);
    memcpy(g_nfc_dump_buf, dump, len);
    memset(g_nfc_valid_bits, 0xFF, NFC_VALID_BITS_SIZE);   /* all pages valid */

    /* NTAG UID: pages 0-1 -> p0[0..2] + p1[0..3] (p0[3] is BCC0). */
    uint8_t uid[7] = { dump[0], dump[1], dump[2], dump[4], dump[5], dump[6], dump[7] };
    uint32_t pages = (uint32_t)(len / 4);

    nfc_ctx_set_dump(4, pages, 0, g_nfc_dump_buf, g_nfc_valid_bits, pages - 1, true);
    Emu_SetPersona(EMU_PERSONA_T2T);
    Emu_SetNfcA(uid, 7, 0x44, 0x00, 0x00);                 /* NTAG: ATQA 0x0044, SAK 0x00 */
    m1_app_send_q_message(nfc_worker_q_hdl, Q_EVENT_NFC_START_EMULATE);
    s_emulating = true;
    return true;
}

/* Live-update the emulated NFC-A identity WITHOUT restarting the listener — for
 * UID fuzzing. Call m1_nfc_emulate_uid() once to start, then this per step. */
bool m1_nfc_emulate_uid_update(const uint8_t *uid, uint8_t uid_len,
                               uint8_t sak, uint8_t atqa0, uint8_t atqa1)
{
    if (!s_emulating || !uid || (uid_len != 4 && uid_len != 7)) return false;
    Emu_SetNfcA(uid, uid_len, atqa0, atqa1, sak);
    return true;
}

/* Stop any active emulation. */
bool m1_nfc_emulate_stop(void)
{
    if (!s_emulating) return true;
    ListenerRequestStop();
    if (nfc_worker_q_hdl)
        m1_app_send_q_message(nfc_worker_q_hdl, Q_EVENT_NFC_EMULATE_STOP);
    s_emulating = false;
    return true;
}
