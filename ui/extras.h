/* plip: the phone's extras on the keychain - find my device, the phone's alarms, the weather, find my phone */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

void extras_init(void);  /* after the shell: hooks into ui/gb.c and a 1 s timer */
bool extras_stop(void);  /* a button or a touch: stops "find my device" or a ringing alarm; true if it did */
bool extras_busy(void);  /* calling out or ringing */
void extras_findphone_open(lv_obj_t *scr); /* the "Найти телефон" app */
void extras_findphone_close(void);
