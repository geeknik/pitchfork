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

/* Initialize the app manager (call once at startup) */
void m1_app_manager_init(void);

/* Browse and run apps from SD card — called as menu sub_func */
void game_apps_browser_run(void);

/* Enumerate installed .m1app apps on the SD card for RPC (qM App Catalog).
 * m1_app_manager_scan() rescans 0:/apps and returns the count; m1_app_manager_get()
 * yields the filename and the .m1meta display name for a given index (pointers
 * into the internal list, valid until the next scan). */
uint16_t m1_app_manager_scan(void);
bool     m1_app_manager_get(uint16_t idx, const char **filename, const char **name);

#endif /* M1_APP_MANAGER_H_ */
