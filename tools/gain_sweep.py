import sys, numpy as np, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import espctl  # noqa: E402

out = sys.argv[1]
esp = espctl.Esp()
fig, ax = plt.subplots(figsize=(11, 5))
N = 4096
win = np.blackman(N)
for gain in (24, 40, 55, 70):
    esp.command(espctl.ESP_SET_GAIN, gain)
    psd = np.zeros(N); peak = np.full(N, -1e9); rms = []
    for _ in range(20):
        iq = esp.snapshot()
        iq = iq - iq.mean()
        rms.append(np.sqrt(np.mean(np.abs(iq) ** 2)))
        segs = iq[: (len(iq) // N) * N].reshape(-1, N)
        p = np.abs(np.fft.fftshift(np.fft.fft(segs * win, axis=1), axes=1)) ** 2
        psd += p.mean(0); peak = np.maximum(peak, p.max(0))
    norm = (512 * win.sum()) ** 2
    f = 2440 - np.fft.fftshift(np.fft.fftfreq(N, 1 / 80))  # ESP: LO minus RF
    ax.plot(f, 10 * np.log10(peak / norm + 1e-20), lw=0.6, label=f"gain {gain} peak (rms {np.mean(rms):.1f}, max |x| {np.abs(iq).max():.0f})")
    print(f"gain {gain}: rms {np.mean(rms):.1f} counts, rf {esp.stat('rf_gain')}, bb {esp.stat('bb_gain')}")
esp.command(espctl.ESP_SET_GAIN, 24)
ax.set_xlabel("MHz (assuming RF = LO - f)"); ax.set_ylabel("dBFS"); ax.legend(); ax.grid(alpha=.3)
ax.set_title("ESP32-S3 snapshot spectrum, LO 2440 MHz, 80 Msps, peak over 20 snapshots")
fig.tight_layout(); fig.savefig(out, dpi=110)
