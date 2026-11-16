/* A pretend iPhone for the simulators: it feeds phone.h like the BLE link will, and answers the keychain's commands. */
#pragma once

void fake_phone_init(void);
/* kind: 'L' link (0 off, 1 pairing, 2 on), 'N' the next sample notification, 'C' call (1 rings, 0 hangs up),
   'M' music (1 plays the sample album, 0 stops) */
void fake_phone(char kind, int arg);
const char *fake_phone_last(void); /* the last command the keychain sent, "" if none */
int fake_phone_shots(void);        /* photos taken with the shutter */
void fake_host(int arg);              /* the fake computer: 0 gone, 1 asks to pair, 2 paired and back */
const char *fake_host_report(void); /* "host=on clicks=.. prevs=.. mx=.. my=..": what the remote sent it */
/* puglik: a pretend Android with Gadgetbridge (event q<n>). what: 0 gone; 1 a notification (Russian), 2 it is gone, 3 a call rings,
   4 it ends, 5 music plays, 6 weather, 7 an alarm, 8 "find my device", 9 setTime + zone, 10 an alarm at 12:01 (connects first if needed) */
void fake_gb(int what);
const char *fake_gb_report(void); /* "gb=1 gbsent=<last line Pip sent> gbn=.. gbclock=.. gbtz=.." */
