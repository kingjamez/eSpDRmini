// SPDX-License-Identifier: GPL-3.0-or-later
/* ESP32-S3 receiver for the standalone display.
 *
 * Adapted from esp-sdr's main/targets/esp32s3/receiver.c
 * (https://github.com/ESPARGOS/esp-sdr, GPL-3.0-or-later): the same PHY
 * bring-up, LO plan, receive-only DC recalibration, gain and analog filter
 * control, and the vendor adctrig capture into SRAM bank 4. Only the serial
 * protocol is replaced by a C API. */
#include "radio.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"

#include "rx_bandwidth.h"
#include "rx_lo.h"
#include "rx_recalibration.h"

static const char *TAG = "radio";

/* adctrig writes the 64 KiB bank at 0x3fcd0000 (SRAM owner 4). Keep it, in
 * both its DRAM and IRAM aliases, out of the heap; sram_guard.ld keeps static
 * data below it. */
SOC_RESERVE_MEMORY_REGION(0x3fcd0000, 0x3fce0000, s3_rf_dump);
#define IQ_BUFFER ((volatile uint32_t *)0x3fcd0000)
#define SRAM_OWNER_REG 0x600c101cu
#define DUMP_CTRL_REG 0x60033d5cu
#define DUMP_PACK_REG 0x60033d90u
#define GAIN_REG 0x6001c02cu
#define SENTINEL 0xa5a0055au

extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void set_rf_freq_offset(unsigned, unsigned, int);
extern void force_rx_gain(unsigned, unsigned, unsigned);

/* Required by the stock RF test archive; no shell is exposed. */
int cmd_parse(char *cmd, char *name, int *argc, char **argv)
{
    (void)cmd; (void)name; (void)argc; (void)argv;
    return -1;
}

static unsigned freq_mhz = 2440;
static bool rx_ready;
static bool gain_agc;
static unsigned gain_index = 40;
/* Widest analog filter, so signals fill the whole sampled span. */
static int rx_filter = -1;
static unsigned rx_filter_saved[2];

unsigned radio_gain_max(void)
{
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum <= 82u ? maximum : 0u; /* never expose uncalibrated slots */
}

static void gain_apply(void)
{
    if (gain_index > radio_gain_max()) gain_index = radio_gain_max();
    force_rx_gain(!gain_agc, gain_index, 0);
}

static void tune(unsigned mhz)
{
    static unsigned calibrated_mhz;
    if (calibrated_mhz != mhz || rx_recalibration_stale()) {
        rx_recalibrate(mhz);
        calibrated_mhz = mhz;
    }
    rx_lo_plan_t plan = rx_lo_plan(mhz);
    bool channel = (mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) || mhz == 2484;
    rx_lo_select(false);
    set_chanfreq(channel ? mhz : 2412, 0);
    if (!channel) set_rf_freq_offset(0, plan.mhz, plan.offset_khz);
}

static void prepare_rx(void)
{
    if (rx_ready && !rx_recalibration_stale()) return;
    tune(freq_mhz);
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    gain_apply();
    rx_lo_select(rx_lo_plan(freq_mhz).alternate);
    esp_rom_delay_us(3000);
    rx_ready = true;
}

/* The filter setting applies only around a capture; restore before retuning. */
static void filter_apply(void)
{
    for (unsigned j = 0; j < 2; j++) {
        rx_filter_saved[j] = rom_chip_i2c_readReg(0x67, 0, 4 + j);
        if (rx_filter >= 0)
            rom_chip_i2c_writeReg(0x67, 0, 4 + j, (rx_filter_saved[j] & ~63u) | (unsigned)rx_filter);
    }
}

static void filter_restore(void)
{
    if (rx_filter >= 0)
        for (unsigned j = 0; j < 2; j++) rom_chip_i2c_writeReg(0x67, 0, 4 + j, rx_filter_saved[j]);
}

void radio_init(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    esp_err_t e = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (e != ESP_OK) ESP_LOGE(TAG, "set_channel: %s", esp_err_to_name(e));
    rx_filter = rx_bandwidth_dcap(0);
    prepare_rx();
}

void radio_set_freq(unsigned mhz)
{
    if (mhz < RADIO_FREQ_MIN) mhz = RADIO_FREQ_MIN;
    if (mhz > RADIO_FREQ_MAX) mhz = RADIO_FREQ_MAX;
    freq_mhz = mhz;
    rx_ready = false;
    prepare_rx();
}

void radio_set_gain(bool agc, unsigned index)
{
    gain_agc = agc;
    gain_index = index;
    /* A forced gain index alone can leave the RX path stale until the next
     * tune; apply it through the same setup as a retune. */
    rx_ready = false;
    prepare_rx();
}

unsigned radio_rate_hz(radio_rate_t rate)
{
    return rate == RADIO_RATE_40 ? 40000000u : rate == RADIO_RATE_16 ? 16000000u : 80000000u;
}

const uint32_t *radio_iq(void)
{
    return (const uint32_t *)IQ_BUFFER;
}

bool radio_capture(unsigned n, radio_rate_t rate, uint32_t *elapsed_us)
{
    if (n > RADIO_MAX_PAIRS) n = RADIO_MAX_PAIRS;
    prepare_rx();
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = SENTINEL;
    filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    int64_t start = esp_timer_get_time();
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(DUMP_PACK_REG, 0u | (1u << 6) | (2u << 12) | (3u << 18));
    REG_WRITE(SRAM_OWNER_REG, (owner & ~15u) | 4u);
    /* bit 15 halves 80 to 40 MS/s; bit 16 selects the 16 MS/s path. */
    uint32_t rate_bits = rate == RADIO_RATE_40 ? (1u << 15) : rate == RADIO_RATE_16 ? (1u << 16) : 0;
    uint32_t ctrl = 0x80000000u | rate_bits | n;
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    REG_WRITE(DUMP_CTRL_REG, ctrl | (1u << 19));
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    while (!(REG_READ(DUMP_CTRL_REG) & (1u << 18)) && esp_timer_get_time() - start < 20000) {
    }
    bool done = (REG_READ(DUMP_CTRL_REG) & (1u << 18)) != 0;
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(SRAM_OWNER_REG, owner);
    filter_restore();
    if (elapsed_us) *elapsed_us = elapsed;
    if (!done) return false;
    for (unsigned j = 0; j < n; j++)
        if (IQ_BUFFER[j] == SENTINEL) return false;
    return true;
}
