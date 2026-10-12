#include "dsp.h"

#include <math.h>
#include <string.h>

#include "dsps_fft2r.h"
#include "esp_heap_caps.h"

#define FULL_SCALE 512.f /* signed 10-bit ADC */

static float *buf;          /* 2 * FFT_MAX, interleaved complex */
static float *win;          /* FFT_MAX */
static float *pmean, *pmax; /* FFT_MAX */
static unsigned win_n;
static float win_sum;
static float ema[SPEC_W], hold[SPEC_W];
static bool fresh = true;

static void *alloc(size_t bytes)
{
    void *p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM);
}

void dsp_init(void)
{
    buf = alloc(2 * FFT_MAX * sizeof(float));
    pmean = alloc(FFT_MAX * sizeof(float));
    pmax = alloc(FFT_MAX * sizeof(float));
    win = alloc(FFT_MAX * sizeof(float));
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, FFT_MAX));
}

void dsp_reset(void)
{
    fresh = true;
}

static void make_window(unsigned n)
{
    win_sum = 0;
    for (unsigned k = 0; k < n; k++) {
        float a = 2.f * (float)M_PI * k / (n - 1);
        win[k] = 0.42f - 0.5f * cosf(a) + 0.08f * cosf(2.f * a);
        win_sum += win[k];
    }
    win_n = n;
}

static inline int sext10(uint32_t v)
{
    return (int32_t)(v << 22) >> 22;
}

void dsp_process(const uint32_t *iq, unsigned n, const rx_settings_t *s, spec_frame_t *f, uint8_t *wf_row)
{
    unsigned N = s->nfft;
    if (N != win_n) make_window(N);

    /* Level, clipping and the DC offset to remove. */
    int32_t si = 0, sq = 0;
    int64_t pw = 0;
    unsigned clipped = 0;
    for (unsigned j = 0; j < n; j++) {
        int i = sext10(iq[j]), q = sext10(iq[j] >> 10);
        si += i;
        sq += q;
        pw += i * i + q * q;
        clipped += (i >= 500 || i <= -500 || q >= 500 || q <= -500);
    }
    float mi = (float)si / n, mq = (float)sq / n;
    float ms = (float)pw / n - mi * mi - mq * mq;
    f->rms_dbfs = 10.f * log10f((ms > 1e-6f ? ms : 1e-6f) / (FULL_SCALE * FULL_SCALE));
    f->clip_pct = 100.f * clipped / n;

    /* Contiguous segments, as in the desktop viewer. The raw convention is
     * RF = LO - f, so I - jQ puts each signal at its RF offset. */
    unsigned segs = n / N;
    memset(pmean, 0, N * sizeof(float));
    memset(pmax, 0, N * sizeof(float));
    for (unsigned seg = 0; seg < segs; seg++) {
        const uint32_t *x = iq + seg * N;
        for (unsigned k = 0; k < N; k++) {
            buf[2 * k] = (sext10(x[k]) - mi) * win[k];
            buf[2 * k + 1] = (mq - sext10(x[k] >> 10)) * win[k];
        }
        dsps_fft2r_fc32(buf, N);
        dsps_bit_rev_fc32(buf, N);
        for (unsigned k = 0; k < N; k++) {
            float p = buf[2 * k] * buf[2 * k] + buf[2 * k + 1] * buf[2 * k + 1];
            pmean[k] += p;
            if (p > pmax[k]) pmax[k] = p;
        }
    }

    /* Columns, lowest frequency first (FFT bin k holds offset k, or k - N). */
    float scale = 1.f / (FULL_SCALE * win_sum * FULL_SCALE * win_sum);
    float a = fresh ? 0.f : s->avg;
    for (unsigned c = 0; c < SPEC_W; c++) {
        unsigned b0 = c * N / SPEC_W, b1 = (c + 1) * N / SPEC_W;
        if (b1 <= b0) b1 = b0 + 1;
        float mean = 0, peak = 0;
        unsigned at = b0;
        for (unsigned b = b0; b < b1; b++) {
            unsigned k = (b + N / 2) & (N - 1);
            if (pmean[k] > mean) { mean = pmean[k]; at = b; }
            if (pmax[k] > peak) peak = pmax[k];
        }
        mean *= scale / segs;
        peak *= scale;
        ema[c] = a * ema[c] + (1.f - a) * mean;
        float now = 10.f * log10f(peak + 1e-20f);
        hold[c] = fresh || now > hold[c] ? now : hold[c];
        f->avg[c] = 10.f * log10f(ema[c] + 1e-20f);
        f->now[c] = now;
        f->hold[c] = hold[c];
        f->peak_bin[c] = at;
        float row = s->wf_avg ? 10.f * log10f(mean + 1e-20f) : now;
        wf_row[c] = WF_LEVEL(row);
    }
    fresh = false;
    f->nfft = N;
    f->segs = segs;
    f->pairs = n;
}
