"""Checks a backend's spectrum orientation against the steady carrier near
2448 MHz that ESP32 boards pick up (a fixed RF signal, not a baseband spur):
correctly oriented samples show it at 2448 MHz, mirrored ones at 2432 MHz.

    python tools/orientation.py [--backend auto|espdr|esp-sdr] [--lo 2440]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import dsp  # noqa: E402
import radios  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--backend", choices=("auto", "espdr", "esp-sdr"), default="auto")
ap.add_argument("--port")
ap.add_argument("--lo", type=float, default=2440.0)
ap.add_argument("--carrier", type=float, default=2448.0, help="reference carrier, MHz")
a = ap.parse_args()

r = radios.open_default(a.port, a.backend)
r.set_rate(80e6 if 80e6 in r.rates else r.rates[0])
r.set_lo(a.lo * 1e6)
if not r.has_agc:
    r.set_gain(48)
n = 8192
acc = 0
for _ in range(30):
    m, _ = dsp.spectrum(r.snapshot(), n, np.blackman(n))
    acc = acc + 10 ** (m / 10)
s = r.settings()
f = dsp.frequencies(s["lo_hz"], s["rate"], n)
db = 10 * np.log10(acc / 30)
mirror = 2 * s["lo_hz"] / 1e6 - a.carrier


def line(c):  # narrow peak above the local floor
    near = np.abs(f - c) < 0.3
    floor = np.median(db[(np.abs(f - c) > 0.5) & (np.abs(f - c) < 3)])
    return db[near].max() - floor


right, wrong = line(a.carrier), line(mirror)
print(f"{r.backend} on {r.chip}: carrier at {a.carrier:g} MHz stands {right:.1f} dB above the floor, "
      f"at its mirror {mirror:g} MHz {wrong:.1f} dB")
print("orientation OK" if right > wrong + 6 else "MIRRORED: flip this chip" if wrong > right + 6
      else "inconclusive: no clear carrier (try another LO or --carrier)")
r.close()
