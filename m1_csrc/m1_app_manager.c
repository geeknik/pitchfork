/* See COPYING.txt for license details. */

/*
*
*  m1_app_manager.c
*
*  App manager — browses, loads, and runs external .m1app (ELF32) files
*  from the SD card. Each app runs as a FreeRTOS task and communicates
*  with the display and buttons via the firmware API.
*
* M1 Project
*
*/

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stm32h5xx_hal.h"
#include "main.h"
#include "m1_app_manager.h"
#include "m1_ui_focus.h"
#include "m1_esp_lease.h"
#include "m1_crash.h"
#include "m1_crash_core.h"
#include "m1_elf_loader.h"
#include "m1_app_api.h"
#include "m1_system.h"
#include "m1_display.h"
#include "m1_lcd.h"
#include "m1_tasks.h"
#include "m1_buzzer.h"
#include "m1_watchdog.h"
#include "m1_games.h"
#include "m1_log_debug.h"
#include "ff.h"

/*************************** D E F I N E S ************************************/

#define TAG "APPMGR"
#define M1_APP_HANG_GRACE_MS 60000  /* supervised app silence before containment */

#define APPS_DIR              "0:/apps"
#define APPS_EXTENSION        ".m1app"
#define APPS_MAX_FILES        M1_APPS_MAX_FILES
#define APPS_NAME_MAX_LEN     40
#define APPS_PATH_MAX_LEN     128

#define LIST_HEADER_HEIGHT    12
#define LIST_ITEM_HEIGHT      9
#define LIST_START_Y          (LIST_HEADER_HEIGHT + 2)
#define LIST_VISIBLE_ITEMS    4

/* App task priority — MUST match the subfunc handler (TASK_PRIORITY_SUBFUNC_HANDLER
 * == tskNORMAL_PRIORITY) that runs the built-in menu features. Apps are drop-in
 * replacements for those features, so they need the same scheduling. At the old
 * value (tskIDLE_PRIORITY+5 = 5) an app sat BELOW the priority-10 RPC/screen task,
 * which starved it out of the ESP link whenever qMonstatek was attached — the app
 * could never acquire s_esp_mtx to finish a WiFi scan, while the built-in scan
 * (priority 24) always could. */
#define APP_TASK_PRIORITY     (tskNORMAL_PRIORITY)

/* Default stack size if manifest doesn't specify (in 32-bit words) */
#define APP_DEFAULT_STACK     2048

/* The app index is shared by the menu task and rpc_task. Keep its lock bounded:
 * a transient contention becomes a retryable RPC BUSY response, never an
 * indefinitely blocked menu or USB worker. */
#define APP_INDEX_LOCK_WAIT_TICKS  pdMS_TO_TICKS(1000)

//************************** S T R U C T U R E S *******************************

typedef struct {
    char filename[APPS_NAME_MAX_LEN];
    char display_name[APPS_NAME_MAX_LEN];
    char full_path[APPS_PATH_MAX_LEN];
    char category[16];      /* manifest tag: default menu folder ("" = Apps) */
    char version[16];       /* manifest release version ("" = legacy app) */
} app_list_entry_t;

typedef struct {
    elf_app_t elf;
    TaskHandle_t task_handle;
    TaskHandle_t caller_handle;
    volatile bool running;
    /* Supervision (api >= 7 apps): the app heartbeats via m1_sys_call; if it
     * goes silent past the grace window the parent CONTAINS it — suspend,
     * revoke its ESP lease + display focus by handle, write a "hung" crash
     * record naming it, and return to the menu. The app task is deliberately
     * NOT force-deleted: vTaskDelete can land mid-allocation inside the
     * tracked-heap list and corrupt it. A contained app stays a suspended
     * zombie holding its ELF+heap; running stays true so no other app
     * launches until reboot. The device stays usable. */
    bool supervised;
    volatile bool cancel_requested;   /* kernel asked the app to exit */
    volatile uint32_t last_hb;        /* tick of the last heartbeat */
} app_run_ctx_t;

/***************************** V A R I A B L E S ******************************/

static app_list_entry_t s_app_list[APPS_MAX_FILES];
/* Work in a separate buffer so a failed scan never destroys the last known-good
 * menu/catalog snapshot. */
static app_list_entry_t s_app_scan_work[APPS_MAX_FILES];
static uint16_t s_app_count = 0;
/* The RPC catalog must distinguish an empty /apps directory from a failed SD
 * scan.  Returning zero for both made qMonstatek wait or display a bad layout
 * while the actual storage error was hidden. */
static FRESULT s_app_scan_result = FR_OK;
static StaticSemaphore_t s_app_index_lock_storage;
static SemaphoreHandle_t s_app_index_lock = NULL;
static volatile bool s_app_index_dirty = true;
static volatile uint32_t s_app_index_generation = 1;
static app_run_ctx_t s_run_ctx;

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static uint16_t apps_scan_directory(app_list_entry_t *out, FRESULT *result_out);
static bool apps_index_init(void);
static bool apps_index_take(TickType_t wait_ticks);
static void apps_index_give(void);
static bool apps_read_manifest_name(const char *path, char *name_out, uint16_t name_len,
                                    char *cat_out, uint16_t cat_len,
                                    char *version_out, uint16_t version_len);
static void apps_draw_list(const char *title, uint16_t count, uint16_t selection);
static void apps_draw_message(const char *line1, const char *line2);
static elf_load_status_t apps_run(const char *path);
static void app_task_wrapper(void *param);

/*************** F U N C T I O N   I M P L E M E N T A T I O N ****************/


/*============================================================================*/
/*
 * @brief  Initialize the app manager (call once at startup)
 */
/*============================================================================*/
void m1_app_manager_init(void)
{
    m1_app_api_init();
    (void)apps_index_init();
    memset(&s_run_ctx, 0, sizeof(s_run_ctx));
    M1_LOG_I(TAG, "App manager initialized");
}

/*============================================================================*/
/* Public app enumeration for RPC and the modular menu. A fresh scan is only
 * performed after an app-directory mutation; otherwise this returns the cached
 * snapshot without touching the SD card. */
/*============================================================================*/
bool m1_app_manager_heartbeat(void)
{
    if (!s_run_ctx.running)
        return false;
    s_run_ctx.last_hb = xTaskGetTickCount();
    return true;
}

bool m1_app_manager_cancel_requested(void)
{
    return s_run_ctx.running && s_run_ctx.cancel_requested;
}

uint16_t m1_app_manager_scan(void)
{
    uint16_t count;

    if (!apps_index_take(APP_INDEX_LOCK_WAIT_TICKS))
    {
        s_app_scan_result = FR_TIMEOUT;
        return 0;
    }

    if (s_app_index_dirty)
    {
        FRESULT result = FR_OK;
        uint32_t scan_generation;

        taskENTER_CRITICAL();
        scan_generation = s_app_index_generation;
        taskEXIT_CRITICAL();
        count = apps_scan_directory(s_app_scan_work, &result);
        s_app_scan_result = result;
        if (result == FR_OK)
        {
            memcpy(s_app_list, s_app_scan_work, sizeof(s_app_list));
            s_app_count = count;
            /* Do not clear a mutation notification that arrived while the SD
             * scan was in flight; that newer change needs its own refresh. */
            taskENTER_CRITICAL();
            s_app_index_dirty = (s_app_index_generation != scan_generation);
            taskEXIT_CRITICAL();
        }
        /* Preserve the previous usable snapshot on any transient SD error. */
    }
    else
    {
        /* A cached read is successful even if a previous caller only failed to
         * acquire the index mutex; do not leak that old BUSY status forward. */
        s_app_scan_result = FR_OK;
    }

    count = s_app_count;
    apps_index_give();
    return count;
}

FRESULT m1_app_manager_last_scan_result(void)
{
    return s_app_scan_result;
}

bool m1_app_manager_get(uint16_t idx,
                        char *filename, uint16_t filename_len,
                        char *name, uint16_t name_len,
                        char *category, uint16_t category_len,
                        char *version, uint16_t version_len)
{
    bool ok = false;

    if (!apps_index_take(APP_INDEX_LOCK_WAIT_TICKS))
        return false;

    if (idx < s_app_count)
    {
        if (filename && filename_len) {
            strncpy(filename, s_app_list[idx].filename, filename_len - 1);
            filename[filename_len - 1] = '\0';
        }
        if (name && name_len) {
            strncpy(name, s_app_list[idx].display_name, name_len - 1);
            name[name_len - 1] = '\0';
        }
        if (category && category_len) {
            strncpy(category, s_app_list[idx].category, category_len - 1);
            category[category_len - 1] = '\0';
        }
        if (version && version_len) {
            strncpy(version, s_app_list[idx].version, version_len - 1);
            version[version_len - 1] = '\0';
        }
        ok = true;
    }

    apps_index_give();
    return ok;
}

void m1_app_manager_mark_dirty(void)
{
    /* Do not take the index lock here: this is called from file-completion
     * paths and must never turn a completed upload into a wait on a scan. */
    taskENTER_CRITICAL();
    s_app_index_generation++;
    s_app_index_dirty = true;
    taskEXIT_CRITICAL();
}

static bool apps_index_init(void)
{
    if (s_app_index_lock != NULL)
        return true;

    taskENTER_CRITICAL();
    if (s_app_index_lock == NULL)
        s_app_index_lock = xSemaphoreCreateMutexStatic(&s_app_index_lock_storage);
    taskEXIT_CRITICAL();
    return s_app_index_lock != NULL;
}

static bool apps_index_take(TickType_t wait_ticks)
{
    if (!apps_index_init())
        return false;
    return xSemaphoreTake(s_app_index_lock, wait_ticks) == pdTRUE;
}

static void apps_index_give(void)
{
    xSemaphoreGive(s_app_index_lock);
}


/*============================================================================*/
/*
 * @brief  Scan the apps directory for .m1app files
 * @retval uint16_t  Number of apps found
 */
/*============================================================================*/
static uint16_t apps_scan_directory(app_list_entry_t *out, FRESULT *result_out)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;
    uint16_t count = 0;

    *result_out = FR_OK;

    /* Ensure the apps directory exists */
    res = f_mkdir(APPS_DIR);
    if (res != FR_OK && res != FR_EXIST)
    {
        *result_out = res;
        return 0;
    }

    res = f_opendir(&dir, APPS_DIR);
    if (res != FR_OK)
    {
        *result_out = res;
        M1_LOG_W(TAG, "Cannot open %s (err=%u)", APPS_DIR, res);
        return 0;
    }

    while (count < APPS_MAX_FILES)
    {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK)
        {
            *result_out = res;
            break;
        }
        if (fno.fname[0] == '\0')
            break;

        /* Skip directories */
        if (fno.fattrib & AM_DIR)
            continue;

        /* Check for .m1app extension */
        uint16_t name_len = (uint16_t)strlen(fno.fname);
        uint16_t ext_len = (uint16_t)strlen(APPS_EXTENSION);

        if (name_len <= ext_len)
            continue;

        if (strcmp(&fno.fname[name_len - ext_len], APPS_EXTENSION) != 0)
            continue;

        /* Store the entry */
        strncpy(out[count].filename, fno.fname, APPS_NAME_MAX_LEN - 1);
        out[count].filename[APPS_NAME_MAX_LEN - 1] = '\0';

        snprintf(out[count].full_path, APPS_PATH_MAX_LEN,
                 "%s/%s", APPS_DIR, fno.fname);

        /* Try to read the display name, category tag, and release version from
         * the manifest. Older manifests zero-fill the newer fields. */
        out[count].category[0] = '\0';
        out[count].version[0] = '\0';
        if (!apps_read_manifest_name(out[count].full_path,
                                      out[count].display_name,
                                      APPS_NAME_MAX_LEN,
                                      out[count].category,
                                      sizeof(out[count].category),
                                      out[count].version,
                                      sizeof(out[count].version)))
        {
            /* Fall back to filename without extension */
            strncpy(out[count].display_name, fno.fname, name_len - ext_len);
            out[count].display_name[name_len - ext_len] = '\0';
        }

        count++;
    }

    res = f_closedir(&dir);
    if (res != FR_OK && *result_out == FR_OK)
        *result_out = res;

    M1_LOG_I(TAG, "Found %u apps in %s", count, APPS_DIR);
    return count;
}


/* ---- Public interface for the modular menu layout (m1_menu.c) ---- */

/* Scan 0:/apps and return the count; use m1_apps_get() to read each entry. */
int m1_apps_scan(void)
{
    return (int)m1_app_manager_scan();
}

/* Copy one scanned app's path/name/category out. Returns false if idx is out of
 * range. Any out pointer may be NULL. */
bool m1_apps_get(int idx, char *path, uint16_t path_len,
                 char *name, uint16_t name_len, char *cat, uint16_t cat_len)
{
    return idx >= 0 && m1_app_manager_get((uint16_t)idx,
                                           path, path_len,
                                           name, name_len,
                                           cat, cat_len,
                                           NULL, 0);
}

/* Load + run a specific .m1app by path (used by menu app-launch entries).
 * Returns the elf_load_status_t as int (0 = ELF_OK). */
int m1_apps_run(const char *path)
{
    return (int)apps_run(path);
}


/*============================================================================*/
/*
 * @brief  Try to read the app name from the .m1meta section of an ELF
 * @param  path      FatFS path to the .m1app file
 * @param  name_out  Buffer to receive the app name
 * @param  name_len  Size of name_out buffer
 * @retval bool      true if name was read successfully
 */
/*============================================================================*/
static bool apps_read_manifest_name(const char *path, char *name_out, uint16_t name_len,
                                    char *cat_out, uint16_t cat_len,
                                    char *version_out, uint16_t version_len)
{
    if (cat_out && cat_len) cat_out[0] = '\0';
    if (version_out && version_len) version_out[0] = '\0';
    FIL fp;
    FRESULT res;
    UINT br;
    Elf32_Ehdr ehdr;
    Elf32_Shdr shdr;
    uint8_t shstrtab[256];
    uint32_t shstrtab_size;

    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK)
        return false;

    /* Read ELF header */
    res = f_read(&fp, &ehdr, sizeof(Elf32_Ehdr), &br);
    if (res != FR_OK || br != sizeof(Elf32_Ehdr))
    {
        f_close(&fp);
        return false;
    }

    /* Quick validate */
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F')
    {
        f_close(&fp);
        return false;
    }

    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 || ehdr.e_shstrndx >= ehdr.e_shnum)
    {
        f_close(&fp);
        return false;
    }

    /* Read shstrtab header */
    uint32_t shstr_off = ehdr.e_shoff + (uint32_t)ehdr.e_shstrndx * ehdr.e_shentsize;
    res = f_lseek(&fp, shstr_off);
    if (res != FR_OK)
    {
        f_close(&fp);
        return false;
    }

    res = f_read(&fp, &shdr, sizeof(Elf32_Shdr), &br);
    if (res != FR_OK || br != sizeof(Elf32_Shdr))
    {
        f_close(&fp);
        return false;
    }

    shstrtab_size = shdr.sh_size;
    if (shstrtab_size > sizeof(shstrtab))
        shstrtab_size = sizeof(shstrtab);

    res = f_lseek(&fp, shdr.sh_offset);
    if (res != FR_OK)
    {
        f_close(&fp);
        return false;
    }

    res = f_read(&fp, shstrtab, shstrtab_size, &br);
    if (res != FR_OK || br != shstrtab_size)
    {
        f_close(&fp);
        return false;
    }

    /* Scan section headers looking for .m1meta */
    uint16_t num_sh = ehdr.e_shnum;
    if (num_sh > ELF_MAX_SECTIONS)
        num_sh = ELF_MAX_SECTIONS;

    uint16_t i;
    for (i = 0; i < num_sh; i++)
    {
        uint32_t hdr_off = ehdr.e_shoff + (uint32_t)i * ehdr.e_shentsize;
        res = f_lseek(&fp, hdr_off);
        if (res != FR_OK)
            break;

        res = f_read(&fp, &shdr, sizeof(Elf32_Shdr), &br);
        if (res != FR_OK || br != sizeof(Elf32_Shdr))
            break;

        if (shdr.sh_name < shstrtab_size)
        {
            const char *sec_name = (const char *)&shstrtab[shdr.sh_name];
            if (strcmp(sec_name, ".m1meta") == 0 &&
                shdr.sh_size >= M1_APP_MANIFEST_CORE_SIZE)
            {
                /* Lenient read: old apps have only the 40-byte core (no category);
                 * newer apps append category[16] and then version[16]. Zero-fill
                 * first so every older app remains compatible. */
                m1_app_manifest_t manifest;
                memset(&manifest, 0, sizeof(manifest));
                uint32_t want = shdr.sh_size < sizeof(manifest)
                                ? shdr.sh_size : (uint32_t)sizeof(manifest);

                res = f_lseek(&fp, shdr.sh_offset);
                if (res != FR_OK)
                    break;

                res = f_read(&fp, &manifest, want, &br);
                if (res != FR_OK || br != want)
                    break;

                if (manifest.magic == M1_APP_MANIFEST_MAGIC &&
                    manifest.name[0] != '\0')
                {
                    manifest.name[sizeof(manifest.name) - 1] = '\0';
                    strncpy(name_out, manifest.name, name_len - 1);
                    name_out[name_len - 1] = '\0';
                    if (cat_out && cat_len) {
                        manifest.category[sizeof(manifest.category) - 1] = '\0';
                        strncpy(cat_out, manifest.category, cat_len - 1);
                        cat_out[cat_len - 1] = '\0';
                    }
                    if (version_out && version_len) {
                        manifest.version[sizeof(manifest.version) - 1] = '\0';
                        strncpy(version_out, manifest.version, version_len - 1);
                        version_out[version_len - 1] = '\0';
                    }
                    f_close(&fp);
                    return true;
                }
                break;
            }
        }
    }

    f_close(&fp);
    return false;
}


/*============================================================================*/
/*
 * @brief  Draw the app list on display
 * @param  title      Header title string
 * @param  count      Number of items
 * @param  selection  Currently selected index
 */
/*============================================================================*/
static void apps_draw_list(const char *title, uint16_t count, uint16_t selection)
{
    uint16_t i;
    uint16_t start_idx;
    uint16_t visible;
    uint8_t y;

    /* Calculate which items are visible */
    if (selection < LIST_VISIBLE_ITEMS)
        start_idx = 0;
    else
        start_idx = selection - LIST_VISIBLE_ITEMS + 1;

    visible = count - start_idx;
    if (visible > LIST_VISIBLE_ITEMS)
        visible = LIST_VISIBLE_ITEMS;

    u8g2_FirstPage(&m1_u8g2);
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);

    /* Title bar */
    u8g2_SetFont(&m1_u8g2, M1_DISP_RUN_MENU_FONT_B);
    u8g2_DrawStr(&m1_u8g2, 2, 10, title);
    u8g2_DrawHLine(&m1_u8g2, 0, LIST_HEADER_HEIGHT, 128);

    /* List items */
    u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
    for (i = 0; i < visible; i++)
    {
        uint16_t idx = start_idx + i;
        char app_name[APPS_NAME_MAX_LEN] = "";
        y = LIST_START_Y + (i * LIST_ITEM_HEIGHT);
        if (!m1_apps_get(idx, NULL, 0, app_name, sizeof(app_name), NULL, 0))
            strncpy(app_name, "<app changed>", sizeof(app_name) - 1);

        if (idx == selection)
        {
            u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
            u8g2_DrawBox(&m1_u8g2, 0, y, 128, LIST_ITEM_HEIGHT);
            u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
            u8g2_DrawStr(&m1_u8g2, 4, y + 8, app_name);
            u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
        }
        else
        {
            u8g2_DrawStr(&m1_u8g2, 4, y + 8, app_name);
        }
    }

    /* Scroll indicator */
    if (count > LIST_VISIBLE_ITEMS)
    {
        uint8_t bar_height = (LIST_VISIBLE_ITEMS * LIST_ITEM_HEIGHT * LIST_VISIBLE_ITEMS) / count;
        uint8_t bar_y;

        if (bar_height < 4)
            bar_height = 4;

        bar_y = LIST_START_Y + (start_idx * (LIST_VISIBLE_ITEMS * LIST_ITEM_HEIGHT - bar_height))
                / (count - LIST_VISIBLE_ITEMS);
        u8g2_DrawBox(&m1_u8g2, 126, bar_y, 2, bar_height);
    }

    /* Bottom bar */
    {
        char page_info[16];
        snprintf(page_info, sizeof(page_info), "%u/%u", (unsigned)(selection + 1), (unsigned)count);
        u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
        m1_draw_bottom_bar(&m1_u8g2, arrowleft_8x8, page_info, "Run", arrowright_8x8);
    }

    m1_u8g2_nextpage();
}


/*============================================================================*/
/*
 * @brief  Draw a simple message screen (two centered lines)
 * @param  line1  First line text (or NULL)
 * @param  line2  Second line text (or NULL)
 */
/*============================================================================*/
static void apps_draw_message(const char *line1, const char *line2)
{
    u8g2_FirstPage(&m1_u8g2);
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);

    /* Title bar */
    u8g2_SetFont(&m1_u8g2, M1_DISP_RUN_MENU_FONT_B);
    u8g2_DrawStr(&m1_u8g2, 2, 10, "Apps");
    u8g2_DrawHLine(&m1_u8g2, 0, 12, 128);

    /* Message lines centered */
    u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
    if (line1 != NULL)
    {
        uint16_t w = (uint16_t)u8g2_GetStrWidth(&m1_u8g2, line1);
        u8g2_DrawStr(&m1_u8g2, (128 - w) / 2, 32, line1);
    }
    if (line2 != NULL)
    {
        uint16_t w = (uint16_t)u8g2_GetStrWidth(&m1_u8g2, line2);
        u8g2_DrawStr(&m1_u8g2, (128 - w) / 2, 44, line2);
    }

    m1_u8g2_nextpage();
}


/*============================================================================*/
/*
 * @brief  FreeRTOS task wrapper that calls the app's entry point
 * @param  param  Pointer to app_run_ctx_t
 */
/*============================================================================*/
static void app_task_wrapper(void *param)
{
    app_run_ctx_t *ctx = (app_run_ctx_t *)param;
    int32_t (*entry)(void *) = (int32_t (*)(void *))ctx->elf.entry_point;

    M1_LOG_I(TAG, "App task started, entry=0x%08lX",
             (unsigned long)ctx->elf.entry_point);

    /* This task owns the display for its whole lifetime: the lease is granted
     * here and revoked on every exit path below, so focus never outlives the
     * app. A failed grant means a previous app leaked its lease — draw anyway
     * (single foreground app) but make the leak visible. */
    if (!m1_ui_focus_try_grant((void *)xTaskGetCurrentTaskHandle()))
        M1_LOG_E(TAG, "display focus already held at app start (leaked?)");

    /* Name the running app in the crash record (first 8 chars, TAMP
     * BKP15/16R) so a fault during this app is attributable. */
    m1_crash_note_app(ctx->elf.manifest.name);

    /* Call the app's main function */
    int32_t result = entry(NULL);

    M1_LOG_I(TAG, "App returned %ld", (long)result);

    /* Signal the caller that we're done */
    ctx->running = false;

    /* Give the display back before waking the menu (idempotent: only the
     * owner can revoke, and this task is the owner). */
    (void)m1_ui_focus_revoke((void *)xTaskGetCurrentTaskHandle());

    /* ESP-channel lease teardown: stop anything this app left running on the
     * radio (monitor/deauth/beacon/spam/captive/...). Each stop is a bounded
     * RPC under the client mutex; the count lands in the log. */
    int stopped = m1_esp_lease_revoke((void *)xTaskGetCurrentTaskHandle());
    if (stopped > 0)
        M1_LOG_W(TAG, "ESP lease teardown: stopped %d runaway op(s)", stopped);

    m1_crash_note_app(NULL);

    /* Notify the caller task */
    if (ctx->caller_handle != NULL)
    {
        xTaskNotifyGive(ctx->caller_handle);
    }

    /* Delete ourselves */
    vTaskDelete(NULL);
}


/*============================================================================*/
/*
 * @brief  Load and run an app from the given path
 * @param  path  FatFS path to the .m1app file
 * @retval elf_load_status_t
 */
/*============================================================================*/
static elf_load_status_t apps_run(const char *path)
{
    elf_load_status_t status;
    const m1_api_interface_t *api;
    BaseType_t xret;
    uint32_t stack_words;
    char abspath[APPS_PATH_MAX_LEN];

    /* The menu stores each app by BASENAME (from the app index). f_open() then
     * resolves it against the FatFs current directory — which any preceding
     * f_opendir (e.g. a qMonstatek File Manager listing of "0:/") can move off
     * 0:/apps, making the open fail FR_NO_FILE. Always launch by absolute path
     * so loading is independent of the current directory. */
    if (path && !strchr(path, '/'))
    {
        snprintf(abspath, sizeof(abspath), "%s/%s", APPS_DIR, path);
        path = abspath;
    }

    /* Show loading screen */
    apps_draw_message("Loading...", NULL);

    api = m1_app_get_api();

    /* Load the ELF */
    memset(&s_run_ctx, 0, sizeof(s_run_ctx));
    status = elf_load_app(path, &s_run_ctx.elf, api);
    if (status != ELF_OK)
    {
        M1_LOG_E(TAG, "Load failed: %d", status);
        return status;
    }

    /* Determine stack size */
    stack_words = s_run_ctx.elf.manifest.stack_size;
    if (stack_words < 512)
        stack_words = 512;

    M1_LOG_I(TAG, "Starting app \"%s\" stack=%lu words",
             s_run_ctx.elf.manifest.name, (unsigned long)stack_words);

    /* Store caller task handle for notification */
    s_run_ctx.caller_handle = xTaskGetCurrentTaskHandle();
    s_run_ctx.running = true;

    /* Create the app task */
    s_run_ctx.supervised = (s_run_ctx.elf.manifest.api_version >= 7);
    s_run_ctx.cancel_requested = false;
    s_run_ctx.last_hb = xTaskGetTickCount();

    xret = xTaskCreate(app_task_wrapper,
                       s_run_ctx.elf.manifest.name,
                       (uint16_t)stack_words,
                       &s_run_ctx,
                       APP_TASK_PRIORITY,
                       &s_run_ctx.task_handle);

    if (xret != pdPASS)
    {
        M1_LOG_E(TAG, "Task create failed (stack=%lu words, heap=%lu)",
                 (unsigned long)stack_words,
                 (unsigned long)xPortGetFreeHeapSize());
        elf_unload_app(&s_run_ctx.elf);
        return ELF_ERR_NO_MEMORY;
    }

    if (s_run_ctx.running)
    {
        M1_LOG_E(TAG, "previous app is hung/contained - reboot required");
        return ELF_ERR_NO_MEMORY;
    }

    /* Wait for the app task to complete. App input belongs to the app task:
     * it may need BACK to stop a radio, flush a file, or release a BLE link
     * before returning. The former parent-side BACK check raced that cleanup
     * and could vTaskDelete() the app immediately after a normal click. */
    while (s_run_ctx.running)
    {
        /* Timed wait keeps this task responsive without stealing app input. */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        (void)notified;

        if (!s_run_ctx.running)
            break;

        if (s_run_ctx.supervised &&
            (xTaskGetTickCount() - s_run_ctx.last_hb) > pdMS_TO_TICKS(M1_APP_HANG_GRACE_MS))
        {
            void *tok = (void *)s_run_ctx.task_handle;
            M1_LOG_E(TAG, "app hung (no heartbeat for %d ms) - containing",
                     (int)M1_APP_HANG_GRACE_MS);
            vTaskSuspend(s_run_ctx.task_handle);
            int stopped = m1_esp_lease_revoke(tok);
            (void)m1_ui_focus_revoke(tok);

            m1_crash_info_t ci;
            memset(&ci, 0, sizeof(ci));
            ci.kind = M1_CRASH_KIND_HUNG;
            ci.uptime_ms = xTaskGetTickCount();
            snprintf(ci.app, sizeof(ci.app), "%s", s_run_ctx.elf.manifest.name);
            m1_crash_record_runtime(&ci);
            if (stopped > 0)
                M1_LOG_W(TAG, "hung app had %d radio op(s) stopped", stopped);

            /* Zombie holds the ELF + tracked heap until reboot; skip the
             * normal cleanup below - it is only safe after a real exit. */
            xQueueReset(main_q_hdl);
            return ELF_OK;
        }
    }

    /* Cleanup */
    s_run_ctx.task_handle = NULL;
    elf_unload_app(&s_run_ctx.elf);

    /* Free any heap the app itself allocated. elf_unload_app only reclaims the
     * ELF sections, so without this a normally-exited (and especially a
     * force-killed) app leaks whatever it malloc'd — which, over many launches,
     * slowly exhausts the shared system heap. Safe here: the app task has
     * already stopped, so nothing is touching the tracked list. */
    m1_app_tracked_free_all();

    /* Drain any stale button events */
    xQueueReset(main_q_hdl);

    M1_LOG_I(TAG, "App finished, heap=%lu",
             (unsigned long)xPortGetFreeHeapSize());

    return ELF_OK;
}


/*============================================================================*/
/*
 * @brief  Browse and run apps from SD card. Called as a menu sub_func.
 */
/*============================================================================*/
void game_apps_browser_run(void)
{
    S_M1_Buttons_Status this_button_status;
    S_M1_Main_Q_t q_item;
    BaseType_t ret;
    uint16_t selection = 0;
    uint16_t count;

    /* Initialize API if not done yet */
    m1_app_api_init();

    /* Refresh/read the shared snapshot. It serializes any simultaneous qM
     * catalog request and preserves a usable prior list on SD failure. */
    apps_draw_message("Scanning...", NULL);
    count = m1_app_manager_scan();

    if (count == 0)
    {
        apps_draw_message("No apps found", "Put .m1app in /apps/");

        /* Wait for BACK button */
        while (1)
        {
            ret = xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY);
            if (ret == pdTRUE && q_item.q_evt_type == Q_EVENT_KEYPAD)
            {
                ret = xQueueReceive(button_events_q_hdl, &this_button_status, 0);
                if (this_button_status.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK)
                {
                    xQueueReset(main_q_hdl);
                    return;
                }
            }
        }
    }

    /* Draw the initial list */
    apps_draw_list("Apps", count, selection);

    while (1)
    {
        ret = xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY);
        if (ret != pdTRUE)
            continue;

        if (q_item.q_evt_type != Q_EVENT_KEYPAD)
            continue;

        ret = xQueueReceive(button_events_q_hdl, &this_button_status, 0);
        if (ret != pdTRUE)
            continue;

        if (this_button_status.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK)
        {
            xQueueReset(main_q_hdl);
            break; /* Exit browser */
        }
        else if (this_button_status.event[BUTTON_UP_KP_ID] == BUTTON_EVENT_CLICK)
        {
            if (selection > 0)
                selection--;
            else
                selection = count - 1;
            apps_draw_list("Apps", count, selection);
        }
        else if (this_button_status.event[BUTTON_DOWN_KP_ID] == BUTTON_EVENT_CLICK)
        {
            if (selection < count - 1)
                selection++;
            else
                selection = 0;
            apps_draw_list("Apps", count, selection);
        }
        else if (this_button_status.event[BUTTON_OK_KP_ID] == BUTTON_EVENT_CLICK)
        {
            /* Run the selected app */
            char app_path[APPS_PATH_MAX_LEN] = "";
            if (!m1_apps_get(selection, app_path, sizeof(app_path), NULL, 0, NULL, 0))
            {
                apps_draw_message("Apps changed", "Reopen list");
                continue;
            }
            elf_load_status_t result = apps_run(app_path);

            if (result != ELF_OK)
            {
                /* Show error */
                const char *err_msg;
                char file_err_buf[24];
                switch (result)
                {
                    case ELF_ERR_FILE:
                        /* Include the FatFS code so an intermittent SD "File error"
                         * self-reports its cause (15=timeout/held-mutex, 1=disk err,
                         * 3=not ready, 4=no file). */
                        snprintf(file_err_buf, sizeof(file_err_buf), "File error (%u)",
                                 (unsigned)elf_last_ferr());
                        err_msg = file_err_buf;
                        break;
                    case ELF_ERR_FORMAT:     err_msg = "Bad format";       break;
                    case ELF_ERR_MACHINE:    err_msg = "Wrong arch";       break;
                    case ELF_ERR_NO_MEMORY:  err_msg = "Out of memory";    break;
                    case ELF_ERR_RELOCATE:   err_msg = "Relocation fail";  break;
                    case ELF_ERR_SYMBOL:     err_msg = "Missing symbol";   break;
                    case ELF_ERR_MANIFEST:   err_msg = "Bad manifest";     break;
                    case ELF_ERR_API_VERSION: err_msg = "API too new";     break;
                    default:                 err_msg = "Unknown error";     break;
                }

                apps_draw_message("Load Error", err_msg);

                /* Wait for any button to dismiss */
                while (1)
                {
                    ret = xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY);
                    if (ret == pdTRUE && q_item.q_evt_type == Q_EVENT_KEYPAD)
                    {
                        xQueueReceive(button_events_q_hdl, &this_button_status, 0);
                        break;
                    }
                }
            }

            /* Re-draw list after app returns */
            apps_draw_list("Apps", count, selection);
        }
    }
}
