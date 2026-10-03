"""Signal processing and SigMF output for eSpDR snapshots.

Snapshots arrive from radios.py in the usual orientation (RF = LO + f) as
raw ADC counts; files are written the same way.
"""
import datetime as dt
import json
import os

import numpy as np

FULL_SCALE = 512  # signed 10-bit ADC


def spectrum(iq, n, window):
    """Mean and peak power (dBFS) per bin over the snapshot's n-point segments,
    ordered by increasing RF frequency."""
    iq = iq - iq.mean()
    segs = iq[: (len(iq) // n) * n].reshape(-1, n)
    p = np.abs(np.fft.fft(segs * window, axis=1)) ** 2 / (FULL_SCALE * window.sum()) ** 2
    p = np.fft.fftshift(p, axes=1)
    return 10 * np.log10(p.mean(0) + 1e-20), 10 * np.log10(p.max(0) + 1e-20)


def frequencies(lo_hz, rate, n):
    """RF frequency (MHz) of each bin returned by spectrum()."""
    return (lo_hz + (np.arange(n) - n / 2) * rate / n) / 1e6


def lowpass(cutoff, fs, taps):
    """Windowed-sinc low-pass FIR, unity gain at DC."""
    k = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / fs * k) * np.blackman(taps)
    return h / h.sum()


def box_plan(rate, f_lo_mhz, f_hi_mhz):
    """Decimation and filter for extracting [f_lo, f_hi]: output rate is at
    least 1.25x the box width."""
    bw = (f_hi_mhz - f_lo_mhz) * 1e6
    decim = max(1, int(rate // (1.25 * bw)))
    taps = max(31, 8 * decim + 1) | 1
    return {"bw": bw, "decim": decim, "rate_out": rate / decim, "taps": taps}


def extract(iq_raw, lo_hz, rate, f_lo_mhz, f_hi_mhz):
    """Shifts the box centre to 0 Hz, filters to the box and decimates one
    snapshot. Returns complex64 in the usual orientation; the filter's edge
    transient is dropped, so the result is slightly shorter than len/decim."""
    plan = box_plan(rate, f_lo_mhz, f_hi_mhz)
    x = iq_raw.astype(np.complex64)
    x = x - x.mean()
    centre = (f_lo_mhz + f_hi_mhz) / 2 * 1e6
    t = np.arange(len(x)) / rate
    x = x * np.exp(-2j * np.pi * (centre - lo_hz) * t).astype(np.complex64)
    h = lowpass(plan["bw"] / 2, rate, plan["taps"])
    y = np.convolve(x, h, mode="valid")[:: plan["decim"]]
    return (y / FULL_SCALE).astype(np.complex64), plan


def _stamp(when):
    return when.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


DEFAULT_HW = "ESP32 internal Wi-Fi receiver"


def _base_meta(datatype, rate, description, settings, mac, hw=DEFAULT_HW):
    meta = {
        "core:datatype": datatype,
        "core:sample_rate": rate,
        "core:version": "1.0.0",
        "core:hw": hw,
        "core:recorder": "eSpDRmini",
        "core:description": description,
        "espdr:gain": "hardware AGC" if settings.get("agc") else settings["gain"],
        "espdr:gain_detail": settings.get("gain_detail", ""),
    }
    if mac:
        meta["espdr:mac"] = mac
    if "lo_mode" in settings:
        meta.update({"espdr:lo_mode": settings["lo_mode"], "espdr:pll_hz": settings["pll_hz"],
                     "espdr:sdm_word": settings["sdm_word"]})
    return meta


def _write(base, data, meta):
    data.tofile(base + ".sigmf-data")
    with open(base + ".sigmf-meta", "w") as f:
        json.dump(meta, f, indent=2)


def save_snapshot(directory, iq_raw, settings, when, mac=None, annotations=(), tag="", hw=DEFAULT_HW):
    """Writes one raw snapshot as SigMF ci16_le. Returns the path without extension."""
    os.makedirs(directory, exist_ok=True)
    lo_mhz, rate = settings["lo_hz"] / 1e6, settings["rate"]
    base = os.path.join(directory, f"iq_{tag}{lo_mhz:.3f}MHz_{rate / 1e6:.0f}Msps_"
                                   f"{when.strftime('%Y%m%dT%H%M%S_%f')[:-3]}Z")
    iq = iq_raw
    data = np.stack([iq.real, iq.imag], 1).astype("<i2")
    meta = {
        "global": _base_meta("ci16_le", rate,
                             f"{len(iq)} contiguous IQ pairs ({len(iq) / rate * 1e6:.0f} us). Raw signed "
                             "10-bit ADC counts (-512..511), oriented so that RF = LO + f.",
                             settings, mac, hw),
        "captures": [{"core:sample_start": 0, "core:frequency": settings["lo_hz"],
                      "core:datetime": _stamp(when)}],
        "annotations": list(annotations),
    }
    _write(base, data, meta)
    return base


def save_box(directory, rows, f_lo_mhz, f_hi_mhz, mac=None, per_snapshot=False, hw=DEFAULT_HW):
    """Extracts a waterfall box. rows: (iq_raw, settings, when) in time order.

    Writes one cf32_le SigMF recording whose capture segments are the
    snapshots (with gaps between them), or one recording per snapshot.
    Returns (output directory, plan, total samples)."""
    first_when = rows[0][2]
    out = os.path.join(directory, f"box_{first_when.strftime('%Y%m%dT%H%M%S')}Z_"
                                  f"{f_lo_mhz:.3f}-{f_hi_mhz:.3f}MHz")
    suffix, unique = 1, out
    while os.path.exists(unique):
        suffix += 1
        unique = f"{out}_{suffix}"
    out = unique
    os.makedirs(out)
    centre_hz = (f_lo_mhz + f_hi_mhz) / 2 * 1e6
    parts, captures, annotations, start, plan = [], [], [], 0, None
    for k, (iq_raw, settings, when) in enumerate(rows):
        y, plan = extract(iq_raw, settings["lo_hz"], settings["rate"], f_lo_mhz, f_hi_mhz)
        if per_snapshot:
            meta = {
                "global": _base_meta("cf32_le", plan["rate_out"], _box_text(f_lo_mhz, f_hi_mhz, plan, settings),
                                     settings, mac, hw),
                "captures": [{"core:sample_start": 0, "core:frequency": centre_hz,
                              "core:datetime": _stamp(when)}],
                "annotations": [],
            }
            _write(os.path.join(out, f"snapshot_{k:04d}_{when.strftime('%H%M%S_%f')[:-3]}"), y, meta)
        else:
            parts.append(y)
            captures.append({"core:sample_start": start, "core:frequency": centre_hz,
                             "core:datetime": _stamp(when)})
            annotations.append({"core:sample_start": start, "core:sample_count": len(y),
                                "core:label": f"snapshot {k}",
                                "core:comment": "separate snapshot; not continuous with the previous segment"})
        start += len(y)
    if not per_snapshot:
        settings = rows[0][1]
        meta = {
            "global": _base_meta("cf32_le", plan["rate_out"],
                                 _box_text(f_lo_mhz, f_hi_mhz, plan, settings) +
                                 f" {len(rows)} snapshots as capture segments; each segment is contiguous, "
                                 "but there are gaps between segments (see core:datetime).",
                                 settings, mac, hw),
            "captures": captures,
            "annotations": annotations,
        }
        _write(os.path.join(out, "box"), np.concatenate(parts), meta)
    return out, plan, start


def _box_text(f_lo, f_hi, plan, settings):
    return (f"Waterfall box {f_lo:.3f}-{f_hi:.3f} MHz, shifted to 0 Hz, low-pass filtered to "
            f"{plan['bw'] / 1e6:.3f} MHz and decimated by {plan['decim']} from "
            f"{settings['rate'] / 1e6:g} Msps. Complex float, full scale = 1.0.")


def now_utc():
    return dt.datetime.now(dt.timezone.utc)
