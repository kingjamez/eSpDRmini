/* Waveshare ESP32-S3-Touch-LCD-4.3: 800x480 RGB panel, GT911 touch and the
 * CH422G IO expander that drives the resets, backlight and USB/CAN switch. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LCD_W 800
#define LCD_H 480

typedef struct {
    int n;          /* touch points, 0 when released */
    int x[5], y[5];
} touch_t;

/* Allocates the strip buffers; call early, before Wi-Fi takes internal RAM. */
void board_prealloc(void);
void board_init(void);
/* The display is refreshed from one PSRAM frame buffer that only DMA writes:
 * the UI renders strips of whole lines in internal RAM and queues them here. */
#define STRIP_H 16
uint16_t *board_strip(int index);           /* two DMA-capable LCD_W x STRIP_H buffers */
void board_strip_send(int index, int y0);   /* copies strip index to lines y0.. */
void board_strip_wait(void);                /* waits for the oldest copy still running */
/* Waits for the start of the next refresh (up to timeout_ms). */
void board_wait_vsync(int timeout_ms);
void board_backlight(bool on);
/* Bounce-buffer refills requested before that buffer's previous refill finished. */
uint32_t board_bounce_late(void);
/* Average and longest refill since the last call, in µs. */
void board_bounce_stats(uint32_t *avg_us, uint32_t *max_us);
/* Reads the controller. Returns false if it had no new report, leaving *t. */
bool board_touch_read(touch_t *t);
