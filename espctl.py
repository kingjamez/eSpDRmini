"""Host side of the eSpDR control protocol (protocol/control.h), for the
ESP32-S3 on its native USB Serial/JTAG port, plus RAM loading via esptool."""
import glob
import os
import struct
import subprocess
import sys
import time
import zlib

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IMAGE = os.path.join(HERE, "firmware", "espdr-snapshot.bin")  # see firmware/README.md
ESPRESSIF_VID = 0x303A

REQ_MAGIC, RESP_MAGIC = 0xB4, 0xB5
CTL_INFO, CTL_SAFE, CTL_STATUS = 1, 2, 3
ESP_ARG_HIGH = 19
ESP_SET_LO, ESP_SET_RATE, ESP_SET_WIDTH, ESP_SET_FILTER = 20, 21, 22, 23
ESP_SET_GAIN, ESP_SET_RF_GAIN, ESP_SET_BB_GAIN, ESP_SET_DC, ESP_SET_IQ = 24, 25, 26, 27, 28
ESP_SNAPSHOT = 40  # added by the snapshot firmware
ESP_AUTO = 0x8000
FIRMWARE_ID = 0x49515305

STATUS_NAMES = {0: "OK", 1: "UNKNOWN_OP", 2: "BAD_ARGUMENT", 3: "BUSY", 4: "NOT_READY",
                5: "RUN_FAILED", 6: "FAILED"}
RADIO_NAMES = {0: "OK", 1: "PHY_FAILED", 2: "PLL_FAILED", 3: "PBUS_FAILED"}
STAT = {"radio": 13, "lo_hz": 15, "rate": 16, "width": 17, "filter": 18, "gain": 19,
        "rf_gain": 20, "bb_gain": 21, "iq": 26, "automatic": 27, "pll": 28}
RATE_SPS = {0: 80e6, 1: 16e6}

SNAP_MAGIC = b"SNAP"
SNAP_PAIRS = 15360  # per capture bank
MAX_BANKS = 4


class ControlError(Exception):
    pass


def find_port():
    """The first Espressif native-USB port (macOS, Linux or Windows)."""
    from serial.tools import list_ports
    ports = sorted(p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID)
    if not ports:  # some platforms omit USB ids; fall back on the usual names
        ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        raise SystemExit("No ESP32-S3 found. Connect it by its native USB port with a data cable; "
                         "if it still doesn't appear, hold BOOT while plugging it in.")
    return ports[0]


def load_ram(port, image=DEFAULT_IMAGE):
    """Resets the ESP into its ROM loader and runs the image from RAM."""
    if not os.path.exists(image):
        raise SystemExit(f"Firmware image not found: {image} (see firmware/README.md)")
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3",
           "--port", port, "--before", "usb-reset", "--after", "no-reset", "--no-stub",
           "load-ram", image]
    subprocess.run(cmd, check=True)
    # The USB device re-enumerates when the image starts; wait for it.
    deadline = time.time() + 10
    while time.time() < deadline:
        time.sleep(0.5)
        try:
            with serial.Serial(port, timeout=0.1):
                pass
            return
        except (serial.SerialException, OSError):
            continue
    raise SystemExit(f"{port} did not come back after loading")


class Esp:
    def __init__(self, port=None, timeout=5.0):
        self.port = port or find_port()
        self.ser = serial.Serial(self.port, 115200, timeout=timeout)
        self.seq = 0
        time.sleep(0.05)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def _read_exact(self, n):
        data = self.ser.read(n)
        if len(data) != n:
            raise ControlError(f"timeout: wanted {n} bytes, got {len(data)}")
        return data

    def _send(self, op, arg):
        self.seq = (self.seq + 1) & 0xFFFF
        head = struct.pack("<BBHH", REQ_MAGIC, op, arg & 0xFFFF, self.seq)
        self.ser.write(head + struct.pack("<I", zlib.crc32(head)))
        return self.seq

    def _response(self, op, seq):
        while True:
            b = self._read_exact(1)
            if b[0] == RESP_MAGIC:
                break
        resp = b + self._read_exact(15)
        if struct.unpack_from("<I", resp, 12)[0] != zlib.crc32(resp[:12]):
            raise ControlError("response CRC mismatch")
        _, node, rop, status, rseq, _, value = struct.unpack_from("<BBBBHHI", resp)
        if rop != op or rseq != seq:
            raise ControlError(f"unexpected response op={rop} seq={rseq}")
        return status, value

    def command(self, op, arg=0, check=True):
        if arg > 0xFFFF:
            self.command(ESP_ARG_HIGH, arg >> 16)
        seq = self._send(op, arg)
        status, value = self._response(op, seq)
        if check and status != 0:
            raise ControlError(f"op {op} arg {arg}: {STATUS_NAMES.get(status, status)}")
        return value

    def stat(self, name):
        return self.command(CTL_STATUS, STAT[name])

    def settings(self):
        return {k: self.stat(k) for k in ("radio", "lo_hz", "rate", "width", "filter", "gain",
                                          "rf_gain", "bb_gain")}

    def snapshot(self, banks=1):
        """One block of banks * SNAP_PAIRS contiguous IQ pairs (banks 1-4) as
        complex64, in raw ADC counts."""
        import numpy as np
        seq = self._send(ESP_SNAPSHOT, banks)
        status, value = self._response(ESP_SNAPSHOT, seq)
        if status != 0:
            raise ControlError(f"snapshot: {STATUS_NAMES.get(status, status)}")
        header = self._read_exact(12)
        magic, pairs, _first = struct.unpack("<4sII", header)
        if magic != SNAP_MAGIC or pairs != banks * SNAP_PAIRS:
            raise ControlError(f"bad snapshot header {header!r}")
        payload = self._read_exact(pairs * 5 // 2)
        crc = struct.unpack("<I", self._read_exact(4))[0]
        if crc != zlib.crc32(header + payload):
            raise ControlError("snapshot CRC mismatch")
        # Two pairs per 5 bytes: 40 bits = I0 | Q0 << 10 | I1 << 20 | Q1 << 30.
        raw = np.frombuffer(payload, np.uint8).reshape(-1, 5).astype(np.uint64)
        bits = raw[:, 0] | raw[:, 1] << 8 | raw[:, 2] << 16 | raw[:, 3] << 24 | raw[:, 4] << 32
        fields = np.stack([(bits >> s) & 0x3FF for s in (0, 10, 20, 30)], axis=1).astype(np.int32)
        fields = np.where(fields >= 512, fields - 1024, fields).reshape(-1, 2)
        return (fields[:, 0] + 1j * fields[:, 1]).astype(np.complex64)


def main():
    import argparse
    ap = argparse.ArgumentParser(description="eSpDR ESP32-S3 control")
    ap.add_argument("--port")
    ap.add_argument("--show-mac", action="store_true", help="print the board's MAC address")
    sub = ap.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("load", help="load the firmware into RAM")
    lp.add_argument("--image", default=DEFAULT_IMAGE)
    sub.add_parser("status", help="identity and receiver settings")
    sp = sub.add_parser("snap", help="capture snapshots and print statistics")
    sp.add_argument("-n", type=int, default=5)
    sp.add_argument("--banks", type=int, default=1, choices=range(1, MAX_BANKS + 1),
                    help="capture banks per snapshot: 15,360 pairs each")
    sp.add_argument("--save", help="write the last snapshot as interleaved int16 .cs16")
    args = ap.parse_args()
    port = args.port or find_port()
    if args.cmd == "load":
        load_ram(port, args.image)
        print("loaded")
    esp = Esp(port)
    fw = esp.command(CTL_INFO, 0)
    line = f"firmware id 0x{fw:08X} ({'eSpDR' if fw == FIRMWARE_ID else 'unknown'})"
    if args.show_mac:
        mac = esp.command(CTL_INFO, 1).to_bytes(4, "little") + esp.command(CTL_INFO, 2).to_bytes(2, "little")
        line += f", MAC {mac.hex(':')}"
    print(line)
    s = esp.settings()
    print(f"radio {RADIO_NAMES.get(s['radio'], s['radio'])}, LO {s['lo_hz'] / 1e6:.6f} MHz, "
          f"rate {RATE_SPS[s['rate']] / 1e6:.0f} Msps, width {s['width']} MHz, filter {s['filter']}, "
          f"gain {s['gain']} (rf {s['rf_gain']}, bb {s['bb_gain']})")
    if args.cmd == "snap":
        import numpy as np
        t0 = time.time()
        for _ in range(args.n):
            iq = esp.snapshot(args.banks)
        dt = (time.time() - t0) / args.n
        print(f"{args.n} snapshots of {len(iq)} pairs, {dt * 1000:.0f} ms each ({1 / dt:.1f}/s)")
        print(f"I mean {iq.real.mean():+.1f} rms {iq.real.std():.1f} range {iq.real.min():.0f}..{iq.real.max():.0f}; "
              f"Q mean {iq.imag.mean():+.1f} rms {iq.imag.std():.1f} range {iq.imag.min():.0f}..{iq.imag.max():.0f}")
        if args.save:
            np.stack([iq.real, iq.imag], 1).astype("<i2").tofile(args.save)
            print("saved", args.save)


if __name__ == "__main__":
    main()
