/* The capture engine writes into the 64 KiB SRAM bank at 0x3fcd0000
 * (IRAM alias 0x403c0000), which radio.c reserves from the heap. */
ASSERT(_iram_end <= 0x403c0000, "capture bank overlaps IRAM")
ASSERT(_data_end <= 0x3fcd0000, "capture bank overlaps data")
ASSERT(_bss_end <= 0x3fcd0000, "capture bank overlaps BSS")
