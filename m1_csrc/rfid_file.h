/* See COPYING.txt for license details. */

/*
 * rfid_file.h
 *
 * Flipper Zero .rfid file format parser
 *
 * M1 Project
 */

#ifndef FLIPPER_RFID_H_
#define FLIPPER_RFID_H_

#include "ff_file.h"
#include "stm32h5xx_hal.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "stream_buffer.h"
#include "lfrfid.h"

typedef struct {
	LFRFIDProtocol protocol;
	uint8_t  data[8];    /* Raw protocol data */
	uint8_t  data_len;
	char     protocol_name[32];
} rfid_file_tag_t;

/* Load a .rfid file */
bool rfid_file_load(const char *path, rfid_file_tag_t *out);

/* Save a .rfid file */
bool rfid_file_save(const char *path, const rfid_file_tag_t *tag);

/* Map Flipper RFID protocol name to M1 LFRFIDProtocol enum */
LFRFIDProtocol rfid_file_parse_protocol(const char *name);

#endif /* FLIPPER_RFID_H_ */
