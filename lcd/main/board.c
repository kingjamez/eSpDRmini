/* Waveshare ESP32-S3-Touch-LCD-4.3 bring-up. Pins and panel timings follow
 * Waveshare's ESP-IDF demo for this board. */
#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_async_memcpy.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "hal/dma_types.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "board";

#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_TP_INT 4

/* CH422G: writing 0x24 sets its mode, writing 0x38 sets outputs EXIO0..7. */
#define CH422G_MODE 0x24
#define CH422G_OUT 0x38
#define EXIO_TP_RST (1u << 1)
#define EXIO_DISP (1u << 2) /* backlight */
#define EXIO_LCD_RST (1u << 3)
#define EXIO_SD_CS (1u << 4)
/* EXIO5 low routes GPIO19/20 to the USB connector; high would hand them to
 * the CAN transceiver and drop the USB port, so it is never set here. */

#define GT911_ADDR 0x5d
#define GT911_STATUS 0x814e
#define GT911_POINTS 0x814f

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t ch422g_mode, ch422g_out, gt911;
static uint8_t exio = EXIO_SD_CS;
static esp_lcd_panel_handle_t panel;
static uint16_t *fb;
static uint16_t *strips[2];
static SemaphoreHandle_t vsync, copied;
static async_memcpy_handle_t dma;

/* Bounce buffers are refilled by a GDMA memory-to-memory transfer from the
 * frame buffer, started from the driver's refill callback. Descriptors must
 * not cross 4095 bytes, and PSRAM addresses must sit on 64-byte bursts. */
#define BB_LINES 10
#define BB_BYTES (LCD_W * BB_LINES * 2)
#define BB_DESC_BYTES 3200
#define BB_DESC_N (BB_BYTES / BB_DESC_BYTES)
/* One channel pair per bounce buffer, so a slow refill never holds up the
 * next one. */
static gdma_channel_handle_t bb_tx[2], bb_rx[2];
static dma_descriptor_t bb_tx_desc[2][BB_DESC_N], bb_rx_desc[2][BB_DESC_N];
static void *bb_buf[2];
static volatile bool bb_busy[2];
static volatile uint32_t bb_late; /* refills requested before that buffer's last one finished */
static uint32_t bb_t0[2], bb_max_cyc, bb_sum_cyc, bb_n;
static int bb_index[2] = {0, 1}; /* callback contexts must be in internal RAM */

static IRAM_ATTR bool on_bb_copied(gdma_channel_handle_t chan, gdma_event_data_t *e, void *ctx)
{
    int i = *(int *)ctx;
    uint32_t d = esp_cpu_get_cycle_count() - bb_t0[i];
    if (d > bb_max_cyc) bb_max_cyc = d;
    bb_sum_cyc += d;
    bb_n++;
    bb_busy[i] = false;
    return false;
}

static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t p, void *bounce, int pos_px, int bytes, void *ctx)
{
    int i = bounce == bb_buf[0] ? 0 : bounce == bb_buf[1] ? 1 : !bb_buf[0] ? 0 : 1;
    bb_buf[i] = bounce;
    if (bb_busy[i]) bb_late++;
    dma_descriptor_t *tx = bb_tx_desc[i], *rx = bb_rx_desc[i];
    uint8_t *src = (uint8_t *)(fb + pos_px), *dst = bounce;
    for (int k = 0; k < BB_DESC_N; k++) {
        bool last = k == BB_DESC_N - 1;
        tx[k].dw0 = (typeof(tx[k].dw0)){.size = BB_DESC_BYTES, .length = BB_DESC_BYTES, .suc_eof = last,
                                         .owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA};
        tx[k].buffer = src + k * BB_DESC_BYTES;
        tx[k].next = last ? NULL : &tx[k + 1];
        rx[k].dw0 = (typeof(rx[k].dw0)){.size = BB_DESC_BYTES, .length = 0, .owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA};
        rx[k].buffer = dst + k * BB_DESC_BYTES;
        rx[k].next = last ? NULL : &rx[k + 1];
    }
    bb_busy[i] = true;
    bb_t0[i] = esp_cpu_get_cycle_count();
    gdma_start(bb_rx[i], (intptr_t)rx);
    gdma_start(bb_tx[i], (intptr_t)tx);
    return false;
}

static void bounce_dma_init(void)
{
    for (int i = 0; i < 2; i++) {
        gdma_channel_alloc_config_t ac = {.flags.isr_cache_safe = true};
        ESP_ERROR_CHECK(gdma_new_ahb_channel(&ac, &bb_tx[i], &bb_rx[i]));
        uint32_t free_ids = 0;
        gdma_get_free_m2m_trig_id_mask(bb_tx[i], &free_ids);
        gdma_trigger_t trig = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
        trig.instance_id = __builtin_ctz(free_ids);
        ESP_ERROR_CHECK(gdma_connect(bb_rx[i], trig));
        ESP_ERROR_CHECK(gdma_connect(bb_tx[i], trig));
        gdma_strategy_config_t st = {.owner_check = false, .auto_update_desc = false, .eof_till_data_popped = true};
        gdma_apply_strategy(bb_tx[i], &st);
        gdma_apply_strategy(bb_rx[i], &st);
        gdma_transfer_config_t tc = {.max_data_burst_size = 64, .access_ext_mem = true};
        ESP_ERROR_CHECK(gdma_config_transfer(bb_tx[i], &tc));
        ESP_ERROR_CHECK(gdma_config_transfer(bb_rx[i], &tc));
        /* Refilling the LCD's next lines must win over drawing into PSRAM. */
        gdma_set_priority(bb_tx[i], 9);
        gdma_set_priority(bb_rx[i], 9);
        gdma_rx_event_callbacks_t cbs = {.on_recv_eof = on_bb_copied};
        ESP_ERROR_CHECK(gdma_register_rx_event_callbacks(bb_rx[i], &cbs, &bb_index[i]));
    }
}

uint32_t board_bounce_late(void)
{
    return bb_late;
}

void board_bounce_stats(uint32_t *avg_us, uint32_t *max_us)
{
    *avg_us = bb_n ? bb_sum_cyc / bb_n / 240 : 0;
    *max_us = bb_max_cyc / 240;
    bb_max_cyc = bb_sum_cyc = bb_n = 0;
}

static void exio_write(uint8_t value)
{
    exio = value & ~(1u << 5);
    ESP_ERROR_CHECK(i2c_master_transmit(ch422g_out, &exio, 1, 100));
}

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t n)
{
    uint8_t a[2] = {reg >> 8, reg & 0xff};
    return i2c_master_transmit_receive(gt911, a, 2, data, n, 50);
}

static esp_err_t gt911_write8(uint16_t reg, uint8_t value)
{
    uint8_t a[3] = {reg >> 8, reg & 0xff, value};
    return i2c_master_transmit(gt911, a, 3, 50);
}

static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t p, const esp_lcd_rgb_panel_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(vsync, &woken);
    return woken == pdTRUE;
}

static IRAM_ATTR bool on_copied(async_memcpy_handle_t h, async_memcpy_event_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(copied, &woken);
    return woken == pdTRUE;
}

static void panel_init(void)
{
    fb = heap_caps_aligned_alloc(64, LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    assert(fb);
    bounce_dma_init();

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        /* The LCD reads internal bounce buffers, which on_bounce_empty refills
         * by DMA from the PSRAM frame buffer. Reading PSRAM directly, the LCD
         * underruns (shifting the picture) whenever PSRAM is busy; refilling
         * by CPU copy, as the driver does with its own frame buffer, takes
         * most of a core. */
        .bounce_buffer_size_px = BB_LINES * LCD_W,
        .dma_burst_size = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            14, 38, 18, 17, 10,     /* B3..B7 */
            39, 0, 45, 48, 47, 21,  /* G2..G7 */
            1, 2, 42, 41, 40,       /* R3..R7 */
        },
        .flags.no_fb = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    vsync = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {.on_vsync = on_vsync, .on_bounce_empty = on_bounce_empty};
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL));

    copied = xSemaphoreCreateCounting(2, 0);
    async_memcpy_config_t mc = {.backlog = 4, .dma_burst_size = 64};
    ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_ahb(&mc, &dma));
    for (int i = 0; i < 2; i++) memset(strips[i], 0, LCD_W * STRIP_H * 2);
    for (int y = 0; y < LCD_H; y += STRIP_H) {
        board_strip_send(0, y);
        board_strip_wait();
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
}

static void touch_init(void)
{
    /* GT911 samples INT during reset: low selects address 0x5d. */
    gpio_config_t io = {.pin_bit_mask = 1ull << PIN_TP_INT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_TP_INT, 0);
    exio_write(exio & ~EXIO_TP_RST);
    vTaskDelay(pdMS_TO_TICKS(20));
    exio_write(exio | EXIO_TP_RST);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(PIN_TP_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(60));

    uint8_t id[4] = {0};
    if (gt911_read(0x8140, id, 4) == ESP_OK)
        ESP_LOGI(TAG, "touch controller GT%.4s", (char *)id);
    else
        ESP_LOGE(TAG, "GT911 not answering");
}

void board_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = 0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .scl_speed_hz = 400000};
    dc.device_address = CH422G_MODE;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &ch422g_mode));
    dc.device_address = CH422G_OUT;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &ch422g_out));
    dc.device_address = GT911_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &gt911));

    /* Latch the outputs before enabling the drivers, so EXIO5 never goes high.
     * Pulse the panel reset with the backlight still off. */
    exio_write(EXIO_SD_CS);
    uint8_t mode = 0x01; /* IO0..7 push-pull outputs */
    ESP_ERROR_CHECK(i2c_master_transmit(ch422g_mode, &mode, 1, 100));
    exio_write(EXIO_SD_CS);
    vTaskDelay(pdMS_TO_TICKS(10));
    exio_write(EXIO_SD_CS | EXIO_LCD_RST);

    touch_init();
    panel_init();
}

void board_prealloc(void)
{
    for (int i = 0; i < 2; i++) {
        strips[i] = heap_caps_aligned_alloc(64, LCD_W * STRIP_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(strips[i]);
    }
}

uint16_t *board_strip(int index)
{
    return strips[index & 1];
}

void board_strip_send(int index, int y0)
{
    ESP_ERROR_CHECK(esp_async_memcpy(dma, fb + y0 * LCD_W, strips[index & 1], LCD_W * STRIP_H * 2, on_copied, NULL));
}

void board_strip_wait(void)
{
    xSemaphoreTake(copied, pdMS_TO_TICKS(100));
}

void board_wait_vsync(int timeout_ms)
{
    xSemaphoreTake(vsync, 0);
    xSemaphoreTake(vsync, pdMS_TO_TICKS(timeout_ms));
}

void board_backlight(bool on)
{
    exio_write(on ? exio | EXIO_DISP : exio & ~EXIO_DISP);
}

bool board_touch_read(touch_t *t)
{
    uint8_t status;
    if (gt911_read(GT911_STATUS, &status, 1) != ESP_OK || !(status & 0x80)) return false;
    int n = status & 0x0f;
    if (n > 5) n = 5;
    uint8_t p[40];
    if (n && gt911_read(GT911_POINTS, p, n * 8) != ESP_OK) n = 0;
    gt911_write8(GT911_STATUS, 0);
    t->n = n;
    for (int i = 0; i < n; i++) {
        t->x[i] = p[i * 8 + 1] | p[i * 8 + 2] << 8;
        t->y[i] = p[i * 8 + 3] | p[i * 8 + 4] << 8;
    }
    return true;
}
