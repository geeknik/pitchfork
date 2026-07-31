/* See COPYING.txt for license details. */

/*
*
*  m1_app_manager.h
*
*  App manager for loading and running external .m1app files from SD card
*
* M1 Project
*
*/

#ifndef M1_APP_MANAGER_H_
#define M1_APP_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"

/* Current in-memory catalog capacity. The app binaries themselves remain on
 * SD; this bounds one scan/menu snapshot, not SD storage. Keep this shared so
 * the app manager, dynamic menu, and RPC catalog cannot silently disagree. */
#define M1_APPS_MAX_FILES 64

/* Initialize the app manager (call once at startup) */
void m1_app_manager_init(void);

/* Browse and run apps from SD card — called as menu sub_func */
void game_apps_browser_run(void);

/* Enumerate installed .m1app apps on the SD card for RPC (qM App Catalog).
 * m1_app_manager_scan() rescans 0:/apps and returns the count; m1_app_manager_get()
 * yields the filename and the .m1meta display name for a given index (pointers
 * into the internal list, valid until the next scan). */
uint16_t m1_app_manager_scan(void);
FRESULT  m1_app_manager_last_scan_result(void);
bool     m1_app_manager_get(uint16_t idx, const char **filename, const char **name,
                            const char **category);

/* Modular menu layout (m1_menu.c): scan 0:/apps, read each app's path/name/tag,
 * and launch a specific app by path. m1_apps_run returns an elf_load_status_t
 * (int) — 0 (ELF_OK) on success. */
int  m1_apps_scan(void);
bool m1_apps_get(int idx, char *path, uint16_t path_len,
                 char *name, uint16_t name_len, char *cat, uint16_t cat_len);
int  m1_apps_run(const char *path);

#endif /* M1_APP_MANAGER_H_ */
