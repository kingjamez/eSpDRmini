#include "prefs.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define PREFS_VERSION 1

typedef struct {
    unsigned version;
    prefs_t p;
} stored_t;

void prefs_defaults(prefs_t *p)
{
    *p = (prefs_t){
        .rx = {
            .lo_mhz = 2442,
            .rate = RADIO_RATE_80,
            .nfft = 2048,
            .agc = false, /* AGC chases Wi-Fi packets and clips on quiet bands */
            .gain = 40,
            .avg = 0.7f,
            .wf_avg = false,
            .snaps_per_s = 25,
        },
        .floor_db = -85,
        .ceil_db = -15,
        .overlay = 1,
        .show_hold = true,
        .show_now = true,
    };
}

static bool open_nvs(nvs_handle_t *h, nvs_open_mode_t mode)
{
    static bool init;
    if (!init) {
        esp_err_t e = nvs_flash_init();
        if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            e = nvs_flash_init();
        }
        if (e != ESP_OK) return false;
        init = true;
    }
    return nvs_open("espdrmini", mode, h) == ESP_OK;
}

void prefs_load(prefs_t *p)
{
    prefs_defaults(p);
    nvs_handle_t h;
    if (!open_nvs(&h, NVS_READONLY)) return;
    stored_t s;
    size_t size = sizeof(s);
    if (nvs_get_blob(h, "prefs", &s, &size) == ESP_OK && size == sizeof(s) && s.version == PREFS_VERSION) {
        *p = s.p;
        p->rx.paused = false;
        if (p->rx.nfft < FFT_MIN || p->rx.nfft > FFT_MAX) p->rx.nfft = 2048;
    }
    nvs_close(h);
}

void prefs_save(const prefs_t *p)
{
    nvs_handle_t h;
    if (!open_nvs(&h, NVS_READWRITE)) return;
    stored_t s;
    memset(&s, 0, sizeof(s));
    s.version = PREFS_VERSION;
    s.p = *p;
    nvs_set_blob(h, "prefs", &s, sizeof(s));
    nvs_commit(h);
    nvs_close(h);
}
