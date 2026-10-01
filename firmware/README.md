# Snapshot firmware

`espdr-snapshot.bin` is the [eSpDR](https://github.com/h0m3us3r/eSpDR) ESP32-S3
firmware (upstream commit `41a0ffe`) with `espdr-snapshot.patch` applied,
built with `-DNO_FORWARDED_CLOCK`. It runs from RAM: the viewer loads it with
`esptool --no-stub load-ram`, nothing is written to flash, and a power cycle
restores whatever the board had before.

The patch adds one control command, `ESP_SNAPSHOT` (op 40, argument 1–4
capture banks). It captures 15,360 contiguous IQ pairs per bank with core 1
idle, chaining banks the way eSpDR's streaming does and checking that they
join exactly, then sends the block over the native USB port, packed 2.5 bytes
per pair with a CRC32. A 4-bank capture saves and restores the ROM working
memory that overlaps bank 3. The normal FPGA streaming path is unchanged.

## Rebuild

Requires ESP-IDF **v5.5.3 or later** (v5.5.1 lacks `I2S_CLK_SRC_PLL_240M`).

    git clone https://github.com/h0m3us3r/eSpDR && cd eSpDR
    git checkout 41a0ffe
    git apply /path/to/eSpDRmini/firmware/espdr-snapshot.patch
    . $IDF_PATH/export.sh
    make -C esp32s3 BUILD=build-snapshot EXTRA_CFLAGS=-DNO_FORWARDED_CLOCK
    cp esp32s3/build-snapshot/iq-source.bin /path/to/eSpDRmini/firmware/espdr-snapshot.bin

## Licenses of the binary's contents

* eSpDR firmware and this patch: 0BSD (`LICENSE-eSpDR-0BSD`).
* Espressif's PHY library (`libphy.a`) and the ESP-IDF clock/RTC sources
  compiled into the image: Apache License 2.0 (`LICENSE-Apache-2.0`).
  Copyright Espressif Systems (Shanghai) CO LTD.
* Xtensa window vectors in `vectors.S`: MIT, Copyright (c) 2015-2019 Cadence
  Design Systems, Inc. (`LICENSE-MIT-Cadence-vectors`).
