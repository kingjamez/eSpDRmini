/* Touch UI (core 1): spectrum, waterfall, controls. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "prefs.h"

void ui_start(const prefs_t *p);
/* Synthetic touch for testing over USB: n = 0 releases. */
void ui_inject_touch(int n, int x, int y);
/* Copies the next displayed frame (RGB565, LCD_W x LCD_H); NULL on timeout. */
const uint16_t *ui_screenshot(int timeout_ms);
void ui_stats(char *out, int size);
