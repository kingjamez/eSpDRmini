"""Receiver backends for the viewer.

Two kinds of firmware are supported, behind one interface:

* ``EspdrRadio``: eSpDR with the snapshot patch (firmware/), ESP32-S3 only,
  loaded into RAM by the viewer. Up to four chained capture banks.
* ``EspSdrRadio``: ESPARGOS esp-sdr (https://github.com/ESPARGOS/esp-sdr),
  flashed onto the board by the user. Supports the ESP32, C3, C5 (2.4 and
  5 GHz), C6, C61, S2, S3 and S31. This module talks to it over its
  documented text protocol (INFO, CAPS, LIMITS?, RANGE?, FREQ, GAIN, CAP20);
  no esp-sdr code is included here.

Every backend returns snapshots as complex64 in raw ADC counts and in the
usual orientation, where a signal at RF = LO + f appears at +f.
"""
import json
import random
import time
import zlib

import numpy as np
import serial

import espctl

# esp-sdr rate codes, from its README: 0-6 = 80, 40, 20, 10, 8, 4, 16 MS/s.
ESPSDR_RATE_CODES = {80e6: 0, 40e6: 1, 20e6: 2, 10e6: 3, 8e6: 4, 4e6: 5, 16e6: 6}

# Spectrum orientation of each esp-sdr chip's raw I + jQ: True when a signal at
# RF = LO + f arrives at -f. Measured against known Wi-Fi channels; chips not
# listed are assumed to match the S3 until checked (see tools/orientation.py).
ESPSDR_MIRRORED = {"S3SDR": True}

# Slider range shown for each chip family (MHz). esp-sdr accepts 100-6000 MHz
# tuning attempts, but PLL lock outside these bands has not been verified.
ESPSDR_UI_RANGE = {"C5SDR": (2300.0, 5950.0)}
DEFAULT_UI_RANGE = (2200.0, 2800.0)


class Radio:
    """Common interface. Frequencies in Hz unless named _mhz."""

    backend = ""          # "eSpDR" or "esp-sdr"
    chip = ""             # e.g. "ESP32-S3"
    firmware = ""         # identity string for the status bar
    hw = ""               # SigMF core:hw
    mac = None
    rates = ()            # supported sample rates, Hz, fastest first
    lo_range_mhz = (0, 0) # accepted by the firmware
    ui_range_mhz = (0, 0) # tuning slider
    lo_step_mhz = 0.001
    gain_max = 127
    has_agc = False
    max_banks = 1         # snapshot length multiples
    bank_pairs = 0        # samples per bank

    def settings(self):
        """{'lo_hz', 'rate', 'gain', 'agc', 'gain_detail'}"""
        raise NotImplementedError

    def set_lo(self, hz): raise NotImplementedError
    def set_rate(self, hz): raise NotImplementedError
    def set_gain(self, index): raise NotImplementedError
    def set_agc(self, on): raise NotImplementedError
    def snapshot(self, banks=1): raise NotImplementedError
    def close(self): pass


# ---- eSpDR (snapshot patch) ------------------------------------------------------------

class EspdrRadio(Radio):
    backend = "eSpDR"
    chip = "ESP32-S3"
    hw = "ESP32-S3 internal Wi-Fi/BT receiver, eSpDR snapshot firmware"
    rates = (80e6, 16e6)
    lo_range_mhz = ui_range_mhz = (2220.0, 2790.0)  # PLL lock measured on the reference board
    lo_step_mhz = 0.0005
    gain_max = 82
    max_banks = espctl.MAX_BANKS
    bank_pairs = espctl.SNAP_PAIRS

    def __init__(self, esp):
        self.esp = esp
        fw = esp.command(espctl.CTL_INFO, 0)
        self.firmware = f"eSpDR firmware 0x{fw:08X}"
        mac = esp.command(espctl.CTL_INFO, 1).to_bytes(4, "little") + esp.command(espctl.CTL_INFO, 2).to_bytes(2, "little")
        self.mac = mac.hex(":")

    def settings(self):
        s = self.esp.settings()
        return {"lo_hz": s["lo_hz"], "rate": espctl.RATE_SPS[s["rate"]], "gain": s["gain"], "agc": False,
                "gain_detail": f"rf {s['rf_gain']} / bb {s['bb_gain']}"}

    def set_lo(self, hz):
        self.esp.command(espctl.ESP_SET_LO, int(round(hz)))

    def set_rate(self, hz):
        self.esp.command(espctl.ESP_SET_RATE, 0 if hz == 80e6 else 1)

    def set_gain(self, index):
        self.esp.command(espctl.ESP_SET_GAIN, int(index))

    def set_agc(self, on):
        pass

    def snapshot(self, banks=1):
        return np.conj(self.esp.snapshot(banks))  # eSpDR's raw convention is RF = LO - f

    def close(self):
        self.esp.close()


# ---- esp-sdr -----------------------------------------------------------------------------

class EspSdrError(Exception):
    pass


class EspSdrRadio(Radio):
    backend = "esp-sdr"
    lo_step_mhz = 1.0
    has_agc = True

    def __init__(self, port, timeout=3.0):
        self.port = port
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, timeout
        self.ser.dtr = self.ser.rts = False  # set before opening: toggling them can reset the chip
        self.ser.open()
        self.resync()
        ident = self.ask("INFO").split()
        if len(ident) < 4 or ident[2] != "burst":
            raise EspSdrError(f"not esp-sdr firmware: {' '.join(ident)!r}")
        self.chip_id, self.protocol, self.bank_pairs = ident[0], int(ident[1]), int(ident[3])
        self.chip = "ESP32-" + self.chip_id[:-3] if self.chip_id != "ESP32SDR" else "ESP32"
        self.caps = set(self.ask("CAPS").split()[1:])
        limits = json.loads(self.ask("LIMITS?").split(" ", 1)[1]) if "RXLIMITS" in self.caps else {}
        self.rates = tuple(float(r) for r in limits.get("rates", [80e6]))
        self.gain_max = limits.get("gain", [0, 127, 1])[1]
        rng = self.ask("RANGE?").split()
        self.lo_range_mhz = (float(rng[1]), float(rng[2])) if len(rng) >= 3 else (2400.0, 2500.0)
        lo, hi = ESPSDR_UI_RANGE.get(self.chip_id, DEFAULT_UI_RANGE)
        self.ui_range_mhz = (max(lo, self.lo_range_mhz[0]), min(hi, self.lo_range_mhz[1]))
        self.mirrored = ESPSDR_MIRRORED.get(self.chip_id, True)
        self.firmware = f"esp-sdr {self.chip_id} protocol {self.protocol}"
        self.hw = f"{self.chip} internal Wi-Fi receiver, ESPARGOS esp-sdr firmware"
        self._lo_mhz = 2440
        self._rate = self.rates[0]
        self.bandwidth = limits.get("bandwidth")  # [min, max, step, default] MHz, or None
        if self.bandwidth:
            self.ask("BANDWIDTH 0")  # widest filter, so the whole sampled span shows signals
        self.set_lo(2440e6)
        self.set_agc(True)

    # -- protocol --
    def _line(self):
        raw = self.ser.readline()
        if not raw.endswith(b"\n"):
            raise EspSdrError("timeout waiting for a reply")
        return raw.decode("ascii", "replace").strip()

    def ask(self, command):
        self.ser.write(command.encode() + b"\n")
        reply = self._line()
        if reply.startswith("ERR"):
            raise EspSdrError(f"{command}: {reply}")
        return reply

    def resync(self, patience=8.0):
        """Lines the protocol up again: waits out any boot log (opening the port
        can reset the chip), then repeats SYNC <nonce> until it is echoed."""
        deadline = time.time() + patience
        quiet_since = time.time()
        while time.time() < deadline and time.time() - quiet_since < 0.3:
            if self.ser.in_waiting:
                self.ser.read(self.ser.in_waiting)
                quiet_since = time.time()
            time.sleep(0.02)
        timeout, self.ser.timeout = self.ser.timeout, 0.4
        try:
            while time.time() < deadline:
                nonce = random.getrandbits(48)
                self.ser.write(f"\nSYNC {nonce}\n".encode())
                until = time.time() + 0.6
                while time.time() < until:
                    raw = self.ser.readline()
                    if raw.strip() == f"SYNC {nonce}".encode():
                        return
        finally:
            self.ser.timeout = timeout
        raise EspSdrError("no SYNC reply")

    # -- Radio interface --
    def settings(self):
        g = self.ask("GAIN?").split()  # GAIN <HARDWARE|MANUAL> <code|-1> 0 <max> <forced>
        agc = g[1] == "HARDWARE"
        return {"lo_hz": self._lo_mhz * 1e6, "rate": self._rate, "gain": None if agc else int(g[2]),
                "agc": agc, "gain_detail": "hardware AGC" if agc else f"index {g[2]} of {g[4]}"}

    def set_lo(self, hz):
        mhz = int(round(hz / 1e6))
        self.ask(f"FREQ {mhz}")
        self._lo_mhz = mhz

    def set_rate(self, hz):
        if hz not in self.rates:
            raise EspSdrError(f"rate {hz / 1e6:g} MS/s not supported")
        self._rate = hz

    def set_gain(self, index):
        self.ask(f"GAIN MANUAL {int(min(max(index, 0), self.gain_max))}")

    def set_agc(self, on):
        if on:
            self.ask("GAIN HARDWARE")

    def snapshot(self, banks=1):
        n = self.bank_pairs
        header = self.ask(f"CAP20 {n} {ESPSDR_RATE_CODES[self._rate]}").split()
        if header[0] != "DATA" or int(header[1]) != n:
            raise EspSdrError(f"unexpected capture reply {' '.join(header)!r}")
        size = (n * 20 + 7) // 8
        payload = self.ser.read(size)
        if len(payload) != size:
            self.resync()
            raise EspSdrError(f"short capture: {len(payload)} of {size} bytes")
        if zlib.crc32(payload) != int(header[2], 16):
            self.resync()
            raise EspSdrError("capture CRC mismatch")
        iq = espctl.unpack20(payload)
        return np.conj(iq) if self.mirrored else iq

    def close(self):
        try:
            self.ser.write(b"RELEASE\n")
            self.ser.flush()
        finally:
            self.ser.close()


# ---- opening a board -----------------------------------------------------------------------

def _probe_espsdr(port):
    try:
        return EspSdrRadio(port, timeout=1.0)
    except (EspSdrError, serial.SerialException, OSError, ValueError):
        return None


def _probe_espdr(port):
    try:
        esp = espctl.Esp(port, timeout=0.5)
        if esp.command(espctl.CTL_INFO, 0) == espctl.FIRMWARE_ID:
            esp.ser.timeout = 5
            return esp
        esp.close()
    except Exception:
        pass
    return None


def open_radio(port, backend="auto", reload=False):
    """Connects to whatever firmware the board runs. With backend "auto", a
    board running esp-sdr is used as is; otherwise the eSpDR snapshot firmware
    is loaded into RAM (ESP32-S3 only)."""
    if backend in ("auto", "esp-sdr") and not reload:
        radio = _probe_espsdr(port)
        if radio:
            radio.ser.timeout = 5
            return radio
        if backend == "esp-sdr":
            raise SystemExit(f"No esp-sdr firmware answered on {port}. Flash it first: "
                             "https://espargos.net/espsdr/app/flash.html")
    esp = None if reload else _probe_espdr(port)
    if esp is None:
        print("loading eSpDR snapshot firmware into RAM...")
        try:
            espctl.load_ram(port)
        except Exception as e:
            raise SystemExit(f"Could not load the eSpDR firmware ({e}). eSpDR runs on the ESP32-S3 only; "
                             "for other ESP32 chips flash esp-sdr: https://espargos.net/espsdr/app/flash.html")
        esp = espctl.Esp(port)
    return EspdrRadio(esp)


def open_default(port=None, backend="auto", reload=False):
    return open_radio(port or espctl.find_port(), backend, reload)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="identify the board and firmware")
    ap.add_argument("--port")
    ap.add_argument("--backend", choices=("auto", "espdr", "esp-sdr"), default="auto")
    a = ap.parse_args()
    r = open_default(a.port, a.backend)
    print(f"{r.backend}: {r.chip}, {r.firmware}; rates {[f'{x / 1e6:g}' for x in r.rates]} MS/s; "
          f"LO {r.lo_range_mhz} MHz (slider {r.ui_range_mhz}); gain 0-{r.gain_max}"
          f"{' + AGC' if r.has_agc else ''}; {r.max_banks} x {r.bank_pairs} pairs")
    print(r.settings())
    t = time.time()
    iq = r.snapshot()
    print(f"snapshot: {len(iq)} pairs in {(time.time() - t) * 1000:.0f} ms, rms {np.std(iq):.1f}")
    r.close()
