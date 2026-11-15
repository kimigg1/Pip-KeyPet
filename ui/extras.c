/* plip: what the phone's extras do on the keychain (Gadgetbridge, ui/gb.c): "find my device" makes Pip call out,
   the phone's alarms ring on Pip (also when the phone is away), the weather moves his mood, and "Найти телефон"
   makes the phone ring. Pure UI: the simulator runs it as it is. */
#include "extras.h"
#include "gb.h"
#include "moods.h"
#include "pip.h"
#include "shell.h"
#include "theme.h"
#include "voice.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RING_S 60 /* an alarm rings this long unless a button or a touch stops it */

static struct {
    bool finding;
    int ringing;      /* seconds the alarm still rings */
    int last_minute;  /* the minute last checked for alarms, -1 none */
    int weather_seen; /* the weather report last reacted to (its time) */
    int fired;        /* minute of the day an alarm last rang, so it rings once */
} x = {.last_minute = -1, .fired = -1};

static void on_find(bool on)
{
    x.finding = on;
    if (on) pip_wake();
}

/* OpenWeatherMap's condition codes: 2xx storm, 3xx drizzle, 5xx rain, 6xx snow, 800 clear */
static void on_weather(void)
{
    const gb_weather_t *w = gb_weather();
    if (!w->have || (int)w->at == x.weather_seen) return;
    x.weather_seen = (int)w->at;
    int g = w->code / 100;
    if (g == 2 || g == 3 || g == 5) pip_react(PIP_GLOOMY, 5, "rain");    /* the little rain cloud over him */
    else if (g == 6) pip_react(PIP_AMAZED, 4, "snow");
    else if (w->temp_c >= 30) pip_react(PIP_TIRED, 4, "hot");
    else if (w->temp_c <= -10) pip_react(PIP_NERVOUS, 3, "cold");
    else if (w->code == 800 && w->temp_c >= 15) pip_react(PIP_HAPPY, 3, "sunny");
}

static void ring_now(void)
{
    x.ringing = RING_S;
    shell_home();
    pip_wake();
}

static void check_alarms(void)
{
    time_t t = (time_t)shell_local_time();
    struct tm tm;
#ifdef _WIN32
    tm = *gmtime(&t);
#else
    gmtime_r(&t, &tm);
#endif
    int minute = tm.tm_hour * 60 + tm.tm_min;
    if (minute == x.last_minute) return;
    x.last_minute = minute;
    const gb_alarm_t *a;
    int n = gb_alarms(&a);
    int day = tm.tm_wday == 0 ? 64 : 1 << (tm.tm_wday - 1); /* Gadgetbridge: Monday 1 .. Sunday 64 */
    for (int i = 0; i < n; i++)
        if (a[i].on && a[i].h * 60 + a[i].m == minute && (!a[i].rep || (a[i].rep & day)) && x.fired != minute) {
            x.fired = minute;
            ring_now();
        }
}

static void pomo_tick(void);
static void tick(lv_timer_t *t)
{
    (void)t;
    check_alarms();
    pomo_tick();
    if (x.finding) { /* "find my device": every second a cry, a look, the screen on */
        pip_wake();
        pip_react(PIP_SURPRISED, 0.9f, "find");
        voice_say(VOICE_POKE);
    }
    if (x.ringing > 0) {
        if (x.ringing % 5 == 0) pip_wake(), pip_react(PIP_SURPRISED, 4, "time's up"); /* the timer's tune */
        x.ringing--;
    }
}

void extras_init(void)
{
    gb_on_find = on_find;
    gb_on_weather = on_weather;
    lv_timer_create(tick, 1000, NULL);
}

bool extras_stop(void)
{
    if (x.finding) { gb_found(); x.finding = false; return true; }
    if (x.ringing > 0) { x.ringing = 0; pip_react(PIP_SLEEPY, 2, "snooze"); return true; }
    return false;
}

bool extras_busy(void) { return x.finding || x.ringing > 0; }

bool extras_next_alarm(int *h, int *m)
{
    time_t t = (time_t)shell_local_time();
    struct tm tm;
#ifdef _WIN32
    tm = *gmtime(&t);
#else
    gmtime_r(&t, &tm);
#endif
    int now = tm.tm_hour * 60 + tm.tm_min, best = -1, best_d = 0;
    const gb_alarm_t *a;
    int n = gb_alarms(&a);
    for (int i = 0; i < n; i++) {
        if (!a[i].on) continue;
        int at = a[i].h * 60 + a[i].m, d = (at - now + 1440) % 1440; /* minutes until it, within a day */
        if (best < 0 || d < best_d) best = at, best_d = d;
    }
    if (best < 0) return false;
    *h = best / 60, *m = best % 60;
    return true;
}

/* "1234 шагов · 78% ~5 ч" and "-5° снег · 07:30": what is known, nothing that is not */
void extras_widgets(char *out, int n, int steps)
{
    char l1[64], l2[80] = "";
    int k = 0;
    if (shell_battery_minutes() >= 0) snprintf(l1, sizeof l1, "%d шаг. · %s %s", steps, shell_battery_text(), shell_battery_left());
    else snprintf(l1, sizeof l1, "%d шаг. · %s", steps, shell_battery_text());
    const gb_weather_t *w = gb_weather();
    if (w->have) k += snprintf(l2 + k, sizeof l2 - k, "%d° %s", w->temp_c, w->txt);
    int h, m;
    if (extras_next_alarm(&h, &m)) k += snprintf(l2 + k, sizeof l2 - k, "%s%02d:%02d", k ? " · " : "", h, m);
    snprintf(out, n, "%s%s%s", l1, l2[0] ? "\n" : "", l2);
}

/* ---------- the app: "Найти телефон" ---------- */
static lv_obj_t *fp_label;
void extras_findphone_open(lv_obj_t *scr)
{
    bool on = gb_on();
    fp_label = th_text(scr, th->body, on ? th->text : th->dim, on ? "Телефон звонит\n\nвыйди, чтобы остановить" : "Телефон не подключён\n\nнужен Gadgetbridge");
    lv_obj_set_style_text_align(fp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(fp_label);
    if (on) gb_find_phone(true);
}

void extras_findphone_close(void)
{
    gb_find_phone(false);
    fp_label = NULL;
}

/* ---------- the app: "Помодоро" ---------- */
#define WORK_S (25 * 60)
#define REST_S (5 * 60)
static struct {
    int phase; /* 0 idle, 1 work, 2 rest */
    bool paused;
    int left, rounds;
    lv_obj_t *time, *what;
} po;

static void pomo_show(void)
{
    if (!po.time) return;
    int l = po.phase ? po.left : WORK_S;
    lv_label_set_text_fmt(po.time, "%02d:%02d", l / 60, l % 60);
    static const char *const word[] = {"верхняя кнопка — старт", "работаем", "отдыхаем"};
    lv_label_set_text_fmt(po.what, "%s%s\nподходов: %d", word[po.phase], po.paused ? " (пауза)" : "", po.rounds);
}

static void pomo_tick(void)
{
    if (!po.phase || po.paused) return;
    if (--po.left > 0) { pomo_show(); return; }
    if (po.phase == 1) po.rounds++, po.phase = 2, po.left = REST_S; /* a round done: rest */
    else po.phase = 1, po.left = WORK_S;                           /* rested: back to work */
    if (!po.time) shell_home(); /* not looking at the app: Pip calls from his face */
    pip_wake();
    pip_react(po.phase == 2 ? PIP_HAPPY : PIP_CURIOUS, 4, "time's up"); /* the timer's tune, glad or eager */
    pomo_show();
}

void extras_pomodoro_open(lv_obj_t *scr)
{
    lv_obj_t *title = th_text(scr, th->small, th->dim, "Помодоро");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);
    po.time = th_text(scr, th->digits ? th->digits : th->body, th->text, "");
    lv_obj_align(po.time, LV_ALIGN_CENTER, 0, -20);
    po.what = th_text(scr, th->small, th->dim, "");
    lv_obj_set_style_text_align(po.what, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(po.what, LV_ALIGN_CENTER, 0, 70);
    pomo_show();
}

void extras_pomodoro_close(void) { po.time = po.what = NULL; }

bool extras_pomodoro_button(int id)
{
    if (id != PIP_BTN_PWR) return false;
    if (!po.phase) po.phase = 1, po.left = WORK_S, po.paused = false, pip_react(PIP_HAPPY, 2, "let's go");
    else po.paused = !po.paused;
    pomo_show();
    return true;
}

int extras_pomodoro_state(void) { return po.paused ? -po.phase : po.phase; }
