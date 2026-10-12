#include "sdr.h"

#include <string.h>

#include "dsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdr";

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static rx_settings_t want;

/* Triple buffering: the task writes a frame that is neither the latest nor
 * the one the UI holds. */
static spec_frame_t *frames[3];
static int latest = -1, reading = -1;

#define WF_RING (WF_ROWS + 8)
static uint8_t *wf;               /* WF_RING x SPEC_W */
static uint16_t wf_lo[WF_RING];
static volatile uint32_t wf_count;

void sdr_get(rx_settings_t *s)
{
    portENTER_CRITICAL(&lock);
    *s = want;
    portEXIT_CRITICAL(&lock);
}

void sdr_set(const rx_settings_t *s)
{
    portENTER_CRITICAL(&lock);
    want = *s;
    portEXIT_CRITICAL(&lock);
}

const spec_frame_t *sdr_frame_acquire(void)
{
    portENTER_CRITICAL(&lock);
    reading = latest;
    portEXIT_CRITICAL(&lock);
    return reading < 0 ? NULL : frames[reading];
}

void sdr_frame_release(void)
{
    portENTER_CRITICAL(&lock);
    reading = -1;
    portEXIT_CRITICAL(&lock);
}

uint32_t sdr_wf_count(void)
{
    return wf_count;
}

const uint8_t *sdr_wf_row(uint32_t count, unsigned k, unsigned *lo_mhz)
{
    unsigned i = (count - 1 - k) % WF_RING;
    *lo_mhz = wf_lo[i];
    return wf + i * SPEC_W;
}

void sdr_wf_clear(void)
{
    wf_count = 0;
}

static void sdr_task(void *arg)
{
    radio_init();

    rx_settings_t have = {0};
    bool first = true;
    uint32_t seq = 0, errors = 0;
    int64_t last = esp_timer_get_time();
    float rate_avg = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        rx_settings_t s;
        sdr_get(&s);
        if (first || s.lo_mhz != have.lo_mhz) radio_set_freq(s.lo_mhz);
        if (first || s.agc != have.agc || s.gain != have.gain) radio_set_gain(s.agc, s.gain);
        if (first || s.lo_mhz != have.lo_mhz || s.rate != have.rate || s.nfft != have.nfft ||
            s.reset != have.reset || s.agc != have.agc || s.gain != have.gain)
            dsp_reset();
        if (!first && (s.rate != have.rate || s.nfft != have.nfft)) wf_count = 0;
        first = false;
        have = s;

        TickType_t period = pdMS_TO_TICKS(1000 / (s.snaps_per_s ? s.snaps_per_s : 20));
        if (s.paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            wake = xTaskGetTickCount();
            continue;
        }

        uint32_t cap_us;
        if (!radio_capture(RADIO_MAX_PAIRS, s.rate, &cap_us)) {
            errors++;
            ESP_LOGW(TAG, "capture failed (%u)", (unsigned)errors);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int w;
        portENTER_CRITICAL(&lock);
        for (w = 0; w == latest || w == reading; w++) {
        }
        portEXIT_CRITICAL(&lock);
        spec_frame_t *f = frames[w];
        int64_t t0 = esp_timer_get_time();
        uint8_t *row = wf + (wf_count % WF_RING) * SPEC_W;
        dsp_process(radio_iq(), RADIO_MAX_PAIRS, &s, f, row);
        int64_t t1 = esp_timer_get_time();
        wf_lo[wf_count % WF_RING] = s.lo_mhz;
        wf_count = wf_count + 1;

        float dt = (t1 - last) / 1e6f;
        last = t1;
        rate_avg = rate_avg ? 0.9f * rate_avg + 0.1f / dt : 1.f / dt;
        f->lo_mhz = s.lo_mhz;
        f->rate_hz = radio_rate_hz(s.rate);
        f->capture_us = cap_us;
        f->dsp_us = (uint32_t)(t1 - t0);
        f->seq = ++seq;
        f->errors = errors;
        f->snaps_per_s = rate_avg;
        portENTER_CRITICAL(&lock);
        latest = w;
        portEXIT_CRITICAL(&lock);

        /* Pace the snapshots, but always let the idle task run. */
        if (xTaskGetTickCount() - wake >= period) {
            wake = xTaskGetTickCount();
            vTaskDelay(1);
        } else {
            vTaskDelayUntil(&wake, period);
        }
    }
}

void sdr_start(const rx_settings_t *initial)
{
    want = *initial;
    dsp_init(); /* before Wi-Fi takes its share of internal RAM */
    for (int i = 0; i < 3; i++) {
        frames[i] = heap_caps_calloc(1, sizeof(spec_frame_t), MALLOC_CAP_SPIRAM);
        assert(frames[i]);
    }
    wf = heap_caps_calloc(WF_RING, SPEC_W, MALLOC_CAP_SPIRAM);
    assert(wf);
    xTaskCreatePinnedToCore(sdr_task, "sdr", 8192, NULL, 5, NULL, 0);
}
