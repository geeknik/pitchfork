/* See COPYING.txt for license details. */

/*
*
*  m1_app_api.c
*
*  Firmware API symbol table for M1 external apps (.m1app)
*
*  This module builds a sorted hash table of firmware function pointers
*  that external apps can call. At init time, hashes are computed from
*  symbol name strings, the table is sorted, and a binary search is
*  used at load time to resolve external references.
*
* M1 Project
*
*/

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "stm32h5xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "m1_elf_loader.h"
#include "m1_app_api.h"
#include "m1_app_syscall.h"
#include "m1_games.h"
#include "m1_log_debug.h"
#include "u8g2.h"
#include "ff.h"
#include "m1_buzzer.h"
#include "m1_gpio.h"
#include "m1_lp5814.h"
#include "m1_display.h"
#include "m1_lcd.h"
#include "m1_file_util.h"
#include "m1_crypto.h"
#include "m1_i2c.h"
#include "m1_rf_spi.h"
#include "m1_lib.h"
#include "m1_virtual_kb.h"
#include "m1_power_ctl.h"
#include "m1_sdcard.h"
#include "m1_infrared.h"
#include "m1_ir_remote.h"
#include "ff_file.h"
#include "ir_file.h"
#include "nfc_fmt.h"
#include "rfid_file.h"
#include "subghz_file.h"
#include "m1_sub_ghz.h"        /* Sub-GHz primitive verbs (analyzers + TX) */
#include "m1_wifi.h"
#include "m1_esp_client.h"   /* expose the ESP32-C6 WiFi/BLE/ESP-NOW API to apps */
#include "m1_field_detect.h"
#include "m1_nfc.h"
#include "m1_rfid_api.h"       /* LF-RFID primitive verbs */
#include "lfrfid_file.h"       /* lfrfid_profile_load/save, lfrfid_get_protocol_by_name */
#include "m1_usb_cdc_msc.h"
#include "m1_badusb.h"
/* Headers for the auto-generated export block (tools/app_api_gen.py). The full
 * per-header include list is itself generated, so every scanned subsystem's
 * declarations are in scope for the generated table below. */
#include "m1_app_api_includes.inc"

/* Core ESP readiness flag (defined in esp_app_main.c, declared in esp_app_main.h
 * which lives outside m1_csrc). Exposed to apps so a ported feature gates on the
 * same local init-status the core uses (get_esp32_main_init_status) instead of a
 * live ESP ping. Forward-declared here to avoid pulling in the esp_app_main.h. */
extern bool get_esp32_main_init_status(void);

/*************************** D E F I N E S ************************************/

#define TAG "API"

/* Maximum number of exported API symbols. Headroom above the ~235 currently
 * populated; the grouped-dispatch umbrella calls keep future domain expansion
 * from needing one symbol per operation, so this need not grow much further. */
#define API_MAX_SYMBOLS  1024   /* headroom for the full primitive surface (every subsystem) */

/********************* T Y P E S *********************************************/

/* Entry used during initialization (name string + function pointer) */
typedef struct {
    const char *name;
    void       *address;
} api_name_entry_t;

/***************************** V A R I A B L E S ******************************/

/* The final sorted hash table used at runtime */
static m1_api_sym_t s_api_table[API_MAX_SYMBOLS];
static uint16_t s_api_count = 0;
static bool s_api_initialized = false;

static m1_api_interface_t s_api_interface = {
    .table = s_api_table,
    .count = 0,
    .api_version = M1_APP_API_VERSION
};

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static void api_sort_table(void);

/*************** W R A P P E R   F U N C T I O N S ***************************/

/*
 * Wrapper for m1_u8g2_nextpage — apps call this as "m1app_display_flush"
 * to push their framebuffer to the display.
 */
static uint8_t m1app_display_flush(void)
{
    return m1_u8g2_nextpage();
}

/* App-safe directory listing. The raw FatFS FILINFO layout is intentionally
 * opaque in the SDK so apps do not depend on FatFS configuration internals;
 * expose only the stable fields a standalone app needs to browse its SD data. */
typedef struct __attribute__((packed)) {
    char     name[40];
    uint32_t size;
    uint8_t  is_dir;
} m1app_dirent_t;

static int m1app_dir_list(const char *path, m1app_dirent_t *out, uint16_t max_entries)
{
    if (!path || !out || max_entries == 0)
        return -1;

    DIR dir;
    FILINFO info;
    if (f_opendir(&dir, path) != FR_OK)
        return -1;

    int count = 0;
    FRESULT res;
    while (count < max_entries && (res = f_readdir(&dir, &info)) == FR_OK) {
        if (info.fname[0] == '\0')
            break;
        if ((strcmp(info.fname, ".") == 0) || (strcmp(info.fname, "..") == 0))
            continue;

        memset(&out[count], 0, sizeof(out[count]));
        size_t name_len = 0;
        while (name_len < sizeof(out[count].name) - 1 && info.fname[name_len] != '\0') {
            ++name_len;
        }
        memcpy(out[count].name, info.fname, name_len);
        out[count].name[name_len] = '\0';
        out[count].size = info.fsize;
        out[count].is_dir = (info.fattrib & AM_DIR) ? 1u : 0u;
        count++;
    }
    f_closedir(&dir);
    return res == FR_OK ? count : -1;
}

/*
 * UI-domain kernel hooks (m1_ui_call in m1_app_ui.c checks the focus lease
 * first; only then do these touch the framebuffer). Split out so the
 * umbrella itself stays host-testable without u8g2.
 */
uint8_t m1_ui_hook_flush(void)
{
    return m1_u8g2_nextpage();
}

void m1_ui_hook_notify(const char *msg, uint16_t duration_ms)
{
    u8g2_t *u = &m1_u8g2;
    u8g2_uint_t w = u8g2_GetDisplayWidth(u);
    u8g2_uint_t h = u8g2_GetDisplayHeight(u);

    /* Inverse band with the message; the caller's next full redraw restores
     * its own frame. Runs in the caller's (lease-holding) context, so there
     * is no cross-task framebuffer race to guard. */
    u8g2_SetFont(u, u8g2_font_6x10_tr);
    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, 0, (h / 2) - 12, w, 24);
    u8g2_SetDrawColor(u, 0);
    u8g2_DrawStr(u, 4, (h / 2) + 4, msg);
    u8g2_SetDrawColor(u, 1);
    (void)m1_u8g2_nextpage();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

/*
 * Wrapper for m1_usb_get_state
 */
static uint8_t m1_usb_get_state(void)
{
    return hUsbDeviceFS.dev_state;
}

/*
 * Wrapper for m1_u8g2_firstpage
 */
static void m1app_display_begin(void)
{
    m1_u8g2_firstpage();
}

/*
 * Wrapper to give apps access to the u8g2 handle
 */
static u8g2_t *m1app_get_u8g2(void)
{
    return &m1_u8g2;
}

/*
 * Per-app heap tracking.
 *
 * Every allocation an app makes through the exported allocator symbols
 * (m1app_malloc/free, and pvPortMalloc/vPortFree which now resolve here too) is
 * recorded in an intrusive doubly-linked list. m1_app_tracked_free_all() frees
 * whatever the app left behind when it exits or is force-killed — elf_unload_app
 * only reclaims the ELF sections, not the app's own allocations, so without this
 * a crashing or BACK-killed app slowly leaks the shared system heap across many
 * launches (the same failure mode root-caused in the classic firmware).
 *
 * One app runs at a time, so a single global list suffices. List-pointer edits
 * happen in a short critical section; pvPortMalloc/vPortFree run outside it.
 * An 8-byte header (two pointers) preserves the heap's 8-byte alignment.
 */
typedef struct app_alloc_hdr {
    struct app_alloc_hdr *next;
    struct app_alloc_hdr *prev;
} app_alloc_hdr_t;

static app_alloc_hdr_t *s_app_alloc_head = NULL;

/*
 * Tracked allocator — apps call "m1app_malloc" (and "pvPortMalloc").
 */
static void *m1app_malloc(size_t size)
{
    app_alloc_hdr_t *h = pvPortMalloc(sizeof(app_alloc_hdr_t) + size);
    if (h == NULL)
        return NULL;

    taskENTER_CRITICAL();
    h->prev = NULL;
    h->next = s_app_alloc_head;
    if (s_app_alloc_head != NULL)
        s_app_alloc_head->prev = h;
    s_app_alloc_head = h;
    taskEXIT_CRITICAL();

    return (void *)(h + 1);
}

/*
 * Tracked free — apps call "m1app_free" (and "vPortFree").
 */
static void m1app_free(void *ptr)
{
    if (ptr == NULL)
        return;

    app_alloc_hdr_t *h = (app_alloc_hdr_t *)ptr - 1;

    taskENTER_CRITICAL();
    if (h->prev != NULL)
        h->prev->next = h->next;
    else
        s_app_alloc_head = h->next;
    if (h->next != NULL)
        h->next->prev = h->prev;
    taskEXIT_CRITICAL();

    vPortFree(h);
}

/*
 * Free every allocation the current app left behind. Called by the app manager
 * once the app task has stopped (normal return or force-kill), so a killed app
 * cannot leak the system heap. Detaches the list under a critical section, then
 * frees outside it (vPortFree must not run inside a critical section).
 */
void m1_app_tracked_free_all(void)
{
    taskENTER_CRITICAL();
    app_alloc_hdr_t *h = s_app_alloc_head;
    s_app_alloc_head = NULL;
    taskEXIT_CRITICAL();

    while (h != NULL) {
        app_alloc_hdr_t *next = h->next;
        vPortFree(h);
        h = next;
    }
}

/*
 * System/info umbrella — apps call "m1_sys_call". See m1_app_syscall.h for the
 * operation codes and the additive-ABI rules. This is the reference for the
 * grouped-dispatch pattern that future domains (NFC, Sub-GHz, RFID, ...) follow
 * so a wide surface costs one export symbol per domain, not one per operation.
 */
static int32_t m1_sys_call(uint32_t op, void *args)
{
    (void)args;
    switch (op)
    {
        case M1_SYS_OP_API_LEVEL: return (int32_t)M1_APP_API_VERSION;
        case M1_SYS_OP_HEAP_FREE: return (int32_t)xPortGetFreeHeapSize();
        case M1_SYS_OP_UPTIME_MS: return (int32_t)HAL_GetTick();
        default:                  return M1_SYS_ERR_BAD_OP;
    }
}

/*
 * Wrapper for vTaskDelay in ms
 */
static void m1app_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/*
 * Wrapper for HAL_GetTick
 */
static uint32_t m1app_get_tick(void)
{
    return HAL_GetTick();
}

/*
 * Wrappers for u8g2 macros (can't export macros via symbol table)
 */
static int8_t m1app_u8g2_get_ascent(u8g2_t *u8g2)
{
    return u8g2_GetAscent(u8g2);
}

static int8_t m1app_u8g2_get_descent(u8g2_t *u8g2)
{
    return u8g2_GetDescent(u8g2);
}

/*************** F U N C T I O N   I M P L E M E N T A T I O N ****************/


/*============================================================================*/
/*
 * @brief  Initialize the API table: compute hashes and sort
 */
/*============================================================================*/
void m1_app_api_init(void)
{
    uint16_t i;

    if (s_api_initialized)
        return;

    /*
     * Define all exported symbols. The string name is what the app's
     * compiler will emit as an external reference. The address is the
     * actual function pointer in firmware.
     */
    static const api_name_entry_t entries[] = {

        /* ===== Display — u8g2 drawing functions ===== */
        { "u8g2_FirstPage",    (void *)u8g2_FirstPage },
        { "u8g2_DrawStr",      (void *)u8g2_DrawStr },
        { "u8g2_DrawBox",      (void *)u8g2_DrawBox },
        { "u8g2_DrawFrame",    (void *)u8g2_DrawFrame },
        { "u8g2_DrawPixel",    (void *)u8g2_DrawPixel },
        { "u8g2_DrawLine",     (void *)u8g2_DrawLine },
        { "u8g2_DrawCircle",   (void *)u8g2_DrawCircle },
        { "u8g2_DrawDisc",     (void *)u8g2_DrawDisc },
        { "u8g2_DrawHLine",    (void *)u8g2_DrawHLine },
        { "u8g2_DrawVLine",    (void *)u8g2_DrawVLine },
        { "u8g2_DrawRBox",     (void *)u8g2_DrawRBox },
        { "u8g2_DrawRFrame",   (void *)u8g2_DrawRFrame },
        { "u8g2_DrawTriangle", (void *)u8g2_DrawTriangle },
        { "u8g2_DrawXBM",      (void *)u8g2_DrawXBM },
        { "u8g2_SetDrawColor", (void *)u8g2_SetDrawColor },
        { "u8g2_SetFont",      (void *)u8g2_SetFont },
        { "u8g2_GetStrWidth",  (void *)u8g2_GetStrWidth },
        { "u8g2_GetAscent",    (void *)m1app_u8g2_get_ascent },
        { "u8g2_GetDescent",   (void *)m1app_u8g2_get_descent },
        { "u8g2_SetFontDirection", (void *)u8g2_SetFontDirection },
        { "u8g2_SetClipWindow",    (void *)u8g2_SetClipWindow },
        { "u8g2_SetMaxClipWindow", (void *)u8g2_SetMaxClipWindow },

        /* ===== Display — M1 wrappers ===== */
        { "m1app_display_begin",  (void *)m1app_display_begin },
        { "m1app_display_flush",  (void *)m1app_display_flush },
        { "m1app_get_u8g2",       (void *)m1app_get_u8g2 },
        { "m1_lcd_cleardisplay",  (void *)m1_lcd_cleardisplay },

        /* ===== Display — M1 UI helpers ===== */
        { "m1_message_box",       (void *)m1_message_box },
        { "m1_message_box_choice",(void *)m1_message_box_choice },
        { "m1_draw_bottom_bar",   (void *)m1_draw_bottom_bar },
        { "m1_draw_icon",         (void *)m1_draw_icon },
        { "m1_draw_text",         (void *)m1_draw_text },
        { "m1_draw_text_box",     (void *)m1_draw_text_box },
        { "m1_info_box_display_init",  (void *)m1_info_box_display_init },
        { "m1_info_box_display_clear", (void *)m1_info_box_display_clear },
        { "m1_info_box_display_draw",  (void *)m1_info_box_display_draw },

        /* ===== Timing and delays ===== */
        { "m1app_delay",       (void *)m1app_delay },
        { "m1app_get_tick",    (void *)m1app_get_tick },
        { "vTaskDelay",        (void *)vTaskDelay },
        { "HAL_GetTick",       (void *)HAL_GetTick },
        { "m1_hard_delay",     (void *)m1_hard_delay },

        /* ===== Real-Time Clock (RTC) ===== */
        { "m1_get_datetime",   (void *)m1_get_datetime },
        { "m1_get_localtime",  (void *)m1_get_localtime },
        { "m1_set_datetime",   (void *)m1_set_datetime },
        { "wifi_sync_rtc",     (void *)wifi_sync_rtc },
        { "wifi_scan_ap",      (void *)wifi_scan_ap },

        /* ===== Memory management ===== */
        { "m1app_malloc",      (void *)m1app_malloc },
        { "m1app_free",        (void *)m1app_free },
        /* Route raw port allocator symbols through the tracked allocator too, so
         * an app that links pvPortMalloc/vPortFree directly is also cleaned up
         * on exit/kill. Both header-prepend and header-strip stay paired. */
        { "pvPortMalloc",      (void *)m1app_malloc },
        { "vPortFree",         (void *)m1app_free },

        /* ===== Grouped syscall umbrellas (see m1_app_syscall.h) ===== */
        { "m1_sys_call",       (void *)m1_sys_call },
        { "m1_storage_call",   (void *)m1_storage_call },   /* m1_app_storage.c */
        { "m1_ui_call",        (void *)m1_ui_call },        /* m1_app_ui.c */

        /* ===== Input ===== */
        { "game_poll_button",  (void *)game_poll_button },
        { "game_rand_seed",    (void *)game_rand_seed },
        { "game_rand_range",   (void *)game_rand_range },

        /* ===== Virtual keyboard ===== */
        { "m1_vkb_get_filename", (void *)m1_vkb_get_filename },
        { "m1_vkb_get_text",     (void *)m1_vkb_get_text },
        { "m1_vkbs_get_data",    (void *)m1_vkbs_get_data },

        /* ===== Audio / Buzzer ===== */
        { "m1_buzzer_notification",  (void *)m1_buzzer_notification },
        { "m1_buzzer_notification2", (void *)m1_buzzer_notification2 },
        { "m1_buzzer_set",           (void *)m1_buzzer_set },

        /* ===== LED (RGB notification LED) ===== */
        { "lp5814_led_on_Red",       (void *)lp5814_led_on_Red },
        { "lp5814_led_on_Green",     (void *)lp5814_led_on_Green },
        { "lp5814_led_on_Blue",      (void *)lp5814_led_on_Blue },
        { "lp5814_led_on_rgb",       (void *)lp5814_led_on_rgb },
        { "lp5814_all_off_RGB",      (void *)lp5814_all_off_RGB },
        { "lp5814_backlight_on",     (void *)lp5814_backlight_on },
        { "lp5814_fastblink_on_R_G_B", (void *)lp5814_fastblink_on_R_G_B },
        { "lp5814_led_on",           (void *)lp5814_led_on },
        { "lp5814_led_off",          (void *)lp5814_led_off },

        /* ===== GPIO ===== */
        { "ext_power_5V_set",      (void *)ext_power_5V_set },
        { "ext_power_3V_set",      (void *)ext_power_3V_set },
        { "HAL_GPIO_ReadPin",      (void *)HAL_GPIO_ReadPin },
        { "HAL_GPIO_WritePin",     (void *)HAL_GPIO_WritePin },
        { "HAL_GPIO_TogglePin",    (void *)HAL_GPIO_TogglePin },
        /* App-friendly header GPIO control (0-based pin ids, hides power pins) */
        { "m1_gpio_ext_app_count",   (void *)m1_gpio_ext_app_count },
        { "m1_gpio_ext_app_name",    (void *)m1_gpio_ext_app_name },
        { "m1_gpio_ext_app_mode",    (void *)m1_gpio_ext_app_mode },
        { "m1_gpio_ext_app_write",   (void *)m1_gpio_ext_app_write },
        { "m1_gpio_ext_app_read",    (void *)m1_gpio_ext_app_read },
        { "m1_gpio_ext_app_release", (void *)m1_gpio_ext_app_release },
        /* Generic 1-Wire bit-bang primitive (DS18B20, iButton, ... in apps) */
        { "m1_ow_init",       (void *)m1_ow_init },
        { "m1_ow_reset",      (void *)m1_ow_reset },
        { "m1_ow_write_byte", (void *)m1_ow_write_byte },
        { "m1_ow_read_byte",  (void *)m1_ow_read_byte },
        { "m1_ow_deinit",     (void *)m1_ow_deinit },

        /* ===== Infrared (TX / RX / learn) ===== */
        { "m1_ir_send",            (void *)m1_ir_send },
        { "m1_ir_send_raw",        (void *)m1_ir_send_raw },
        { "m1_ir_receive",         (void *)m1_ir_receive },
        { "m1_ir_carrier_on",      (void *)m1_ir_carrier_on },
        { "m1_ir_carrier_off",     (void *)m1_ir_carrier_off },
        { "m1_ir_protocol_name",   (void *)m1_ir_protocol_name },
        { "m1_ir_protocol_count",  (void *)m1_ir_protocol_count },
        /* Multi-button IR remote (load/create on-device, .ir manifest) */
        { "ir_remote",             (void *)ir_remote },

        /* ===== I2C ===== */
        { "m1_i2c_hal_trans_req",  (void *)m1_i2c_hal_trans_req },
        { "m1_i2c_hal_get_error",  (void *)m1_i2c_hal_get_error },
        /* Bit-bang scan of the GPIO-header I2C bus (SCL=pin6, SDA=pin7) */
        { "m1_i2c_scan",           (void *)m1_i2c_scan },

        /* ===== SPI ===== */
        { "m1_spi_hal_trans_req",  (void *)m1_spi_hal_trans_req },

        /* ===== Infrared ===== */
        { "infrared_encode_sys_init",   (void *)infrared_encode_sys_init },
        { "infrared_encode_sys_deinit", (void *)infrared_encode_sys_deinit },
        { "infrared_transmit",          (void *)infrared_transmit },

        /* ===== Power / Battery ===== */
        { "m1_check_battery_level", (void *)m1_check_battery_level },

        /* ===== SD card info ===== */
        { "m1_sdcard_get_status",        (void *)m1_sdcard_get_status },
        { "m1_sd_detected",              (void *)m1_sd_detected },
        { "m1_sdcard_get_free_capacity", (void *)m1_sdcard_get_free_capacity },
        { "m1_sdcard_get_total_capacity",(void *)m1_sdcard_get_total_capacity },

        /* ===== FatFS — file operations ===== */
        { "f_open",     (void *)f_open },
        { "f_close",    (void *)f_close },
        { "f_read",     (void *)f_read },
        { "f_write",    (void *)f_write },
        { "f_lseek",    (void *)f_lseek },
        { "f_sync",     (void *)f_sync },
        { "f_truncate", (void *)f_truncate },
        { "f_printf",   (void *)f_printf },
        { "f_puts",     (void *)f_puts },

        /* ===== FatFS — directory operations ===== */
        { "f_opendir",  (void *)f_opendir },
        { "f_closedir", (void *)f_closedir },
        { "f_readdir",  (void *)f_readdir },
        { "f_mkdir",    (void *)f_mkdir },
        { "f_unlink",   (void *)f_unlink },
        { "f_rename",   (void *)f_rename },
        { "f_stat",     (void *)f_stat },
        { "m1app_dir_list", (void *)m1app_dir_list },

        /* ===== File utilities ===== */
        { "fu_get_filename",             (void *)fu_get_filename },
        { "fu_get_file_extension",       (void *)fu_get_file_extension },
        { "fu_get_filename_without_ext", (void *)fu_get_filename_without_ext },
        { "fu_get_directory_path",       (void *)fu_get_directory_path },
        { "fu_path_combine",             (void *)fu_path_combine },
        { "fs_file_exists",              (void *)fs_file_exists },
        { "fs_directory_exists",         (void *)fs_directory_exists },
        { "fs_directory_ensure",         (void *)fs_directory_ensure },

        /* ===== Flipper file format parser ===== */
        { "ff_open",              (void *)ff_open },
        { "ff_open_write",        (void *)ff_open_write },
        { "ff_close",             (void *)ff_close },
        { "ff_read_line",         (void *)ff_read_line },
        { "ff_parse_kv",          (void *)ff_parse_kv },
        { "ff_get_key",           (void *)ff_get_key },
        { "ff_get_value",         (void *)ff_get_value },
        { "ff_is_separator",      (void *)ff_is_separator },
        { "ff_validate_header",   (void *)ff_validate_header },
        { "ff_write_kv_str",      (void *)ff_write_kv_str },
        { "ff_write_kv_uint32",   (void *)ff_write_kv_uint32 },
        { "ff_write_kv_hex",      (void *)ff_write_kv_hex },
        { "ff_write_separator",   (void *)ff_write_separator },
        { "ff_write_comment",     (void *)ff_write_comment },
        { "ff_parse_hex_bytes",   (void *)ff_parse_hex_bytes },
        { "ff_parse_int32_array", (void *)ff_parse_int32_array },

        /* ===== Flipper IR file format ===== */
        { "ir_file_open",          (void *)ir_file_open },
        { "ir_file_read_signal",   (void *)ir_file_read_signal },
        { "ir_file_write_header",  (void *)ir_file_write_header },
        { "ir_file_write_signal",  (void *)ir_file_write_signal },
        { "ir_file_count_signals", (void *)ir_file_count_signals },
        { "ir_file_proto_to_irmp", (void *)ir_file_proto_to_irmp },
        { "ir_file_irmp_to_proto", (void *)ir_file_irmp_to_proto },

        /* ===== Flipper NFC file format ===== */
        { "nfc_fmt_load",       (void *)nfc_fmt_load },
        { "nfc_fmt_save",       (void *)nfc_fmt_save },
        { "nfc_fmt_parse_type", (void *)nfc_fmt_parse_type },

        /* ===== Flipper RFID file format ===== */
        { "rfid_file_load",           (void *)rfid_file_load },
        { "rfid_file_save",           (void *)rfid_file_save },
        { "rfid_file_parse_protocol", (void *)rfid_file_parse_protocol },

        /* ===== Flipper Sub-GHz file format ===== */
        { "subghz_file_load", (void *)subghz_file_load },
        { "subghz_file_save", (void *)subghz_file_save },

        /* ===== Crypto (AES-256-CBC) ===== */
        { "m1_crypto_derive_key",  (void *)m1_crypto_derive_key },
        { "m1_crypto_generate_iv", (void *)m1_crypto_generate_iv },
        { "m1_crypto_encrypt",     (void *)m1_crypto_encrypt },
        { "m1_crypto_decrypt",     (void *)m1_crypto_decrypt },
        { "m1_crypto_encrypt_with_key", (void *)m1_crypto_encrypt_with_key },
        { "m1_crypto_decrypt_with_key", (void *)m1_crypto_decrypt_with_key },

        /* ===== String utilities ===== */
        { "m1_strtolower",      (void *)m1_strtolower },
        { "m1_strtoupper",      (void *)m1_strtoupper },
        { "m1_byte_to_hextext", (void *)m1_byte_to_hextext },
        { "m1_float_to_string", (void *)m1_float_to_string },

        /* ===== u8g2 font data ===== */
        { "u8g2_font_4x6_tr",              (void *)u8g2_font_4x6_tr },
        { "u8g2_font_5x8_tr",              (void *)u8g2_font_5x8_tr },
        { "u8g2_font_6x10_tr",             (void *)u8g2_font_6x10_tr },
        { "u8g2_font_10x20_mr",            (void *)u8g2_font_10x20_mr },
        { "u8g2_font_helvB08_tr",          (void *)u8g2_font_helvB08_tr },
        { "u8g2_font_helvB08_tf",          (void *)u8g2_font_helvB08_tf },
        { "u8g2_font_finderskeepers_tf",   (void *)u8g2_font_finderskeepers_tf },
        { "u8g2_font_resoledmedium_tr",    (void *)u8g2_font_resoledmedium_tr },
        { "u8g2_font_NokiaSmallPlain_tf",  (void *)u8g2_font_NokiaSmallPlain_tf },
        { "u8g2_font_squeezed_b7_tr",      (void *)u8g2_font_squeezed_b7_tr },
        { "u8g2_font_spleen5x8_mf",        (void *)u8g2_font_spleen5x8_mf },
        { "u8g2_font_spleen8x16_mf",       (void *)u8g2_font_spleen8x16_mf },
        { "u8g2_font_nine_by_five_nbp_tf", (void *)u8g2_font_nine_by_five_nbp_tf },
        { "u8g2_font_courB08_tf",          (void *)u8g2_font_courB08_tf },
        { "u8g2_font_Terminal_tr",          (void *)u8g2_font_Terminal_tr },
        { "u8g2_font_lubB08_tf",           (void *)u8g2_font_lubB08_tf },
        { "u8g2_font_profont17_tr",         (void *)u8g2_font_profont17_tr },
        { "u8g2_font_VCR_OSD_tu",          (void *)u8g2_font_VCR_OSD_tu },
        { "u8g2_font_Pixellari_tu",         (void *)u8g2_font_Pixellari_tu },
        { "u8g2_font_pcsenior_8f",          (void *)u8g2_font_pcsenior_8f },

        /* ===== Field Detection ===== */
        { "m1_field_detect_start",    (void *)m1_field_detect_start },
        { "m1_field_detect_stop",     (void *)m1_field_detect_stop },
        { "m1_field_detect_nfc",      (void *)m1_field_detect_nfc },
        { "m1_field_detect_rfid",     (void *)m1_field_detect_rfid },
        { "m1_field_detect_rfid_raw", (void *)m1_field_detect_rfid_raw },
        { "m1_field_detect_nfc_raw",  (void *)m1_field_detect_nfc_raw },
        { "m1_field_detect_nfc_opctl",(void *)m1_field_detect_nfc_opctl },
        /* NFC primitive API (composable read/transceive/emulate) */
        { "m1_nfc_field_on",          (void *)m1_nfc_field_on },
        { "m1_nfc_poll",              (void *)m1_nfc_poll },
        { "m1_nfc_transceive",        (void *)m1_nfc_transceive },
        { "m1_nfc_ntag_read_page",    (void *)m1_nfc_ntag_read_page },
        { "m1_nfc_ntag_pwd_auth",     (void *)m1_nfc_ntag_pwd_auth },
        { "m1_nfc_ntag_write_page",   (void *)m1_nfc_ntag_write_page },
        { "m1_nfc_mifare_read_block", (void *)m1_nfc_mifare_read_block },
        { "nfc_tag_info",             (void *)nfc_tool_tag_info },
        { "m1_nfc_emulate_uid",       (void *)m1_nfc_emulate_uid },
        { "m1_nfc_emulate_ntag",      (void *)m1_nfc_emulate_ntag },
        { "m1_nfc_emulate_uid_update",(void *)m1_nfc_emulate_uid_update },
        { "m1_nfc_emulate_stop",      (void *)m1_nfc_emulate_stop },

        /* ===== LF-RFID (125 kHz): read / clone / write / erase / emulate ===== */
        { "m1_rfid_start",             (void *)m1_rfid_start },
        { "m1_rfid_stop",              (void *)m1_rfid_stop },
        { "m1_rfid_read",              (void *)m1_rfid_read },
        { "m1_rfid_write_t5577",       (void *)m1_rfid_write_t5577 },
        { "m1_rfid_write_t5577_blocks",(void *)m1_rfid_write_t5577_blocks },
        { "m1_rfid_emulate_start",     (void *)m1_rfid_emulate_start },
        { "m1_rfid_emulate_stop",      (void *)m1_rfid_emulate_stop },
        /* LF-RFID engine helpers (already in firmware) */
        { "protocol_get_name",         (void *)protocol_get_name },
        { "protocol_get_manufacturer", (void *)protocol_get_manufacturer },
        { "protocol_get_data_size",    (void *)protocol_get_data_size },
        { "protocol_get_data",         (void *)protocol_get_data },
        { "protocol_render_data",      (void *)protocol_render_data },
        { "lfrfid_GetTagInfo",         (void *)lfrfid_GetTagInfo },
        { "lfrfid_tag_info_init",      (void *)lfrfid_tag_info_init },
        { "lfrfid_get_protocol_by_name",(void *)lfrfid_get_protocol_by_name },
        { "lfrfid_profile_load",       (void *)lfrfid_profile_load },
        { "lfrfid_profile_save",       (void *)lfrfid_profile_save },
        { "bytes_to_u32_array",        (void *)bytes_to_u32_array },
        { "lfrfid_write_verify",       (void *)lfrfid_write_verify },

        /* ===== Sub-GHz (SI4463): self-contained analyzers + TX ===== *
         * Each analyzer self-inits the radio (menu_sub_ghz_init) and runs its own
         * BACK-to-exit loop, so an app just calls it. TX plays a .sub/.sgh file via
         * the standalone blocking replay wrappers (own event loop, no scene needed). */
        { "sub_ghz_rssi_meter",         (void *)sub_ghz_rssi_meter },
        { "sub_ghz_spectrum_analyzer",  (void *)sub_ghz_spectrum_analyzer },
        { "sub_ghz_freq_scanner",       (void *)sub_ghz_freq_scanner },
        { "sub_ghz_frequency_reader",   (void *)sub_ghz_frequency_reader },
        { "sub_ghz_weather_station",    (void *)sub_ghz_weather_station },
        { "sub_ghz_brute_force",        (void *)sub_ghz_brute_force },
        { "sub_ghz_replay_ff_file",     (void *)sub_ghz_replay_ff_file },
        { "sub_ghz_replay_datafile",    (void *)sub_ghz_replay_datafile },
        /* Scene-based Sub-GHz apps: each launches the scene engine at one scene. */
        { "sub_ghz_proto_pirate_entry", (void *)sub_ghz_proto_pirate_entry },
        { "sub_ghz_playlist_entry",     (void *)sub_ghz_playlist_entry },
        { "sub_ghz_bind_entry",         (void *)sub_ghz_bind_entry },
        { "sub_ghz_remote_entry",       (void *)sub_ghz_remote_entry },

        /* ===== USB HID (BadUSB) ===== */
        { "badusb_run",              (void *)badusb_run },
        { "m1_usb_switch_to_hid",    (void *)m1_usb_switch_to_hid },
        { "m1_usb_switch_to_normal", (void *)m1_usb_switch_to_normal },
        { "m1_usb_get_state",        (void *)m1_usb_get_state },
        { "badusb_send_key",         (void *)badusb_send_key },
        { "badusb_type_char",        (void *)badusb_type_char },
        { "badusb_type_string",      (void *)badusb_type_string_forced },

        /* ===== C library ===== */
        { "memset",    (void *)memset },
        { "memcpy",    (void *)memcpy },
        { "memcmp",    (void *)memcmp },
        { "strlen",    (void *)strlen },
        { "strcmp",     (void *)strcmp },
        { "strncmp",   (void *)strncmp },
        { "strncpy",   (void *)strncpy },
        /* GCC -Os rewrites snprintf(buf, "constant") into strcpy(), so apps need
         * it exported even when they never call strcpy directly. */
        { "strcpy",    (void *)strcpy },
        { "strncat",   (void *)strncat },
        { "strstr",    (void *)strstr },
        { "strchr",    (void *)strchr },
        { "snprintf",  (void *)snprintf },
        { "atoi",      (void *)atoi },
        { "strtol",    (void *)strtol },
        { "rand",      (void *)rand },
        { "srand",     (void *)srand },
        { "abs",       (void *)abs },

        /* ===== ESP32-C6 coprocessor: WiFi / BLE / ESP-NOW =====
         * Programmatic access to the wireless card so custom apps can scan,
         * connect, run BLE, drive the offensive WiFi suite, and peer-link. */
        { "m1_esp_client_ping",            (void *)m1_esp_client_ping },
        { "m1_esp_client_fw_version",      (void *)m1_esp_client_fw_version },
        /* Local ESP-subsystem init flag — the readiness check the core uses
         * (get_esp32_main_init_status), NOT a live ping. */
        { "get_esp32_main_init_status",    (void *)get_esp32_main_init_status },
        /* WiFi station */
        { "m1_esp_client_wifi_scan",       (void *)m1_esp_client_wifi_scan },
        { "m1_esp_client_wifi_connect",    (void *)m1_esp_client_wifi_connect },
        { "m1_esp_client_wifi_disconnect", (void *)m1_esp_client_wifi_disconnect },
        { "m1_esp_client_wifi_status",     (void *)m1_esp_client_wifi_status },
        /* BLE */
        { "m1_esp_client_ble_scan",        (void *)m1_esp_client_ble_scan },
        { "m1_esp_client_ble_advertise",   (void *)m1_esp_client_ble_advertise },
        { "m1_esp_client_ble_connect",     (void *)m1_esp_client_ble_connect },
        { "m1_esp_client_ble_hid_init",    (void *)m1_esp_client_ble_hid_init },
        { "m1_esp_client_ble_hid_deinit",  (void *)m1_esp_client_ble_hid_deinit },
        { "m1_esp_client_ble_hid_key",     (void *)m1_esp_client_ble_hid_key },
        { "m1_esp_client_ble_hid_status",  (void *)m1_esp_client_ble_hid_status },
        { "m1_esp_client_ble_spam_start",  (void *)m1_esp_client_ble_spam_start },
        { "m1_esp_client_ble_spam_stop",   (void *)m1_esp_client_ble_spam_stop },
        { "m1_esp_client_ble_beacon_ibeacon",   (void *)m1_esp_client_ble_beacon_ibeacon },
        { "m1_esp_client_ble_beacon_eddystone", (void *)m1_esp_client_ble_beacon_eddystone },
        { "m1_esp_client_ble_beacon_stop",      (void *)m1_esp_client_ble_beacon_stop },
        /* Offensive WiFi */
        { "m1_esp_client_deauth_start",    (void *)m1_esp_client_deauth_start },
        { "m1_esp_client_deauth_stop",     (void *)m1_esp_client_deauth_stop },
        { "m1_esp_client_sta_scan",        (void *)m1_esp_client_sta_scan },
        { "m1_esp_client_hs_start",        (void *)m1_esp_client_hs_start },
        { "m1_esp_client_hs_status",       (void *)m1_esp_client_hs_status },
        { "m1_esp_client_hs_read",         (void *)m1_esp_client_hs_read },
        { "m1_esp_client_hs_stop",         (void *)m1_esp_client_hs_stop },
        { "m1_esp_client_beacon_start",    (void *)m1_esp_client_beacon_start },
        { "m1_esp_client_beacon_stop",     (void *)m1_esp_client_beacon_stop },
        { "m1_esp_client_probe_start",     (void *)m1_esp_client_probe_start },
        { "m1_esp_client_probe_stop",      (void *)m1_esp_client_probe_stop },
        { "m1_esp_client_karma_start",     (void *)m1_esp_client_karma_start },
        { "m1_esp_client_karma_stop",      (void *)m1_esp_client_karma_stop },
        { "m1_esp_client_monitor_start",   (void *)m1_esp_client_monitor_start },
        { "m1_esp_client_monitor_stop",    (void *)m1_esp_client_monitor_stop },
        { "m1_esp_client_monitor_stats",   (void *)m1_esp_client_monitor_stats },
        { "m1_esp_client_monitor_read",    (void *)m1_esp_client_monitor_read },
        { "m1_esp_client_captive_start",   (void *)m1_esp_client_captive_start },
        { "m1_esp_client_captive_stop",    (void *)m1_esp_client_captive_stop },
        { "m1_esp_client_captive_creds",   (void *)m1_esp_client_captive_creds },
        { "m1_esp_client_captive_diag",    (void *)m1_esp_client_captive_diag },
        /* 802.15.4 (Zigbee/Thread) */
        { "m1_esp_client_zb_sniff_start",  (void *)m1_esp_client_zb_sniff_start },
        { "m1_esp_client_zb_sniff_stop",   (void *)m1_esp_client_zb_sniff_stop },
        { "m1_esp_client_zb_sniff_get",    (void *)m1_esp_client_zb_sniff_get },
        { "m1_esp_client_zb_flood_start",  (void *)m1_esp_client_zb_flood_start },
        { "m1_esp_client_zb_flood_stop",   (void *)m1_esp_client_zb_flood_stop },
        /* ESP-NOW peer link */
        { "m1_esp_client_now_start",       (void *)m1_esp_client_now_start },
        { "m1_esp_client_now_stop",        (void *)m1_esp_client_now_stop },
        { "m1_esp_client_now_announce",    (void *)m1_esp_client_now_announce },
        { "m1_esp_client_now_get_peers",   (void *)m1_esp_client_now_get_peers },
        { "m1_esp_client_now_send",        (void *)m1_esp_client_now_send },
        { "m1_esp_client_now_recv",        (void *)m1_esp_client_now_recv },

        /* Auto-generated app-facing surface (tools/app_api_gen.py) — every
         * public function from the subsystem headers not already listed above.
         * Regenerate after touching a public header; never edit .inc by hand. */
#include "m1_app_api_generated.inc"
    };

    uint16_t num_entries = sizeof(entries) / sizeof(entries[0]);

    if (num_entries > API_MAX_SYMBOLS)
    {
        M1_LOG_E(TAG, "Too many API symbols (%u > %u)", num_entries, API_MAX_SYMBOLS);
        num_entries = API_MAX_SYMBOLS;
    }

    /* Compute hashes and populate the table */
    for (i = 0; i < num_entries; i++)
    {
        s_api_table[i].hash    = elf_gnu_hash(entries[i].name);
        s_api_table[i].name    = entries[i].name;   /* retained for collision-safe resolve */
        s_api_table[i].address = entries[i].address;
    }

    s_api_count = num_entries;

    /* Sort by hash for binary search */
    api_sort_table();

    s_api_interface.table = s_api_table;
    s_api_interface.count = s_api_count;
    s_api_interface.api_version = M1_APP_API_VERSION;

    s_api_initialized = true;

    M1_LOG_I(TAG, "API initialized: %u symbols, version %u",
             s_api_count, M1_APP_API_VERSION);
}


/*============================================================================*/
/*
 * @brief  Sort the API table by hash value (insertion sort — small table)
 */
/*============================================================================*/
static void api_sort_table(void)
{
    uint16_t i, j;
    m1_api_sym_t temp;

    for (i = 1; i < s_api_count; i++)
    {
        temp = s_api_table[i];
        j = i;
        while (j > 0 && s_api_table[j - 1].hash > temp.hash)
        {
            s_api_table[j] = s_api_table[j - 1];
            j--;
        }
        s_api_table[j] = temp;
    }

    /* Check for hash collisions (debug build only) */
    for (i = 1; i < s_api_count; i++)
    {
        if (s_api_table[i].hash == s_api_table[i - 1].hash)
        {
            M1_LOG_W(TAG, "Hash collision at index %u (hash=0x%08lX)",
                     i, (unsigned long)s_api_table[i].hash);
        }
    }
}


/*============================================================================*/
/*
 * @brief  Get the firmware API interface
 * @retval const m1_api_interface_t*  Pointer to the API interface
 */
/*============================================================================*/
const m1_api_interface_t *m1_app_get_api(void)
{
    if (!s_api_initialized)
    {
        m1_app_api_init();
    }
    return &s_api_interface;
}
