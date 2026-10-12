/* eSpDRmini LCD: a standalone spectrum analyser on the Waveshare
 * ESP32-S3-Touch-LCD-4.3. Core 0 runs the receiver and the FFTs, core 1 the
 * display and touch. */
#include "console.h"
#include "esp_log.h"
#include "prefs.h"
#include "sdr.h"
#include "ui.h"

void app_main(void)
{
    static prefs_t prefs;
    prefs_load(&prefs); /* also initialises NVS, which the PHY uses for calibration data */
    console_start();
    ui_start(&prefs);
    sdr_start(&prefs.rx);
}
