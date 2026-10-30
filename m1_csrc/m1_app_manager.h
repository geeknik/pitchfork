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

/* Enumerate installed .m1app apps for RPC (qM App Catalog). The manager owns a
 * single cached snapshot: a scan refreshes it only when it has been marked dirty,
 * and all callers see copied entries rather than mutable internal pointers. */
/* Supervision (see M1_SYS_OP_HEARTBEAT / CANCEL_POLL in m1_app_syscall.h). */
bool m1_app_manager_heartbeat(void);
bool m1_app_manager_cancel_requested(void);

uint16_t m1_app_manager_scan(void);
FRESULT  m1_app_manager_last_scan_result(void);
bool     m1_app_manager_get(uint16_t idx,
                            char *filename, uint16_t filename_len,
                            char *name, uint16_t name_len,
                            char *category, uint16_t category_len,
                            char *version, uint16_t version_len);

/* Mark the cached index stale after a change under 0:/apps. The next reader
 * performs the one serialized refresh; this call never performs SD I/O. */
void m1_app_manager_mark_dirty(void);

/* Modular menu layout (m1_menu.c): scan 0:/apps, read each app's path/name/tag,
 * and launch a specific app by path. m1_apps_run returns an elf_load_status_t
 * (int) — 0 (ELF_OK) on success. */
int  m1_apps_scan(void);
bool m1_apps_get(int idx, char *path, uint16_t path_len,
                 char *name, uint16_t name_len, char *cat, uint16_t cat_len);
int  m1_apps_run(const char *path);

#endif /* M1_APP_MANAGER_H_ */
