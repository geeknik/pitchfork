/* See COPYING.txt for license details. */

/*
*
*  m1_menu.c
*
*  M1 menu handler
*
* M1 Project
*
*/
/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include "stm32h5xx_hal.h"
#include "main.h"
#include <string.h>
#include "ff.h"              /* menu.cfg reader (data-driven top-level layout) */
#include "m1_app_manager.h"  /* m1_apps_scan/get/run — inject apps into folders */
//#include "mui.h"
//#include "u8x8.h"
//#include "U8g2lib.h"
#include "m1_gpio.h"
#include "m1_peer_link.h"   /* M1<->M1 ESP-NOW peer link (Tools) */
#include "m1_field_detector.h"   /* NFC/RFID reader-field detector (Tools) */
#include "m1_infrared.h"
#include "m1_nfc.h"
#include "m1_rfid.h"
#include "m1_settings.h"
#include "m1_sub_ghz.h"
#include "m1_power_ctl.h"
#include "m1_fw_update.h"
#include "m1_esp32_fw_update.h"
#include "m1_storage.h"
#include "m1_wifi.h"
#include "m1_bt.h"
#include "m1_802154.h"
#include "m1_esp32_hal.h"
#include "m1_esp_client.h"   /* m1_esp_client_ble_direct — apply Bluetooth Direct at boot */
#include "esp_app_main.h"
#include "m1_compile_cfg.h"

#ifdef M1_APP_FILE_IMPORT_ENABLE
#include "m1_flipper_integration.h"
#endif

/*************************** D E F I N E S ************************************/

//************************** C O N S T A N T **********************************/

/************************** S T R U C T U R E S *******************************/

/*----------------------------- > Sub-GHz ------------------------------------*/

/* Sub-GHz cutover to hapax scene engine: the entire subsystem (Read, Saved,
 * Add Manually, Frequency Reader, Spectrum, RSSI, Freq Scanner, Weather, Brute
 * Force, Regional, Radio Settings, Jammer) is now driven internally by the
 * scene manager. The top-level menu is a single leaf action into
 * sub_ghz_scene_entry(); all former sub-item structs are retired. */
S_M1_Menu_t menu_Sub_GHz =
{
    "Sub-GHz", sub_ghz_scene_entry, NULL, NULL, 0, 1, menu_m1_icon_wave, NULL, {0}   /* reserved=1: Sub-GHz manages orientation per-scene */
};

/*----------------------------- > 125KHz RFID --------------------------------*/

S_M1_Menu_t menu_125KHz_RFID_Read =
{
    "Read", rfid_125khz_read, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_125KHz_RFID_Saved =
{
    "Saved", rfid_125khz_saved, NULL, NULL, 0, 0, NULL, NULL, NULL
};


S_M1_Menu_t menu_125KHz_RFID_Add_Manually =
{
    "Add", rfid_125khz_add_manually, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_125KHz_RFID_Utilities =
{
    "Utilities", rfid_125khz_utilities, NULL, NULL, 0, 0, NULL, NULL, NULL
};

#ifdef M1_APP_FILE_IMPORT_ENABLE
S_M1_Menu_t menu_125KHz_RFID_Import =
{
    "Import .rfid", rfid_import_flipper, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_125KHz_RFID =
{
    "RFID", menu_125khz_rfid_init, menu_125khz_rfid_deinit, NULL, 5, 0, menu_m1_icon_rfid, NULL,
    {&menu_125KHz_RFID_Read, &menu_125KHz_RFID_Saved, &menu_125KHz_RFID_Add_Manually, &menu_125KHz_RFID_Import, &menu_125KHz_RFID_Utilities}
};
#else
S_M1_Menu_t menu_125KHz_RFID =
{
    "RFID", menu_125khz_rfid_init, menu_125khz_rfid_deinit, NULL, 4, 0, menu_m1_icon_rfid, NULL,
    {&menu_125KHz_RFID_Read, &menu_125KHz_RFID_Saved, &menu_125KHz_RFID_Add_Manually, &menu_125KHz_RFID_Utilities}
};
#endif

/*-------------------------------- > NFC -------------------------------------*/

S_M1_Menu_t menu_NFC_Read =
{
    "Read", nfc_read, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_NFC_Detect_Reader =
{
    "Detect Reader", nfc_detect_reader, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_NFC_Saved =
{
    "Saved", nfc_saved, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_NFC_Extra_Actions =
{
    "Extra Actions", nfc_extra_actions, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_NFC_Add_Manually =
{
    "Add Manually", nfc_add_manually, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_NFC_Tools =
{
    "Tools", nfc_tools, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_NFC =
{
    "NFC", &menu_nfc_init, menu_nfc_deinit, NULL, 6, 0, menu_m1_icon_nfc, NULL,
    {&menu_NFC_Read, &menu_NFC_Detect_Reader, &menu_NFC_Saved, &menu_NFC_Extra_Actions, &menu_NFC_Add_Manually, &menu_NFC_Tools }
};

/*----------------------------- > Infrared -----------------------------------*/

S_M1_Menu_t menu_Infrared_Universal_Remotes =
{
    "Universal Remotes", infrared_universal_remotes, NULL, NULL, 0, 1, NULL, NULL, NULL   /* reserved=1: IR manages orientation (Remote mode) */
};

S_M1_Menu_t menu_Infrared_Learn_New_Remote =
{
    "Learn", infrared_learn_new_remote, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Infrared_Saved_Remotes =
{
    "Replay", infrared_saved_remotes, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Infrared =
{
    "Infrared", menu_infrared_init, NULL, NULL, 3, 0, menu_m1_icon_infrared, NULL,
    {&menu_Infrared_Universal_Remotes, &menu_Infrared_Learn_New_Remote, &menu_Infrared_Saved_Remotes}
};

/*------------------------------- > GPIO -------------------------------------*/

S_M1_Menu_t menu_GPIO_GPIO_Manual_Control =
{
    "GPIO Control", gpio_manual_control, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_GPIO_3_3V_On_GPIO =
{
    "3.3V power", gpio_3_3v_on_gpio, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_GPIO_5V_On_GPIO =
{
    "5V power", gpio_5v_on_gpio, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_GPIO_USB_UART =
{
    "USB-UART bridge", gpio_usb_uart_bridge, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_GPIO =
{
    "GPIO", menu_gpio_init, menu_gpio_exit, gpio_xkey_handler, 4, 2, menu_m1_icon_gpio, gpio_gui_update,   /* reserved=2: GPIO subtree is landscape */
    {&menu_GPIO_GPIO_Manual_Control, &menu_GPIO_3_3V_On_GPIO, &menu_GPIO_5V_On_GPIO, &menu_GPIO_USB_UART}
};

/*------------------------------- > TOOLS ------------------------------------*/

S_M1_Menu_t menu_Tools_I2C_Scan =
{
    "I2C Scanner", tool_i2c_scanner, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Tools_Pin_Reader =
{
    "Pin Reader", tool_pin_reader, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Tools_Peer_Link =
{
    "Peer Link", m1_peer_link_run, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Tools_Field_Detect =
{
    "Field Detector", m1_field_detector_run, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Tools_Peer_Recover =
{
    "Recover Peer", tool_peer_recover, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Tools =
{
    "Tools", NULL, NULL, NULL, 5, 0, menu_m1_icon_gpio, NULL,
    {&menu_Tools_I2C_Scan, &menu_Tools_Pin_Reader, &menu_Tools_Peer_Link, &menu_Tools_Field_Detect, &menu_Tools_Peer_Recover}
};

/*------------------------------- > Settings ---------------------------------*/

/*------------------------- > Settings-Storage -------------------------------*/

S_M1_Menu_t menu_Setting_Storage_About =
{
    "About SD Card", storage_about, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Storage_Explore =
{
    "Explore SD Card", storage_explore, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Storage_Mount =
{
    "Mount SD Card", storage_mount, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Storage_Unmount =
{
    "Unmount SD Card", storage_unmount, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Storage_Format =
{
    "Format SD Card", storage_format, NULL, NULL, 0, 0, NULL, NULL, NULL
};

/*------------------------- > Settings-Storage-End ---------------------------*/

/*------------------------- > Settings-Power ---------------------------------*/

S_M1_Menu_t menu_Setting_Power_Info =
{
    "Battery Info", power_battery_info, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Power_Reboot =
{
    "Reboot", power_reboot, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Power_Off =
{
    "Power Off", power_off, NULL, NULL, 0, 0, NULL, NULL, NULL
};

/*----------------------- > Settings-Power-End -------------------------------*/


/*---------------------- > Settings-Firmware Update --------------------------*/

S_M1_Menu_t menu_Setting_Firmware_Update_Image_File =
{
    "Image File", firmware_update_get_image_file, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Firmware_Update_Start =
{
    "Firmware update", firmware_update_start, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Firmware_Swap_Banks =
{
    "Swap Banks", firmware_swap_banks, NULL, NULL, 0, 0, NULL, NULL, NULL
};


/*---------------------- > Settings-Firmware Update-End ----------------------*/


/*---------------------- > Settings-ESP32 Update --------------------------*/

S_M1_Menu_t menu_Setting_ESP32_Image_File =
{
    "Image File", setting_esp32_image_file, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_ESP32_Start_Address =
{
    "Start Address", setting_esp32_start_address, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_ESP32_Firmware_Update =
{
    "Firmware Update", setting_esp32_firmware_update, NULL, NULL, 0, 0, NULL, NULL, NULL
};

/*---------------------- > Settings-ESP32 Update-End ----------------------*/

S_M1_Menu_t menu_Settings_LCD_and_Notifications =
{
    "LCD and Notifications", settings_lcd_and_notifications, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Settings_Storage =
{
    "Storage", menu_setting_storage_init, NULL, NULL, 5, 0, NULL, NULL, {&menu_Setting_Storage_About, &menu_Setting_Storage_Explore, &menu_Setting_Storage_Mount, &menu_Setting_Storage_Unmount, &menu_Setting_Storage_Format}
};

S_M1_Menu_t menu_Settings_Power =
{
    "Power", menu_setting_power_init, NULL, NULL, 3, 0, NULL, NULL, {&menu_Setting_Power_Info, &menu_Setting_Power_Reboot, &menu_Setting_Power_Off}
};

S_M1_Menu_t menu_Settings_System =
{
    "System", settings_system, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Setting_Firmware_Update =
{
    "Firmware update", firmware_update_init, firmware_update_exit, NULL, 3, 0, NULL, firmware_update_gui_update, {&menu_Setting_Firmware_Update_Image_File, &menu_Setting_Firmware_Update_Start, &menu_Setting_Firmware_Swap_Banks}
};

S_M1_Menu_t menu_Setting_ESP32 =
{
    "ESP32 update", setting_esp32_init, setting_esp32_exit, setting_esp32_xkey_handler, 3, 0, NULL, setting_esp32_gui_update, {&menu_Setting_ESP32_Image_File, &menu_Setting_ESP32_Start_Address, &menu_Setting_ESP32_Firmware_Update}
};

S_M1_Menu_t menu_Settings_About =
{
    "About", settings_about, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Settings =
{
    "Settings", menu_settings_init, NULL, NULL, 7, 2, menu_m1_icon_setting, NULL,   /* reserved=2: whole Settings subtree is landscape */
    {&menu_Settings_LCD_and_Notifications, &menu_Settings_Storage, &menu_Settings_Power, &menu_Settings_System, &menu_Setting_Firmware_Update, &menu_Setting_ESP32, &menu_Settings_About}
};

/*------------------------------ > 802.15.4 ----------------------------------*/

S_M1_Menu_t menu_802154_Scan =
{
    "802.15.4 Scan", ieee802154_scan_all, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_802154_Flood =
{
    "802.15.4 Flood", zigbee_beacon_flood, NULL, NULL, 0, 0, NULL, NULL, NULL
};

/*--------------------------------- > Wifi -----------------------------------*/

S_M1_Menu_t menu_Wifi_Scan_AP =
{
    "WiFi Scan+Connect", wifi_scan_ap, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Deauth =
{
    "Deauth", wifi_deauth_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Handshake =
{
    "Handshake", wifi_handshake_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Beacon =
{
    "Beacon", wifi_beacon_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Monitor =
{
    "Packet Monitor", wifi_monitor_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Captive =
{
    "Captive Portal", wifi_captive_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_ProbeFlood =
{
    "Probe Flood", wifi_probe_flood_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Karma =
{
    "Karma", wifi_karma_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Config =
{
    "Saved Networks", wifi_config, NULL, NULL, 0, 0, NULL, NULL, NULL
};

#ifdef M1_APP_WIFI_CONNECT_ENABLE
S_M1_Menu_t menu_Wifi_Status =
{
    "Status", wifi_show_status, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Disconnect =
{
    "Disconnect", wifi_disconnect, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi_Hotspot =
{
    "WiFi Hotspot", wifi_hotspot_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Wifi =
{
#if defined(M1OS_BAREBONES)
    /* Core WiFi only: scan, connect/saved, hotspot, status, disconnect. The
     * offensive tools (deauth, handshake, beacon, probe flood, karma, monitor,
     * captive, 802.15.4) ship as downloadable apps, not baked into the FW. */
    "Wifi", menu_wifi_init, NULL, NULL, 5, 0, menu_m1_icon_wifi, NULL,
    {&menu_Wifi_Scan_AP, &menu_Wifi_Config, &menu_Wifi_Hotspot, &menu_Wifi_Status, &menu_Wifi_Disconnect}
#else
    "Wifi", menu_wifi_init, NULL, NULL, 14, 0, menu_m1_icon_wifi, NULL,
    {&menu_Wifi_Scan_AP, &menu_Wifi_Deauth, &menu_Wifi_Handshake, &menu_Wifi_Beacon, &menu_Wifi_ProbeFlood, &menu_Wifi_Karma, &menu_Wifi_Monitor, &menu_Wifi_Captive, &menu_802154_Scan, &menu_802154_Flood, &menu_Wifi_Config, &menu_Wifi_Hotspot, &menu_Wifi_Status, &menu_Wifi_Disconnect}
#endif
};
#else
S_M1_Menu_t menu_Wifi =
{
    "Wifi", menu_wifi_init, NULL, NULL, 11, 0, menu_m1_icon_wifi, NULL,
    {&menu_Wifi_Scan_AP, &menu_Wifi_Deauth, &menu_Wifi_Handshake, &menu_Wifi_Beacon, &menu_Wifi_ProbeFlood, &menu_Wifi_Karma, &menu_Wifi_Monitor, &menu_Wifi_Captive, &menu_802154_Scan, &menu_802154_Flood, &menu_Wifi_Config}
};
#endif

/*--------------------------------- > BT ------------------------------------*/
S_M1_Menu_t menu_Bluetooth_Scan =
{
    "Scan", bluetooth_scan, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth_Spam =
{
    "BLE Spam", ble_spam_menu, NULL, NULL, 0, 0, NULL, NULL, NULL
};

#ifdef M1_APP_BT_MANAGE_ENABLE
S_M1_Menu_t menu_Bluetooth_Saved =
{
    "Saved", bluetooth_saved_devices, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth_Advertise =
{
    "Advertise", bluetooth_advertise, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth_Info =
{
    "BT Info", bluetooth_info, NULL, NULL, 0, 0, NULL, NULL, NULL
};

#ifdef M1_APP_BADBT_ENABLE
#include "m1_badbt.h"

S_M1_Menu_t menu_Bluetooth_BadBT =
{
    "Bad-BT", badbt_run, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth_BTName =
{
    "BT Name", bluetooth_set_badbt_name, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth =
{
#if defined(M1OS_BAREBONES)
    /* Core Bluetooth only: scan, saved devices, info. Spam / advertise / Bad-BT
     * ship as downloadable apps. */
    "Bluetooth", menu_bluetooth_init, NULL, NULL, 3, 0, menu_m1_icon_bluetooth, NULL,
    {&menu_Bluetooth_Scan, &menu_Bluetooth_Saved, &menu_Bluetooth_Info}
#else
    "Bluetooth", menu_bluetooth_init, NULL, NULL, 7, 0, menu_m1_icon_bluetooth, NULL,
    {&menu_Bluetooth_Scan, &menu_Bluetooth_Spam, &menu_Bluetooth_Saved, &menu_Bluetooth_Advertise, &menu_Bluetooth_BadBT, &menu_Bluetooth_BTName, &menu_Bluetooth_Info}
#endif
};
#else
S_M1_Menu_t menu_Bluetooth =
{
    "Bluetooth", menu_bluetooth_init, NULL, NULL, 5, 0, menu_m1_icon_bluetooth, NULL,
    {&menu_Bluetooth_Scan, &menu_Bluetooth_Spam, &menu_Bluetooth_Saved, &menu_Bluetooth_Advertise, &menu_Bluetooth_Info}
};
#endif /* M1_APP_BADBT_ENABLE */

#else
S_M1_Menu_t menu_Bluetooth_Config =
{
    "Config", bluetooth_config, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth_Advertise =
{
    "Advertise", bluetooth_advertise, NULL, NULL, 0, 0, NULL, NULL, NULL
};

S_M1_Menu_t menu_Bluetooth =
{
    "Bluetooth", menu_bluetooth_init, NULL, NULL, 4, 0, menu_m1_icon_bluetooth, NULL,
    {&menu_Bluetooth_Config, &menu_Bluetooth_Scan, &menu_Bluetooth_Spam, &menu_Bluetooth_Advertise}
};
#endif /* M1_APP_BT_MANAGE_ENABLE */

/*-------------------------------- > BadUSB ----------------------------------*/
#ifdef M1_APP_BADUSB_ENABLE
#include "m1_badusb.h"

S_M1_Menu_t menu_BadUSB =
{
    "BadUSB", badusb_run, NULL, NULL, 0, 0, menu_m1_icon_badusb, NULL, NULL
};
#endif /* M1_APP_BADUSB_ENABLE */

/*-------------------------------- > Games -----------------------------------*/
#ifdef M1_APP_GAMES_ENABLE
#include "m1_games.h"

S_M1_Menu_t menu_Snake   = { "Snake",       game_snake_run,  NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_Tetris  = { "Tetris",      game_tetris_run, NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_TRex    = { "T-Rex Runner",game_trex_run,   NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_Pong    = { "Pong",        game_pong_run,   NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_Dice    = { "Dice Roll",   game_dice_run,   NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_Flappy  = { "Flappy Bird", game_flappy_run,     NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_Coin    = { "Coin Flip",   game_coin_flip_run,  NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_RPS     = { "Rock Paper Sc",game_rps_run,       NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_Tama    = { "Tamagotchi",  game_tamagotchi_run, NULL, NULL, 0, 0, NULL, NULL, {NULL} };
S_M1_Menu_t menu_TTT     = { "Tic-Tac-Toe", game_peer_ttt_run,   NULL, NULL, 0, 0, NULL, NULL, {NULL} };

S_M1_Menu_t menu_Games =
{
    "Games", NULL, NULL, NULL, 10, 0, menu_m1_icon_games, NULL,
    {&menu_Snake, &menu_Tetris, &menu_TRex, &menu_Pong, &menu_Dice,
     &menu_Flappy, &menu_Coin, &menu_RPS, &menu_Tama, &menu_TTT}
};
#endif /* M1_APP_GAMES_ENABLE */

/*-------------------------------- > Apps ------------------------------------*/
#ifdef M1_APP_APPS_ENABLE
#include "m1_app_manager.h"

S_M1_Menu_t menu_Apps =
{
    "Apps", game_apps_browser_run, NULL, NULL, 0, 0, menu_m1_icon_apps, NULL, {NULL}
};
#endif /* M1_APP_APPS_ENABLE */

/*------------------------------- > MAIN MENU --------------------------------*/

const S_M1_Menu_t menu_Main =
{
#if defined(M1OS_BAREBONES)
    /* Modular runtime: core-only top level. Sub-GHz/RFID/NFC/IR/GPIO/BadUSB/
     * Games/Tools become SD apps installed from the qM Studio App Catalog. */
    "Main Menu", NULL, NULL, NULL, 4, 0, NULL, NULL,
    {&menu_Wifi, &menu_Bluetooth, &menu_Apps, &menu_Settings}
#elif defined(M1_APP_BADUSB_ENABLE) && defined(M1_APP_GAMES_ENABLE) && defined(M1_APP_APPS_ENABLE)
    "Main Menu", NULL, NULL, NULL, 12, 0, NULL, NULL,
    {&menu_Sub_GHz, &menu_125KHz_RFID, &menu_NFC, &menu_Infrared, &menu_GPIO, &menu_Wifi, &menu_Bluetooth, &menu_BadUSB, &menu_Games, &menu_Apps, &menu_Tools, &menu_Settings}
#elif defined(M1_APP_BADUSB_ENABLE)
    "Main Menu", NULL, NULL, NULL, 10, 0, NULL, NULL,
    {&menu_Sub_GHz, &menu_125KHz_RFID, &menu_NFC, &menu_Infrared, &menu_GPIO, &menu_Wifi, &menu_Bluetooth, &menu_BadUSB, &menu_Tools, &menu_Settings}
#elif defined(M1_APP_GAMES_ENABLE) && defined(M1_APP_APPS_ENABLE)
    "Main Menu", NULL, NULL, NULL, 11, 0, NULL, NULL,
    {&menu_Sub_GHz, &menu_125KHz_RFID, &menu_NFC, &menu_Infrared, &menu_GPIO, &menu_Wifi, &menu_Bluetooth, &menu_Games, &menu_Apps, &menu_Tools, &menu_Settings}
#else
    "Main Menu", NULL, NULL, NULL, 9, 0, NULL, NULL,
    {&menu_Sub_GHz, &menu_125KHz_RFID, &menu_NFC, &menu_Infrared, &menu_GPIO, &menu_Wifi, &menu_Bluetooth, &menu_Tools, &menu_Settings}
#endif
};


/***************************** V A R I A B L E S ******************************/

static S_M1_Menu_Control_t		menu_ctl;
static const S_M1_Menu_t 		*pthis_submenu;
TaskHandle_t					subfunc_handler_task_hdl;
TaskHandle_t					menu_main_handler_task_hdl;

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static void menu_main_init(void);
void menu_main_handler_task(void *param);
void subfunc_handler_task(void *param);

/* Per-leaf "reserved" flag captured at dispatch: 1 = the activity manages its own
 * orientation (skip the auto force-landscape wrap); 0 = wrap it. */
static uint8_t s_subfunc_self_orient = 0;

/*---------------- Modular menu layout (data-driven from 0:/System/menu.cfg) ----------------*/
/*
 * The top level is built at boot from a flat file the qMonstatek Studio "Menu
 * Customization" view rewrites on every save. Format (regenerated whole each time):
 *
 *   MENU settings              # top-level order = MENU-line order (any arrangement)
 *   MENU wifi     sort=custom      # APP-line order
 *   MENU apps     sort=alpha-desc  # Z-A
 *     APP wifi_scanner.m1app
 *
 * Acts on the full format: top-level ORDER, per-menu app SORT (custom=APP-line
 * order with unlisted apps appended A-Z; alpha=A-Z; alpha-desc=Z-A), and APP
 * placement lines (which
 * override an app's manifest tag, so apps can be moved between folders). A newly
 * installed app with no APP line yet still lands in its manifest-tag folder, so it
 * is reachable immediately. Any missing/corrupt file, or categories not listed,
 * fall back to the default legacy order so nothing is ever hidden or unreachable.
 */

typedef struct {
    const char    *key;      /* token used in menu.cfg (MENU <key>)          */
    const char    *title;    /* legacy display name                          */
    const uint8_t *icon;     /* legacy icon                                  */
    S_M1_Menu_t   *builtin;  /* backing built-in menu, or NULL = app-folder  */
} menu_cat_t;

/* Default legacy order. Built-ins (WiFi/BT/Apps/Settings) keep their own entries;
 * the rest are app-only category folders (empty -> "install an app" message). */
static const menu_cat_t k_cats[] = {
    { "wifi",      "Wifi",      menu_m1_icon_wifi,      &menu_Wifi      },
    { "bluetooth", "Bluetooth", menu_m1_icon_bluetooth, &menu_Bluetooth },
    { "subghz",    "Sub-GHz",   menu_m1_icon_wave,      NULL            },
    { "rfid",      "RFID",      menu_m1_icon_rfid,      NULL            },
    { "nfc",       "NFC",       menu_m1_icon_nfc,       NULL            },
    { "ir",        "Infrared",  menu_m1_icon_infrared,  NULL            },
    { "gpio",      "GPIO",      menu_m1_icon_gpio,      NULL            },
    { "games",     "Games",     menu_m1_icon_games,     NULL            },
    { "apps",      "Apps",      menu_m1_icon_apps,      NULL            },
    { "tools",     "Tools",     menu_m1_icon_gpio,      NULL            },
    { "settings",  "Settings",  menu_m1_icon_setting,   &menu_Settings  },
};
#define MENU_CAT_COUNT ((int)(sizeof(k_cats) / sizeof(k_cats[0])))

static S_M1_Menu_t s_main_dyn;                     /* runtime-built top level */
static S_M1_Menu_t s_appfolder[MENU_CAT_COUNT];    /* RAM folders for app-only cats */

/* Empty app-folder leaf: shown when a category has no apps installed yet. */
static void menu_layout_empty_folder(void)
{
    for (;;) {
        m1_u8g2_firstpage();
        u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
        u8g2_DrawStr(&m1_u8g2, 4, 16, "No apps here yet.");
        u8g2_DrawStr(&m1_u8g2, 4, 32, "Install one from");
        u8g2_DrawStr(&m1_u8g2, 4, 46, "qMonstatek Studio.");
        m1_u8g2_nextpage();

        S_M1_Main_Q_t q_item;
        S_M1_Buttons_Status btn;
        if (xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY) == pdTRUE &&
            q_item.q_evt_type == Q_EVENT_KEYPAD) {
            xQueueReceive(button_events_q_hdl, &btn, 0);
            if (btn.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK ||
                btn.event[BUTTON_OK_KP_ID]   == BUTTON_EVENT_CLICK) {
                xQueueReset(main_q_hdl);
                return;
            }
        }
    }
}

/* ---- App launch entries: each is a leaf whose sub_func runs a specific .m1app ---- */
#define MENU_MAX_APP_ENTRIES M1_APPS_MAX_FILES
#define MENU_APP_PATH_LEN    128
#define MENU_CFG_MAX_BYTES   4096

static struct {
    S_M1_Menu_t entry;
    char        path[MENU_APP_PATH_LEN];
} s_app_entries[MENU_MAX_APP_ENTRIES];
static int s_app_entry_used;

/* Set by the menu handler at leaf dispatch so the generic launcher knows which
 * app entry was chosen (sub_func takes no args). */
const S_M1_Menu_t *s_dispatched_leaf;

/* Generic app-launch sub_func: run the .m1app bound to the dispatched leaf. */
static void menu_launch_app(void)
{
    for (int i = 0; i < s_app_entry_used; i++)
        if (&s_app_entries[i].entry == s_dispatched_leaf) {
            m1_apps_run(s_app_entries[i].path);
            return;
        }
}

/* Make a leaf entry that launches the given app; NULL if the pool is full. */
static S_M1_Menu_t *menu_make_app_entry(const char *path, const char *name)
{
    if (s_app_entry_used >= MENU_MAX_APP_ENTRIES) return NULL;
    S_M1_Menu_t *e = &s_app_entries[s_app_entry_used].entry;
    memset(e, 0, sizeof(*e));
    strncpy(e->title, name, MENU_TITLE_LEN - 1);
    e->sub_func = menu_launch_app;      /* leaf: num_submenu_items stays 0 */
    strncpy(s_app_entries[s_app_entry_used].path, path, MENU_APP_PATH_LEN - 1);
    s_app_entry_used++;
    return e;
}

/* Manifest category key -> k_cats index, or -1 if empty/unknown. */
static int menu_cat_index(const char *key)
{
    if (!key || !key[0]) return -1;
    for (int c = 0; c < MENU_CAT_COUNT; c++)
        if (strcmp(key, k_cats[c].key) == 0) return c;
    return -1;
}

/* Parsed 0:/System/menu.cfg: top-level order, per-category sort mode, and the
 * explicit APP placement/order lines (which override an app's manifest tag). */
typedef struct {
    int  order[MENU_CAT_COUNT];
    int  n_order;
    bool sort_custom[MENU_CAT_COUNT];
    bool sort_alpha_desc[MENU_CAT_COUNT];
    struct { int cat; char file[40]; } placed[MENU_MAX_APP_ENTRIES];
    int  n_placed;
} menu_cfg_t;

/* Parse the whole menu.cfg into cfg. Returns n_order (0 => no/corrupt file =>
 * caller uses the default legacy order). Tolerant of unknown keys, stray APP
 * lines, and overflow (safely clamped). */
static int menu_cfg_parse(menu_cfg_t *cfg)
{
    FIL fh;
    static char buf[MENU_CFG_MAX_BYTES];
    UINT br = 0;

    memset(cfg, 0, sizeof(*cfg));
    if (f_open(&fh, "0:/System/menu.cfg", FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return 0;
    if (f_read(&fh, buf, sizeof(buf) - 1, &br) != FR_OK) { f_close(&fh); return 0; }
    f_close(&fh);
    buf[br] = 0;

    int cur_cat = -1;
    char *line = buf;
    while (line < buf + br) {
        char *eol = line;
        while (eol < buf + br && *eol != '\n' && *eol != '\r') eol++;
        char saved = *eol; *eol = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "MENU ", 5) == 0) {
            char *key = p + 5;
            while (*key == ' ' || *key == '\t') key++;
            char *ke = key;
            while (*ke && *ke != ' ' && *ke != '\t') ke++;
            char ksaved = *ke; *ke = 0;
            int ci = menu_cat_index(key);
            *ke = ksaved;                 /* restore so the sort= scan sees the rest */
            cur_cat = ci;
            if (ci >= 0) {
                int dup = 0;
                for (int j = 0; j < cfg->n_order; j++) if (cfg->order[j] == ci) dup = 1;
                if (!dup && cfg->n_order < MENU_CAT_COUNT) cfg->order[cfg->n_order++] = ci;
                if (strstr(ke, "sort=custom")) cfg->sort_custom[ci] = true;
                else if (strstr(ke, "sort=alpha-desc")) cfg->sort_alpha_desc[ci] = true;
            }
        } else if (strncmp(p, "APP ", 4) == 0 && cur_cat >= 0 &&
                   cfg->n_placed < MENU_MAX_APP_ENTRIES) {
            char *fn = p + 4;
            while (*fn == ' ' || *fn == '\t') fn++;
            char *fe = fn;
            while (*fe && *fe != ' ' && *fe != '\t' && *fe != '\r') fe++;
            *fe = 0;
            if (*fn) {
                cfg->placed[cfg->n_placed].cat = cur_cat;
                strncpy(cfg->placed[cfg->n_placed].file, fn,
                        sizeof(cfg->placed[0].file) - 1);
                cfg->n_placed++;
            }
        }
        *eol = saved;
        line = (eol < buf + br) ? eol + 1 : buf + br;
    }
    return cfg->n_order;
}

/* Custom-order comparator: A sorts AFTER B when this returns true. cfg-listed
 * apps (seq>=0) come first in listed order; unlisted apps follow, alpha by name. */
static bool menu_app_order_gt(int seqA, const char *nA, int seqB, const char *nB)
{
    bool la = (seqA >= 0), lb = (seqB >= 0);
    if (la && lb) return seqA > seqB;
    if (la != lb) return !la;             /* listed is "smaller" */
    return strcmp(nA, nB) > 0;
}

/* Build the runtime top-level from the menu.cfg order, injecting installed apps
 * into their category folder by manifest tag (untagged/unknown -> Apps), sorted
 * A-Z. Built-in menus (WiFi/BT/Settings) keep their own entries and get their
 * tagged apps appended. Any failure falls back to default order, nothing hidden. */
const S_M1_Menu_t *menu_layout_build(void)
{
    static menu_cfg_t cfg;
    int  n     = menu_cfg_parse(&cfg);      /* 0 => no/corrupt file */
    int *order = cfg.order;

    bool seen[MENU_CAT_COUNT];
    for (int c = 0; c < MENU_CAT_COUNT; c++) seen[c] = false;
    for (int i = 0; i < n; i++) seen[order[i]] = true;
    for (int c = 0; c < MENU_CAT_COUNT; c++)
        if (!seen[c] && n < MENU_CAT_COUNT) order[n++] = c;

    /* Scan apps; resolve each to a category: an explicit APP placement line in
     * menu.cfg wins (matched by filename), else the manifest tag, else Apps. A
     * newly installed app with no APP line yet still lands in its tag folder, so
     * it is reachable immediately (self-healing). */
    const int apps_cat = menu_cat_index("apps");
    int app_count = m1_apps_scan();
    if (app_count > MENU_MAX_APP_ENTRIES) app_count = MENU_MAX_APP_ENTRIES;

    static char a_path[MENU_MAX_APP_ENTRIES][MENU_APP_PATH_LEN];
    static char a_name[MENU_MAX_APP_ENTRIES][MENU_TITLE_LEN];
    static int  a_cat[MENU_MAX_APP_ENTRIES];
    static int  a_seq[MENU_MAX_APP_ENTRIES];   /* cfg placement index, -1 = unlisted */
    for (int i = 0; i < app_count; i++) {
        char cat[16] = {0};
        m1_apps_get(i, a_path[i], MENU_APP_PATH_LEN, a_name[i], MENU_TITLE_LEN,
                    cat, sizeof(cat));
        const char *base = strrchr(a_path[i], '/');
        base = base ? base + 1 : a_path[i];
        int ci = -1, seq = -1;
        for (int pl = 0; pl < cfg.n_placed; pl++)
            if (strcmp(cfg.placed[pl].file, base) == 0) {
                ci = cfg.placed[pl].cat; seq = pl; break;
            }
        if (ci < 0) { int t = menu_cat_index(cat); ci = (t >= 0) ? t : apps_cat; }
        a_cat[i] = ci;
        a_seq[i] = seq;
    }

    s_app_entry_used = 0;
    memset(&s_main_dyn, 0, sizeof(s_main_dyn));
    strncpy(s_main_dyn.title, "Main Menu", MENU_TITLE_LEN - 1);

    int items = 0;
    for (int i = 0; i < n && items < SUB_MENU_ITEMS_MAX; i++) {
        int ci = order[i];
        const menu_cat_t *cat = &k_cats[ci];
        S_M1_Menu_t   *folder = &s_appfolder[ci];
        int used;

        if (cat->builtin) {
            *folder = *cat->builtin;              /* copy entries + init fn + icon */
            used = folder->num_submenu_items;
        } else {
            memset(folder, 0, sizeof(*folder));
            strncpy(folder->title, cat->title, MENU_TITLE_LEN - 1);
            folder->icon_ptr = cat->icon;
            used = 0;
        }

        /* Collect this category's apps, then order them: custom (cfg APP order,
         * unlisted appended A-Z), A-Z, or Z-A according to the saved layout. */
        int idxs[MENU_MAX_APP_ENTRIES], na = 0;
        for (int a = 0; a < app_count; a++)
            if (a_cat[a] == ci) idxs[na++] = a;

        bool custom = cfg.sort_custom[ci];
        for (int x = 1; x < na; x++) {           /* insertion sort */
            int key = idxs[x], y = x - 1;
            if (custom) {
                while (y >= 0 && menu_app_order_gt(a_seq[idxs[y]], a_name[idxs[y]],
                                                   a_seq[key], a_name[key]))
                    { idxs[y + 1] = idxs[y]; y--; }
            } else {
                while (y >= 0) {
                    int cmp = strcmp(a_name[idxs[y]], a_name[key]);
                    bool after = cfg.sort_alpha_desc[ci] ? (cmp < 0) : (cmp > 0);
                    if (!after) break;
                    idxs[y + 1] = idxs[y];
                    y--;
                }
            }
            idxs[y + 1] = key;
        }
        for (int k = 0; k < na && used < SUB_MENU_ITEMS_MAX; k++) {
            S_M1_Menu_t *e = menu_make_app_entry(a_path[idxs[k]], a_name[idxs[k]]);
            if (!e) break;
            folder->submenu[used++] = e;
        }
        folder->num_submenu_items = (uint8_t)used;

        /* Empty app-only folder -> show the "install an app" leaf message. */
        if (!cat->builtin && used == 0)
            folder->sub_func = menu_layout_empty_folder;

        s_main_dyn.submenu[items++] = folder;
    }
    s_main_dyn.num_submenu_items = (uint8_t)items;
    return &s_main_dyn;
}

/* Set (from the RPC/file task) when 0:/apps changes — the menu handler rebuilds
 * the top level at its next main-menu idle tick, so a newly installed app is
 * usable without a reboot. */
static volatile bool s_apps_dirty = false;
void m1_menu_apps_changed(void) { s_apps_dirty = true; }

/* Re-scan apps + rebuild the top level in place. Call ONLY at the main menu
 * (level 0) so no submenu is mid-navigation; refresh the count + clamp selection. */
void menu_layout_rebuild(void)
{
    menu_layout_build();                       /* refills s_main_dyn in place */
    menu_ctl.num_menu_items = s_main_dyn.num_submenu_items;
    if (menu_ctl.menu_item_active >= menu_ctl.num_menu_items)
        menu_ctl.menu_item_active = menu_ctl.num_menu_items ? (menu_ctl.num_menu_items - 1) : 0;
}

/*************** F U N C T I O N   I M P L E M E N T A T I O N ****************/

/*============================================================================*/
/*
 * This function initializes the main menu.
*/
/*============================================================================*/
static void menu_main_init(void)
{
    menu_ctl.menu_level = 0; // main menu
    menu_ctl.menu_item_active = 0; // first menu item
    menu_ctl.last_selected_items[0] = 0; // last selected item is the current active item
    /* Build the top level from 0:/System/menu.cfg (legacy default order + folders
     * if the file is missing/corrupt; nothing hidden). Replaces the const menu_Main. */
    menu_ctl.main_menu_ptr[0] = menu_layout_build();
    menu_ctl.num_menu_items = menu_ctl.main_menu_ptr[0]->num_submenu_items;

    assert(menu_ctl.num_menu_items >= 3);

	pthis_submenu = menu_ctl.main_menu_ptr[menu_ctl.menu_level];
    menu_ctl.this_func = pthis_submenu->sub_func;

    m1_gui_init();

} // static void menu_main_init(void)



/*============================================================================*/
/*
 * This function handles all tasks of the M1 main menu.
*/
/*============================================================================*/
void menu_main_handler_task(void *param)
{
	uint8_t key, sel_item, n_items;
	uint8_t menu_update_stat;
	S_M1_Buttons_Status this_button_status;
	S_M1_Main_Q_t q_item;
	BaseType_t ret;
	/* Last battery state drawn on the main menu, so the periodic refresh only
	 * redraws when the % or plugged state actually changes. */
	uint8_t batt_lvl_shown = 0xFF, batt_plugged_shown = 0xFF;

	settings_load_from_sd();  /* Load southpaw and other user settings (needs stack > sys_init) */

	if (m1_esp32_auto_init)
	{
		m1_esp32_init();
		esp32_main_init();
	}

	/* Auto-connect to the primary WiFi network if the user enabled it. No-op
	 * unless the toggle is on and a primary network is set; brings the ESP up
	 * itself if ESP-at-boot was off. */
	wifi_boot_autoconnect();

	/* Apply the Bluetooth Direct toggle: tell the ESP to advertise its NUS RPC
	 * service so a phone can connect over BLE. Only when the ESP is already up. */
	if (m1_ble_direct && m1_esp32_get_init_status())
		m1_esp_client_ble_direct(true);

	/* Apply the WiFi Hotspot (SoftAP) toggle: bring the AP back up at boot. */
	if (m1_hotspot_on && m1_esp32_get_init_status())
		m1_esp_client_softap_start(m1_hotspot_ssid, m1_hotspot_pass, 1);

	vTaskDelay(POWER_UP_SYS_CONFIG_WAIT_TIME); // Give some time to startup_config_handler() during power-up
	while(1)
	{
		menu_update_stat = MENU_UPDATE_NONE;
		ret = xQueueReceive(main_q_hdl, &q_item, pdMS_TO_TICKS(1000));
		if ( ret!=pdTRUE )
		{
			/* Apps installed/removed since the menu was built? Rebuild the top
			 * level in place so a new app is usable without a reboot — only while
			 * idle at the main menu, so no submenu is mid-navigation. */
			if ( s_apps_dirty
			     && m1_device_stat.op_mode == M1_OPERATION_MODE_MENU_ON
			     && menu_ctl.menu_level == 0 )
			{
				s_apps_dirty = false;
				menu_layout_rebuild();
				m1_gui_menu_update(pthis_submenu, menu_ctl.menu_item_active, MENU_UPDATE_RESET);
			}
			/* Queue-wait timeout (no button). Live-refresh the battery indicator
			 * on the main menu so charge % / plugged state track without a keypress.
			 * Redraw only when it changed, and only on the top-level menu (the only
			 * screen drawn here that shows the icon). All drawing stays in this task,
			 * so there's no race with the periodic battery task (which only updates
			 * the cached value, never the display). */
			uint8_t plugged = (m1_batt_stat != 0) ? 1 : 0;
			if ( m1_batt_level != batt_lvl_shown || plugged != batt_plugged_shown )
			{
				if ( m1_device_stat.op_mode == M1_OPERATION_MODE_MENU_ON
				     && menu_ctl.menu_level == 0 )
				{
					batt_lvl_shown = m1_batt_level;
					batt_plugged_shown = plugged;
					m1_gui_menu_update(pthis_submenu, menu_ctl.menu_item_active, MENU_UPDATE_REFRESH);
				}
				else if ( m1_device_stat.op_mode == M1_OPERATION_MODE_DISPLAY_ON )
				{
					/* Home/version screen: side-effect-free redraw so the battery
					 * stays current without touching the backlight or sleep timer.
					 * While the screen saver has the backlight off this is an
					 * invisible buffer update; on wake the value is already fresh. */
					batt_lvl_shown = m1_batt_level;
					batt_plugged_shown = plugged;
					startup_info_screen_draw("");
				}
			}
			continue;
		}
		if ( q_item.q_evt_type!=Q_EVENT_KEYPAD )
			continue;
		// Notification is only sent to this task when there's any button activity,
		// so it doesn't need to wait when reading the event from the queue
		ret = xQueueReceive(button_events_q_hdl, &this_button_status, 0);
		if ( ret!=pdTRUE ) // This should never happen!
			continue; // Wait for a new notification when the attempt to read the button event fails

		for (key=0; key<NUM_BUTTONS_MAX; key++)
	    {
	    	if ( this_button_status.event[key]!=BUTTON_EVENT_IDLE )
	        {
	    		switch( key )
	    		{
	    			case BUTTON_OK_KP_ID:
	    				if ( this_button_status.event[BUTTON_OK_KP_ID]==BUTTON_EVENT_CLICK )
	    				{
	    					if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
	    					{
	                            n_items = pthis_submenu->submenu[menu_ctl.menu_item_active]->num_submenu_items;  // get the number of submenu items of the selected item
	                            menu_ctl.last_selected_items[menu_ctl.menu_level] = menu_ctl.menu_item_active; // save the selected item before going the next menu level
	                            if ( n_items != 0 ) // This menu item has another submenu?
	                            {
	                                menu_ctl.menu_level++; // go to next menu level
	                                menu_ctl.main_menu_ptr[menu_ctl.menu_level] = pthis_submenu->submenu[menu_ctl.menu_item_active];
	                                pthis_submenu = menu_ctl.main_menu_ptr[menu_ctl.menu_level];
	                                if ( pthis_submenu->reserved == 2 ) m1_orient_enter_landscape();
	                                menu_ctl.this_func = pthis_submenu->sub_func;
	                            	menu_ctl.num_menu_items = n_items; // update this field
	                                menu_ctl.menu_item_active = 0; // default for new submenu
	                                sel_item = 0;
	                                if ( menu_ctl.this_func != NULL )
	                                {
	                                    menu_ctl.this_func(); // run the function of the selected submenu item to initialize it
	                                    // This function should complete quickly after initializing the display!!!
	                                } // if ( menu_ctl.this_func != NULL )
	                                menu_update_stat = MENU_UPDATE_RESET;
	                            } // if ( n_items != 0 )

	                            else if ( pthis_submenu->submenu[menu_ctl.menu_item_active]->sub_func != NULL ) // This menu item has no submenu. Does it have a function to run?
	                            {
	                                m1_device_stat.op_mode = M1_OPERATION_MODE_SUB_FUNC_RUNNING;
	                                s_dispatched_leaf = pthis_submenu->submenu[menu_ctl.menu_item_active]; // capture WHICH leaf (app-launch entries need it)
	                                m1_device_stat.sub_func = pthis_submenu->submenu[menu_ctl.menu_item_active]->sub_func; // let schedule to run the function of the selected submenu item
	                                s_subfunc_self_orient = pthis_submenu->submenu[menu_ctl.menu_item_active]->reserved; // 1 = self-managed orientation
	                                //this_button_status.event[key].event = BUTTON_EVENT_IDLE; // clear before return
	                                // Notify the sub-function handler
	                                xTaskNotify(subfunc_handler_task_hdl, 0, eNoAction);
	                                // Wait for the sub-function to complete and notify this task from subfunc_handler_task
	                                xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
	                        		m1_device_stat.op_mode = M1_OPERATION_MODE_MENU_ON;
	                                // Return from sub-function. Let update GUI.
	                                sel_item = menu_ctl.menu_item_active;
	                                menu_update_stat = MENU_UPDATE_REFRESH; // The sub-function may have changed the GUI. It needs update.
	                            } // else if ( menu_ctl.this_function != NULL )

	                            else // This case should never happen. It doesn't exist!
	                            {
	                            	assert(("num_menu_items=0, this_func=NULL", FALSE));
	                            }
	                            key = NUM_BUTTONS_MAX; // Exit condition to stop checking other buttons!
	    					} // if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
	    					else if ( m1_device_stat.op_mode==M1_OPERATION_MODE_DISPLAY_ON )
	    					{
	    						menu_main_init();
	    						sel_item = 0;
	    						menu_update_stat = MENU_UPDATE_INIT;
	    						m1_device_stat.op_mode = M1_OPERATION_MODE_MENU_ON; // update new state
	    					} // else if ( m1_device_stat.op_mode==M1_OPERATION_MODE_DISPLAY_ON )
#ifdef BUTTON_REPEATED_PRESS_ENABLE
	    					else if ( m1_device_stat.op_mode==M1_OPERATION_MODE_POWER_UP )
	    					{
	    						m1_device_stat.op_mode = M1_OPERATION_MODE_DISPLAY_ON; // update new state
	    						m1_gui_welcome_scr();
	    						; // Change settings/config to exit out of shutdown/sleep state
	    					} // if ( m1_device_stat.op_mode==M1_OPERATION_MODE_POWER_UP )
#endif // #ifdef BUTTON_REPEATED_PRESS_ENABLE
	    				} // if ( this_button_status.event[BUTTON_OK_KP_ID]==BUTTON_EVENT_CLICK )
#ifndef BUTTON_REPEATED_PRESS_ENABLE
	    				else if ( this_button_status.event[BUTTON_OK_KP_ID]==BUTTON_EVENT_LCLICK )
	    				{
	    					if ( m1_device_stat.op_mode==M1_OPERATION_MODE_POWER_UP )
	    					{
	    						m1_device_stat.op_mode = M1_OPERATION_MODE_DISPLAY_ON; // update new state
	    						m1_gui_welcome_scr();
	    						; // Change settings/config to exit out of shutdown/sleep state
	    					} // if ( m1_device_stat.op_mode==M1_OPERATION_MODE_POWER_UP )
	    					else if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
	    					{
	    						//m1_device_stat.op_mode = OPERATION_MODE_SHUTDOWN; // force to sleep mode immediately
	    						//System_Shutdown();
	    					} // else if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
	    				} // if ( this_button_status.event[BUTTON_OK_KP_ID]==BUTTON_EVENT_LCLICK )
#endif // #ifndef BUTTON_REPEATED_PRESS_ENABLE
	                    break;

	    			case BUTTON_UP_KP_ID:
						if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						{
							if ( this_button_status.event[BUTTON_DOWN_KP_ID]==BUTTON_EVENT_CLICK ) // UP and DOWN pressed at the same time?
								break; // Do nothing
							sel_item = menu_ctl.menu_item_active; // take the current active menu item
							if ( sel_item==0 ) // first menu item?
							{
								sel_item = menu_ctl.num_menu_items - 1; // move to last menu item
								//menu_ctl.total_menu_items = menu_ctl.main_menu_ptr[menu_ctl.menu_level]->submenu_items;
							}
							else // not the first item
							{
								sel_item--;
							}
							menu_ctl.menu_item_active = sel_item; // update the active index
							menu_update_stat = MENU_UPDATE_MOVE_UP;
						} // if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						else
						{
							; // Do something here if necessary. This case may never happen!
						}
	    				break;

	    			case BUTTON_DOWN_KP_ID:
						if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						{
							if ( this_button_status.event[BUTTON_UP_KP_ID]==BUTTON_EVENT_CLICK ) // UP and DOWN pressed at the same time?
								break; // Do nothing
	                        sel_item = menu_ctl.menu_item_active; // take the current active menu item
	                        if ( sel_item==(menu_ctl.num_menu_items - 1) ) // last menu item?
	                        {
	                        	sel_item = 0; // move to first menu item
	                        }
	                        else // not the last item
	                        {
	                        	sel_item++;
	                        }
	                        menu_ctl.menu_item_active = sel_item; // update the active index
	                        menu_update_stat = MENU_UPDATE_MOVE_DOWN;
						}
						else
						{
							; // Do something here if necessary. This case may never happen!
							if ( this_button_status.event[BUTTON_DOWN_KP_ID]==BUTTON_EVENT_LCLICK )
							{
								m1_buzzer_notification();
							}
						}
	    				break;

	    			case BUTTON_LEFT_KP_ID:
						if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						{
							if ( pthis_submenu->xkey_handler )
								pthis_submenu->xkey_handler(this_button_status.event[BUTTON_LEFT_KP_ID], BUTTON_LEFT_KP_ID, sel_item);
						}
						else if ( m1_device_stat.op_mode==M1_OPERATION_MODE_DISPLAY_ON )
						{
							storage_explore();
							m1_gui_welcome_scr();
						}
	    				break;

	    			case BUTTON_RIGHT_KP_ID:
						if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						{
							if ( pthis_submenu->xkey_handler )
								pthis_submenu->xkey_handler(this_button_status.event[BUTTON_RIGHT_KP_ID], BUTTON_RIGHT_KP_ID, sel_item);
						}
						else
						{
							; // Do something here if necessary. This case may never happen!
						}
	    				break;

	    			case BUTTON_BACK_KP_ID:
						if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						{
							if ( menu_update_stat!=MENU_UPDATE_NONE ) // Other buttons pressed?
								break; // Do nothing, let other buttons take their higher priority!
							if ( menu_ctl.menu_level==0 ) // already at main menu screen?
							{
	    						m1_device_stat.op_mode = M1_OPERATION_MODE_DISPLAY_ON; // update new state
	    						m1_gui_welcome_scr();
								; // Do something before going to default home screen
								//
							} // if ( menu_ctl.menu_level==0 )
							else
							{
								if ( menu_ctl.num_menu_items ) // Submenu with active items?
								{
									if ( pthis_submenu->deinit_func )
										pthis_submenu->deinit_func(); // Run deinit function of this submenu before leaving
								} // if ( menu_ctl.num_menu_items )
								if ( pthis_submenu->reserved == 2 ) m1_orient_restore();
								menu_ctl.menu_level--; // go back one level
								menu_ctl.menu_item_active = menu_ctl.last_selected_items[menu_ctl.menu_level]; // restore  previous selected item of the upper menu level
								pthis_submenu = menu_ctl.main_menu_ptr[menu_ctl.menu_level]; // save the current menu level index
								menu_ctl.this_func = pthis_submenu->sub_func;
								menu_ctl.num_menu_items = pthis_submenu->num_submenu_items;
								n_items = menu_ctl.num_menu_items;
								sel_item = menu_ctl.menu_item_active;
								if ( menu_ctl.this_func != NULL )
								{
									menu_ctl.this_func(); // run the function of the selected submenu item to initialize it
									// It's not necessary to set the flag sub_func_is_running here.
									// This function should complete quickly after initializing the display!!!
								} // if ( menu_ctl.this_func != NULL )
								menu_update_stat = MENU_UPDATE_RESTORE;
							} // else
						} // if ( m1_device_stat.op_mode==M1_OPERATION_MODE_MENU_ON )
						else
						{
							; // Do something here if necessary. This case may never happen!
						}
	    				break;

	    			default: // undefined buttons, or buttons do not exist.
	    				break;
	    		} // switch( key )

	        } // if ( this_button_status.event[key]!=BUTTON_EVENT_IDLE )
	    } // for (key=0; key<NUM_BUTTONS_MAX; key++)

	    if ( menu_update_stat!=MENU_UPDATE_NONE )
	    	m1_gui_menu_update(pthis_submenu, sel_item, menu_update_stat);
	} // while(1)

} // void menu_main_handler_task(void *param)



/*============================================================================*/
/*
 * This task handles the execution of any sub-function
*/
/*============================================================================*/
void subfunc_handler_task(void *param)
{
	while(1)
	{
		// Waiting for notification from menu_main_handler_task,
		// or from button_event_handler_task
		xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
		assert(m1_device_stat.sub_func!=NULL);
		// Run the sub-function. Bespoke activity screens are landscape-only, so in
		// Portrait we flip to Normal for the duration and restore on return (no-op
		// in Normal/Southpaw, and skipped for leaves that manage their own orient).
		if (s_subfunc_self_orient != 1) m1_orient_enter_landscape();
		m1_device_stat.sub_func();
		if (s_subfunc_self_orient != 1) m1_orient_restore();
		// Sub-function completes, let notify menu_main_handler_task
		xTaskNotify(menu_main_handler_task_hdl, 0, eNoAction);
	} // while(1)
} // void subfunc_handler_task(void *param)
