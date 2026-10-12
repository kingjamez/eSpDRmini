/* Line commands on the native USB port, for testing without touching the
 * screen (tools/lcdctl.py speaks this):
 *
 *   STAT                 one line of receiver and display figures
 *   SHOT                 "SHOT <w> <h> <bytes>\n", then the frame as RGB565 LE
 *   TOUCH <x> <y> / UP   press (or move) and release a synthetic finger
 *   TAP <x> <y>          press and release
 *   SWIPE <x0> <y0> <x1> <y1> <ms>
 */
#include "console.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

static void send(const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        int k = usb_serial_jtag_write_bytes(p, n > 4096 ? 4096 : n, pdMS_TO_TICKS(500));
        if (k <= 0) return; /* host gone */
        p += k;
        n -= k;
    }
}

static void reply(const char *s)
{
    send(s, strlen(s));
}

static void command(char *line)
{
    int x0, y0, x1, y1, ms;
    char out[384];
    if (!strcmp(line, "STAT")) {
        ui_stats(out, sizeof(out) - 1);
        strcat(out, "\n");
        reply(out);
    } else if (!strcmp(line, "SHOT")) {
        const uint16_t *px = ui_screenshot(2000);
        if (!px) { reply("ERR timeout\n"); return; }
        snprintf(out, sizeof(out), "SHOT %d %d %d\n", LCD_W, LCD_H, LCD_W * LCD_H * 2);
        reply(out);
        send(px, LCD_W * LCD_H * 2);
    } else if (sscanf(line, "TOUCH %d %d", &x0, &y0) == 2) {
        ui_inject_touch(1, x0, y0);
        reply("OK\n");
    } else if (!strcmp(line, "UP")) {
        ui_inject_touch(0, 0, 0);
        reply("OK\n");
    } else if (sscanf(line, "TAP %d %d", &x0, &y0) == 2) {
        ui_inject_touch(1, x0, y0);
        vTaskDelay(pdMS_TO_TICKS(120));
        ui_inject_touch(0, 0, 0);
        reply("OK\n");
    } else if (sscanf(line, "SWIPE %d %d %d %d %d", &x0, &y0, &x1, &y1, &ms) == 5) {
        int steps = ms / 20 > 1 ? ms / 20 : 1;
        for (int i = 0; i <= steps; i++) {
            ui_inject_touch(1, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ui_inject_touch(0, 0, 0);
        reply("OK\n");
    } else {
        reply("ERR command\n");
    }
}

static void console_task(void *arg)
{
    char line[96];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n) command(line);
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

void console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 4096, .rx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(console_task, "console", 6144, NULL, 3, NULL, 0);
}
