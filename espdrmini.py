"""eSpDRmini: live spectrum and waterfall from eSpDR snapshots on an ESP32-S3 (no FPGA).

Works with two kinds of firmware (radios.py): eSpDR's snapshot mode on the
ESP32-S3, loaded into RAM automatically (1 to 4 capture banks of 15,360
contiguous IQ pairs per snapshot), or ESPARGOS esp-sdr flashed on any
supported ESP32 (16,380 pairs per snapshot, 2.4 GHz and on the C5 5 GHz). The display is a series of short looks
at the band, not a continuous stream, so brief bursts can fall between them.

Features: markers with delta (SNR), Wi-Fi/BLE channel overlays, a burst
trigger that saves snapshots, and a waterfall "DVR": drag a box over the
waterfall to save that band and time span as filtered, decimated IQ.
Recordings are SigMF in captures/.

    python espdrmini.py [--lo 2440] [--gain 45] [--rate 80]
"""
import argparse
import collections
import json
import logging
import os
import queue
import threading
import time

import matplotlib
import numpy as np

import dsp
import espctl
import radios

HERE = os.path.dirname(os.path.abspath(__file__))
CAPTURE_DIR = os.path.join(HERE, "captures")
PREFS = os.path.join(HERE, ".viewer_prefs.json")  # display choices kept between runs
FFT_SIZES = (1024, 2048, 4096, 8192)
DEFAULTS = {"meter": True, "overlay": 0, "fft": 2048, "avg": 0.7, "floor": -85.0, "ceiling": -15.0,
            "wf": 0, "trig_thr": -40.0, "trig_hold": 0.5, "trig_max": 50, "page": 0, "box_mode": 0,
            "banks": 1}

# TMOG palette (tmog.org :root).
INK, INK2, PANEL, RAISED = "#070b0f", "#0b1117", "#0e151c", "#111a22"
TEXT, MUTED, MUTED2 = "#edf5fa", "#91a1ac", "#60717d"
LINE = (0.643, 0.8, 0.878, 0.16)
BORDER = "#1d3a4c"
CYAN, BLUE, GREEN, LIME = "#16c5ff", "#078cff", "#45ed73", "#86ff35"
YELLOW, ORANGE, RED, PURPLE = "#ffe016", "#ff8a22", "#ff4f55", "#d248ff"
DISPLAY = ["Avenir Next", "Avenir", "Helvetica Neue", "DejaVu Sans", "sans-serif"]
MONO = ["Menlo", "DejaVu Sans Mono", "monospace"]


class Receiver(threading.Thread):
    """Owns the radio: applies queued settings and captures snapshots."""

    ORDER = ("rate", "agc", "gain", "lo")

    def __init__(self, radio, frames):
        super().__init__(daemon=True)
        self.radio, self.frames = radio, frames
        self.requests = queue.Queue()
        self.running = True
        self.paused = False
        self.banks = 1  # snapshot length in capture banks (eSpDR)
        self.error = None
        self.settings = radio.settings()

    def set(self, what, value):
        self.requests.put((what, value))

    def run(self):
        apply = {"rate": self.radio.set_rate, "agc": self.radio.set_agc, "gain": self.radio.set_gain,
                 "lo": self.radio.set_lo}
        while self.running:
            try:
                pending = {}
                while not self.requests.empty():  # a dragged slider sends many; keep the last
                    what, value = self.requests.get()
                    pending[what] = value
                for what in sorted(pending, key=self.ORDER.index):
                    try:
                        apply[what](pending[what])
                    except Exception as e:
                        self.error = f"setting refused: {e}"
                if pending:
                    self.settings = self.radio.settings()
                if self.paused:  # settings still apply; capture resumes on play
                    time.sleep(0.02)
                    continue
                iq = self.radio.snapshot(self.banks)
                frame = (iq, dict(self.settings), dsp.now_utc())
                try:
                    self.frames.put_nowait(frame)
                except queue.Full:
                    pass
            except Exception as e:  # keep the GUI alive and report
                self.error = str(e)
                time.sleep(0.5)


def duration(seconds):
    return f"{seconds * 1e6:.0f} µs" if seconds < 1e-3 else f"{seconds * 1e3:.2f} ms"


def load_prefs(path):
    prefs = dict(DEFAULTS)
    try:
        with open(path) as f:
            prefs.update({k: v for k, v in json.load(f).items() if k in DEFAULTS})
    except (OSError, ValueError):
        pass
    return prefs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first Espressif USB device)")
    ap.add_argument("--backend", choices=("auto", "espdr", "esp-sdr"), default="auto",
                    help="auto: use esp-sdr if the board runs it, else load eSpDR into RAM (S3)")
    ap.add_argument("--lo", type=float, default=2440.0, help="LO, MHz")
    ap.add_argument("--gain", type=int, help="manual gain index (eSpDR default 45; esp-sdr default: AGC)")
    ap.add_argument("--rate", type=float, default=80, help="MS/s (80 or 16 on eSpDR; esp-sdr per chip)")
    ap.add_argument("--rows", type=int, default=400, help="waterfall / DVR history, snapshots")
    ap.add_argument("--show-mac", action="store_true",
                    help="show the board's MAC address and record it in SigMF metadata")
    ap.add_argument("--reload", action="store_true", help="reload the firmware even if it is running")
    ap.add_argument("--prefs", default=PREFS, help="display preferences file")
    ap.add_argument("--screenshot", help="render a few seconds headless, save to this PNG and exit")
    args = ap.parse_args()

    logging.getLogger("matplotlib.font_manager").setLevel(logging.ERROR)
    if args.screenshot:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import patheffects
    from matplotlib.collections import LineCollection
    from matplotlib.colors import LinearSegmentedColormap, Normalize
    from matplotlib.patches import Circle, Polygon, Rectangle
    from matplotlib.transforms import blended_transform_factory
    from matplotlib.widgets import (Button, CheckButtons, RectangleSelector, Slider, SpanSelector,
                                    TextBox, Widget)

    plt.rcParams.update({
        "font.family": DISPLAY, "text.color": TEXT, "axes.labelcolor": MUTED,
        "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelsize": 8, "ytick.labelsize": 8,
        "axes.edgecolor": BORDER, "axes.facecolor": PANEL, "figure.facecolor": INK,
        "toolbar": "None", "keymap.save": [], "keymap.fullscreen": [], "keymap.pan": [],
        "keymap.zoom": [], "keymap.back": [], "keymap.forward": [], "keymap.home": [],
        "keymap.quit": [], "keymap.grid": [], "keymap.yscale": [], "keymap.xscale": [],
    })

    port = args.port or espctl.find_port()
    radio = radios.open_radio(port, args.backend, args.reload)
    mac = radio.mac if args.show_mac else None
    LO_MIN, LO_MAX = radio.ui_range_mhz
    rate0 = args.rate * 1e6 if args.rate * 1e6 in radio.rates else radio.rates[0]
    radio.set_rate(rate0)
    lo0 = min(max(args.lo, LO_MIN), LO_MAX)
    radio.set_lo(lo0 * 1e6)
    if args.gain is not None or not radio.has_agc:
        radio.set_gain(args.gain if args.gain is not None else 45)
    gain0 = args.gain if args.gain is not None else 45

    frames = queue.Queue(maxsize=4)
    rx = Receiver(radio, frames)
    rx.start()

    prefs = load_prefs(args.prefs)

    def save_prefs():
        try:
            with open(args.prefs, "w") as f:
                json.dump(prefs, f)
        except OSError:
            pass

    rows = args.rows
    state = {
        "n": prefs["fft"], "vmin": prefs["floor"], "vmax": prefs["ceiling"],
        "avg": None, "hold": None, "span": None, "f": None, "count": 0, "t0": time.time(),
        "times": collections.deque(maxlen=40), "last": None, "last_spectra": None, "toast": None,
        "saved": {"manual": 0, "trigger": 0, "box": 0}, "last_file": "none yet",
        "markers": [None, None], "next_marker": 0, "marker_undo": None,
        "box": None, "box_pending": False, "autopaused": False,
        "trig_band": None, "trig_count": 0, "trig_saved": 0, "trig_last": 0.0, "trig_last_text": "",
    }
    window = {"w": np.blackman(state["n"])}
    waterfall = {"a": np.full((rows, state["n"]), state["vmin"], dtype=np.float32)}
    dvr = {"raw": None, "meta": [None] * rows, "head": -1, "filled": 0}

    fig = plt.figure(figsize=(14, 8.8))
    if fig.canvas.manager:
        fig.canvas.manager.set_window_title(f"eSpDRmini · {radio.chip} snapshot SDR")

    # ---- page-aware builders ------------------------------------------------------
    PAGES = ("Receiver", "Display", "Trigger", "Measure & DVR")
    page_artists = {p: [] for p in PAGES}
    building = [None]
    # Matplotlib holds widget callbacks weakly: an unreferenced widget is
    # garbage-collected and silently stops responding, so every widget is kept
    # here. Widgets on hidden pages still see clicks at their position, so each
    # page's widgets are switched off while it is hidden (show_page).
    widgets = []
    page_widgets = {p: [] for p in PAGES}

    def keep(w):
        widgets.append(w)
        if building[0]:
            page_widgets[building[0]].append(w)
        return w

    def track(artist):
        if building[0]:
            page_artists[building[0]].append(artist)
        return artist

    def label(x, y, s, size=8, color=MUTED, **kw):
        return track(fig.text(x, y, s, fontsize=size, color=color, **kw))

    def caps(x, y, s):  # small letter-spaced section heading
        return label(x, y, " ".join(s.upper()), 6.5, MUTED2)

    def axes(rect, **kw):
        return track(fig.add_axes(rect, **kw))

    def fig_rect(xy, w, h, color):
        r = Rectangle(xy, w, h, transform=fig.transFigure, facecolor=color, edgecolor="none")
        fig.patches.append(r)
        return track(r)

    def spines(ax, color=BORDER):
        for s in ax.spines.values():
            s.set_color(color)

    def button(rect, text, fill=PANEL, hover=RAISED, fg=TEXT, size=9):
        b = keep(Button(axes(rect), text, color=fill, hovercolor=hover))
        b.label.set_color(fg)
        b.label.set_fontsize(size)
        spines(b.ax)
        return b

    def slider(rect, lo, hi, init, step, color, fmt):
        s = keep(Slider(axes(rect), "", lo, hi, valinit=init, valstep=step, color=color, track_color=RAISED,
                        handle_style={"facecolor": TEXT, "edgecolor": color, "size": 8}, valfmt=fmt))
        s.valtext.set_color(TEXT)
        s.valtext.set_family(MONO)
        s.valtext.set_fontsize(8)
        return s

    def checkbox(rect, text, active):
        ax = axes(rect, facecolor=PANEL)
        cb = keep(CheckButtons(ax, [text], [active], label_props={"color": [TEXT], "fontsize": [8.5]},
                               frame_props={"edgecolor": [MUTED], "facecolor": [INK], "s": [60]},
                               check_props={"facecolor": [CYAN], "s": [60]}))
        spines(ax)

        def row_click(ev):  # CheckButtons only reacts on its box or text; make the whole row a toggle
            if (ev.inaxes is ax and ev.button == 1 and cb.get_active() and cb.eventson
                    and not cb._frames.contains(ev)[0] and not cb.labels[0].contains(ev)[0]):
                cb.set_active(0)

        fig.canvas.mpl_connect("button_press_event", row_click)
        return cb

    class Segmented:
        """A row of buttons, one selected."""

        def __init__(self, rect, options, active, on_change, size=8.5):
            x, y, w, h = rect
            gap = 0.004
            bw = (w - gap * (len(options) - 1)) / len(options)
            self.buttons = [button([x + i * (bw + gap), y, bw, h], o, size=size) for i, o in enumerate(options)]
            for i, b in enumerate(self.buttons):
                b.on_clicked(lambda _e, i=i: self.select(i))
            self.on_change, self.index = on_change, None
            widgets.append(self)  # its buttons are kept (and paged) by button()
            self.select(active, notify=False)

        def select(self, i, notify=True):
            changed = i != self.index
            self.index = i
            for k, b in enumerate(self.buttons):
                on = k == i
                b.color, b.hovercolor = ("#0f2a3a", "#14374b") if on else (PANEL, RAISED)
                b.ax.set_facecolor(b.color)
                b.label.set_color(CYAN if on else MUTED)
                spines(b.ax, CYAN if on else BORDER)
            if notify and changed:
                self.on_change(i)
            fig.canvas.draw_idle()

    glow = [patheffects.withStroke(linewidth=6, foreground=(0.09, 0.77, 1.0, 0.18))]

    # ---- frame: sidebar, navigation, status bar -----------------------------------------
    SB = 0.19
    fig_rect((0, 0.035), SB, 0.965, INK2).set_zorder(-10)
    fig_rect((0, 0), 1, 0.035, INK2).set_zorder(-10)
    fig.add_artist(plt.Line2D([SB, SB], [0.035, 1], transform=fig.transFigure, color=LINE, lw=1))
    fig.add_artist(plt.Line2D([0, 1], [0.035, 0.035], transform=fig.transFigure, color=LINE, lw=1))
    label(0.016, 0.945, "eSpDRmini", 22, CYAN, weight="heavy", path_effects=glow)
    label(0.016, 0.92, f"{radio.chip} snapshot SDR", 8.5, MUTED)

    sx, sw = 0.016, SB - 0.032
    nav_colors = (CYAN, GREEN, ORANGE, PURPLE)
    nav = []
    for i, (name, color) in enumerate(zip(PAGES, nav_colors)):
        b = Button(fig.add_axes([0.008, 0.845 - i * 0.037, SB - 0.016, 0.034]), name,
                   color=INK2, hovercolor=RAISED)
        b.label.set_horizontalalignment("left")
        b.label.set_position((0.13, 0.5))
        b.label.set_fontsize(9.5)
        b.ax.add_patch(Rectangle((0.045, 0.3), 0.035, 0.4, transform=b.ax.transAxes, facecolor=color,
                                 edgecolor="none"))
        spines(b.ax, INK2)
        nav.append(b)
    fig.add_artist(plt.Line2D([0.016, SB - 0.016], [0.722, 0.722], transform=fig.transFigure, color=LINE))

    # ---- page: Receiver -------------------------------------------------------------
    building[0] = "Receiver"
    label(sx, 0.685, "LO frequency (MHz)  ·  Enter to tune", 8)
    lo_box = keep(TextBox(axes([sx, 0.642, sw, 0.036]), "", initial=f"{lo0:.3f}",
                          color=PANEL, hovercolor=RAISED, textalignment="center"))
    lo_box.text_disp.set_color(TEXT)
    lo_box.text_disp.set_fontsize(14)
    lo_box.text_disp.set_family(MONO)
    spines(lo_box.ax)
    steps = (-20, -5, 5, 20)
    bw = (sw - 0.006 * 3) / 4
    step_buttons = [(button([sx + i * (bw + 0.006), 0.603, bw, 0.031], f"{s:+d}", size=8.5), s)
                    for i, s in enumerate(steps)]
    label(sx, 0.587, "step MHz", 6.5, MUTED2)
    label(sx, 0.555, "Gain selector", 8)
    gain_slider = slider([sx, 0.525, sw - 0.03, 0.022], 0, radio.gain_max, min(gain0, radio.gain_max), 1,
                         GREEN, "%d")
    agc_check = checkbox([sx + sw - 0.062, 0.548, 0.062, 0.024], "AGC", radio.has_agc and args.gain is None) \
        if radio.has_agc else None
    label(sx, 0.507, "40–55 indoors  ·  70+ clips", 6.5, MUTED2)
    label(sx, 0.475, "Sample rate", 8)
    label(sx + 0.075, 0.475, "MS/s", 7, MUTED2)
    Segmented([sx, 0.437, sw, 0.032], tuple(f"{r / 1e6:g}" for r in radio.rates), radio.rates.index(rate0),
              lambda i: (rx.set("rate", radio.rates[i]), state.update(hold=None), length_labels(radio.rates[i])),
              size=8 if len(radio.rates) > 3 else 8.5)
    length_label = label(sx, 0.405, "Snapshot length", 8)
    length_seg = Segmented([sx, 0.368, sw, 0.031], tuple(str(k + 1) for k in range(radio.max_banks)),
                           min(prefs["banks"], radio.max_banks) - 1, lambda i: set_banks(i + 1), size=8)
    caps(sx, 0.335, "band guide")
    guide = [("Wi-Fi ch 1 / 6 / 11", "2412 / 2437 / 2462", GREEN),
             ("BLE advertising", "2402 / 2426 / 2480", PURPLE),
             ("Microwave ovens", "≈ 2450, broad", ORANGE)]
    for i, (name, freq, color) in enumerate(guide):
        y = 0.305 - i * 0.037
        fig_rect((sx, y + 0.004), 0.004, 0.022, color)
        label(sx + 0.01, y + 0.014, name, 8, TEXT)
        label(sx + 0.01, y, freq, 7, MUTED, family=MONO)

    # ---- page: Display ----------------------------------------------------------------
    building[0] = "Display"
    show_meter = checkbox([sx, 0.655, sw, 0.034], "Show level meter   (key: m)", prefs["meter"])
    label(sx, 0.625, "Channel overlay", 8)
    Segmented([sx, 0.588, sw, 0.031], ("Off", "Wi-Fi", "BLE"), prefs["overlay"], lambda i: set_overlay(i))
    rbw_text = label(sx, 0.555, "FFT size", 8)
    Segmented([sx, 0.518, sw, 0.031], ("1k", "2k", "4k", "8k"), FFT_SIZES.index(prefs["fft"]),
              lambda i: set_fft(FFT_SIZES[i]))
    label(sx, 0.487, "Averaging", 8)
    avg_slider = slider([sx, 0.46, sw - 0.035, 0.02], 0, 0.95, prefs["avg"], 0.05, CYAN, "%.2f")
    label(sx, 0.43, "Floor (dBFS)", 8)
    floor_slider = slider([sx, 0.403, sw - 0.035, 0.02], -120, -50, prefs["floor"], 1, BLUE, "%d")
    label(sx, 0.373, "Ceiling (dBFS)", 8)
    ceil_slider = slider([sx, 0.346, sw - 0.035, 0.02], -60, 0, prefs["ceiling"], 1, YELLOW, "%d")
    label(sx, 0.316, "Waterfall rows show", 8)
    wf_seg = Segmented([sx, 0.279, sw, 0.031], ("Peak", "Average"), prefs["wf"], lambda i: set_wf(i))
    label(sx, 0.255, "Floor and ceiling set the spectrum scale\nand the waterfall colours.", 6.5, MUTED2,
          va="top", linespacing=1.4)

    # ---- page: Trigger ----------------------------------------------------------------
    building[0] = "Trigger"
    label(sx, 0.70, "Saves an IQ snapshot when the strongest\nbin in the band crosses the threshold.\n"
                    "Drag across the spectrum to set the band.", 7.5, MUTED, va="top", linespacing=1.4)
    trig_check = checkbox([sx, 0.585, sw, 0.034], "Armed", False)
    label(sx, 0.555, "Threshold (dBFS)", 8)
    thr_slider = slider([sx, 0.528, sw - 0.035, 0.02], -100, 0, prefs["trig_thr"], 1, ORANGE, "%d")
    label(sx, 0.497, "Band", 8)
    band_text = label(sx, 0.475, "", 8.5, TEXT, family=MONO)
    full_button = button([sx, 0.43, sw, 0.03], "Use full span", size=8.5)
    label(sx, 0.40, "Hold-off between saves (s)", 8)
    hold_slider = slider([sx, 0.373, sw - 0.035, 0.02], 0, 5, prefs["trig_hold"], 0.1, ORANGE, "%.1f")
    label(sx, 0.343, "Stop after (saves)", 8)
    max_slider = slider([sx, 0.316, sw - 0.035, 0.02], 1, 500, prefs["trig_max"], 1, ORANGE, "%d")
    trig_counts = label(sx, 0.275, "", 10, TEXT, weight="bold")
    trig_last = label(sx, 0.253, "", 7, MUTED, family=MONO)
    label(sx, 0.232, "files → captures/triggers/", 7, MUTED2)

    # ---- page: Measure & DVR ----------------------------------------------------------
    building[0] = "Measure & DVR"
    caps(sx, 0.69, "markers")
    label(sx, 0.67, "Click the spectrum: M1, then M2.\nRight-click clears. With M1 on a signal\n"
                    "and M2 on the noise floor, Δ is the\nSNR at the current RBW.", 7.5, MUTED,
          va="top", linespacing=1.4)
    half = (sw - 0.006) / 2
    peak_button = button([sx, 0.55, half, 0.031], "Peak → M1  (p)", size=8)
    clear_button = button([sx + half + 0.006, 0.55, half, 0.031], "Clear  (c)", size=8)
    caps(sx, 0.505, "dvr  ·  waterfall box")
    label(sx, 0.488, "Drag a box on the waterfall; the view\npauses. Each snapshot in the box is\n"
                     "shifted to 0 Hz, filtered and decimated.", 7.5, MUTED, va="top", linespacing=1.4)
    box_text = label(sx, 0.405, "", 7.5, TEXT, family=MONO, va="top", linespacing=1.45)
    label(sx, 0.315, "Output", 8)
    box_seg = Segmented([sx, 0.278, sw, 0.031], ("One SigMF", "File per snapshot"), prefs["box_mode"],
                        lambda i: (prefs.update(box_mode=i), save_prefs()), size=8)
    save_box_button = button([sx, 0.232, half, 0.034], "Save box  (b)", fill="#2b1a3a", hover="#3d2552",
                             size=8.5)
    clear_box_button = button([sx + half + 0.006, 0.232, half, 0.034], "Clear  (esc)", size=8.5)
    building[0] = None

    # ---- sidebar footer (all pages) -----------------------------------------------------
    fig.add_artist(plt.Line2D([0.016, SB - 0.016], [0.205, 0.205], transform=fig.transFigure, color=LINE))
    snap_button = button([sx, 0.14, sw, 0.045], "IQ snapshot  ↘", fill="#3db8f5", hover=CYAN, fg=INK, size=11)
    snap_hint = label(sx, 0.122, "", 7, MUTED2)
    pause_button = button([sx, 0.075, half, 0.034], "❚❚  Pause", size=9)
    hold_button = button([sx + half + 0.006, 0.075, half, 0.034], "Reset peak hold", size=8.5)
    label(sx, 0.05, "space pause · i save · m meter · p peak", 6.5, MUTED2)

    # ---- main: header and level meter ----------------------------------------------------
    MX0, MX1 = SB + 0.04, 0.975
    MW = MX1 - MX0
    label(MX0, 0.935, "Spectrum", 28, TEXT)
    subtitle = label(MX0, 0.913, "", 9, MUTED)
    level_text = label(MX1, 0.873, "", 17, CYAN, ha="right", family=MONO, path_effects=glow)
    level_label = label(MX1, 0.905, "ADC level (rms)", 7.5, MUTED2, ha="right")
    meter_ax = fig.add_axes([MX0, 0.872, MW - 0.13, 0.024])
    meter_ax.set_axis_off()
    meter_ax.set_xlim(0, 1)
    meter_ax.set_ylim(0, 1)
    BLOCKS, M_LO, M_HI = 36, -60.0, 0.0
    METER_GAP = 0.04  # height the meter row takes
    blocks = []
    for i in range(BLOCKS):
        r = Rectangle((i / BLOCKS + 0.002, 0.05), 1 / BLOCKS - 0.006, 0.9, facecolor="#0d2131", edgecolor="none")
        meter_ax.add_patch(r)
        blocks.append(r)

    def block_color(i):
        db = M_LO + (i + 0.5) / BLOCKS * (M_HI - M_LO)
        return RED if db > -4 else YELLOW if db > -10 else BLUE

    # ---- main: tuning range slider ---------------------------------------------------------
    tune_label = label(MX0, 0.842, "Tuning range", 8, MUTED)
    tune_hint = label(MX1, 0.842, "", 8, MUTED, ha="right", family=MONO)
    tune_ax = fig.add_axes([MX0, 0.808, MW, 0.026], facecolor=PANEL)
    freq_slider = Slider(tune_ax, "", LO_MIN, LO_MAX, valinit=lo0, valstep=max(radio.lo_step_mhz, 0.5),
                         color=(0, 0, 0, 0),
                         track_color=RAISED, initcolor="none",
                         handle_style={"facecolor": CYAN, "edgecolor": TEXT, "size": 11})
    freq_slider.valtext.set_visible(False)
    tune_ax.axvspan(2400, 2483.5, 0.2, 0.8, color=CYAN, alpha=0.18, lw=0)
    window_span = tune_ax.axvspan(lo0 - 40, lo0 + 40, 0.2, 0.8, color=GREEN, alpha=0.35, lw=0)
    trans = blended_transform_factory(tune_ax.transData, tune_ax.transAxes)
    span_mhz = LO_MAX - LO_MIN
    step = next(s for s in (50, 100, 250, 500, 1000) if span_mhz / s <= 8)
    ticks = [LO_MIN] + [t for t in np.arange(np.ceil(LO_MIN / step) * step, LO_MAX, step)
                        if min(t - LO_MIN, LO_MAX - t) > span_mhz * 0.06] + [LO_MAX]
    for f in ticks:
        tune_ax.text(f, -0.35, f"{f:g}", transform=trans, ha="center", va="top", fontsize=7,
                     color=MUTED2, family=MONO)
    # Band names sit above the bar on a 2.4 GHz-only slider; on a wide (C5)
    # slider that space holds the bar's own labels, so they go inside it.
    wide = span_mhz > 1000
    band_y, band_va = (0.5, "center") if wide else (1.15, "bottom")
    tune_ax.text(2441.75, band_y, "2.4 GHz" if wide else "ISM 2.4 GHz", transform=trans, ha="center",
                 va=band_va, fontsize=6.5, color=CYAN, alpha=0.9, zorder=5)
    if LO_MAX > 5150:
        tune_ax.axvspan(5150, 5895, 0.2, 0.8, color=CYAN, alpha=0.18, lw=0)
        tune_ax.text(5522, band_y, "Wi-Fi 5 GHz", transform=trans, ha="center", va=band_va,
                     fontsize=6.5, color=CYAN, alpha=0.9, zorder=5)

    # ---- main: spectrum --------------------------------------------------------------------
    trace_cmap = LinearSegmentedColormap.from_list("trace", [CYAN, GREEN, LIME, YELLOW, ORANGE])
    wf_cmap = LinearSegmentedColormap.from_list(
        "wf", [INK, "#0a2238", "#0b4f8c", BLUE, CYAN, GREEN, YELLOW, ORANGE, RED])

    def style_panel(ax):
        ax.set_facecolor(PANEL)
        spines(ax)
        ax.tick_params(length=3, color=BORDER)
        ax.grid(color=LINE, lw=0.6)

    ax_spec = fig.add_axes([MX0, 0.525, MW, 0.235])
    style_panel(ax_spec)
    spec_texts = [label(MX0, 0.767, "Live spectrum", 8, MUTED)]
    for i, (name, color) in enumerate((("average", GREEN), ("this snapshot", YELLOW), ("peak hold", RED))):
        spec_texts.append(label(MX1 - 0.215 + i * 0.075, 0.767, "━ " + name, 7.5, color))
    fill = Polygon(np.zeros((2, 2)), closed=True, facecolor=GREEN, alpha=0.07, edgecolor="none")
    ax_spec.add_patch(fill)
    (glow1,) = ax_spec.plot([], [], lw=6, color=GREEN, alpha=0.06, solid_capstyle="round")
    (glow2,) = ax_spec.plot([], [], lw=2.8, color=GREEN, alpha=0.14)
    trace = LineCollection([], cmap=trace_cmap, lw=1.3)
    ax_spec.add_collection(trace)
    (line_peak,) = ax_spec.plot([], [], lw=0.5, color=YELLOW, alpha=0.45)
    (line_hold,) = ax_spec.plot([], [], lw=0.6, color=RED, alpha=0.65)
    ax_spec.set_ylabel("dBFS", fontsize=8)
    ax_spec.set_xlabel("RF frequency, MHz", fontsize=8, labelpad=2)
    # Trigger band and threshold.
    trig_span = ax_spec.axvspan(0, 1, color=ORANGE, alpha=0.07, lw=0, visible=False)
    (trig_line,) = ax_spec.plot([], [], color=ORANGE, lw=1, ls=(0, (4, 3)), visible=False)
    trig_tag = ax_spec.text(0, 0, "", color=ORANGE, fontsize=7, ha="left", va="bottom", visible=False)
    # Markers.
    marker_colors = (CYAN, PURPLE)
    marker_lines = [ax_spec.axvline(0, color=c, lw=0.8, ls=(0, (3, 3)), visible=False) for c in marker_colors]
    marker_pts = [ax_spec.plot([], [], "v", ms=7, color=c, mec=INK, mew=0.8, zorder=6)[0] for c in marker_colors]
    marker_tags = [ax_spec.text(0, 0, f"M{k + 1}", color=c, fontsize=8, ha="center", va="bottom",
                                weight="bold", visible=False, zorder=6) for k, c in enumerate(marker_colors)]
    readout = ax_spec.text(0.008, 0.975, "", transform=ax_spec.transAxes, va="top", ha="left", fontsize=8,
                           family=MONO, color=TEXT, zorder=7, visible=False,
                           bbox={"facecolor": INK, "alpha": 0.85, "edgecolor": BORDER, "boxstyle": "round,pad=0.4"})
    overlay_artists = []

    # ---- main: waterfall ---------------------------------------------------------------------
    ax_wf = fig.add_axes([MX0, 0.205, MW, 0.26], sharex=ax_spec)
    style_panel(ax_wf)
    ax_wf.grid(False)
    image = ax_wf.imshow(waterfall["a"], aspect="auto", cmap=wf_cmap, vmin=state["vmin"], vmax=state["vmax"],
                         interpolation="nearest", origin="upper")
    ax_wf.set_xlabel("RF frequency, MHz", fontsize=8, labelpad=2)
    ax_wf.set_ylabel("snapshots ago", fontsize=8)
    label(MX0, 0.472, "Scrolling history  ·  DVR", 8, MUTED)
    wf_hint = label(MX1, 0.472, "", 7.5, MUTED2, ha="right")

    # ---- main: statistics grid -----------------------------------------------------------------
    stat_names = ("LO", "Span · RBW", "Gain", "Snapshot", "Rate", "Clipped", "Saved", "Last saved")
    stats = {}
    for i, name in enumerate(stat_names):
        col, row = i % 4, i // 4
        x = MX0 + col * MW / 4
        y = 0.093 - row * 0.042
        label(x, y + 0.016, name, 7.5, MUTED2)
        stats[name] = label(x, y, "", 10, TEXT, weight="bold")

    # ---- status bar -----------------------------------------------------------------------------
    dot = fig.add_artist(Circle((0.012, 0.0175), 0.0035, transform=fig.transFigure, facecolor=GREEN,
                                edgecolor="none"))
    status = label(0.02, 0.011, "", 8, MUTED)
    clock = label(0.985, 0.011, "00:00:00", 9, CYAN, ha="right", family=MONO)

    # ================================ behaviour ===================================================

    def toast(text, color=GREEN, seconds=6):
        state["toast"] = (text, color, time.time() + seconds)

    def quiet(widget, value):
        widget.eventson = False
        widget.set_val(value)
        widget.eventson = True

    def repaint():
        redraw()
        refresh_status()
        fig.canvas.draw_idle()

    # ---- pages ----
    def show_page(i):
        prefs["page"] = i
        save_prefs()
        lo_box.stop_typing()
        for k, (name, b) in enumerate(zip(PAGES, nav)):
            on = k == i
            for a in page_artists[name]:
                a.set_visible(on)
            for w in page_widgets[name]:
                Widget.set_active(w, on)  # base method: CheckButtons.set_active means "tick box N"
            b.color = RAISED if on else INK2
            b.ax.set_facecolor(b.color)
            b.label.set_color(TEXT if on else MUTED)
            spines(b.ax, BORDER if on else INK2)
        span_sel.set_active(PAGES[i] == "Trigger")
        update_trigger_artists()
        fig.canvas.draw_idle()

    # ---- tuning ----
    def request_lo(mhz, source=None):
        mhz = min(max(mhz, LO_MIN), LO_MAX)
        mhz = round(round(mhz / radio.lo_step_mhz) * radio.lo_step_mhz, 4)
        rx.set("lo", mhz * 1e6)
        if source is not lo_box:
            quiet(lo_box, f"{mhz:.3f}")
        if source is not freq_slider:
            quiet(freq_slider, mhz)
        state["hold"] = None

    def on_lo(text):
        try:
            request_lo(float(text), lo_box)
        except ValueError:
            pass

    def length_labels(rate):
        unit = radio.bank_pairs / rate
        for k, b in enumerate(length_seg.buttons):
            d = (k + 1) * unit
            b.label.set_text(f"{d * 1e6:.0f} µs" if d < 1e-3 else f"{d * 1e3:.1f} ms")
        fig.canvas.draw_idle()

    def set_banks(n):
        prefs["banks"] = n
        save_prefs()
        rx.banks = n
        length_label.set_text(f"Snapshot length  ·  {n * radio.bank_pairs:,} pairs"
                              + ("" if radio.max_banks > 1 else "  (fixed)"))
        state["hold"] = None

    # ---- display ----
    def layout_meter(_label=None):
        visible = show_meter.get_status()[0]
        for artist in (meter_ax, level_text, level_label):
            artist.set_visible(visible)
        # Close up: the tuning bar takes the meter's place and the spectrum the freed height.
        d = 0.0 if visible else METER_GAP
        tune_label.set_y(0.842 + d)
        tune_hint.set_y(0.842 + d)
        tune_ax.set_position([MX0, 0.808 + d, MW, 0.026])
        for t in spec_texts:
            t.set_y(0.767 + d)
        ax_spec.set_position([MX0, 0.525, MW, 0.235 + d])
        prefs["meter"] = visible
        save_prefs()
        fig.canvas.draw_idle()

    def set_range(_v=None):
        lo, hi = floor_slider.val, ceil_slider.val
        if hi - lo < 10:
            hi = min(lo + 10, 0)
            quiet(ceil_slider, hi)
        state["vmin"], state["vmax"] = lo, hi
        image.set_clim(lo, hi)
        ax_spec.set_ylim(lo - 5, hi + 10)
        trace.set_norm(Normalize(lo + 10, hi))
        prefs.update(floor=lo, ceiling=hi)
        save_prefs()
        repaint()

    def set_overlay(i):
        prefs["overlay"] = i
        save_prefs()
        while overlay_artists:
            overlay_artists.pop().remove()
        top = blended_transform_factory(ax_spec.transData, ax_spec.transAxes)

        def tag(x, text, color, size, y=0.985, weight="normal"):
            overlay_artists.append(ax_spec.text(x, y, text, transform=top, ha="center", va="top", fontsize=size,
                                                color=color, weight=weight, clip_on=True))

        if i == 1:  # Wi-Fi 5 GHz (U-NII 1-4): 20 MHz channels 36-177, drawn wherever they are in view
            for ch in [*range(36, 65, 4), *range(100, 145, 4), *range(149, 178, 4)]:
                c = 5000.0 + 5 * ch
                overlay_artists.append(ax_spec.axvspan(c - 10, c + 10, color=GREEN,
                                                       alpha=0.06 if (ch // 4) % 2 else 0.025, lw=0))
                overlay_artists.append(ax_spec.axvline(c, ymin=0.93, ymax=1, color=GREEN, lw=0.8, alpha=0.8))
                tag(c, str(ch), GREEN, 8, 0.925, "bold")
        if i == 1:  # Wi-Fi 2.4 GHz channels 1-14
            for ch in range(1, 15):
                c = 2484.0 if ch == 14 else 2412.0 + 5 * (ch - 1)
                main = ch in (1, 6, 11)
                if main:
                    overlay_artists.append(ax_spec.axvspan(c - 10, c + 10, color=GREEN, alpha=0.06, lw=0))
                overlay_artists.append(ax_spec.axvline(c, ymin=0.93, ymax=1, color=GREEN if main else MUTED2,
                                                       lw=0.8, alpha=0.8))
                tag(c, str(ch), GREEN if main else MUTED, 8 if main else 6.5, 0.925,
                    "bold" if main else "normal")
        elif i == 2:  # BLE: 40 RF channels, 2 MHz apart; 37/38/39 advertise
            for k in range(40):
                c = 2402.0 + 2 * k
                index = {0: 37, 12: 38, 39: 39}.get(k, k - 1 if k < 12 else k - 2)
                if index >= 37:
                    overlay_artists.append(ax_spec.axvspan(c - 1, c + 1, color=PURPLE, alpha=0.2, lw=0))
                    tag(c, str(index), PURPLE, 8, weight="bold")
                else:
                    overlay_artists.append(ax_spec.axvspan(c - 1, c + 1, color=PURPLE,
                                                           alpha=0.035 if k % 2 else 0.0, lw=0))
                    if index % 5 == 0:
                        tag(c, str(index), MUTED, 6.5)
        for a in overlay_artists:
            a.set_zorder(0.5)
        fig.canvas.draw_idle()

    def recompute_waterfall():
        a = waterfall["a"]
        a[:] = state["vmin"]
        for i in range(dvr["filled"]):
            mean, peak = dsp.spectrum(dvr_iq(i), state["n"], window["w"])
            a[i] = peak if wf_seg.index == 0 else mean

    def set_fft(n):
        prefs["fft"] = n
        save_prefs()
        state["n"] = n
        window["w"] = np.blackman(n)
        waterfall["a"] = np.full((rows, n), state["vmin"], dtype=np.float32)
        recompute_waterfall()
        image.set_data(waterfall["a"])
        if state["span"]:
            state["f"] = dsp.frequencies(state["span"][0], state["span"][1], n)
        if state["last"]:
            mean, peak = dsp.spectrum(state["last"][0], n, window["w"])
            state["avg"], state["hold"], state["last_spectra"] = mean, peak, (mean, peak)
        update_rbw()
        repaint()

    def set_wf(i):
        prefs["wf"] = i
        save_prefs()
        recompute_waterfall()
        repaint()

    def update_rbw():
        if state["span"]:
            rbw_text.set_text(f"FFT size  ·  RBW {state['span'][1] / state['n'] / 1e3:.1f} kHz")

    # ---- DVR ring: raw IQ behind every waterfall row ----
    def dvr_push(iq, settings, when):
        if dvr["raw"] is None or dvr["raw"].shape[1] != len(iq):
            dvr["raw"] = np.zeros((rows, len(iq), 2), dtype=np.int16)
            dvr["filled"] = 0
        dvr["head"] = (dvr["head"] + 1) % rows
        dvr["raw"][dvr["head"], :, 0] = iq.real
        dvr["raw"][dvr["head"], :, 1] = iq.imag
        dvr["meta"][dvr["head"]] = (settings, when)
        dvr["filled"] = min(dvr["filled"] + 1, rows)

    def dvr_slot(i):  # waterfall row i (0 = newest)
        return (dvr["head"] - i) % rows

    def dvr_iq(i):
        r = dvr["raw"][dvr_slot(i)]
        return (r[:, 0] + 1j * r[:, 1]).astype(np.complex64)

    # ---- markers ----
    def place_marker(fmhz):
        state["marker_undo"] = (list(state["markers"]), state["next_marker"], time.time())
        state["markers"][state["next_marker"]] = fmhz
        state["next_marker"] ^= 1
        repaint()

    def clear_markers(_e=None):
        state["markers"] = [None, None]
        state["next_marker"] = 0
        repaint()

    def peak_marker(_e=None):
        if state["avg"] is None:
            return
        state["markers"][0] = float(state["f"][int(np.argmax(state["avg"]))])
        state["next_marker"] = 1
        repaint()

    def draw_markers():
        f, avg = state["f"], state["avg"]
        found, lines = [], []
        for k, fm in enumerate(state["markers"]):
            on = fm is not None and f[0] <= fm <= f[-1]
            for a in (marker_lines[k], marker_tags[k], marker_pts[k]):
                a.set_visible(on)
            if on:
                j = int(np.argmin(np.abs(f - fm)))
                fk, lk = f[j], avg[j]
                marker_lines[k].set_xdata([fk, fk])
                marker_pts[k].set_data([fk], [lk])
                marker_tags[k].set_position((fk, lk + 3))
                found.append((fk, lk))
                lines.append(f"M{k + 1}  {fk:9.3f} MHz  {lk:6.1f} dBFS")
        if len(found) == 2:
            (f1, l1), (f2, l2) = found
            lines.append(f"Δ   {f2 - f1:+9.3f} MHz  {l1 - l2:6.1f} dB   "
                         f"(RBW {state['span'][1] / state['n'] / 1e3:.1f} kHz)")
        readout.set_text("\n".join(lines))
        readout.set_visible(bool(lines))

    # ---- trigger ----
    def trigger_band():
        f = state["f"]
        return state["trig_band"] or ((f[0], f[-1]) if f is not None else (0.0, 1.0))

    def update_trigger_artists():
        armed = trig_check.get_status()[0]
        visible = armed or PAGES[prefs["page"]] == "Trigger"
        lo, hi = trigger_band()
        trig_span.set_x(lo)
        trig_span.set_width(hi - lo)
        trig_line.set_data([lo, hi], [thr_slider.val] * 2)
        trig_tag.set_position((lo, thr_slider.val + 0.5))
        trig_tag.set_text(f" trigger {thr_slider.val:.0f} dBFS" + ("  ·  ARMED" if armed else ""))
        for a in (trig_span, trig_line, trig_tag):
            a.set_visible(visible)
        band_text.set_text(f"{lo:.2f} – {hi:.2f} MHz" + ("" if state["trig_band"] else "  (full)"))
        trig_counts.set_text(f"{state['trig_count']} triggers  ·  {state['trig_saved']} saved")
        trig_last.set_text(state["trig_last_text"])

    def on_trigger_settings(_v=None):
        prefs.update(trig_thr=thr_slider.val, trig_hold=hold_slider.val, trig_max=int(max_slider.val))
        save_prefs()
        update_trigger_artists()
        fig.canvas.draw_idle()

    def on_arm(_label=None):
        if trig_check.get_status()[0]:
            state["trig_count"] = state["trig_saved"] = 0
            state["trig_last_text"] = ""
            lo, hi = trigger_band()
            toast(f"Trigger armed: ≥ {thr_slider.val:.0f} dBFS in {lo:.1f}–{hi:.1f} MHz", ORANGE)
        update_trigger_artists()
        repaint()

    def on_span_select(lo, hi):
        if hi - lo < 0.05:
            return
        state["trig_band"] = (lo, hi)
        update_trigger_artists()
        fig.canvas.draw_idle()

    def check_trigger(peak, frame):
        if not trig_check.get_status()[0]:
            return
        lo, hi = trigger_band()
        sel = (state["f"] >= lo) & (state["f"] <= hi)
        if not sel.any():
            return
        k = int(np.argmax(np.where(sel, peak, -np.inf)))
        level = peak[k]
        if level < thr_slider.val:
            return
        state["trig_count"] += 1
        now = time.time()
        if now - state["trig_last"] < hold_slider.val:
            return
        state["trig_last"] = now
        iq, settings, when = frame
        note = {"core:sample_start": 0, "core:sample_count": len(iq), "core:label": "trigger",
                "core:freq_lower_edge": lo * 1e6, "core:freq_upper_edge": hi * 1e6,
                "core:comment": f"peak {level:.1f} dBFS at {state['f'][k]:.3f} MHz >= "
                                f"{thr_slider.val:.0f} dBFS"}
        base = dsp.save_snapshot(os.path.join(CAPTURE_DIR, "triggers"), iq, settings, when, mac, [note], "trig_", hw=radio.hw)
        state["trig_saved"] += 1
        state["saved"]["trigger"] += 1
        state["last_file"] = os.path.basename(base)
        state["trig_last_text"] = f"{when.strftime('%H:%M:%S')}  {level:.1f} dBFS @ {state['f'][k]:.2f}"
        toast(f"Trigger: {level:.1f} dBFS at {state['f'][k]:.3f} MHz → triggers/{os.path.basename(base)}",
              ORANGE, 3)
        if state["trig_saved"] >= int(max_slider.val):
            trig_check.eventson = False
            trig_check.set_active(0)
            trig_check.eventson = True
            toast(f"Trigger stopped after {state['trig_saved']} saves", ORANGE)
        update_trigger_artists()

    # ---- DVR box ----
    def on_box(eclick, erelease):
        if dvr["filled"] == 0 or state["f"] is None:
            return
        f0, f1 = sorted((eclick.xdata, erelease.xdata))
        f0, f1 = max(f0, state["f"][0]), min(f1, state["f"][-1])
        y0, y1 = sorted((eclick.ydata, erelease.ydata))
        i0, i1 = max(0, int(np.floor(y0))), min(dvr["filled"] - 1, int(np.ceil(y1)) - 1)
        if f1 - f0 < 0.01 or i1 < i0:
            return
        state["box"] = (f0, f1, i0, i1)
        state["box_pending"] = True
        state["autopaused"] = False
        if not rx.paused:
            set_paused(True)
        describe_box()
        show_page(PAGES.index("Measure & DVR"))
        toast("Box selected: Save box (b) writes it as IQ; Esc clears", PURPLE)
        repaint()

    def describe_box():
        if not state["box"]:
            box_text.set_text("no box yet")
            return
        f0, f1, i0, i1 = state["box"]
        n = i1 - i0 + 1
        settings_new, when_new = dvr["meta"][dvr_slot(i0)]
        when_old = dvr["meta"][dvr_slot(i1)][1]
        plan = dsp.box_plan(settings_new["rate"], f0, f1)
        per = (dvr["raw"].shape[1] - plan["taps"] + 1) // plan["decim"]
        secs = (when_new - when_old).total_seconds()
        box_text.set_text(f"{f0:.3f} – {f1:.3f} MHz\n"
                          f"{f1 - f0:.3f} MHz wide · {n} snapshot{'s' if n > 1 else ''}\n"
                          f"over {secs:.2f} s · {plan['rate_out'] / 1e6:.2f} Msps out\n"
                          f"≈ {n * per:,} samples (cf32)")

    def clear_box(_e=None):
        state["box"] = None
        if hasattr(box_sel, "clear"):
            box_sel.clear()
        else:
            box_sel.set_visible(False)
        describe_box()
        repaint()

    def save_box(_e=None):
        if not state["box"]:
            toast("Drag a box on the waterfall first", ORANGE)
            repaint()
            return
        f0, f1, i0, i1 = state["box"]
        chosen = [(dvr_iq(i),) + tuple(dvr["meta"][dvr_slot(i)]) for i in range(i1, i0 - 1, -1)]  # oldest first
        out, plan, total = dsp.save_box(CAPTURE_DIR, chosen, f0, f1, mac, per_snapshot=box_seg.index == 1, hw=radio.hw)
        state["saved"]["box"] += 1
        state["last_file"] = os.path.basename(out)
        toast(f"Saved captures/{os.path.basename(out)}/  ·  {len(chosen)} snapshots  ·  {total:,} samples at "
              f"{plan['rate_out'] / 1e6:.2f} Msps", PURPLE, 8)
        print("saved", out)
        repaint()

    # ---- capture / pause ----
    def on_snapshot(_event=None):
        if not state["last"]:
            toast("no snapshot yet", ORANGE)
            return
        iq, settings, when = state["last"]
        base = dsp.save_snapshot(CAPTURE_DIR, iq, settings, when, mac, hw=radio.hw)
        state["saved"]["manual"] += 1
        state["last_file"] = os.path.basename(base)
        kb = os.path.getsize(base + ".sigmf-data") / 1024
        toast(f"Saved captures/{os.path.basename(base)}.sigmf-data  ·  {len(iq):,} pairs  ·  "
              f"{duration(len(iq) / settings['rate'])}  ·  {kb:.0f} KB  (+ .sigmf-meta)", CYAN)
        print("saved", base + ".sigmf-data")
        repaint()

    def set_paused(paused):
        rx.paused = paused
        pause_button.label.set_text("▶  Play" if paused else "❚❚  Pause")
        pause_button.color = "#3a3410" if paused else PANEL
        pause_button.ax.set_facecolor(pause_button.color)
        pause_button.label.set_color(YELLOW if paused else TEXT)
        if not paused and state["box"]:
            clear_box()  # the rows move again, so the box would no longer match
        refresh_status()
        fig.canvas.draw_idle()

    # ---- mouse and keys ----
    press = {}

    def on_press(ev):
        if ev.dblclick and ev.inaxes in (ax_spec, ax_wf) and ev.xdata:
            undo = state["marker_undo"]
            if ev.inaxes is ax_spec and undo and time.time() - undo[2] < 0.6:
                state["markers"], state["next_marker"] = undo[0], undo[1]  # first click of the pair
            if state["autopaused"]:
                state["autopaused"] = False
                set_paused(False)
            request_lo(ev.xdata)
            return
        if ev.inaxes is ax_spec and ev.button == 1:
            press["spec"] = ev.x
        if ev.inaxes is ax_spec and ev.button == 3:
            clear_markers()
        if ev.inaxes is ax_wf and ev.button == 1 and not rx.paused:
            set_paused(True)  # freeze the rows while a box is drawn
            state["autopaused"] = True
            state["box_pending"] = False

    def on_release(ev):
        x = press.pop("spec", None)
        if x is not None and ev.inaxes is ax_spec and ev.button == 1 and abs(ev.x - x) < 5 and ev.xdata:
            place_marker(ev.xdata)
        if state["autopaused"] and not state["box_pending"]:
            state["autopaused"] = False
            set_paused(False)  # a click, not a box

    def on_key(event):
        if lo_box.capturekeystrokes:
            return
        actions = {" ": lambda: set_paused(not rx.paused), "i": on_snapshot,
                   "m": lambda: show_meter.set_active(0), "p": peak_marker, "c": clear_markers,
                   "b": save_box, "escape": clear_box}
        if event.key in actions:
            actions[event.key]()

    # Selectors connect before the click handlers, so a finished box is known
    # by the time the release handler runs.
    box_sel = RectangleSelector(ax_wf, on_box, useblit=False, button=[1], minspanx=0.05, minspany=0.5,
                                spancoords="data", interactive=True,
                                props={"facecolor": PURPLE, "alpha": 0.18, "edgecolor": PURPLE, "lw": 1.2,
                                       "fill": True})
    span_sel = SpanSelector(ax_spec, on_span_select, "horizontal", useblit=False, minspan=0.05, button=1,
                            props={"facecolor": ORANGE, "alpha": 0.15})

    lo_box.on_submit(on_lo)
    freq_slider.on_changed(lambda v: request_lo(v, freq_slider))
    def on_gain(v):
        if agc_check is not None and agc_check.get_status()[0]:
            agc_check.set_active(0)  # moving the slider means manual gain (on_agc sends it)
        else:
            rx.set("gain", int(v))
        state["hold"] = None

    def on_agc(_label=None):
        if agc_check.get_status()[0]:
            rx.set("agc", True)
        else:
            rx.set("gain", int(gain_slider.val))
        state["hold"] = None

    gain_slider.on_changed(on_gain)
    if agc_check is not None:
        agc_check.on_clicked(on_agc)
    for b, step in step_buttons:
        b.on_clicked(lambda _e, s=step: request_lo(freq_slider.val + s))
    for i, b in enumerate(nav):
        b.on_clicked(lambda _e, i=i: show_page(i))
    show_meter.on_clicked(layout_meter)
    avg_slider.on_changed(lambda v: (prefs.update(avg=v), save_prefs()))
    floor_slider.on_changed(set_range)
    ceil_slider.on_changed(set_range)
    trig_check.on_clicked(on_arm)
    for s in (thr_slider, hold_slider, max_slider):
        s.on_changed(on_trigger_settings)
    full_button.on_clicked(lambda _e: (state.update(trig_band=None), update_trigger_artists(),
                                       fig.canvas.draw_idle()))
    peak_button.on_clicked(peak_marker)
    clear_button.on_clicked(clear_markers)
    save_box_button.on_clicked(save_box)
    clear_box_button.on_clicked(clear_box)
    snap_button.on_clicked(on_snapshot)
    pause_button.on_clicked(lambda _e: set_paused(not rx.paused))
    hold_button.on_clicked(lambda _e: state.update(hold=None))
    fig.canvas.mpl_connect("button_press_event", on_press)
    fig.canvas.mpl_connect("button_release_event", on_release)
    fig.canvas.mpl_connect("key_press_event", on_key)

    # ---- data flow -------------------------------------------------------------------------------
    def reset_span(settings, iq_len):
        lo_hz, rate = settings["lo_hz"], settings["rate"]
        state["span"] = (lo_hz, rate)
        state["iq_len"] = iq_len
        state["f"] = f = dsp.frequencies(lo_hz, rate, state["n"])
        state["avg"] = state["hold"] = None
        waterfall["a"][:] = state["vmin"]
        dvr["filled"] = 0
        if state["box"]:
            clear_box()
        lo, r = lo_hz / 1e6, rate / 1e6
        image.set_extent((f[0], f[-1], rows, 0))
        ax_spec.set_xlim(f[0], f[-1])
        window_span.set_x(lo - r / 2)
        window_span.set_width(r)
        tune_hint.set_text(f"window {lo - r / 2:.1f} – {lo + r / 2:.1f} MHz")
        snap_hint.set_text(f"{iq_len:,} pairs · {duration(iq_len / rate)} · SigMF → captures/")
        wf_hint.set_text(f"last {rows} snapshots kept as IQ  ·  drag a box to save part of it")
        band = state["trig_band"]
        if band and not (f[0] <= band[0] and band[1] <= f[-1]):
            state["trig_band"] = None
        update_rbw()
        update_trigger_artists()

    def ingest():
        got = False
        while True:
            try:
                frame = frames.get_nowait()
            except queue.Empty:
                break
            if rx.paused:  # captured just before the pause; drop it to keep the rows still
                continue
            got = True
            iq, settings, when = frame
            if (settings["lo_hz"], settings["rate"]) != state["span"] or len(iq) != state.get("iq_len"):
                reset_span(settings, len(iq))
            mean, peak = dsp.spectrum(iq, state["n"], window["w"])
            a = avg_slider.val
            state["avg"] = mean if state["avg"] is None else a * state["avg"] + (1 - a) * mean
            state["hold"] = peak if state["hold"] is None else np.maximum(state["hold"], peak)
            wf = waterfall["a"]
            wf[1:] = wf[:-1]
            wf[0] = peak if wf_seg.index == 0 else mean
            dvr_push(iq, settings, when)
            state["count"] += 1
            state["times"].append(time.time())
            state["last"] = frame
            state["last_spectra"] = (mean, peak)
            check_trigger(peak, frame)
        return got

    def redraw():
        if state["last"] is None or state["avg"] is None:
            return
        f, avg = state["f"], state["avg"]
        pts = np.column_stack([f, avg])
        trace.set_segments(np.stack([pts[:-1], pts[1:]], axis=1))
        trace.set_array((avg[:-1] + avg[1:]) / 2)
        glow1.set_data(f, avg)
        glow2.set_data(f, avg)
        fill.set_xy(np.vstack([[f[0], state["vmin"] - 5], pts, [f[-1], state["vmin"] - 5]]))
        line_peak.set_data(f, state["last_spectra"][1])
        line_hold.set_data(f, state["hold"] if state["hold"] is not None else state["last_spectra"][1])
        image.set_data(waterfall["a"])
        draw_markers()

        iq, settings, _ = state["last"]
        centred = iq - iq.mean()
        rms = float(np.sqrt(np.mean(np.abs(centred) ** 2)))
        dbfs = 20 * np.log10(max(rms, 1e-3) / dsp.FULL_SCALE)
        lit = int(round((dbfs - M_LO) / (M_HI - M_LO) * BLOCKS))
        for i, r in enumerate(blocks):
            r.set_facecolor(block_color(i) if i < lit else "#0d2131")
        clip = np.mean((np.abs(iq.real) >= 500) | (np.abs(iq.imag) >= 500)) * 100
        level_text.set_text(f"{dbfs:5.1f} dBFS")
        level_text.set_color(RED if clip > 0.1 else CYAN)

        lo, rate = settings["lo_hz"] / 1e6, settings["rate"]
        subtitle.set_text(f"{radio.chip} internal receiver  ·  {radio.backend}  ·  LO {lo:.4f} MHz  ·  "
                          f"{lo - rate / 2e6:.0f} – {lo + rate / 2e6:.0f} MHz")
        t = state["times"]
        rate_now = (len(t) - 1) / (t[-1] - t[0]) if len(t) > 1 and t[-1] > t[0] else 0.0
        stats["LO"].set_text(f"{lo:.4f} MHz")
        stats["Span · RBW"].set_text(f"{rate / 1e6:.0f} MHz · {rate / state['n'] / 1e3:.1f} kHz")
        stats["Gain"].set_text(f"{'AGC' if settings['agc'] else settings['gain']}  ({settings['gain_detail']})")
        stats["Snapshot"].set_text(f"{len(iq):,} pairs · {duration(len(iq) / rate)}")
        stats["Rate"].set_text("paused" if rx.paused else f"{rate_now:.1f} snapshots/s")
        stats["Clipped"].set_text(f"{clip:.2f} %")
        stats["Clipped"].set_color(RED if clip > 0.1 else TEXT)

    def refresh_status():
        """Status bar, clock and save counters; True if anything changed."""
        before = (status.get_text(), clock.get_text(), stats["Saved"].get_text(), tuple(dot.get_facecolor()))
        s = state["saved"]
        stats["Saved"].set_text(f"{s['manual']} snap · {s['trigger']} trig · {s['box']} box")
        stats["Last saved"].set_text(state["last_file"][:36])
        stats["Last saved"].set_fontsize(8 if sum(s.values()) else 10)
        h, rem = divmod(int(time.time() - state["t0"]), 3600)
        clock.set_text(f"{h:02d}:{rem // 60:02d}:{rem % 60:02d}")
        armed = trig_check.get_status()[0]
        toast_msg = state["toast"]
        if rx.error:
            color, text = RED, f"Receiver error: {rx.error}"
        elif toast_msg and time.time() < toast_msg[2]:
            color, text = toast_msg[1], toast_msg[0]
        elif rx.paused:
            color, text = YELLOW, (f"Paused at snapshot {state['count']}  ·  IQ snapshot saves this frame  ·  "
                                   "drag a box on the waterfall to save part of it  ·  space resumes")
        else:
            color = ORANGE if armed else GREEN
            text = (f"Radio healthy  ·  {radio.chip}  ·  {radio.firmware}  ·  {port}"
                    + (f"  ·  MAC {radio.mac}" if args.show_mac else "")
                    + f"  ·  snapshot {state['count']}"
                    + (f"  ·  trigger armed ≥ {thr_slider.val:.0f} dBFS" if armed else ""))
        dot.set_facecolor(color)
        status.set_text(text)
        status.set_color(MUTED if color == GREEN else color)
        return before != (status.get_text(), clock.get_text(), stats["Saved"].get_text(),
                          tuple(dot.get_facecolor()))

    # ---- start --------------------------------------------------------------------------------
    set_range()
    set_banks(prefs["banks"])
    length_labels(rate0)
    set_overlay(prefs["overlay"])
    layout_meter()
    describe_box()
    show_page(prefs["page"])

    if args.screenshot:
        deadline = time.time() + 6
        while time.time() < deadline:
            if ingest():
                redraw()
            refresh_status()
            time.sleep(0.02)
        repaint()
        fig.savefig(args.screenshot, dpi=110, facecolor=INK)
        rx.running = False
        print(f"saved {args.screenshot}: {state['count']} snapshots")
        return

    def tick():  # must not return False: matplotlib timers drop such callbacks
        got = ingest()
        if got:
            redraw()
        if refresh_status() or got:
            fig.canvas.draw_idle()

    timer = fig.canvas.new_timer(interval=30)
    timer.add_callback(tick)
    timer.start()
    plt.show()
    rx.running = False
    radio.close()


if __name__ == "__main__":
    main()
