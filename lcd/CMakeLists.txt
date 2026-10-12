cmake_minimum_required(VERSION 3.16)

# The receiver code comes from esp-sdr (a pinned git submodule), and its FFT
# library from esp-sdr's pinned ESP-DSP. Nothing is fetched at build time.
set(ESP_SDR_DIR "${CMAKE_CURRENT_LIST_DIR}/esp-sdr")
if(NOT EXISTS "${ESP_SDR_DIR}/main/common/rx_recalibration.c"
   OR NOT EXISTS "${ESP_SDR_DIR}/components/esp-dsp/CMakeLists.txt")
    message(FATAL_ERROR "esp-sdr sources missing; run: git submodule update --init --recursive")
endif()
set(EXTRA_COMPONENT_DIRS "${ESP_SDR_DIR}/components/esp-dsp")
set(ENV{IDF_COMPONENT_MANAGER} "0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(espdrmini_lcd)
