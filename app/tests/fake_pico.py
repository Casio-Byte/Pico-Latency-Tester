"""Simulated Pico for integration-testing LatencyTester.exe under Wine + Xvfb.

Speaks the same serial protocol as firmware/main.py over a pty. Clicks through
the X server's XTEST extension and "senses" light by reading the pixel at the
screen centre, so the app's real full-screen target is exercised end to end.
Also emulates the MicroPython raw REPL so the firmware installer can be tested:
uploaded files are written to --fsroot and compared afterwards.
"""
import argparse
import base64
import os
import random
import sys
import threading
import time

import serial
from Xlib import X, display
from Xlib.ext import xtest

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("--fsroot", default="/tmp/fakepico_fs")
ap.add_argument("--extra-ms", type=float, default=0.0, help="added display delay")
args = ap.parse_args()

ser = serial.Serial(args.port, 115200, timeout=0.01)
d = display.Display()
root = d.screen().root
W, H = d.screen().width_in_pixels, d.screen().height_in_pixels
T0 = time.perf_counter()
LOG = open("/tmp/fakepico.log", "w")
cfg = {"dir": 1, "delta": 200, "timeout": 500000, "hold": 60, "confirm": 3, "button": 1}
seq = 0
mode = "tester"   # or "repl"/"raw"


def log(*a):
    print(*a, file=LOG, flush=True)


def now_us():
    return int((time.perf_counter() - T0) * 1e6) & 0x3FFFFFFF


def level():
    img = root.get_image(W // 2, H // 2, 1, 1, X.ZPixmap, 0xFFFFFFFF)
    b = img.data
    lum = (b[0] + b[1] + b[2]) / 3.0
    return int(150 + lum / 255.0 * 3200 + random.uniform(-6, 6))


def send(s):
    ser.write((s + "\r\n").encode())


def press(down):
    btn = 3 if cfg["button"] == 2 else 1
    xtest.fake_input(d, X.ButtonPress if down else X.ButtonRelease, btn)
    d.sync()


def do_trial():
    global seq
    seq += 1
    base = level()
    t0 = now_us()
    press(True)
    ts = now_us()
    t1 = -1
    end = time.perf_counter() + cfg["timeout"] / 1e6
    while time.perf_counter() < end:
        v = level()
        if (cfg["dir"] >= 0 and v > base + cfg["delta"]) or (cfg["dir"] <= 0 and v < base - cfg["delta"]):
            if args.extra_ms:
                time.sleep(args.extra_ms / 1000)
            t1 = now_us()
            break
    send("R %d %d %d %d %d" % (seq, t0, t1, ts, base))
    rem = cfg["hold"] / 1000 - ((now_us() - t0) & 0x3FFFFFFF) / 1e6
    if rem > 0:
        time.sleep(rem)
    press(False)
    log("trial", seq, t0, t1, (t1 - t0) / 1000 if t1 >= 0 else None)


def do_cap(a):
    n, period, pre = int(a[0]), int(a[1]), int(a[2])
    click = int(a[3]) if len(a) > 3 else 1
    samples = []
    t_pre = now_us()
    for _ in range(pre):
        samples.append(level())
    t0 = now_us()
    if click:
        press(True)
    t_post = now_us()
    # real pixel reads are slow, so synthesise a response from the measured levels
    lo = samples[-1] if samples else level()
    delay = 8000 + random.uniform(0, 4000)
    for i in range(n - pre):
        t = i * period
        if click:
            f = min(1.0, max(0.0, (t - delay) / 3000.0))
            v = lo + (3300 - lo) * f
        else:
            v = 2400 + 600 * (1 if (t // 2083) % 2 else -1)   # 240 Hz PWM
        samples.append(int(v + random.uniform(-5, 5)))
    if click:
        time.sleep(cfg["hold"] / 1000)
        press(False)
    send("CAP %d %d %d %d %d" % (n, period, pre, t_pre - t0, t_post - t0))
    raw = b"".join(int(v).to_bytes(2, "little") for v in samples)
    for i in range(0, len(raw), 1024):
        send("D " + raw[i:i + 1024].hex())
    send("CAPEND")


def handle(line):
    global mode
    w = line.split()
    if not w:
        return
    c = w[0].upper()
    log("cmd", line)
    if c == "HELLO":
        send("HELLO lattest 2.0 Raspberry_Pi_Pico_W_with_RP2040 200")
    elif c == "PING":
        time.sleep(random.uniform(0.0002, 0.0012))      # USB frame timing
        send("P %d %s" % (now_us(), w[1] if len(w) > 1 else "0"))
    elif c == "LEVEL":
        vals = [level() for _ in range(16)]
        send("LEVEL %d %d %d" % (sum(vals) // 16, min(vals), max(vals)))
    elif c == "CFG":
        for kv in w[1:]:
            k, v = kv.split("=")
            cfg[k] = int(v)
        send("OK")
    elif c == "TRIAL":
        do_trial()
    elif c == "CAP":
        do_cap(w[1:])
    elif c == "STREAM":
        stream()
    elif c in ("STOP",):
        send("OK")
    elif c == "LED":
        send("OK")
    elif c == "EXIT":
        send("BYE")
        mode = "repl"
    else:
        send("ERR unknown " + c)


pending = b""


def stream():
    global pending
    while True:
        if ser.in_waiting:
            return
        vals = [str(level()) for _ in range(10)]
        send("L " + " ".join(vals))
        time.sleep(0.02)


# ---------- raw REPL emulation (for the installer) ----------
fs = args.fsroot
os.makedirs(fs, exist_ok=True)
raw_code = b""
RAW_GLOBALS = {"__name__": "__fakepico__"}


def raw_exec(code):
    """Run the uploaded snippet in a sandbox dir that stands in for the Pico's flash."""
    out, err = [], []
    g = RAW_GLOBALS  # like MicroPython, globals persist between raw-REPL runs
    cwd = os.getcwd()
    os.chdir(fs)
    import io, contextlib, binascii, types
    fake_sys = types.SimpleNamespace(implementation=types.SimpleNamespace(_machine="Raspberry Pi Pico W with RP2040"))
    real_import = __builtins__.__import__ if hasattr(__builtins__, "__import__") else __import__

    def imp(name, *a, **k):
        if name == "sys":
            return fake_sys
        return real_import(name, *a, **k)
    if "__builtins__" not in g:
        g["__builtins__"] = dict(__builtins__.__dict__ if hasattr(__builtins__, "__dict__") else __builtins__)
        g["__builtins__"]["__import__"] = imp
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            exec(compile(code.decode(), "<raw>", "exec"), g)
    except Exception as e:
        err.append(repr(e))
    finally:
        os.chdir(cwd)
    return buf.getvalue().replace("\n", "\r\n"), "".join(err)


def main():
    global mode, pending, raw_code
    log("fake pico up", W, H)
    raw_g = None
    while True:
        data = ser.read(4096)
        if not data:
            continue
        if mode == "tester":
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                handle(line.decode(errors="replace").strip())
            if mode != "tester":
                pending = b""
        elif mode == "repl":
            for ch in data:
                if ch == 1:                                 # Ctrl-A
                    ser.write(b"raw REPL; CTRL-B to exit\r\n>")
                    mode = "raw"
                    raw_code = b""
                    log("enter raw")
                    break
                elif ch == 13:
                    ser.write(b"\r\n>>> ")
                elif ch == 4:
                    ser.write(b"MPY: soft reboot\r\n")
                    log("soft reboot")
                    mode = "tester"
                    break
        elif mode == "raw":
            for ch in data:
                if ch == 4:
                    ser.write(b"OK")
                    out, err = raw_exec(raw_code)
                    ser.write(out.encode() + b"\x04" + err.encode() + b"\x04>")
                    log("exec", len(raw_code), "err" if err else "ok", err[:200])
                    raw_code = b""
                elif ch == 2:
                    mode = "repl"
                    ser.write(b"\r\nMicroPython\r\n>>> ")
                    log("exit raw")
                elif ch in (3,):
                    pass
                else:
                    raw_code += bytes([ch])


main()
