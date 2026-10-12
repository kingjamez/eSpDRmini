/* ESP32-S3 receiver: tuning, gain, analog filter and one-shot IQ capture. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RADIO_MAX_PAIRS 16380u
#define RADIO_FREQ_MIN 1842u /* below 2210 MHz esp-sdr divides the LO by 6/5 */
#define RADIO_FREQ_MAX 2850u

/* esp-sdr rate codes the S3 capture engine supports natively. */
typedef enum { RADIO_RATE_80 = 0, RADIO_RATE_40 = 1, RADIO_RATE_16 = 6 } radio_rate_t;

void radio_init(void);
void radio_set_freq(unsigned mhz);
void radio_set_gain(bool agc, unsigned index);
unsigned radio_gain_max(void);
/* Captures n IQ pairs (<= RADIO_MAX_PAIRS) into the capture bank. Each word
 * holds signed 10-bit I in bits 0..9 and Q in bits 10..19, in the chip's raw
 * orientation (RF = LO - f). Returns false on a timeout. */
bool radio_capture(unsigned n, radio_rate_t rate, uint32_t *elapsed_us);
const uint32_t *radio_iq(void);
unsigned radio_rate_hz(radio_rate_t rate);
