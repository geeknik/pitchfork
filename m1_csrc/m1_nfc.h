/* See COPYING.txt for license details. */

/*
*
*  m1_nfc.h
*
*  M1 NFC functions
*
* M1 Project
*
*/

#ifndef M1_NFC_H_
#define M1_NFC_H_

void nfc_read(void);
void nfc_detect_reader(void);
void nfc_saved(void);
void nfc_extra_actions(void);
void nfc_add_manually(void);
void nfc_tools(void);
void nfc_tool_tag_info(void);   /* standalone Tag Info (exported as nfc_tag_info) */

void menu_nfc_init(void);
void menu_nfc_deinit(void);

/* ===================================================================== */
/*  Composable NFC primitive API (m1_nfc_api.c) — exported to .m1app apps */
/* ===================================================================== */

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    M1_NFC_TECH_A = 0,      /* ISO14443-A (MIFARE, NTAG/UL, DESFire) */
    M1_NFC_TECH_B,          /* ISO14443-B */
    M1_NFC_TECH_F,          /* FeliCa */
    M1_NFC_TECH_V,          /* ISO15693 (vicinity) */
    M1_NFC_TECH_ST25TB,     /* ST25TB */
    M1_NFC_TECH_UNKNOWN = 0xFF
} m1_nfc_tech_t;

/* A discovered tag. `atqa`/`sak` are meaningful only for NFC-A. */
typedef struct {
    uint8_t tech;        /* m1_nfc_tech_t */
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;         /* NFC-A SEL_RES */
    uint8_t atqa[2];     /* NFC-A SENS_RES: [0]=LSB, [1]=MSB */
} m1_nfc_target_t;

/* Power the reader field + bring up (on) or tear down (off) the ST25R3916. */
bool m1_nfc_field_on(bool on);
/* Poll for a tag (ms). 1 = found (fills *out, target left activated), 0 = none, <0 = error. */
int  m1_nfc_poll(m1_nfc_target_t *out, uint16_t timeout_ms);
/* Send a raw frame to the activated target; returns response length (>=0) or <0. */
int  m1_nfc_transceive(const uint8_t *tx, uint16_t txlen,
                       uint8_t *rx, uint16_t rxcap, uint32_t timeout_ms);
/* NTAG/Type-2: read one 4-byte page. Returns 4 or <0. */
int  m1_nfc_ntag_read_page(uint8_t page, uint8_t out[4]);
/* NTAG PWD_AUTH (0x1B). true + fills pack_out[2] on success. */
bool m1_nfc_ntag_pwd_auth(const uint8_t pwd[4], uint8_t pack_out[2]);
/* Write one 4-byte NTAG/Type-2 page (writable pages, or magic-card UID pages). */
bool m1_nfc_ntag_write_page(uint8_t page, const uint8_t data[4]);
/* MIFARE Classic: auth block's sector (keytype 0=A,1=B) then read 16-byte block. */
bool m1_nfc_mifare_read_block(uint8_t block, uint8_t keytype,
                              const uint8_t key[6], uint8_t out[16]);

/* --- Emulation (card emulation) --- */
/* Emulate a simple NFC-A identity (uid_len 4 or 7; persona RAW). */
bool m1_nfc_emulate_uid(const uint8_t *uid, uint8_t uid_len,
                        uint8_t sak, uint8_t atqa0, uint8_t atqa1);
/* Emulate a full NTAG/Type-2 dump (4-byte pages; UID from pages 0-1). */
bool m1_nfc_emulate_ntag(const uint8_t *dump, uint16_t len);
/* Live-update the emulated NFC-A UID without restarting (UID fuzzing). */
bool m1_nfc_emulate_uid_update(const uint8_t *uid, uint8_t uid_len,
                               uint8_t sak, uint8_t atqa0, uint8_t atqa1);
/* Stop any active emulation. */
bool m1_nfc_emulate_stop(void);

#endif /* M1_NFC_H_ */
