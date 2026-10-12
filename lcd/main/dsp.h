/* Spectra of one snapshot: Blackman-windowed contiguous FFT segments,
 * mean and peak power per bin, reduced to display columns. */
#pragma once
#include "sdr.h"

void dsp_init(void);
/* Restarts averaging and peak hold, e.g. after a retune. */
void dsp_reset(void);
void dsp_process(const uint32_t *iq, unsigned n, const rx_settings_t *s, spec_frame_t *f, uint8_t *wf_row);
