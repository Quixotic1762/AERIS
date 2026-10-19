/*
 * aeris_app.h - top-level AERIS application, called from main.c.
 *
 * Bare-metal cooperative loop. Every module's poll() is non-blocking and
 * deadline-driven from the 1 ms SysTick, so acquisition keeps running no
 * matter what the ESP8266, Wi-Fi, broker or dashboard are doing.
 */
#ifndef AERIS_APP_H
#define AERIS_APP_H

void aeris_app_init(void);
void aeris_app_loop(void);

#endif /* AERIS_APP_H */
