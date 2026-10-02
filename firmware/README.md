# Snapshot firmware

`espdr-snapshot.bin` is the [eSpDR](https://github.com/h0m3us3r/eSpDR) ESP32-S3
firmware (upstream commit `f279bf823eee41796dfd1ac21f13e1ed9b418c82`) with `espdr-snapshot.patch` applied,
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
    git checkout f279bf823eee41796dfd1ac21f13e1ed9b418c82
    git apply /path/to/eSpDRmini/firmware/espdr-snapshot.patch
    . $IDF_PATH/export.sh
    make -C esp32s3 BUILD=build-snapshot EXTRA_CFLAGS=-DNO_FORWARDED_CLOCK
    cp esp32s3/build-snapshot/iq-source.bin /path/to/eSpDRmini/firmware/espdr-snapshot.bin

The bundled image was built with ESP-IDF **v5.5.5** and the
`esp-14.2.0_20260121` Xtensa toolchain. It reports firmware ID `0x49515306`
and includes upstream's [5/6 LO extension](https://github.com/h0m3us3r/eSpDR/blob/f279bf823eee41796dfd1ac21f13e1ed9b418c82/docs/LO-EXTENSION.md).
Requests below 2210 MHz select 5/6 conversion automatically; higher requests
use normal conversion. The effective LO request range is 1,841,666,667 to
2,790,000,000 Hz, subject to each board's PLL lock range. Failed tunes restore
the previous LO and conversion mode. Gain, filter, width and sample-rate
changes retain the selected mode. Snapshot framing and sample packing are
unchanged.

Use `--reload` when replacing firmware that already reports this ID. The
original eSpDR streaming image has the same ID as this snapshot build.

## Validation

`python -m unittest discover -s tests -v` runs the host regression tests.
The upstream `tests/lo_plan_test.c` and `tests/radio_tuning_test.py` also pass
against the patched source, covering frequency planning, conversion changes,
register preservation and failed-tune rollback. A fresh checkout with this
patch and the toolchain above reproduces the bundled image byte for byte.

[validation.json](validation.json) records the finite checks on an ESP32-S3:
800 snapshots across ten LO requests, both rates and all four bank sizes;
40 conversion transitions; six rejected-tune recovery checks; and eight
gain/filter/width/rate changes retaining 5/6 mode. Every matrix snapshot passed
the firmware's bank-join checks and the host CRC check. The tested board locked
at the lower endpoint and at 2780 MHz; 2790 MHz failed and restored the previous
mode. These are functional results for this board, not antenna sensitivity or
continuous-reception qualification.

Conducted B205-mini tones were received at eight LO settings from the lower
endpoint to 2700 MHz, at both 16 and 80 Msps. Source-on/off contrast was
18.9–30.7 dB across those points. Independent 250 kHz source and receiver
steps moved the observed tone by approximately +250 and -250 kHz; halving
source amplitude reduced its level by 5.47 dB. Changing the source's LO/DSP
split retained the wanted tone. All 37 finite source bursts completed with
acknowledgments and no reported source errors; no captured samples clipped.
The JSON includes the measured IFs and conversion fits. Delivered RF power
and antenna sensitivity were not calibrated.

## Licenses of the binary's contents

* eSpDR firmware and this patch: 0BSD (`LICENSE-eSpDR-0BSD`).
* Espressif's PHY library (`libphy.a`) and the ESP-IDF clock/RTC sources
  compiled into the image: Apache License 2.0 (`LICENSE-Apache-2.0`).
  Copyright Espressif Systems (Shanghai) CO LTD.
* Xtensa window vectors in `vectors.S`: MIT, Copyright (c) 2015-2019 Cadence
  Design Systems, Inc. (`LICENSE-MIT-Cadence-vectors`).
