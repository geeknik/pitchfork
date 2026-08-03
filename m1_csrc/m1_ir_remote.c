/* See COPYING.txt for license details. */

/*
 * m1_ir_remote.c
 *
 * IR Remote — multi-button infrared remote control (load + create on-device).
 *
 * The IR equivalent of the Sub-GHz Remote. Five hardware buttons (UP / DOWN /
 * LEFT / RIGHT / OK) each map to one IR signal; pressing a mapped button fires
 * that signal immediately.
 *
 * An IR Remote *is* a standard .ir file whose signals are named after the
 * buttons ("up", "down", "left", "right", "ok"). This makes it self-contained
 * (no external file references to break), reloadable, and interoperable with
 * any tool that reads .ir files.
 *
 * On entry the feature offers two choices:
 *   - Load existing : browse 0:/IR for a .ir remote; signals named up/down/
 *                     left/right/ok are mapped to their buttons.
 *   - Create new    : for each button, pick a .ir file then a command within
 *                     it (or skip). Name it and it saves 0:/IR/<name>.ir.
 *
 * IR uses the blocking-menu UI model (like RFID / the IR universal remote),
 * not the Sub-GHz scene engine, so this whole flow is one blocking call that
 * drives its own draw + input loop off main_q_hdl / button_events_q_hdl.
 *
 * M1 Project
 */

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "main.h"
#include "m1_ir_remote.h"
#include "m1_system.h"
#include "m1_tasks.h"
#include "m1_display.h"
#include "m1_lcd.h"
#include "m1_infrared.h"
#include "m1_storage.h"
#include "m1_file_browser.h"
#include "m1_virtual_kb.h"
#include "ff_file.h"
#include "ir_file.h"

/*************************** D E F I N E S ************************************/

#define IR_REM_BTN_COUNT   5

#define IR_REM_BTN_UP      0
#define IR_REM_BTN_DOWN    1
#define IR_REM_BTN_LEFT    2
#define IR_REM_BTN_RIGHT   3
#define IR_REM_BTN_OK      4

/* Max commands listed by the in-file command picker. */
#define IR_REM_PICK_MAX    64

/* Keys returned by the blocking input helper. */
enum {
	IR_KEY_NONE = 0,
	IR_KEY_UP,
	IR_KEY_DOWN,
	IR_KEY_LEFT,
	IR_KEY_RIGHT,
	IR_KEY_OK,
	IR_KEY_BACK
};

/****************************** V A R I A B L E S *****************************/

/* One IR signal per button; .valid == false means "unassigned" (shown "---"). */
static ir_file_signal_t s_rem_sig[IR_REM_BTN_COUNT];

/* Pad title (remote name, no extension). */
static char s_rem_title[40];

/* Scratch for the command picker (names only, kept off the stack). */
static char     s_pick_names[IR_REM_PICK_MAX][FLIPPER_IR_NAME_MAX_LEN];
static uint16_t s_pick_count;

static const char *const s_rem_btn_names[IR_REM_BTN_COUNT] =
	{ "UP", "DOWN", "LEFT", "RIGHT", "OK" };
static const char *const s_rem_btn_keys[IR_REM_BTN_COUNT] =
	{ "up", "down", "left", "right", "ok" };

/*************************** I N P U T   H E L P E R **************************/

/* Block until a button is clicked; return which one. Mirrors the input model
 * used by the IR universal remote (main_q_hdl gate + button_events_q_hdl). */
static uint8_t ir_rem_get_key(void)
{
	S_M1_Buttons_Status bs;
	S_M1_Main_Q_t       q;

	for (;;)
	{
		if (xQueueReceive(main_q_hdl, &q, portMAX_DELAY) != pdTRUE)
			continue;
		if (q.q_evt_type != Q_EVENT_KEYPAD)
			continue;

		xQueueReceive(button_events_q_hdl, &bs, 0);

		if (bs.event[BUTTON_BACK_KP_ID]  == BUTTON_EVENT_CLICK) return IR_KEY_BACK;
		if (bs.event[BUTTON_UP_KP_ID]    == BUTTON_EVENT_CLICK) return IR_KEY_UP;
		if (bs.event[BUTTON_DOWN_KP_ID]  == BUTTON_EVENT_CLICK) return IR_KEY_DOWN;
		if (bs.event[BUTTON_LEFT_KP_ID]  == BUTTON_EVENT_CLICK) return IR_KEY_LEFT;
		if (bs.event[BUTTON_RIGHT_KP_ID] == BUTTON_EVENT_CLICK) return IR_KEY_RIGHT;
		if (bs.event[BUTTON_OK_KP_ID]    == BUTTON_EVENT_CLICK) return IR_KEY_OK;
	}
}

/****************************** D R A W ***************************************/

#define IR_REM_LABEL(i) (s_rem_sig[i].valid ? s_rem_sig[i].name : "---")

static void draw_choice(uint8_t sel)
{
	static const char *opts[2] = { "Load existing", "Create new" };

	u8g2_ClearBuffer(&m1_u8g2);
	u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
	m1_draw_text(&m1_u8g2, 2, 9, 124, "IR Remote", TEXT_ALIGN_CENTER);
	u8g2_DrawHLine(&m1_u8g2, 0, 10, 128);

	u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
	for (uint8_t i = 0; i < 2; i++)
	{
		uint8_t y = 24 + i * 14;
		if (i == sel)
		{
			u8g2_DrawRBox(&m1_u8g2, 1, y - 9, 126, 12, 2);
			u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
		}
		u8g2_DrawStr(&m1_u8g2, 6, y, opts[i]);
		u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
	}
	u8g2_DrawStr(&m1_u8g2, 4, 62, "OK: select   BACK: exit");
	u8g2_SendBuffer(&m1_u8g2);
}

static void draw_create(uint8_t btn)
{
	char buf[40];
	uint8_t assigned = 0;

	u8g2_ClearBuffer(&m1_u8g2);
	u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
	m1_draw_text(&m1_u8g2, 2, 9, 124, "Create Remote", TEXT_ALIGN_CENTER);
	u8g2_DrawHLine(&m1_u8g2, 0, 10, 128);

	u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
	snprintf(buf, sizeof(buf), "Assign %s button", s_rem_btn_names[btn]);
	u8g2_DrawStr(&m1_u8g2, 4, 24, buf);

	for (uint8_t i = 0; i < IR_REM_BTN_COUNT; i++)
		if (s_rem_sig[i].valid) assigned++;
	snprintf(buf, sizeof(buf), "Button %u/%u  (%u set)",
	         (unsigned)(btn + 1), (unsigned)IR_REM_BTN_COUNT, (unsigned)assigned);
	u8g2_DrawStr(&m1_u8g2, 4, 38, buf);

	u8g2_DrawStr(&m1_u8g2, 4, 62, "OK:pick  R:skip  BACK:cancel");
	u8g2_SendBuffer(&m1_u8g2);
}

/* Command picker list: title + up to 4 visible command names + selection. */
static void draw_cmd_list(const char *title, uint16_t sel)
{
	uint16_t first;

	u8g2_ClearBuffer(&m1_u8g2);
	u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
	m1_draw_text(&m1_u8g2, 2, 9, 124, title, TEXT_ALIGN_CENTER);
	u8g2_DrawHLine(&m1_u8g2, 0, 10, 128);

	u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);

	/* Keep the selection inside a 4-row window. */
	first = (sel < 4) ? 0 : (sel - 3);

	for (uint16_t row = 0; row < 4; row++)
	{
		uint16_t idx = first + row;
		uint8_t  y   = 22 + row * 11;
		if (idx >= s_pick_count)
			break;

		if (idx == sel)
		{
			u8g2_DrawRBox(&m1_u8g2, 1, y - 9, 126, 11, 2);
			u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
		}
		u8g2_DrawStr(&m1_u8g2, 4, y, s_pick_names[idx]);
		u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
	}

	u8g2_SendBuffer(&m1_u8g2);
}

static void draw_pad(void)
{
	u8g2_ClearBuffer(&m1_u8g2);

	u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
	char title_buf[32];
	snprintf(title_buf, sizeof(title_buf), "Remote: %.12s",
	         s_rem_title[0] ? s_rem_title : "?");
	m1_draw_text(&m1_u8g2, 2, 9, 124, title_buf, TEXT_ALIGN_CENTER);
	u8g2_DrawHLine(&m1_u8g2, 0, 10, 128);

	u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
	u8g2_DrawStr(&m1_u8g2, 32, 21, "\x18");
	u8g2_DrawStr(&m1_u8g2, 42, 21, IR_REM_LABEL(IR_REM_BTN_UP));
	u8g2_DrawStr(&m1_u8g2, 0,  34, "\x1b");
	u8g2_DrawStr(&m1_u8g2, 8,  34, IR_REM_LABEL(IR_REM_BTN_LEFT));
	u8g2_DrawStr(&m1_u8g2, 52, 34, "OK:");
	u8g2_DrawStr(&m1_u8g2, 68, 34, IR_REM_LABEL(IR_REM_BTN_OK));
	u8g2_DrawStr(&m1_u8g2, 96, 34, IR_REM_LABEL(IR_REM_BTN_RIGHT));
	u8g2_DrawStr(&m1_u8g2, 122, 34, "\x1a");
	u8g2_DrawStr(&m1_u8g2, 32, 47, "\x19");
	u8g2_DrawStr(&m1_u8g2, 42, 47, IR_REM_LABEL(IR_REM_BTN_DOWN));
	u8g2_DrawStr(&m1_u8g2, 4, 62, "Press to send");

	u8g2_SendBuffer(&m1_u8g2);
}

static void draw_status(const char *msg)
{
	u8g2_ClearBuffer(&m1_u8g2);
	u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
	m1_draw_text(&m1_u8g2, 2, 34, 124, msg, TEXT_ALIGN_CENTER);
	u8g2_SendBuffer(&m1_u8g2);
}

/****************************** F I R E **************************************/

static void ir_rem_fire(uint8_t btn)
{
	if (btn >= IR_REM_BTN_COUNT || !s_rem_sig[btn].valid)
		return;

	if (s_rem_sig[btn].type == FLIPPER_IR_SIGNAL_PARSED)
	{
		m1_ir_send(s_rem_sig[btn].parsed.protocol,
		           s_rem_sig[btn].parsed.address,
		           s_rem_sig[btn].parsed.command,
		           s_rem_sig[btn].parsed.flags);
	}
	else
	{
		m1_ir_send_raw(s_rem_sig[btn].raw.frequency,
		               s_rem_sig[btn].raw.samples,
		               s_rem_sig[btn].raw.sample_count);
	}
}

/****************************** L O A D **************************************/

/* Map an IR button key ("up".."ok") to a button index, or 0xFF if not one. */
static uint8_t ir_rem_key_to_btn(const char *name)
{
	for (uint8_t i = 0; i < IR_REM_BTN_COUNT; i++)
		if (strcasecmp(name, s_rem_btn_keys[i]) == 0)
			return i;
	return 0xFF;
}

/* Load a .ir remote: signals named up/down/left/right/ok map to their buttons.
 * Returns true if at least one button was mapped. */
static bool ir_rem_load(void)
{
	S_M1_file_info *fi = storage_browse("0:/IR");
	if (!fi || fi->status != FB_OK || !fi->file_is_selected)
		return false;

	char path[80];
	snprintf(path, sizeof(path), "%s/%s", fi->dir_name, fi->file_name);

	for (uint8_t i = 0; i < IR_REM_BTN_COUNT; i++)
		s_rem_sig[i].valid = false;

	ff_file_t ff;
	if (!ir_file_open(&ff, path))
		return false;

	ir_file_signal_t sig;
	while (ir_file_read_signal(&ff, &sig))
	{
		uint8_t b = ir_rem_key_to_btn(sig.name);
		if (b != 0xFF)
			s_rem_sig[b] = sig;
	}
	ff_close(&ff);

	/* Title = filename without extension. */
	strncpy(s_rem_title, fi->file_name, sizeof(s_rem_title) - 1);
	s_rem_title[sizeof(s_rem_title) - 1] = '\0';
	char *dot = strrchr(s_rem_title, '.');
	if (dot)
		*dot = '\0';

	for (uint8_t i = 0; i < IR_REM_BTN_COUNT; i++)
		if (s_rem_sig[i].valid)
			return true;

	return false;
}

/****************************** C R E A T E **********************************/

/* Pick one command from a .ir file. Two passes: collect names, let the user
 * choose, then re-read to the chosen index for the full signal (keeps only one
 * heavy ir_file_signal_t in flight). Returns true if a command was chosen. */
static bool ir_rem_pick_command(const char *path, ir_file_signal_t *out)
{
	ff_file_t        ff;
	ir_file_signal_t sig;
	const char      *title;
	uint16_t         sel = 0;

	s_pick_count = 0;
	if (!ir_file_open(&ff, path))
		return false;
	while (s_pick_count < IR_REM_PICK_MAX && ir_file_read_signal(&ff, &sig))
	{
		strncpy(s_pick_names[s_pick_count], sig.name, FLIPPER_IR_NAME_MAX_LEN - 1);
		s_pick_names[s_pick_count][FLIPPER_IR_NAME_MAX_LEN - 1] = '\0';
		s_pick_count++;
	}
	ff_close(&ff);

	if (s_pick_count == 0)
		return false;

	title = strrchr(path, '/');
	title = title ? title + 1 : path;

	for (;;)
	{
		draw_cmd_list(title, sel);
		uint8_t k = ir_rem_get_key();
		if (k == IR_KEY_BACK)
			return false;
		else if (k == IR_KEY_UP)
			sel = (sel > 0) ? (sel - 1) : (s_pick_count - 1);
		else if (k == IR_KEY_DOWN)
			sel = (sel < s_pick_count - 1) ? (sel + 1) : 0;
		else if (k == IR_KEY_OK)
			break;
	}

	/* Second pass: read the signal at the chosen index. */
	if (!ir_file_open(&ff, path))
		return false;
	uint16_t idx = 0;
	bool     got = false;
	while (ir_file_read_signal(&ff, &sig))
	{
		if (idx == sel)
		{
			*out = sig;
			got  = true;
			break;
		}
		idx++;
	}
	ff_close(&ff);
	return got;
}

/* Assign each button in turn, name the remote, and write 0:/IR/<name>.ir.
 * Returns true when a usable remote now lives in s_rem_sig[]. */
static bool ir_rem_create(void)
{
	for (uint8_t i = 0; i < IR_REM_BTN_COUNT; i++)
		s_rem_sig[i].valid = false;

	for (uint8_t b = 0; b < IR_REM_BTN_COUNT; b++)
	{
		uint8_t k;

		/* Prompt: OK = pick, RIGHT = skip, BACK = cancel the whole create. */
		for (;;)
		{
			draw_create(b);
			k = ir_rem_get_key();
			if (k == IR_KEY_OK || k == IR_KEY_RIGHT || k == IR_KEY_BACK)
				break;
		}
		if (k == IR_KEY_BACK)
			return false;
		if (k == IR_KEY_RIGHT)
			continue;                       /* leave this button unassigned */

		S_M1_file_info *fi = storage_browse("0:/IR");
		if (!fi || fi->status != FB_OK || !fi->file_is_selected)
			continue;                       /* cancelled browse == skip */

		char path[80];
		snprintf(path, sizeof(path), "%s/%s", fi->dir_name, fi->file_name);

		ir_file_signal_t chosen;
		if (ir_rem_pick_command(path, &chosen))
		{
			s_rem_sig[b] = chosen;
			/* Rename to the button key so it reloads to the right button. */
			strncpy(s_rem_sig[b].name, s_rem_btn_keys[b], FLIPPER_IR_NAME_MAX_LEN - 1);
			s_rem_sig[b].name[FLIPPER_IR_NAME_MAX_LEN - 1] = '\0';
			s_rem_sig[b].valid = true;
		}
	}

	/* Require at least one mapped button. */
	bool any = false;
	for (uint8_t i = 0; i < IR_REM_BTN_COUNT; i++)
		if (s_rem_sig[i].valid) any = true;
	if (!any)
		return false;

	char def_name[24] = "ir_remote";
	char new_name[24] = "";
	if (!m1_vkb_get_text("Remote name:", def_name, new_name, sizeof(new_name)))
		return false;                       /* cancelled */
	if (new_name[0] == '\0')
		return false;

	char save_path[80];
	snprintf(save_path, sizeof(save_path), "0:/IR/%s.ir", new_name);

	ff_file_t ff;
	if (!ff_open_write(&ff, save_path))
		return false;
	ir_file_write_header(&ff);
	for (uint8_t b = 0; b < IR_REM_BTN_COUNT; b++)
		if (s_rem_sig[b].valid)
			ir_file_write_signal(&ff, &s_rem_sig[b]);
	ff_close(&ff);

	strncpy(s_rem_title, new_name, sizeof(s_rem_title) - 1);
	s_rem_title[sizeof(s_rem_title) - 1] = '\0';
	return true;
}

/****************************** P A D   L O O P ******************************/

static void ir_rem_pad(void)
{
	for (;;)
	{
		draw_pad();
		uint8_t k = ir_rem_get_key();
		switch (k)
		{
			case IR_KEY_BACK:  return;
			case IR_KEY_UP:    ir_rem_fire(IR_REM_BTN_UP);    break;
			case IR_KEY_DOWN:  ir_rem_fire(IR_REM_BTN_DOWN);  break;
			case IR_KEY_LEFT:  ir_rem_fire(IR_REM_BTN_LEFT);  break;
			case IR_KEY_RIGHT: ir_rem_fire(IR_REM_BTN_RIGHT); break;
			case IR_KEY_OK:    ir_rem_fire(IR_REM_BTN_OK);    break;
			default:                                          break;
		}
	}
}

/****************************** E N T R Y ************************************/

void ir_remote(void)
{
	uint8_t choice_sel = 0;
	int     choice;
	bool    ready;

	memset(s_rem_sig, 0, sizeof(s_rem_sig));
	s_rem_title[0] = '\0';

	/* Choice screen: Load existing / Create new. */
	choice = -1;
	for (;;)
	{
		draw_choice(choice_sel);
		uint8_t k = ir_rem_get_key();
		if (k == IR_KEY_BACK)
			break;
		if (k == IR_KEY_UP || k == IR_KEY_DOWN)
			choice_sel ^= 1u;
		else if (k == IR_KEY_OK)
		{
			choice = choice_sel;
			break;
		}
	}

	if (choice < 0)
	{
		xQueueReset(main_q_hdl);
		return;
	}

	ready = (choice == 0) ? ir_rem_load() : ir_rem_create();

	if (!ready)
	{
		draw_status((choice == 0) ? "No remote loaded" : "Cancelled");
		/* Brief acknowledgement, then leave on any button. */
		ir_rem_get_key();
		xQueueReset(main_q_hdl);
		return;
	}

	ir_rem_pad();
	xQueueReset(main_q_hdl);
}
