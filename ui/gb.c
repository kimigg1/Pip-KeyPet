#include "gb.h"
#include "gbjs.h"
#include "phone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINE_MAX_ 4096 /* a long notification in \u escapes; malloc'd (>= 2 KB lands in PSRAM on the board) */

void (*gb_on_find)(bool on);
void (*gb_on_weather)(void);
void (*gb_on_alarms)(void);

static struct {
    gb_host_t host;
    bool up, finding, playing, has_track;
    char *line;
    int len;
    bool overflow;
    gbjs_t *js;
    char track[64], artist[64];
    int dur, pos;
    uint32_t call_uid, calls;
    int bat;
    bool chg;
    gb_weather_t weather;
    gb_alarm_t alarms[GB_ALARMS];
    int nalarms;
} g = {.bat = -1};

void gb_start(const gb_host_t *host)
{
    g.host = *host;
    if (!g.line) g.line = malloc(LINE_MAX_);
    if (!g.js) g.js = malloc(sizeof *g.js);
}

bool gb_on(void) { return g.up; }

static void send(const char *s)
{
    if (g.up && g.host.send) g.host.send(s);
}

static void send_battery(void)
{
    if (g.bat < 0) return;
    char s[64];
    snprintf(s, sizeof s, "{\"t\":\"status\",\"bat\":%d,\"chg\":%d}\n", g.bat, g.chg ? 1 : 0);
    send(s);
}

void gb_link(bool up)
{
    if (up == g.up) return;
    g.up = up, g.len = 0, g.overflow = false;
    phone_link(up ? PHONE_ON : PHONE_OFF, 0);
    if (!up) {
        if (g.call_uid) phone_call(0, NULL, NULL), g.call_uid = 0;
        phone_track(NULL, NULL, 0, 0, false);
        return;
    }
    send("{\"t\":\"ver\",\"fw\":\"plip\",\"hw\":\"Pip\"}\n");
    send_battery();
}

static void local_hm(int *h, int *m)
{
    time_t t = time(NULL) + (g.host.tz_min ? g.host.tz_min() : 0) * 60;
    struct tm tm;
#ifdef _WIN32
    tm = *gmtime(&t);
#else
    gmtime_r(&t, &tm);
#endif
    *h = tm.tm_hour, *m = tm.tm_min;
}

static void show_track(void)
{
    phone_track(g.has_track ? g.track : NULL, g.artist, g.dur, (float)g.pos, g.playing);
}

static void copy(char *d, size_t n, const char *s)
{
    snprintf(d, n, "%s", s ? s : "");
}

/* one GB({...}) message */
static void message(int o)
{
    gbjs_t *j = g.js;
    const char *t = gbjs_str(j, o, "t", "");
    if (!strcmp(t, "notify")) {
        int h, m;
        local_hm(&h, &m);
        const char *src = gbjs_str(j, o, "src", ""), *title = gbjs_str(j, o, "title", NULL);
        const char *sender = gbjs_str(j, o, "sender", NULL), *subject = gbjs_str(j, o, "subject", NULL);
        const char *body = gbjs_str(j, o, "body", "");
        if (!title || !*title) title = sender && *sender ? sender : *src ? src : "Уведомление";
        char text[480];
        if (subject && *subject) snprintf(text, sizeof text, "%s\n%s", subject, body);
        else copy(text, sizeof text, body);
        phone_note((uint32_t)gbjs_num(j, o, "id", 0), *src ? src : "Android", title, text, h, m, false);
    } else if (!strcmp(t, "notify-")) {
        phone_note_gone((uint32_t)gbjs_num(j, o, "id", 0));
    } else if (!strcmp(t, "call")) {
        const char *cmd = gbjs_str(j, o, "cmd", "");
        if (!strcmp(cmd, "incoming")) {
            const char *name = gbjs_str(j, o, "name", ""), *num = gbjs_str(j, o, "number", "");
            g.call_uid = 0xCA110000u | (++g.calls & 0xFFFF);
            phone_call(g.call_uid, *name ? name : *num ? num : "Звонок", "входящий");
        } else if (g.call_uid) { /* accepted, rejected, started or over: the ringing stops */
            phone_call(0, NULL, NULL);
            g.call_uid = 0;
        }
    } else if (!strcmp(t, "musicinfo")) {
        copy(g.track, sizeof g.track, gbjs_str(j, o, "track", ""));
        copy(g.artist, sizeof g.artist, gbjs_str(j, o, "artist", ""));
        g.dur = (int)gbjs_num(j, o, "dur", 0), g.has_track = g.track[0] != 0;
        show_track();
    } else if (!strcmp(t, "musicstate")) {
        g.playing = !strcmp(gbjs_str(j, o, "state", ""), "play");
        g.pos = (int)gbjs_num(j, o, "position", 0);
        show_track();
    } else if (!strcmp(t, "find")) {
        g.finding = gbjs_bool(j, o, "n", false);
        if (gb_on_find) gb_on_find(g.finding);
    } else if (!strcmp(t, "weather")) {
        gb_weather_t *w = &g.weather;
        /* Gadgetbridge's temperatures are kelvin */
        w->temp_c = (int)(gbjs_num(j, o, "temp", 273) - 273), w->hi_c = (int)(gbjs_num(j, o, "hi", 273) - 273);
        w->lo_c = (int)(gbjs_num(j, o, "lo", 273) - 273), w->hum = (int)gbjs_num(j, o, "hum", -1);
        w->rain = (int)gbjs_num(j, o, "rain", -1), w->code = (int)gbjs_num(j, o, "code", 0);
        copy(w->txt, sizeof w->txt, gbjs_str(j, o, "txt", ""));
        copy(w->loc, sizeof w->loc, gbjs_str(j, o, "loc", ""));
        w->at = (int64_t)time(NULL), w->have = true;
        if (gb_on_weather) gb_on_weather();
    } else if (!strcmp(t, "alarm")) {
        int d = gbjs_get(j, o, "d");
        g.nalarms = 0;
        for (int c = d >= 0 ? j->n[d].child : -1; c >= 0 && g.nalarms < GB_ALARMS; c = j->n[c].next) {
            gb_alarm_t *a = &g.alarms[g.nalarms++];
            a->h = (uint8_t)gbjs_num(j, c, "h", 0), a->m = (uint8_t)gbjs_num(j, c, "m", 0);
            a->rep = (uint8_t)gbjs_num(j, c, "rep", 0), a->on = gbjs_bool(j, c, "on", true);
        }
        if (gb_on_alarms) gb_on_alarms();
    }
}

/* one line: GB({...}) messages, or the clock: setTime(1700000000);E.setTimeZone(3);... */
static void line(char *s, int n)
{
    while (n > 0 && (*s == 0x10 || *s == ' ')) s++, n--; /* DLE: "don't echo" on a real Espruino */
    if (n >= 3 && !memcmp(s, "GB(", 3)) {
        int o = gbjs_parse(g.js, s + 3, (size_t)(n - 3));
        if (o >= 0) message(o);
        return;
    }
    s[n] = 0;
    char *st = strstr(s, "setTime("), *tz = strstr(s, "setTimeZone(");
    if (st && g.host.set_time) {
        long long unix_s = strtoll(st + 8, NULL, 10);
        int zone = tz ? (int)(strtod(tz + 12, NULL) * 60 + (strtod(tz + 12, NULL) < 0 ? -0.5 : 0.5)) : (g.host.tz_min ? g.host.tz_min() : 0);
        g.host.set_time(unix_s, zone);
    }
}

void gb_feed(const char *data, int n)
{
    if (!g.line || !g.js) return;
    for (int i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!g.overflow) line(g.line, g.len);
            g.len = 0, g.overflow = false;
        } else if (g.len < LINE_MAX_ - 1) g.line[g.len++] = c;
        else g.overflow = true; /* too long to understand: drop the whole line */
    }
}

bool gb_command(int cmd, uint32_t uid)
{
    if (!g.up) return false;
    static const char *const music[] = {[PHONE_PLAY_PAUSE] = "playpause", [PHONE_NEXT] = "next", [PHONE_PREV] = "previous",
                                        [PHONE_VOL_UP] = "volumeup", [PHONE_VOL_DOWN] = "volumedown"};
    char s[96];
    switch (cmd) {
    case PHONE_PLAY_PAUSE:
    case PHONE_NEXT:
    case PHONE_PREV:
    case PHONE_VOL_UP:
    case PHONE_VOL_DOWN:
        if (cmd == PHONE_PLAY_PAUSE) g.playing = !g.playing, show_track(); /* the phone confirms with musicstate */
        snprintf(s, sizeof s, "{\"t\":\"music\",\"n\":\"%s\"}\n", music[cmd]);
        break;
    case PHONE_CALL_ANSWER:
    case PHONE_CALL_DECLINE:
        snprintf(s, sizeof s, "{\"t\":\"call\",\"n\":\"%s\"}\n", cmd == PHONE_CALL_ANSWER ? "ACCEPT" : "REJECT");
        if (g.call_uid) phone_call(0, NULL, NULL), g.call_uid = 0;
        break;
    case PHONE_NOTE_DISMISS:
        snprintf(s, sizeof s, "{\"t\":\"notify\",\"id\":%lu,\"n\":\"DISMISS\"}\n", (unsigned long)uid);
        break;
    default:
        return false; /* the camera's shutter and pairing are not Gadgetbridge's */
    }
    send(s);
    return true;
}

void gb_battery(int pct, bool charging)
{
    if (pct == g.bat && charging == g.chg) return;
    g.bat = pct, g.chg = charging;
    send_battery();
}

void gb_steps(int new_steps)
{
    if (new_steps <= 0) return;
    char s[64];
    snprintf(s, sizeof s, "{\"t\":\"act\",\"stp\":%d}\n", new_steps);
    send(s);
}

void gb_find_phone(bool ring)
{
    send(ring ? "{\"t\":\"findPhone\",\"n\":true}\n" : "{\"t\":\"findPhone\",\"n\":false}\n");
}

const gb_weather_t *gb_weather(void) { return &g.weather; }

int gb_alarms(const gb_alarm_t **out)
{
    *out = g.alarms;
    return g.nalarms;
}

bool gb_finding(void) { return g.finding; }

void gb_found(void)
{
    if (!g.finding) return;
    g.finding = false;
    if (gb_on_find) gb_on_find(false);
}
