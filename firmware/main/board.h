/* What the board glue in main.c shares with ble.c. */
#pragma once
#include <stdbool.h>

void ui_lock(void);   /* LVGL is not thread-safe: every touch of the UI from another task goes inside */
void ui_unlock(void);
extern int tz_min;    /* local time zone, minutes east of UTC; the iPhone sets it */

void ble_start(void); /* the iPhone link: ANCS notifications and calls, AMS music, CTS time */
void ble_quiet(bool quiet); /* every link down and no advertising (saving energy while asleep); false: back on air */
void batlog_dump(void (*out)(const char *)); /* the battery log as "@B seq uptime_s mV % flags lit%" rows, then "@B end" */
void batlog_clear(void);

void debug_start(void);
void settings_save(void);       /* power, features, brightness, privacy into NVS */
/* plip's switches, each can bring the original behaviour back: "still" skip unchanged face frames, "lsleep" let the
   chip sleep between frames while lit, "bt" the radio's slow pace. Kept in NVS with the settings. */
void plip_option(const char *name, bool on);
extern bool plip_lit_sleep;
extern bool plip_sense_save; /* "sense": gyroscope only for the remote, sensors 25/s while dark, mic only with the face lit */
extern bool plip_panel_sleep; /* "panel": the panel controller sleeps (SLPIN) while the screen is dark */
extern int plip_charge_code; /* AXP2101 charge current code (9 = 300 mA), 0: the chip's own */
void plip_charge_apply(void);
extern int plip_mode;    /* 0 Экономия, 1 Обычный (default), 2 Максимум (the original) */
extern bool plip_radio_off; /* plip: no phone - Bluetooth off entirely */
extern int plip_autoeco; /* % below which "Экономия" comes on by itself until the charger, 0 never */
void plip_set_mode(int m);
void plip_battery_watch(int pct, bool charging);
bool plip_autoeco_on(void);
extern bool plip_bt_save; /* "bt": links idle slowly, advertising fast only for 30 s after a drop, a boot or a wake (ble.c) */
void settings_bright(int pct);  /* the panel's full brightness, 10..100 */
int settings_brightness(void);
void debug_line(char *line);              /* a command line from the BLE debug link (queued for the debug task) */
bool ble_debug_on(void);
void ble_debug_send(const char *s, int n); /* to the BLE debug page */
 /* the USB debug link: status out, commands in (tools/pipdebug.py) */
