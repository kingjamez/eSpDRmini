set(sdr "${CMAKE_CURRENT_LIST_DIR}/../esp-sdr/main")

idf_component_register(
    SRCS "main.c" "radio.c" "board.c" "gfx.c" "render.c" "fonts.c" "dsp.c" "sdr.c" "ui.c" "console.c" "prefs.c"
         "${sdr}/common/rx_recalibration.c" "${sdr}/common/rx_calibration_state.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${sdr}/common"
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_wifi esp_phy esp_event nvs_flash esp_timer esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_dma esp_mm
             esp_driver_usb_serial_jtag esp_hw_support esp_psram heap esp-dsp)

target_link_options(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_LIST_DIR}/sram_guard.ld")
# esp-sdr's receive-only recalibration hooks (see its rx_recalibration.c and
# rx_calibration_state.c): tune the DC measurement to the requested LO and
# count fresh DC measurements.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=chip_v7_set_chan_ana" "-Wl,--wrap=set_rx_gain_cal_dc")
