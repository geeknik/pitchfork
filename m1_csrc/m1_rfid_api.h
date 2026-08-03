/* See COPYING.txt for license details. */

/*
 * m1_rfid_api.h — composable 125 kHz LF-RFID primitive API for .m1app apps.
 *
 * Thin, stable verbs over the existing lfrfid engine + T5577 driver so apps can
 * build their own LF tools (read / clone / write / erase / emulate) instead of
 * only launching the built-in menus. Single-owner model: the app owns the RFID
 * engine for the duration (start -> ... -> stop); the engine's own decode task
 * is parked while an app drives the field directly from the caller's task.
 *
 * Lifecycle:
 *     m1_rfid_start();
 *         m1_rfid_read(&tag, 6000);          // scan a source tag
 *         m1_rfid_write_t5577(&tag);         // clone onto a blank T5577
 *         m1_rfid_emulate_start(&tag); ... m1_rfid_emulate_stop();
 *     m1_rfid_stop();
 */

#ifndef M1_RFID_API_H_
#define M1_RFID_API_H_

#include <stdint.h>
#include <stdbool.h>

#include "lfrfid.h"   /* LFRFID_TAG_INFO, protocol enums, engine globals */

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the LF-RFID engine: EN_EXT_5V rail high + lfrfid_Init().
 * Returns false if allocation / task creation failed. */
bool m1_rfid_start(void);

/* Tear the engine down: lfrfid_DeInit(); RFID_PULL low; EN_EXT_5V rail low. */
void m1_rfid_stop(void);

/* Blocking read with carrier auto-cycle (ASK 125k / ASK 134.2k / PSK, ~2 s each).
 * Fills *out (uid/protocol/bitrate) on success.
 * Returns 1 = tag decoded, 0 = timeout, <0 = error. */
int m1_rfid_read(LFRFID_TAG_INFO *out, uint16_t timeout_ms);

/* Clone a decoded tag onto a blank T5577 (also serves Clone read->write and
 * Erase: pass a blank tag {protocol=LFRFIDProtocolEM4100, uid all zero}).
 * Returns true if the write sequence was issued. */
bool m1_rfid_write_t5577(const LFRFID_TAG_INFO *tag);

/* Expert: raw ATA5577 block programming. block_data[0..max_blocks-1] are the
 * 32-bit block words (block 0 is the config block). max_blocks 1..8.
 * Returns true if the write sequence was issued. */
bool m1_rfid_write_t5577_blocks(const uint32_t *block_data, uint32_t max_blocks);

/* Start LF emulation of a tag (protocol encoder -> emul HW output).
 * Returns true if emulation started. */
bool m1_rfid_emulate_start(const LFRFID_TAG_INFO *tag);

/* Stop LF emulation. */
void m1_rfid_emulate_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* M1_RFID_API_H_ */
