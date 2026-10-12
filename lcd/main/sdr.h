/* The receiver task (core 0): captures, computes spectra, and shares the
 * results and the waterfall history with the UI (core 1). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

#define SPEC_W 760      /* display columns of the spectrum and waterfall */
#define WF_ROWS 140     /* waterfall history, one row per snapshot */
#define FFT_MIN 256
#define FFT_MAX 2048    /* its buffers take 40 KiB of internal RAM */
/* Waterfall rows store levels as (dBFS + 128) * 2, clamped to 0..255. */
#define WF_LEVEL(db) ((db) <= -128.f ? 0 : (db) >= -0.5f ? 255 : (int)(((db) + 128.f) * 2.f))

typedef struct {
    unsigned lo_mhz;
    radio_rate_t rate;
    unsigned nfft;
    bool agc;
    unsigned gain;
    float avg;          /* weight of the history in the smoothed trace, 0..0.95 */
    bool wf_avg;        /* waterfall rows: snapshot mean instead of its peak */
    bool paused;
    unsigned snaps_per_s;
    uint32_t reset;     /* bump to restart averaging and peak hold */
} rx_settings_t;

typedef struct {
    float avg[SPEC_W];        /* smoothed mean power, dBFS */
    float now[SPEC_W];        /* this snapshot's peak over its segments, dBFS */
    float hold[SPEC_W];       /* peak hold, dBFS */
    uint16_t peak_bin[SPEC_W];/* FFT bin (0 = lowest frequency) of each column's maximum */
    unsigned lo_mhz, rate_hz, nfft, segs, pairs;
    float rms_dbfs, clip_pct;
    uint32_t capture_us, dsp_us, seq, errors;
    float snaps_per_s;
} spec_frame_t;

void sdr_start(const rx_settings_t *initial);
void sdr_get(rx_settings_t *s);
void sdr_set(const rx_settings_t *s);

/* Latest spectrum; hold it until sdr_frame_release(). NULL before the first. */
const spec_frame_t *sdr_frame_acquire(void);
void sdr_frame_release(void);

/* Waterfall history: rows written so far, and row k back from the one
 * numbered count (0 = newest) with the LO it was captured at. The ring keeps
 * a few rows more than WF_ROWS, so a count read once stays valid while one
 * frame is drawn. */
uint32_t sdr_wf_count(void);
const uint8_t *sdr_wf_row(uint32_t count, unsigned k, unsigned *lo_mhz);
void sdr_wf_clear(void);
