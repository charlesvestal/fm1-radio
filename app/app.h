/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Radio: an internet radio for the FM-1, on fm1-base's WiFi. Radio Garden's places built in, a place's stations
 * fetched live, MP3 streams over HTTP or HTTPS, punch-in effects on the keys. This header is the app's half of the
 * contract with the platform (fm1-base: device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "RADIO"
#define APP_SPLASH_RGB RGB(38, 166, 154)
#define APP_BLOCK 256u
#define APP_CANVAS_PX (240u * 48u)       /* a row at a time: WiFi's buffers need the pool (fm1-base gfx.c) */
#define APP_BIG_FONT FONT_M             /* the splash and the panel's screens: L's 20 KB go to the places */
#define FM1B_TRS 0                       /* no MIDI in from the jack: a radio (the flash goes to the places) */

/* the store: the panel calibration (the platform's), the radio's address and calibration (OBJ_BLE), the networks
 * kept (OBJ_WIFI), the station of the last session (OBJ_RADIO) */
enum { OBJ_PANEL, OBJ_BLE, OBJ_WIFI, OBJ_RADIO, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u, 0xDE000u, 0xDA000u, 0xD8000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
#define APP_LOOP                         /* every pass of the main loop: the decoder and the tuning */
void app_loop(void);
#define APP_SYSEX                        /* Radio's own commands over USB (F0 7D 57 ...: tools/radio_ctl.py) */
int app_sysex(const uint8_t *p, uint32_t n);
