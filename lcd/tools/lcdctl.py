#!/usr/bin/env python3
"""Talks to the eSpDRmini LCD firmware over its native USB port.

    lcdctl.py stat                      receiver and display figures
    lcdctl.py shot screen.png           save what the display shows
    lcdctl.py tap X Y                   synthetic touch
    lcdctl.py swipe X0 Y0 X1 Y1 [MS]
    lcdctl.py touch X Y / lcdctl.py up

Opening the port doesn't reset the board. Uses pyserial, numpy and Pillow
(all in the repository's requirements.txt).
"""
import argparse
import sys
import time

import numpy as np
import serial
from serial.tools import list_ports


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    sys.exit("no Espressif USB port found")


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 5
    # The port opens with DTR and RTS both set. Dropping DTR first would reset
    # the chip (RTS without DTR), so drop RTS first, then DTR.
    s.dtr, s.rts = True, False
    s.open()
    s.dtr = False
    time.sleep(0.05)
    s.reset_input_buffer()
    return s


def ask(s, line):
    s.write((line + "\n").encode())
    while True:
        reply = s.readline().decode(errors="replace").strip()
        if not reply:
            sys.exit(f"no reply to {line!r}")
        if reply.startswith(("OK", "ERR", "SHOT", "fps")):
            return reply


def shot(s, path):
    head = ask(s, "SHOT").split()
    if head[0] != "SHOT":
        sys.exit(" ".join(head))
    w, h, n = map(int, head[1:4])
    data = bytearray()
    while len(data) < n:
        chunk = s.read(n - len(data))
        if not chunk:
            sys.exit(f"short read: {len(data)} of {n} bytes")
        data += chunk
    px = np.frombuffer(bytes(data), "<u2").reshape(h, w).astype(np.uint32)
    rgb = np.stack([(px >> 11) & 31, (px >> 5) & 63, px & 31], axis=-1).astype(np.float32)
    rgb = (rgb / [31, 63, 31] * 255 + 0.5).astype(np.uint8)
    from PIL import Image
    Image.fromarray(rgb, "RGB").save(path)
    print(f"saved {path} ({w}x{h})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("cmd", choices=["stat", "shot", "tap", "swipe", "touch", "up"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    s = open_port(a.port or find_port())
    if a.cmd == "stat":
        print(ask(s, "STAT"))
    elif a.cmd == "shot":
        shot(s, a.args[0] if a.args else "screen.png")
    elif a.cmd == "tap":
        print(ask(s, "TAP %s %s" % tuple(a.args[:2])))
    elif a.cmd == "swipe":
        x0, y0, x1, y1 = a.args[:4]
        ms = a.args[4] if len(a.args) > 4 else "400"
        print(ask(s, f"SWIPE {x0} {y0} {x1} {y1} {ms}"))
    elif a.cmd == "touch":
        print(ask(s, "TOUCH %s %s" % tuple(a.args[:2])))
    else:
        print(ask(s, "UP"))


if __name__ == "__main__":
    main()
