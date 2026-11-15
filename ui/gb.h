/* plip: Android through Gadgetbridge. To Gadgetbridge Pip is a "Bangle.js": it writes lines of JavaScript to Pip's
   Nordic UART (notifications, calls, music, the time, weather, alarms, "find my device"), and Pip answers with JSON
   lines (music keys, call answers, battery, steps, "find my phone"). This file is the protocol and feeds the same
   phone model (phone.h) the iPhone does; the host (firmware/main/ble.c, the simulator) moves the bytes. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void (*send)(const char *line);             /* one line to the phone (the host adds nothing) */
    void (*set_time)(int64_t unix_utc, int tz_min); /* the phone's clock: setTime() and E.setTimeZone() */
    int (*tz_min)(void);                         /* the local zone now, minutes east of UTC */
} gb_host_t;

void gb_start(const gb_host_t *host);
void gb_link(bool up);                 /* Gadgetbridge connected / gone */
bool gb_on(void);
void gb_feed(const char *data, int n); /* bytes the phone wrote, any chunking */
bool gb_command(int cmd, uint32_t uid); /* a PHONE_* command from the UI: true if it went to Gadgetbridge */

/* Pip -> phone */
void gb_battery(int pct, bool charging); /* sent when it changes */
void gb_steps(int new_steps);            /* steps since the last report, as Gadgetbridge activity */
void gb_find_phone(bool ring);

/* What the phone told us, for the clock, Pip and the apps */
typedef struct {
    bool have;
    int temp_c, hi_c, lo_c, hum, rain, code; /* code: OpenWeatherMap condition id */
    char txt[40], loc[40];
    int64_t at; /* unix time it came */
} gb_weather_t;
const gb_weather_t *gb_weather(void);

typedef struct { uint8_t h, m, rep; bool on; } gb_alarm_t; /* rep: weekday bits, Monday = 1 .. Sunday = 64; 0 once */
#define GB_ALARMS 8
int gb_alarms(const gb_alarm_t **out);

bool gb_finding(void);       /* the phone is looking for Pip ("find my device") */
void gb_found(void);         /* a touch or a button: stop */

/* plip's UI hooks it calls (set by whoever shows them; NULL: nothing) */
extern void (*gb_on_find)(bool on);
extern void (*gb_on_weather)(void);
extern void (*gb_on_alarms)(void);
