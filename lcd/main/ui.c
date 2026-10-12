#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gfx.h"
#include "render.h"
#include "sdr.h"

/* TMOG palette, as in the desktop viewer (tmog.org :root). */
#define C_INK RGB(0x070b0f)
#define C_INK2 RGB(0x0b1117)
#define C_PANEL RGB(0x0e151c)
#define C_RAISED RGB(0x111a22)
#define C_TEXT RGB(0xedf5fa)
#define C_MUTED RGB(0x91a1ac)
#define C_MUTED2 RGB(0x60717d)
#define C_BORDER RGB(0x1d3a4c)
#define C_GRID RGB(0x132029)
#define C_CYAN RGB(0x16c5ff)
#define C_BLUE RGB(0x078cff)
#define C_GREEN RGB(0x45ed73)
#define C_YELLOW RGB(0xffe016)
#define C_ORANGE RGB(0xff8a22)
#define C_RED RGB(0xff4f55)
#define C_PURPLE RGB(0xd248ff)
#define C_PRESS RGB(0x14374b)
#define C_ACTIVE RGB(0x0f2a3a)
#define C_BAND RGB(0x0c2018)
#define C_BAND_BLE RGB(0x1c1030)
#define C_METER_OFF RGB(0x0d2131)

/* Layout (800 x 480). */
#define HDR_H 54
#define PLOT_X 36
#define SPEC_Y 58
#define SPEC_H 196
#define AXIS_Y (SPEC_Y + SPEC_H)
#define WF_Y 272
#define WF_H WF_ROWS
#define BAR_Y 418
#define MENU_Y HDR_H
#define MENU_H (BAR_Y - 4 - HDR_H)

/* ---- state -------------------------------------------------------------- */

static prefs_t P;
static int64_t prefs_dirty_at;
static float fps;
static uint32_t t_draw, t_dma_wait; /* µs, last frame, for STAT */

typedef struct {
    bool on;
    double mhz;
} marker_t;
static marker_t mk[2];

static bool menu_open, keypad_open;
static char keypad[8];
static char toast[80];
static int64_t toast_until;
static color_t toast_color;

static portMUX_TYPE inject_lock = portMUX_INITIALIZER_UNLOCKED;
static bool inject_on;
static touch_t inject;

static SemaphoreHandle_t shot_done;
static volatile bool shot_wanted;
static uint16_t *shot;

/* Hit regions registered while drawing; later ones are on top. */
enum {
    G_BAR = 1, G_FFT, G_AVG, G_RATE, G_WF, G_OV, G_TRACE, G_FLOOR, G_CEIL, G_ACT,
    G_KEY, G_PRESET, G_SPEC, G_WFALL, G_MENU, G_KEYPAD,
};
#define ID(g, i) ((g) << 8 | (i))
#define GROUP(id) ((id) >> 8)
#define INDEX(id) ((id) & 255)
typedef struct {
    int id, x, y, w, h;
} hit_t;
static hit_t hits[128];
static int nhits;

/* Each frame is drawn in strips, so an element can be drawn more than once. */
static void hit_add(int id, int x, int y, int w, int h)
{
    for (int i = 0; i < nhits; i++)
        if (hits[i].id == id) return;
    if (nhits < (int)(sizeof(hits) / sizeof(hits[0]))) hits[nhits++] = (hit_t){id, x, y, w, h};
}

static int hit_test(int x, int y)
{
    for (int i = nhits - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return hits[i].id;
    return 0;
}

/* Touch tracking. */
static struct {
    bool down;
    int x, y, x0, y0;
    int id;            /* hit region under the first contact */
    int64_t t0, t_repeat;
    int mode;          /* gesture in a plot: 0 undecided, 1 pan, 2 level shift, 3 done */
    unsigned lo0;
    float floor0, ceil0;
    int64_t last_tap;
    int last_tap_x, last_tap_y, last_tap_marker;
} T = {.last_tap_marker = -1};

/* ---- helpers ------------------------------------------------------------ */

static void show_toast(const char *msg, color_t c)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_color = c;
    toast_until = esp_timer_get_time() + 2500000;
}

static void changed(void)
{
    sdr_set(&P.rx);
    prefs_dirty_at = esp_timer_get_time();
}

static unsigned span_mhz(void)
{
    return radio_rate_hz(P.rx.rate) / 1000000u;
}

static unsigned step_small(void)
{
    return P.rx.rate == RADIO_RATE_80 ? 5 : P.rx.rate == RADIO_RATE_40 ? 2 : 1;
}

static unsigned step_big(void)
{
    return P.rx.rate == RADIO_RATE_80 ? 20 : P.rx.rate == RADIO_RATE_40 ? 10 : 4;
}

static void set_lo(int mhz)
{
    if (mhz < (int)RADIO_FREQ_MIN) mhz = RADIO_FREQ_MIN;
    if (mhz > (int)RADIO_FREQ_MAX) mhz = RADIO_FREQ_MAX;
    if ((unsigned)mhz != P.rx.lo_mhz) {
        P.rx.lo_mhz = mhz;
        changed();
    }
}

static double x_to_mhz(int x)
{
    return P.rx.lo_mhz - span_mhz() / 2.0 + (x - PLOT_X + 0.5) * span_mhz() / SPEC_W;
}

static int mhz_to_x(double mhz)
{
    return PLOT_X + (int)floor((mhz - (P.rx.lo_mhz - span_mhz() / 2.0)) * SPEC_W / span_mhz());
}

static int db_to_y(float db)
{
    float t = (P.ceil_db - db) / (P.ceil_db - P.floor_db);
    int y = SPEC_Y + (int)(t * (SPEC_H - 1) + 0.5f);
    return y < SPEC_Y ? SPEC_Y : y > SPEC_Y + SPEC_H - 1 ? SPEC_Y + SPEC_H - 1 : y;
}

/* Columns of a frame captured at lo_mhz sit this far right of the axis. */
static int frame_shift(unsigned lo_mhz)
{
    return (int)lroundf(((float)lo_mhz - (float)P.rx.lo_mhz) * SPEC_W / span_mhz());
}

static color_t cmap(const uint32_t *stops, int n, float t)
{
    if (t <= 0) return RGB(stops[0]);
    if (t >= 1) return RGB(stops[n - 1]);
    float p = t * (n - 1);
    int i = (int)p;
    float f = p - i;
    uint32_t a = stops[i], b = stops[i + 1];
    int r = (int)(((a >> 16) & 255) * (1 - f) + ((b >> 16) & 255) * f);
    int g = (int)(((a >> 8) & 255) * (1 - f) + ((b >> 8) & 255) * f);
    int bl = (int)((a & 255) * (1 - f) + (b & 255) * f);
    return RGB((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl);
}

static const uint32_t WF_STOPS[] = {0x070b0f, 0x0a2238, 0x0b4f8c, 0x078cff, 0x16c5ff,
                                    0x45ed73, 0xffe016, 0xff8a22, 0xff4f55};
static const uint32_t TRACE_STOPS[] = {0x16c5ff, 0x45ed73, 0x86ff35, 0xffe016, 0xff8a22};
static color_t wf_lut[256], trace_lut[64];
static float lut_floor = 1, lut_ceil = 1;

static void build_luts(void)
{
    if (lut_floor == P.floor_db && lut_ceil == P.ceil_db) return;
    lut_floor = P.floor_db;
    lut_ceil = P.ceil_db;
    for (int i = 0; i < 256; i++) {
        float db = i / 2.f - 128.f;
        wf_lut[i] = cmap(WF_STOPS, 9, (db - P.floor_db) / (P.ceil_db - P.floor_db));
    }
    for (int i = 0; i < 64; i++) trace_lut[i] = cmap(TRACE_STOPS, 5, i / 63.f);
}

static color_t trace_color(float db)
{
    int i = (int)((db - P.floor_db) / (P.ceil_db - P.floor_db) * 63.f);
    return trace_lut[i < 0 ? 0 : i > 63 ? 63 : i];
}

static void button(int id, int x, int y, int w, int h, const char *caption, const char *value, bool active)
{
    bool pressed = T.down && T.id == id;
    color_t fill = pressed ? C_PRESS : active ? C_ACTIVE : C_PANEL;
    gfx_round_fill(x, y, w, h, 6, fill);
    gfx_round_rect(x, y, w, h, 6, active ? C_CYAN : C_BORDER);
    if (caption && *caption) {
        gfx_text(&font_small, x + w / 2, y + 6, caption, C_MUTED, ALIGN_CENTER);
        gfx_text(&font_bold, x + w / 2, y + h - font_bold.height - 6, value, active ? C_CYAN : C_TEXT, ALIGN_CENTER);
    } else {
        gfx_text(&font_bold, x + w / 2, y + (h - font_bold.height) / 2, value, active ? C_CYAN : C_TEXT,
                 ALIGN_CENTER);
    }
    hit_add(id, x, y, w, h);
}

static void segments(int group, int x, int y, int w, int h, const char *const *labels, int n, int active)
{
    int gap = 6, bw = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) button(ID(group, i), x + i * (bw + gap), y, bw, h, NULL, labels[i], i == active);
}

static void fmt_rbw(char *s, size_t n, unsigned rate_hz, unsigned nfft)
{
    float rbw = (float)rate_hz / nfft;
    if (rbw >= 1000) snprintf(s, n, "%.1f kHz", rbw / 1000);
    else snprintf(s, n, "%.0f Hz", rbw);
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_header(const spec_frame_t *f)
{
    char s[64];
    if (!gfx_rows_visible(0, HDR_H)) return;
    gfx_fill(0, 0, LCD_W, HDR_H, C_INK);
    gfx_hline(0, HDR_H - 1, LCD_W, C_BORDER);
    gfx_text(&font_title, 10, 4, "eSpDRmini", C_CYAN, ALIGN_LEFT);
    gfx_text(&font_small, 11, 33, "ESP32-S3 · touch", C_MUTED2, ALIGN_LEFT);

    snprintf(s, sizeof(s), "%u MHz", P.rx.lo_mhz);
    int w = gfx_text(&font_big, 168, 1, s, C_TEXT, ALIGN_LEFT);
    gfx_text(&font_small, 170 + w + 8, 8, "LO", C_MUTED2, ALIGN_LEFT);
    char rbw[16];
    fmt_rbw(rbw, sizeof(rbw), radio_rate_hz(P.rx.rate), P.rx.nfft);
    unsigned half = span_mhz() / 2;
    snprintf(s, sizeof(s), "%u – %u MHz  ·  RBW %s", P.rx.lo_mhz - half, P.rx.lo_mhz + half, rbw);
    gfx_text(&font_small, 170, 35, s, C_MUTED, ALIGN_LEFT);

    /* Level meter: 25 blocks over -60..0 dBFS, as in the desktop viewer. */
    float level = f ? f->rms_dbfs : -100;
    bool clip = f && f->clip_pct > 0.1f;
    int lit = (int)lroundf((level + 60.f) / 60.f * 25.f);
    for (int i = 0; i < 25; i++) {
        float db = -60.f + (i + 0.5f) * 2.4f;
        color_t c = i >= lit ? C_METER_OFF : db > -4 ? C_RED : db > -10 ? C_YELLOW : C_BLUE;
        gfx_fill(440 + i * 8, 12, 6, 16, c);
    }
    if (f) snprintf(s, sizeof(s), "ADC rms  ·  clipped %.2f %%", f->clip_pct);
    else snprintf(s, sizeof(s), "ADC rms");
    gfx_text(&font_small, 440, 35, s, clip ? C_RED : C_MUTED2, ALIGN_LEFT);

    if (f) snprintf(s, sizeof(s), "%.1f dBFS", level);
    else snprintf(s, sizeof(s), "--.- dBFS");
    gfx_text(&font_mono_big, 792, 3, s, clip ? C_RED : C_CYAN, ALIGN_RIGHT);
    if (f) snprintf(s, sizeof(s), "%.1f snap/s  ·  %.0f fps", f->snaps_per_s, fps);
    else snprintf(s, sizeof(s), "starting receiver");
    gfx_text(&font_small, 792, 35, s, C_MUTED2, ALIGN_RIGHT);
}

static uint32_t bar_sig = ~0u;
static uint32_t last_seq;
static int64_t last_draw;

/* Plot contents, prepared once per frame and composited strip by strip.
 * Arrays are indexed by plot column; y < 0 means no data in that column. */
static struct {
    bool usable;
    int shift;
    color_t bg[SPEC_W], under[SPEC_W], col_avg[SPEC_W];
    int16_t y_avg[SPEC_W];
    int16_t a_avg[SPEC_W], b_avg[SPEC_W], a_now[SPEC_W], b_now[SPEC_W], a_hold[SPEC_W], b_hold[SPEC_W];
    uint8_t grid[SPEC_H];
} PL;

static void band(double lo_mhz, double hi_mhz, color_t c)
{
    int x0 = mhz_to_x(lo_mhz) - PLOT_X, x1 = mhz_to_x(hi_mhz) - PLOT_X;
    if (x1 <= x0) x1 = x0 + 1;
    for (int x = x0 < 0 ? 0 : x0; x < x1 && x < SPEC_W; x++) PL.bg[x] = c;
}

static void prepare_plot(const spec_frame_t *f)
{
    for (int x = 0; x < SPEC_W; x++) PL.bg[x] = C_INK2;
    if (P.overlay == 1) {
        static const int main_ch[3] = {2412, 2437, 2462};
        for (int i = 0; i < 3; i++) band(main_ch[i] - 10, main_ch[i] + 10, C_BAND);
    } else if (P.overlay == 2) {
        static const int adv[3] = {2402, 2426, 2480};
        for (int i = 0; i < 3; i++) band(adv[i] - 1, adv[i] + 1, C_BAND_BLE);
    }
    for (int x = 0; x < SPEC_W; x++) PL.under[x] = gfx_mix(PL.bg[x], C_CYAN, 3);

    memset(PL.grid, 0, sizeof(PL.grid));
    int top = (int)floorf(P.ceil_db / 10.f) * 10;
    for (int db = top; db >= P.floor_db; db -= 10) PL.grid[db_to_y(db) - SPEC_Y] = 1;

    PL.usable = f && f->rate_hz == radio_rate_hz(P.rx.rate);
    static int16_t y_now[SPEC_W], y_hold[SPEC_W];
    for (int x = 0; x < SPEC_W; x++) PL.y_avg[x] = y_now[x] = y_hold[x] = -1;
    if (PL.usable) {
        PL.shift = frame_shift(f->lo_mhz);
        for (int c = 0; c < SPEC_W; c++) {
            int x = c + PL.shift;
            if (x < 0 || x >= SPEC_W) continue;
            PL.y_avg[x] = db_to_y(f->avg[c]);
            y_now[x] = db_to_y(f->now[c]);
            y_hold[x] = db_to_y(f->hold[c]);
            PL.col_avg[x] = trace_color(f->avg[c]);
        }
    }
    trace_spans(PL.y_avg, SPEC_W, PL.a_avg, PL.b_avg);
    trace_spans(y_now, SPEC_W, PL.a_now, PL.b_now);
    trace_spans(y_hold, SPEC_W, PL.a_hold, PL.b_hold);
}

static void draw_overlay_labels(void)
{
    char s[8];
    double lo = P.rx.lo_mhz - span_mhz() / 2.0, hi = lo + span_mhz();
    if (P.overlay == 1) {
        for (int ch = 1; ch <= 14; ch++) {
            double c = ch == 14 ? 2484 : 2407 + 5 * ch;
            if (c < lo || c > hi) continue;
            bool main = ch == 1 || ch == 6 || ch == 11;
            int x = mhz_to_x(c);
            snprintf(s, sizeof(s), "%d", ch);
            gfx_vline(x, SPEC_Y + 1, 5, main ? C_GREEN : C_MUTED2);
            gfx_text(main ? &font_bold : &font_small, x, SPEC_Y + 6, s, main ? C_GREEN : C_MUTED2, ALIGN_CENTER);
        }
    } else if (P.overlay == 2) {
        for (int k = 0; k < 40; k++) {
            int mhz = 2402 + 2 * k;
            if (mhz < lo || mhz > hi) continue;
            int ch = k == 0 ? 37 : k == 12 ? 38 : k == 39 ? 39 : k < 12 ? k - 1 : k - 2;
            bool adv = ch >= 37;
            int x = mhz_to_x(mhz);
            gfx_vline(x, SPEC_Y + 1, adv ? 6 : 3, adv ? C_PURPLE : C_MUTED2);
            if (adv || span_mhz() <= 40) {
                snprintf(s, sizeof(s), "%d", ch);
                if (adv || ch % 5 == 0)
                    gfx_text(adv ? &font_bold : &font_small, x, SPEC_Y + 7, s, adv ? C_PURPLE : C_MUTED2,
                             ALIGN_CENTER);
            }
        }
    }
}

static unsigned grid_step_mhz(void)
{
    return span_mhz() >= 80 ? 10 : span_mhz() >= 40 ? 5 : 2;
}

static void draw_vgrid(void)
{
    double lo = P.rx.lo_mhz - span_mhz() / 2.0;
    unsigned step = grid_step_mhz();
    for (unsigned m = ((unsigned)ceil(lo) + step - 1) / step * step; m <= lo + span_mhz(); m += step) {
        int x = mhz_to_x(m);
        if (x > PLOT_X && x < PLOT_X + SPEC_W) gfx_vline(x, SPEC_Y, SPEC_H, C_GRID);
    }
}

static void draw_markers(const spec_frame_t *f, int shift)
{
    static const color_t mc[2] = {C_CYAN, C_PURPLE};
    float level[2] = {0, 0};
    bool shown[2] = {false, false};
    for (int i = 0; i < 2; i++) {
        if (!mk[i].on) continue;
        int x = mhz_to_x(mk[i].mhz);
        int c = x - PLOT_X - shift;
        if (x < PLOT_X || x >= PLOT_X + SPEC_W || c < 0 || c >= SPEC_W || !f) continue;
        shown[i] = true;
        level[i] = f->avg[c];
        int y = db_to_y(level[i]);
        gfx_vdash(x, SPEC_Y, SPEC_H, 3, 3, mc[i]);
        for (int r = 0; r < 6; r++) gfx_hline(x - 5 + r, y - 9 + r, 11 - 2 * r, mc[i]);
        gfx_text(&font_bold, x, y - 28 < SPEC_Y + 18 ? y + 4 : y - 28, i ? "M2" : "M1", mc[i], ALIGN_CENTER);
    }
    if (!shown[0] && !shown[1]) return;
    char s[64];
    int lines = shown[0] + shown[1] + (shown[0] && shown[1]);
    int bx = PLOT_X + 6, by = SPEC_Y + 24, bw = 300, lh = font_mono.height + 1;
    gfx_blend(bx, by, bw, lines * lh + 8, C_INK, 26);
    gfx_rect(bx, by, bw, lines * lh + 8, C_BORDER);
    int y = by + 4;
    for (int i = 0; i < 2; i++) {
        if (!shown[i]) continue;
        snprintf(s, sizeof(s), "M%d %9.3f MHz %7.1f dBFS", i + 1, mk[i].mhz, level[i]);
        gfx_text(&font_mono, bx + 6, y, s, mc[i], ALIGN_LEFT);
        y += lh;
    }
    if (shown[0] && shown[1]) {
        snprintf(s, sizeof(s), "Δ  %+9.3f MHz %7.1f dB", mk[1].mhz - mk[0].mhz, level[0] - level[1]);
        gfx_text(&font_mono, bx + 6, y, s, C_TEXT, ALIGN_LEFT);
    }
}

static void draw_spectrum(const spec_frame_t *f)
{
    char s[16];
    if (!gfx_rows_visible(HDR_H, AXIS_Y - HDR_H)) return;
    gfx_fill(0, HDR_H, PLOT_X, AXIS_Y - HDR_H, C_INK);
    gfx_fill(PLOT_X, HDR_H, LCD_W - PLOT_X, SPEC_Y - HDR_H, C_INK);
    gfx_fill(PLOT_X + SPEC_W, SPEC_Y, LCD_W - PLOT_X - SPEC_W, SPEC_H, C_INK);
    render_plot(PLOT_X, SPEC_Y, SPEC_W, SPEC_H, PL.bg, PL.under, PL.y_avg, PL.grid, C_GRID);
    gfx_clip(PLOT_X, SPEC_Y, SPEC_W, SPEC_H);
    draw_vgrid();
    if (PL.usable) {
        if (P.show_now) render_spans(PLOT_X, SPEC_W, PL.a_now, PL.b_now, NULL, gfx_mix(C_INK2, C_YELLOW, 15), 1);
        if (P.show_hold) render_spans(PLOT_X, SPEC_W, PL.a_hold, PL.b_hold, NULL, gfx_mix(C_INK2, C_RED, 26), 1);
        render_spans(PLOT_X, SPEC_W, PL.a_avg, PL.b_avg, PL.col_avg, 0, 2);
    }
    draw_overlay_labels();
    gfx_noclip();
    gfx_rect(PLOT_X - 1, SPEC_Y - 1, SPEC_W + 2, SPEC_H + 2, C_BORDER);

    int top = (int)floorf(P.ceil_db / 10.f) * 10;
    for (int db = top; db >= P.floor_db; db -= 10) {
        int y = db_to_y(db);
        snprintf(s, sizeof(s), "%d", db);
        gfx_text(&font_small, PLOT_X - 4, y - font_small.height / 2, s, C_MUTED2, ALIGN_RIGHT);
    }
    if (PL.usable) draw_markers(f, PL.shift);
    else if (!f) gfx_text(&font_body, LCD_W / 2, SPEC_Y + SPEC_H / 2 - 10, "Starting receiver…", C_MUTED,
                          ALIGN_CENTER);
    if (!menu_open && !keypad_open) hit_add(ID(G_SPEC, 0), PLOT_X - 30, SPEC_Y, SPEC_W + 30, SPEC_H);
}

static void draw_axis(void)
{
    char s[16];
    if (!gfx_rows_visible(AXIS_Y, WF_Y - AXIS_Y)) return;
    gfx_fill(0, AXIS_Y, LCD_W, WF_Y - AXIS_Y, C_INK);
    double lo = P.rx.lo_mhz - span_mhz() / 2.0;
    unsigned step = grid_step_mhz();
    for (unsigned m = ((unsigned)ceil(lo) + step - 1) / step * step; m <= lo + span_mhz(); m += step) {
        int x = mhz_to_x(m);
        if (x < PLOT_X + 12 || x > PLOT_X + SPEC_W - 30) continue;
        snprintf(s, sizeof(s), "%u", m);
        gfx_vline(x, AXIS_Y + 1, 3, C_MUTED2);
        gfx_text(&font_small, x, AXIS_Y + 3, s, C_MUTED, ALIGN_CENTER);
    }
    gfx_text(&font_small, PLOT_X + SPEC_W, AXIS_Y + 3, "MHz", C_MUTED2, ALIGN_RIGHT);
}

static uint32_t wf_count; /* history rows, read once per frame */


static void draw_waterfall(void)
{
    if (!gfx_rows_visible(WF_Y, BAR_Y - WF_Y)) return;
    gfx_fill(0, WF_Y, PLOT_X, WF_H, C_INK);
    /* Colour scale, ceiling at the top. */
    for (int y = 0; y < WF_H; y++) {
        float db = P.ceil_db - (P.ceil_db - P.floor_db) * y / (WF_H - 1);
        gfx_hline(PLOT_X - 12, WF_Y + y, 6, wf_lut[WF_LEVEL(db)]);
    }
    unsigned rows = wf_count < WF_ROWS ? wf_count : WF_ROWS;
    for (unsigned r = 0; r < WF_H; r++) {
        uint16_t *dst = gfx_row(WF_Y + r);
        if (!dst) continue;
        unsigned lo = 0;
        const uint8_t *src = r < rows ? sdr_wf_row(wf_count, r, &lo) : NULL;
        render_wf_line(dst + PLOT_X, SPEC_W, src, src ? frame_shift(lo) : 0, wf_lut, C_INK);
    }
    for (int i = 0; i < 2; i++) {
        int x = mhz_to_x(mk[i].mhz);
        if (mk[i].on && x >= PLOT_X && x < PLOT_X + SPEC_W) gfx_vdash(x, WF_Y, WF_H, 3, 3, C_TEXT);
    }
    gfx_fill(PLOT_X + SPEC_W, WF_Y, LCD_W - PLOT_X - SPEC_W, WF_H, C_INK);
    gfx_fill(0, WF_Y + WF_H, LCD_W, BAR_Y - WF_Y - WF_H, C_INK);
    if (!menu_open && !keypad_open) hit_add(ID(G_WFALL, 0), PLOT_X, WF_Y, SPEC_W, WF_H);
}

#define BAR_BW 75
#define BAR_X(i) (7 + (i) * (BAR_BW + 4))
#define BAR_BY (BAR_Y + 4)
#define BAR_BH (LCD_H - BAR_Y - 8)

static void bar_hits(void)
{
    for (int i = 0; i < 10; i++) hit_add(ID(G_BAR, i), BAR_X(i), BAR_BY, BAR_BW, BAR_BH);
}

static void draw_bar(void)
{
    char cap[24], val[16];
    if (!gfx_rows_visible(BAR_Y, LCD_H - BAR_Y)) return;
    gfx_fill(0, BAR_Y, LCD_W, LCD_H - BAR_Y, C_INK);
    gfx_hline(0, BAR_Y, LCD_W, C_BORDER);
    const int bw = BAR_BW, y = BAR_BY, h = BAR_BH;
    for (int i = 0; i < 10; i++) {
        int x = BAR_X(i);
        switch (i) {
        case 0: snprintf(cap, sizeof(cap), "−%u MHz", step_big()); button(ID(G_BAR, i), x, y, bw, h, cap, "◀◀", false); break;
        case 1: snprintf(cap, sizeof(cap), "−%u MHz", step_small()); button(ID(G_BAR, i), x, y, bw, h, cap, "◀", false); break;
        case 2: snprintf(val, sizeof(val), "%u", P.rx.lo_mhz); button(ID(G_BAR, i), x, y, bw, h, "LO MHz", val, keypad_open); break;
        case 3: snprintf(cap, sizeof(cap), "+%u MHz", step_small()); button(ID(G_BAR, i), x, y, bw, h, cap, "▶", false); break;
        case 4: snprintf(cap, sizeof(cap), "+%u MHz", step_big()); button(ID(G_BAR, i), x, y, bw, h, cap, "▶▶", false); break;
        case 5: snprintf(val, sizeof(val), "%u MHz", span_mhz()); button(ID(G_BAR, i), x, y, bw, h, "SPAN", val, false); break;
        case 6: button(ID(G_BAR, i), x, y, bw, h, "GAIN", "−", false); break;
        case 7:
            if (P.rx.agc) snprintf(val, sizeof(val), "AGC");
            else snprintf(val, sizeof(val), "%u", P.rx.gain);
            button(ID(G_BAR, i), x, y, bw, h, P.rx.agc ? "auto" : "manual", val, P.rx.agc);
            break;
        case 8: button(ID(G_BAR, i), x, y, bw, h, "GAIN", "+", false); break;
        case 9: button(ID(G_BAR, i), x, y, bw, h, P.rx.paused ? "paused" : "", menu_open ? "Close" : "Menu", menu_open); break;
        }
    }
}

static const unsigned FFTS[] = {256, 512, 1024, 2048};
static const float AVGS[] = {0, 0.5f, 0.7f, 0.85f, 0.95f};
static const unsigned RATES[] = {5, 10, 25, 50};

static void menu_label(int x, int y, int h, const char *text)
{
    gfx_text(&font_label, x, y + (h - font_label.height) / 2, text, C_MUTED, ALIGN_LEFT);
}

static void draw_menu(const spec_frame_t *f)
{
    char s[160];
    int x = 0, y = MENU_Y, w = LCD_W, h = MENU_H;
    gfx_fill(x, y, w, h, C_INK2);
    gfx_hline(x, y + h - 1, w, C_BORDER);
    hit_add(ID(G_MENU, 0), x, y, w, h);

    const int L = 10, R = 406, lw = 84, cw = 384, bh = 48, row = 56;
    int ry = y + 12;
    static const char *fft_l[] = {"256", "512", "1k", "2k"};
    int ai = 0;
    for (int i = 0; i < 4; i++) if (FFTS[i] == P.rx.nfft) ai = i;
    menu_label(L, ry, bh, "FFT size");
    segments(G_FFT, L + lw, ry, cw - lw, bh, fft_l, 4, ai);
    static const char *ov_l[] = {"Off", "Wi-Fi", "BLE"};
    menu_label(R, ry, bh, "Overlay");
    segments(G_OV, R + lw, ry, cw - lw, bh, ov_l, 3, P.overlay);

    ry += row;
    static const char *avg_l[] = {"Off", ".5", ".7", ".85", ".95"};
    ai = 0;
    for (int i = 0; i < 5; i++) if (fabsf(AVGS[i] - P.rx.avg) < 0.01f) ai = i;
    menu_label(L, ry, bh, "Averaging");
    segments(G_AVG, L + lw, ry, cw - lw, bh, avg_l, 5, ai);
    static const char *wf_l[] = {"Peak", "Average", "Clear"};
    menu_label(R, ry, bh, "Waterfall");
    segments(G_WF, R + lw, ry, cw - lw, bh, wf_l, 3, P.rx.wf_avg ? 1 : 0);

    ry += row;
    static const char *rate_l[] = {"5", "10", "25", "50"};
    ai = 0;
    for (int i = 0; i < 4; i++) if (RATES[i] == P.rx.snaps_per_s) ai = i;
    menu_label(L, ry, bh, "Snaps/s");
    segments(G_RATE, L + lw, ry, cw - lw, bh, rate_l, 4, ai);
    menu_label(R, ry, bh, "Traces");
    int tw = (cw - lw - 12) / 3;
    button(ID(G_TRACE, 0), R + lw, ry, tw, bh, NULL, "Hold", P.show_hold);
    button(ID(G_TRACE, 1), R + lw + tw + 6, ry, tw, bh, NULL, "Snapshot", P.show_now);
    button(ID(G_TRACE, 2), R + lw + 2 * (tw + 6), ry, tw, bh, NULL, "Reset", false);

    ry += row;
    int vw = cw - lw - 2 * 56 - 12;
    menu_label(L, ry, bh, "Floor");
    button(ID(G_FLOOR, 0), L + lw, ry, 56, bh, NULL, "−5", false);
    snprintf(s, sizeof(s), "%.0f dBFS", P.floor_db);
    gfx_text(&font_bold, L + lw + 62 + vw / 2, ry + (bh - font_bold.height) / 2, s, C_TEXT, ALIGN_CENTER);
    button(ID(G_FLOOR, 1), L + lw + 68 + vw, ry, 56, bh, NULL, "+5", false);
    menu_label(R, ry, bh, "Ceiling");
    button(ID(G_CEIL, 0), R + lw, ry, 56, bh, NULL, "−5", false);
    snprintf(s, sizeof(s), "%.0f dBFS", P.ceil_db);
    gfx_text(&font_bold, R + lw + 62 + vw / 2, ry + (bh - font_bold.height) / 2, s, C_TEXT, ALIGN_CENTER);
    button(ID(G_CEIL, 1), R + lw + 68 + vw, ry, 56, bh, NULL, "+5", false);

    ry += row;
    int aw = (LCD_W - 2 * L - 4 * 8) / 5;
    static const char *act_l[] = {"Pause", "Auto scale", "M1 → peak", "Clear markers", "Close"};
    for (int i = 0; i < 5; i++)
        button(ID(G_ACT, i), L + i * (aw + 8), ry, aw, bh, NULL, i == 0 && P.rx.paused ? "Run" : act_l[i],
               i == 0 && P.rx.paused);

    ry += row;
    if (f)
        snprintf(s, sizeof(s), "%u pairs · %u-pt FFT × %u segments · capture %u µs · DSP %.1f ms · %.1f snap/s · "
                 "%u errors · heap %u kB",
                 f->pairs, f->nfft, f->segs, (unsigned)f->capture_us, f->dsp_us / 1000.f, f->snaps_per_s,
                 (unsigned)f->errors, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    else
        snprintf(s, sizeof(s), "receiver starting");
    gfx_text(&font_small, L, ry, s, C_MUTED2, ALIGN_LEFT);
    gfx_text(&font_small, L, ry + 18,
             "Drag the spectrum sideways to tune, up or down to shift the scale. Tap: markers. "
             "Double-tap: centre. Hold: clear markers.",
             C_MUTED2, ALIGN_LEFT);
}

static const unsigned PRESETS[] = {2412, 2437, 2442, 2462, 2480};
static const char *const PRESET_L[] = {"Wi-Fi 1", "Wi-Fi 6", "2.4 GHz band", "Wi-Fi 11", "BLE 39"};

static void draw_keypad(void)
{
    int x = 150, y = MENU_Y + 6, w = 500, h = MENU_H - 12;
    gfx_round_fill(x, y, w, h, 10, C_INK2);
    gfx_round_rect(x, y, w, h, 10, C_CYAN);
    hit_add(ID(G_KEYPAD, 0), x, y, w, h);
    char s[32];
    snprintf(s, sizeof(s), "%s%s MHz", keypad, (esp_timer_get_time() / 500000) & 1 ? "_" : " ");
    gfx_fill(x + 16, y + 14, 300, 44, C_PANEL);
    gfx_rect(x + 16, y + 14, 300, 44, C_BORDER);
    gfx_text(&font_big, x + 26, y + 20, s, C_TEXT, ALIGN_LEFT);
    snprintf(s, sizeof(s), "LO %u – %u MHz", RADIO_FREQ_MIN, RADIO_FREQ_MAX);
    gfx_text(&font_small, x + 16, y + 62, s, C_MUTED2, ALIGN_LEFT);
    static const char *keys[12] = {"7", "8", "9", "4", "5", "6", "1", "2", "3", "⌫", "0", "OK"};
    const int kw = 92, kh = 58, kg = 8, kx = x + 16, ky = y + 82;
    for (int i = 0; i < 12; i++)
        button(ID(G_KEY, i), kx + (i % 3) * (kw + kg), ky + (i / 3) * (kh + kg), kw, kh, NULL, keys[i], i == 11);
    int px = x + 16 + 3 * (kw + kg) + 6, pw = x + w - 16 - px;
    gfx_text(&font_small, px, y + 18, "Presets", C_MUTED2, ALIGN_LEFT);
    for (int i = 0; i < 5; i++) {
        snprintf(s, sizeof(s), "%u", PRESETS[i]);
        button(ID(G_PRESET, i), px, y + 36 + i * 50, pw, 44, PRESET_L[i], s, P.rx.lo_mhz == PRESETS[i]);
    }
    button(ID(G_PRESET, 5), px, ky + 3 * (kh + kg), pw, kh, NULL, "Cancel", false);
}

static void draw_toast(void)
{
    if (esp_timer_get_time() > toast_until) return;
    int w = gfx_text_width(&font_body, toast) + 32;
    int x = (LCD_W - w) / 2, y = AXIS_Y - 48;
    gfx_round_fill(x, y, w, 34, 8, C_RAISED);
    gfx_round_rect(x, y, w, 34, 8, toast_color);
    gfx_text(&font_body, LCD_W / 2, y + (34 - font_body.height) / 2, toast, toast_color, ALIGN_CENTER);
}

/* ---- actions -------------------------------------------------------------- */

static bool repeats(int id)
{
    int g = GROUP(id), i = INDEX(id);
    return (g == G_BAR && (i == 0 || i == 1 || i == 3 || i == 4 || i == 6 || i == 8)) || g == G_FLOOR ||
           g == G_CEIL;
}

static void auto_scale(const spec_frame_t *f)
{
    if (!f) return;
    float sorted[SPEC_W];
    memcpy(sorted, f->avg, sizeof(sorted));
    /* Noise floor: a low percentile of the average trace. */
    for (int i = 1; i < SPEC_W; i++) {
        float v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = v;
    }
    float noise = sorted[SPEC_W / 10], top = sorted[SPEC_W - 1];
    for (int c = 0; c < SPEC_W; c++) if (f->hold[c] > top) top = f->hold[c];
    P.floor_db = floorf((noise - 10.f) / 5.f) * 5.f;
    P.ceil_db = ceilf((top + 5.f) / 5.f) * 5.f;
    if (P.ceil_db - P.floor_db < 30) P.ceil_db = P.floor_db + 30;
    if (P.floor_db < -128) P.floor_db = -125;
    if (P.ceil_db > 0) P.ceil_db = 0;
    prefs_dirty_at = esp_timer_get_time();
}

static void marker_to_peak(const spec_frame_t *f, int k, int c_lo, int c_hi)
{
    if (!f) return;
    int shift = frame_shift(f->lo_mhz);
    if (c_lo < 0) c_lo = 0;
    if (c_hi > SPEC_W - 1) c_hi = SPEC_W - 1;
    int best = -1;
    for (int c = c_lo; c <= c_hi; c++) {
        int x = PLOT_X + c + shift;
        if (x < PLOT_X || x >= PLOT_X + SPEC_W) continue;
        if (best < 0 || f->avg[c] > f->avg[best]) best = c;
    }
    if (best < 0) return;
    /* The column's strongest FFT bin gives the frequency to the RBW. */
    double rate = f->rate_hz / 1e6;
    mk[k].mhz = f->lo_mhz + ((double)f->peak_bin[best] - f->nfft / 2.0) * rate / f->nfft;
    mk[k].on = true;
}

static void open_keypad(void)
{
    keypad_open = true;
    menu_open = false;
    keypad[0] = 0;
}

static void act(int id, const spec_frame_t *f)
{
    int g = GROUP(id), i = INDEX(id);
    switch (g) {
    case G_BAR:
        switch (i) {
        case 0: set_lo(P.rx.lo_mhz - step_big()); break;
        case 1: set_lo(P.rx.lo_mhz - step_small()); break;
        case 2: if (keypad_open) keypad_open = false; else open_keypad(); break;
        case 3: set_lo(P.rx.lo_mhz + step_small()); break;
        case 4: set_lo(P.rx.lo_mhz + step_big()); break;
        case 5:
            P.rx.rate = P.rx.rate == RADIO_RATE_80 ? RADIO_RATE_40 : P.rx.rate == RADIO_RATE_40 ? RADIO_RATE_16 : RADIO_RATE_80;
            changed();
            break;
        case 6: case 8: {
            if (P.rx.agc) {
                P.rx.agc = false;
            } else {
                int gmax = radio_gain_max() ? (int)radio_gain_max() : 82;
                int gn = (int)P.rx.gain + (i == 8 ? 1 : -1);
                P.rx.gain = gn < 0 ? 0 : gn > gmax ? gmax : gn;
            }
            changed();
            break;
        }
        case 7:
            P.rx.agc = !P.rx.agc;
            changed();
            show_toast(P.rx.agc ? "Hardware AGC: follows Wi-Fi packets; may clip on quiet bands" : "Manual gain",
                       C_CYAN);
            break;
        case 9: menu_open = !menu_open; keypad_open = false; break;
        }
        break;
    case G_FFT: P.rx.nfft = FFTS[i]; changed(); break;
    case G_AVG: P.rx.avg = AVGS[i]; changed(); break;
    case G_RATE: P.rx.snaps_per_s = RATES[i]; changed(); break;
    case G_WF:
        if (i == 2) sdr_wf_clear();
        else { P.rx.wf_avg = i == 1; changed(); }
        break;
    case G_OV: P.overlay = i; prefs_dirty_at = esp_timer_get_time(); break;
    case G_TRACE:
        if (i == 0) P.show_hold = !P.show_hold;
        else if (i == 1) P.show_now = !P.show_now;
        else { P.rx.reset++; sdr_set(&P.rx); }
        prefs_dirty_at = esp_timer_get_time();
        break;
    case G_FLOOR:
        P.floor_db += i ? 5 : -5;
        if (P.floor_db < -125) P.floor_db = -125;
        if (P.floor_db > P.ceil_db - 10) P.floor_db = P.ceil_db - 10;
        prefs_dirty_at = esp_timer_get_time();
        break;
    case G_CEIL:
        P.ceil_db += i ? 5 : -5;
        if (P.ceil_db > 0) P.ceil_db = 0;
        if (P.ceil_db < P.floor_db + 10) P.ceil_db = P.floor_db + 10;
        prefs_dirty_at = esp_timer_get_time();
        break;
    case G_ACT:
        if (i == 0) { P.rx.paused = !P.rx.paused; sdr_set(&P.rx); }
        else if (i == 1) auto_scale(f);
        else if (i == 2) marker_to_peak(f, 0, 0, SPEC_W - 1);
        else if (i == 3) mk[0].on = mk[1].on = false;
        else menu_open = false;
        break;
    case G_KEY: {
        size_t n = strlen(keypad);
        if (i == 9) { if (n) keypad[n - 1] = 0; }
        else if (i == 11) {
            int v = atoi(keypad);
            if (v >= (int)RADIO_FREQ_MIN && v <= (int)RADIO_FREQ_MAX) {
                set_lo(v);
                keypad_open = false;
            } else {
                char s[64];
                snprintf(s, sizeof(s), "Enter %u – %u MHz", RADIO_FREQ_MIN, RADIO_FREQ_MAX);
                show_toast(s, C_ORANGE);
            }
        } else if (n < 4) {
            static const char digit[] = "789456123-0-";
            keypad[n] = digit[i];
            keypad[n + 1] = 0;
        }
        break;
    }
    case G_PRESET:
        if (i < 5) {
            set_lo(PRESETS[i]);
            if (i == 2 && P.rx.rate != RADIO_RATE_80) { P.rx.rate = RADIO_RATE_80; changed(); }
        }
        keypad_open = false;
        break;
    }
}

/* Plot gestures: drag sideways to tune, vertically on the spectrum to shift
 * the level scale; tap for markers, double-tap to centre, hold to clear. */
static void plot_move(void)
{
    int dx = T.x - T.x0, dy = T.y - T.y0;
    if (T.mode == 0) {
        if (abs(dx) >= 12 && abs(dx) >= abs(dy)) T.mode = 1;
        else if (abs(dy) >= 12 && GROUP(T.id) == G_SPEC) T.mode = 2;
    }
    if (T.mode == 1) {
        set_lo((int)T.lo0 - (int)lroundf((float)dx * span_mhz() / SPEC_W));
    } else if (T.mode == 2) {
        float d = (float)dy * (T.ceil0 - T.floor0) / SPEC_H;
        d = roundf(d);
        if (T.ceil0 + d > 0) d = -T.ceil0;
        if (T.floor0 + d < -125) d = -125 - T.floor0;
        P.floor_db = T.floor0 + d;
        P.ceil_db = T.ceil0 + d;
        prefs_dirty_at = esp_timer_get_time();
    } else if (T.mode == 0 && esp_timer_get_time() - T.t0 > 700000) {
        if (mk[0].on || mk[1].on) show_toast("Markers cleared", C_CYAN);
        mk[0].on = mk[1].on = false;
        T.mode = 3;
    }
}

static void plot_tap(const spec_frame_t *f)
{
    int64_t now = esp_timer_get_time();
    bool dbl = now - T.last_tap < 350000 && abs(T.x - T.last_tap_x) < 40 && abs(T.y - T.last_tap_y) < 40;
    if (dbl) {
        if (T.last_tap_marker >= 0) mk[T.last_tap_marker].on = false;
        set_lo((int)lround(x_to_mhz(T.x)));
        T.last_tap = 0;
        return;
    }
    T.last_tap = now;
    T.last_tap_x = T.x;
    T.last_tap_y = T.y;
    T.last_tap_marker = -1;
    if (GROUP(T.id) != G_SPEC || !f) return;
    int k;
    if (!mk[0].on) k = 0;
    else if (!mk[1].on) k = 1;
    else k = fabs(x_to_mhz(T.x) - mk[0].mhz) <= fabs(x_to_mhz(T.x) - mk[1].mhz) ? 0 : 1;
    int c = T.x - PLOT_X - frame_shift(f->lo_mhz);
    marker_to_peak(f, k, c - 6, c + 6);
    T.last_tap_marker = mk[k].on ? k : -1;
}

static void handle_touch(const touch_t *t, const spec_frame_t *f)
{
    int64_t now = esp_timer_get_time();
    bool down = t->n > 0;
    if (down && !T.down) {
        T.down = true;
        T.x = T.x0 = t->x[0];
        T.y = T.y0 = t->y[0];
        T.t0 = T.t_repeat = now;
        T.id = hit_test(T.x, T.y);
        T.mode = 0;
        T.lo0 = P.rx.lo_mhz;
        T.floor0 = P.floor_db;
        T.ceil0 = P.ceil_db;
        if (repeats(T.id)) act(T.id, f);
    } else if (down) {
        T.x = t->x[0];
        T.y = t->y[0];
        int g = GROUP(T.id);
        if (g == G_SPEC || g == G_WFALL) plot_move();
        else if (repeats(T.id) && now - T.t0 > 450000 && now - T.t_repeat > 120000) {
            T.t_repeat = now;
            if (hit_test(T.x, T.y) == T.id) act(T.id, f);
        }
    } else if (T.down) {
        T.down = false;
        int g = GROUP(T.id);
        if (g == G_SPEC || g == G_WFALL) {
            if (T.mode == 0) plot_tap(f);
        } else if (!repeats(T.id) && T.id && hit_test(T.x, T.y) == T.id && g != G_MENU && g != G_KEYPAD) {
            act(T.id, f);
        }
    }
}

/* ---- task --------------------------------------------------------------- */

void ui_inject_touch(int n, int x, int y)
{
    portENTER_CRITICAL(&inject_lock);
    inject_on = true;
    inject.n = n;
    inject.x[0] = x;
    inject.y[0] = y;
    portEXIT_CRITICAL(&inject_lock);
}

const uint16_t *ui_screenshot(int timeout_ms)
{
    xSemaphoreTake(shot_done, 0);
    shot_wanted = true;
    return xSemaphoreTake(shot_done, pdMS_TO_TICKS(timeout_ms)) ? shot : NULL;
}

void ui_stats(char *out, int size)
{
    const spec_frame_t *f = sdr_frame_acquire();
    if (f)
        snprintf(out, size, "fps %.1f snaps %.1f lo %u rate %u nfft %u segs %u cap_us %u dsp_us %u rms %.1f clip %.2f "
                 "errors %u heap_int %u heap_psram %u",
                 fps, f->snaps_per_s, f->lo_mhz, f->rate_hz, f->nfft, f->segs, (unsigned)f->capture_us,
                 (unsigned)f->dsp_us, f->rms_dbfs, f->clip_pct, (unsigned)f->errors,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    else
        snprintf(out, size, "fps %.1f receiver starting", fps);
    sdr_frame_release();
    uint32_t bavg, bmax;
    board_bounce_stats(&bavg, &bmax);
    int n = strlen(out);
    snprintf(out + n, size - n, " draw_us %u dma_wait_us %u refill_us avg %u max %u late %u", (unsigned)t_draw,
             (unsigned)t_dma_wait, (unsigned)bavg, (unsigned)bmax, (unsigned)board_bounce_late());
}

static void ui_task(void *arg)
{
    board_init();
    touch_t touch = {0};
    int64_t last = esp_timer_get_time();
    bool lit = false;
    for (;;) {
        bool real = board_touch_read(&touch);
        portENTER_CRITICAL(&inject_lock);
        touch_t t = inject_on && !(real && touch.n) ? inject : touch;
        if (real && touch.n) inject_on = false;
        portEXIT_CRITICAL(&inject_lock);

        int64_t t0 = esp_timer_get_time();
        const spec_frame_t *f = sdr_frame_acquire();
        /* Redraw for new data, while touched (and once after), for toasts and
         * the keypad cursor; otherwise leave PSRAM bandwidth to the display. */
        bool touched = t.n > 0 || T.down;
        handle_touch(&t, f);
        uint32_t seq = f ? f->seq : 0;
        if (!touched && seq == last_seq && !shot_wanted && t0 > toast_until + 100000 && !keypad_open &&
            t0 - last_draw < 250000) {
            sdr_frame_release();
            board_wait_vsync(40);
            continue;
        }
        last_seq = seq;
        last_draw = t0;
        build_luts();
        prepare_plot(f);
        wf_count = sdr_wf_count();
        nhits = 0;
        bool shooting = shot_wanted;

        /* The button bar only changes with the settings it shows or a press:
         * otherwise its strips (and their copies) are skipped. */
        uint32_t sig = P.rx.lo_mhz ^ P.rx.rate << 12 ^ P.rx.gain << 16 ^ P.rx.agc << 23 ^ P.rx.paused << 24 ^
                       menu_open << 25 ^ keypad_open << 26 ^ (uint32_t)(T.down && GROUP(T.id) == G_BAR) << 27 ^
                       (uint32_t)INDEX(T.id) << 28;
        bool bar_same = sig == bar_sig && !shooting;
        bar_sig = sig;
        if (bar_same) bar_hits();

        /* Strips alternate between two buffers: draw one while DMA copies the
         * other into the frame buffer. */
        int64_t waited = 0, w0;
        int inflight = 0, bi = 0;
        for (int k = 0; k < LCD_H / STRIP_H; k++) {
            if (bar_same && k * STRIP_H >= BAR_Y - 4) continue;
            if (inflight == 2) {
                w0 = esp_timer_get_time();
                board_strip_wait();
                waited += esp_timer_get_time() - w0;
                inflight--;
            }
            gfx_begin(board_strip(bi), k * STRIP_H, STRIP_H);
            draw_header(f);
            draw_spectrum(f);
            draw_axis();
            draw_waterfall();
            draw_bar();
            if (menu_open) draw_menu(f);
            if (keypad_open) draw_keypad();
            draw_toast();
            if (shooting) memcpy(shot + k * STRIP_H * LCD_W, board_strip(bi), LCD_W * STRIP_H * 2);
            board_strip_send(bi, k * STRIP_H);
            inflight++;
            bi ^= 1;
        }
        w0 = esp_timer_get_time();
        while (inflight--) board_strip_wait();
        int64_t now = esp_timer_get_time();
        t_dma_wait = (uint32_t)(waited + now - w0);
        t_draw = (uint32_t)(now - t0);
        sdr_frame_release();
        if (shooting) {
            shot_wanted = false;
            xSemaphoreGive(shot_done);
        }
        if (!lit) {
            board_backlight(true);
            lit = true;
        }
        if (prefs_dirty_at && now - prefs_dirty_at > 3000000) {
            prefs_dirty_at = 0;
            prefs_save(&P);
        }
        float inst = 1e6f / (now - last);
        fps = fps ? 0.9f * fps + 0.1f * inst : inst;
        last = now;
        /* Start each frame at a refresh, so the strips overtake the scan-out
         * rather than tearing across it. */
        board_wait_vsync(40);
    }
}

void ui_start(const prefs_t *p)
{
    P = *p;
    board_prealloc();
    shot = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    shot_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(ui_task, "ui", 12288, NULL, 4, NULL, 1);
}
