# Latency Tester firmware v2.0 - Raspberry Pi Pico / Pico W, MicroPython >= 1.23
#
# Hardware: light sensor on GP26 (Adafruit ALS-PT19: + -> 3V3, - -> AGND, OUT -> GP26)
#           and the board's own LED. Nothing else.
# The Pico appears to the PC as a USB mouse (to click) AND a USB serial port
# (to talk to the LatencyTester app). The app runs every test; the Pico does the
# time-critical part: click, then watch the sensor with microsecond timing.
#
# Onboard LED:  slow blink  = waiting for the app
#               off         = app connected, idle
#               on          = click being held down
#               fast blink  = maintenance mode (Thonny can connect)
#
# Maintenance mode: press the BOOTSEL button within 2 s of plugging in
# (while the LED flickers). The program stops before USB mouse mode starts,
# so Thonny can connect. The app's "Exit to REPL" button does the same.
#
# Serial protocol (text lines, PC -> Pico; replies start with a tag):
#   HELLO                        -> HELLO lattest <ver> <board> <cpu_mhz>
#   PING <id>                    -> P <pico_us> <id>
#   LEVEL                        -> LEVEL <avg> <min> <max>          (12-bit ADC)
#   STREAM <hz>                  -> L <v> <v> ... every 20 ms, until the next command
#   CFG key=val ...              -> OK      keys: dir delta timeout hold confirm button
#   TRIAL                        -> R <seq> <t0> <t1> <t_sent> <base>   (t1 = -1 timeout)
#   CAP <n> <period_us> <pre> <click>
#                                -> CAP <n> <period> <pre> <pre_rel_us> <post_rel_us>
#                                   D <hex> ... (12-bit samples, little-endian u16)
#                                   CAPEND
#   LED <0|1>                    -> OK
#   EXIT                         -> BYE (stops the program; REPL available)
# Times are the Pico's microsecond timer, masked to 30 bits (wraps every ~17.9 min).

import array
import binascii
import gc
import machine
import micropython
import rp2
import select
import sys
import time
from machine import ADC, Pin, mem32

VERSION = "2.0"
SENSOR_ADC = 26
OVERCLOCK = True
MASK30 = const(0x3FFFFFFF)

# The native code below reads RP2040 registers directly. The RP2350 (Pico 2) has
# them elsewhere, so refuse to run there rather than crash.
if "RP2040" not in sys.implementation._machine:
    print("Latency tester: needs a Pico or Pico W (RP2040). Found:", sys.implementation._machine)
    raise SystemExit

if OVERCLOCK:
    machine.freq(200_000_000)

try:
    led = Pin("LED", Pin.OUT, value=0)       # Pico W (via Wi-Fi chip) and Pico (GP25)
except Exception:
    led = Pin(25, Pin.OUT, value=0)


# ---------------- native hot paths (direct hardware registers) ----------------
# TIMERAWL 0x40054028 (1 MHz), ADC RESULT 0x4004c004

@micropython.viper
def now_us() -> int:
    return ptr32(0x40054028)[0] & 0x3FFFFFFF


@micropython.viper
def adc_now() -> int:
    return ptr32(0x4004c004)[0] & 0xFFF


@micropython.viper
def adc_avg(shift: int) -> int:
    """Average 2**shift fresh ADC samples."""
    res = ptr32(0x4004c004)
    tmr = ptr32(0x40054028)
    n = 1 << shift
    s = 0
    i = 0
    while i < n:
        s += res[0] & 0xFFF
        t = tmr[0]
        while tmr[0] - t < 2:            # a new conversion every 2 us
            pass
        i += 1
    return s >> shift


@micropython.viper
def wait_change(base: int, delta: int, dirn: int, t0: int, timeout: int, confirm: int) -> int:
    """Spin until the sensor moves `delta` away from `base` (dirn +1 up, -1 down,
    0 either way) for `confirm` consecutive samples. Returns the time of the first
    of those samples, or -1 on timeout."""
    res = ptr32(0x4004c004)
    tmr = ptr32(0x40054028)
    hi = base + delta
    lo = base - delta
    count = 0
    first = 0
    last = -1
    while True:
        t = tmr[0] & 0x3FFFFFFF
        if ((t - t0) & 0x3FFFFFFF) > timeout:
            return -1
        if t == last:                    # same microsecond, same sample - skip
            continue
        last = t
        v = res[0] & 0xFFF
        hit = 0
        if dirn >= 0 and v > hi:
            hit = 1
        if dirn <= 0 and v < lo:
            hit = 1
        if hit:
            if count == 0:
                first = t
            count += 1
            if count >= confirm:
                return first
        else:
            count = 0


@micropython.viper
def capture(buf, start: int, n: int, period: int, out):
    """Sample the ADC every `period` us into buf[start:start+n] (u16).
    out[0] = start time, out[1] = end time (raw 32-bit timer)."""
    p = ptr16(buf)
    o = ptr32(out)
    res = ptr32(0x4004c004)
    tmr = ptr32(0x40054028)
    nxt = tmr[0]
    o[0] = nxt
    i = 0
    while i < n:
        while tmr[0] - nxt < 0:
            pass
        p[start + i] = res[0] & 0xFFF
        nxt += period
        i += 1
    o[1] = tmr[0]


# ---------------- ADC free-running ----------------
sensor = ADC(SENSOR_ADC)
sensor.read_u16()                        # selects the channel
mem32[0x4004c000] |= 0b1001              # CS: EN | START_MANY -> 500k samples/s


# ---------------- maintenance mode check (before USB mouse mode) ----------------
def maintenance_check(ms=2000):
    end = time.ticks_add(time.ticks_ms(), ms)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        led.toggle()
        if rp2.bootsel_button():
            for _ in range(10):
                led.toggle()
                time.sleep_ms(60)
            led.on()
            print("Latency tester: maintenance mode - Thonny can connect.")
            return True
        time.sleep_ms(40)
    led.off()
    return False


if maintenance_check():
    raise SystemExit

# ---------------- USB mouse ----------------
import usb.device
from usb.device.mouse import MouseInterface

mouse = MouseInterface()
mouse.interval_ms = 1                    # ask for 1 ms polling (the full-speed minimum)
usb.device.get().init(mouse, builtin_driver=True)   # keeps the USB serial port
_t = time.ticks_ms()
while not mouse.is_open():
    if time.ticks_diff(time.ticks_ms(), _t) > 100:
        led.toggle()
        _t = time.ticks_ms()
    time.sleep_ms(5)
led.off()

micropython.kbd_intr(-1)                 # serial bytes can never interrupt the program

# ---------------- settings (changed by CFG) ----------------
cfg = {
    "dir": 1,          # +1 brighter, -1 darker, 0 either
    "delta": 200,      # ADC counts away from the baseline that count as "changed"
    "timeout": 500000, # us
    "hold": 60,        # ms the click is held down
    "confirm": 3,      # consecutive samples over the threshold
    "button": 1,       # 1 left, 2 right
}

seq = 0
last_host_ms = time.ticks_ms() - 100000
_out = array.array("I", [0, 0])
_lvl = array.array("H", [0] * 256)      # 256 samples for LEVEL


def press(down):
    if cfg["button"] == 2:
        mouse.click_right(down)
    else:
        mouse.click_left(down)


def send(s):
    sys.stdout.write(s)
    sys.stdout.write("\n")


# ---------------- commands ----------------
def cmd_level():
    capture(_lvl, 0, 256, 2, _out)
    s = 0
    lo = 4095
    hi = 0
    for x in _lvl:
        s += x
        if x < lo:
            lo = x
        if x > hi:
            hi = x
    send("LEVEL %d %d %d" % (s // 256, lo, hi))


def cmd_trial():
    global seq
    seq += 1
    base = adc_avg(6)
    gc.collect()
    gc.disable()
    t0 = now_us()
    press(True)
    ts = now_us()
    t1 = wait_change(base, cfg["delta"], cfg["dir"], t0, cfg["timeout"], cfg["confirm"])
    gc.enable()
    send("R %d %d %d %d %d" % (seq, t0, t1, ts, base))
    led.on()
    rem = cfg["hold"] - ((now_us() - t0) & MASK30) // 1000
    if rem > 0:
        time.sleep_ms(rem)
    press(False)
    led.off()


def cmd_cap(args):
    n = min(max(int(args[0]), 16), 30000)
    period = max(int(args[1]), 2)
    pre = min(max(int(args[2]), 0), n)
    do_click = int(args[3]) if len(args) > 3 else 1
    gc.collect()
    buf = bytearray(n * 2)
    gc.disable()
    capture(buf, 0, pre, period, _out)
    pre_start = _out[0] & MASK30
    t0 = now_us()
    if do_click:
        press(True)
    capture(buf, pre, n - pre, period, _out)
    post_start = _out[0] & MASK30
    gc.enable()
    if do_click:
        led.on()
        rem = cfg["hold"] - ((now_us() - t0) & MASK30) // 1000
        if rem > 0:
            time.sleep_ms(rem)
        press(False)
        led.off()
    pre_rel = ((pre_start - t0 + 0x20000000) & MASK30) - 0x20000000
    post_rel = ((post_start - t0 + 0x20000000) & MASK30) - 0x20000000
    send("CAP %d %d %d %d %d" % (n, period, pre, pre_rel, post_rel))
    mv = memoryview(buf)
    step = 1024                          # bytes per line (512 samples)
    for i in range(0, n * 2, step):
        sys.stdout.write("D ")
        sys.stdout.write(binascii.hexlify(mv[i:i + step]).decode())
        sys.stdout.write("\n")
    send("CAPEND")
    del buf
    gc.collect()


def cmd_cfg(args):
    for a in args:
        if "=" in a:
            k, v = a.split("=", 1)
            if k in cfg:
                cfg[k] = int(v)
    cfg["confirm"] = max(1, cfg["confirm"])
    send("OK")


poller = select.poll()
poller.register(sys.stdin, select.POLLIN)
_line = []


def read_line(timeout_ms):
    """Collect one line from the USB serial port. Returns str or None."""
    while poller.poll(timeout_ms):
        c = sys.stdin.read(1)
        if not c:
            return None
        if c == "\n" or c == "\r":
            if _line:
                s = "".join(_line)
                _line.clear()
                return s
        else:
            if len(_line) < 200:
                _line.append(c)
        timeout_ms = 0
    return None


def cmd_stream(args):
    hz = min(max(int(args[0]) if args else 500, 10), 2000)
    period_ms = 1000 / hz
    per_line = max(1, int(20 / period_ms))
    shift = 3
    nxt = time.ticks_us()
    vals = []
    while True:
        if poller.poll(0):
            return                       # next command arrived: stop streaming
        while time.ticks_diff(time.ticks_us(), nxt) < 0:
            pass
        nxt = time.ticks_add(nxt, int(period_ms * 1000))
        vals.append(adc_avg(shift))
        if len(vals) >= per_line:
            send("L " + " ".join(str(v) for v in vals))
            vals = []


def handle(line):
    global last_host_ms
    parts = line.strip().split()
    if not parts:
        return
    last_host_ms = time.ticks_ms()
    c = parts[0].upper()
    a = parts[1:]
    try:
        if c == "PING":
            send("P %d %s" % (now_us(), a[0] if a else "0"))
        elif c == "TRIAL":
            cmd_trial()
        elif c == "HELLO":
            board = sys.implementation._machine.replace(" ", "_")
            send("HELLO lattest %s %s %d" % (VERSION, board, machine.freq() // 1000000))
        elif c == "LEVEL":
            cmd_level()
        elif c == "CFG":
            cmd_cfg(a)
        elif c == "CAP":
            cmd_cap(a)
        elif c == "STREAM":
            cmd_stream(a)
        elif c == "STOP":
            send("OK")
        elif c == "LED":
            led.value(1 if a and a[0] == "1" else 0)
            send("OK")
        elif c == "EXIT":
            led.off()
            send("BYE")
            micropython.kbd_intr(3)
            raise SystemExit
        else:
            send("ERR unknown " + c)
    except SystemExit:
        raise
    except Exception as e:
        gc.enable()
        press(False)
        send("ERR " + repr(e))


# ---------------- main loop ----------------
_beat = time.ticks_ms()
while True:
    line = read_line(5)
    if line is not None:
        handle(line)
        while poller.poll(0):            # handle anything else that queued up
            line = read_line(0)
            if line is None:
                break
            handle(line)
    # heartbeat while no app is talking to us
    if time.ticks_diff(time.ticks_ms(), last_host_ms) > 3000:
        if time.ticks_diff(time.ticks_ms(), _beat) > 1500:
            led.on()
            time.sleep_ms(40)
            led.off()
            _beat = time.ticks_ms()
