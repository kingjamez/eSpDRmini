/* Settings kept in NVS between power cycles. */
#pragma once
#include <stdbool.h>

#include "sdr.h"

typedef struct {
    rx_settings_t rx;
    float floor_db, ceil_db;
    int overlay;        /* 0 off, 1 Wi-Fi, 2 Bluetooth LE */
    bool show_hold, show_now;
} prefs_t;

void prefs_defaults(prefs_t *p);
void prefs_load(prefs_t *p);
void prefs_save(const prefs_t *p);
