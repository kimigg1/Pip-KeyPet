/* plip: the phone's extras on the keychain - find my device, the phone's alarms, the weather, find my phone */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

void extras_init(void);  /* after the shell: hooks into ui/gb.c and a 1 s timer */
bool extras_stop(void);  /* a button or a touch: stops "find my device" or a ringing alarm; true if it did */
bool extras_busy(void);  /* calling out or ringing */
bool extras_next_alarm(int *h, int *m); /* the next enabled phone alarm (today's later ones first) */
void extras_widgets(char *out, int n, int steps); /* plip: the clock's complications, two lines */
void extras_findphone_open(lv_obj_t *scr); /* the "Найти телефон" app */
void extras_findphone_close(void);
/* the "Помодоро" app: 25 minutes of work, 5 of rest, round and round; the top button starts and pauses, it keeps
   counting with the app closed and Pip calls you at each turn */
void extras_pomodoro_open(lv_obj_t *scr);
void extras_pomodoro_close(void);
bool extras_pomodoro_button(int id);
int extras_pomodoro_state(void); /* 0 idle, 1 working, 2 resting; < 0 paused */
