# Latency tester (standalone) - hardware layer for the RP2040.
# Everything that touches the chip lives here, so lt_app.py can also run
# against a simulated board on a PC (tests/sim_standalone.py).
#
# Time-critical loops are native code (viper) reading registers directly:
#   TIMERAWL 0x40054028  1 MHz timer
#   ADC RESULT 0x4004c004 (ADC free-running at 500k samples/s)
#   SIO GPIO_IN 0xd0000004
import array
import gc
import random
import time

import machine
import micropython
from machine import ADC, Pin, mem32

MASK30 = const(0x3FFFFFFF)


@micropython.viper
def now_us() -> int:
    return ptr32(0x40054028)[0] & 0x3FFFFFFF


@micropython.viper
def adc_now() -> int:
    return ptr32(0x4004c004)[0] & 0xFFF


@micropython.viper
def adc_avg(shift: int) -> int:
    res = ptr32(0x4004c004)
    tmr = ptr32(0x40054028)
    n = 1 << shift
    s = 0
    i = 0
    while i < n:
        s += res[0] & 0xFFF
        t = tmr[0]
        while tmr[0] - t < 2:
            pass
        i += 1
    return s >> shift


@micropython.viper
def wait_change(base: int, delta: int, dirn: int, t0: int, timeout: int, confirm: int) -> int:
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
        if t == last:
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
def wait_press(mask: int, chunk_us: int) -> int:
    """Spin until the button (active low) reads pressed. Returns the time of
    contact, or -1 after chunk_us so the USB stack can run in between."""
    gin = ptr32(0xd0000004)
    tmr = ptr32(0x40054028)
    start = tmr[0]
    while gin[0] & mask:
        if tmr[0] - start > chunk_us:
            return -1
    return tmr[0] & 0x3FFFFFFF


@micropython.viper
def capture(buf, start: int, n: int, period: int, out):
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


def _signed30(x):
    return ((x + 0x20000000) & MASK30) - 0x20000000


class HW:
    """The real board: Waveshare RP2040-LCD-0.96 (or any RP2040 Pico),
    ALS-PT19 on GP26, push button from GP15 to GND."""

    def __init__(self, button_pin=15, sensor_adc=26):
        import sys
        if "RP2040" not in sys.implementation._machine:
            raise RuntimeError("needs an RP2040 board")
        self.button_pin = button_pin
        self.btn = Pin(button_pin, Pin.IN, Pin.PULL_UP)
        self._mask = 1 << button_pin
        sensor = ADC(sensor_adc)
        sensor.read_u16()                      # selects the channel
        mem32[0x4004c000] |= 0b1001            # EN | START_MANY: free-running
        self._out = array.array("I", [0, 0])
        self._lvl = array.array("H", [0] * 256)
        self.mouse = None
        self.now_us = now_us
        self.adc_now = adc_now
        self.adc_avg = adc_avg

    # ---- USB mouse
    def start_usb(self):
        import usb.device
        from usb.device.mouse import MouseInterface
        self.mouse = MouseInterface()
        self.mouse.interval_ms = 1             # 1 ms polling (full-speed minimum)
        usb.device.get().init(self.mouse, builtin_driver=True)

    def usb_ready(self):
        return self.mouse is not None and self.mouse.is_open()

    def press(self, down, button=1):
        if self.mouse is None:
            return
        if button == 2:
            self.mouse.click_right(down)
        else:
            self.mouse.click_left(down)

    # ---- time / button
    def ms(self):
        return time.ticks_ms()

    def diff_ms(self, a, b):
        return time.ticks_diff(a, b)

    def sleep_ms(self, n):
        time.sleep_ms(n)

    def pressed(self):
        return self.btn.value() == 0

    def rand(self, a, b):
        return random.randint(a, b)

    # ---- sensor
    def level(self):
        """(avg, min, max) over 256 samples 2 us apart."""
        capture(self._lvl, 0, 256, 2, self._out)
        s = 0
        lo = 4095
        hi = 0
        for x in self._lvl:
            s += x
            if x < lo:
                lo = x
            if x > hi:
                hi = x
        return s // 256, lo, hi

    def trial(self, delta, dirn, timeout_us, confirm, button):
        """Click and time until the light changes. Returns (base, t0, t1, t_sent);
        t1 = -1 on timeout. The button stays down - call press(False) after."""
        base = adc_avg(6)
        gc.collect()
        gc.disable()
        t0 = now_us()
        self.press(True, button)
        ts = now_us()
        t1 = wait_change(base, delta, dirn, t0, timeout_us, confirm)
        gc.enable()
        return base, t0, t1, ts

    def manual_trial(self, delta, dirn, timeout_us, confirm, button, chunk_us):
        """Wait (up to chunk_us) for the button; on press, click the mouse and
        time from contact to light. None if not pressed in time."""
        base = adc_avg(4)
        gc.collect()
        gc.disable()
        t0 = wait_press(self._mask, chunk_us)
        if t0 < 0:
            gc.enable()
            return None
        self.press(True, button)
        ts = now_us()
        t1 = wait_change(base, delta, dirn, t0, timeout_us, confirm)
        gc.enable()
        return base, t0, t1, ts

    def capture(self, n, period, pre, click, button):
        """High-speed capture around a click. Returns (samples, pre_rel_us,
        post_rel_us); sample i<pre at pre_rel+i*period, else post_rel+(i-pre)*period."""
        gc.collect()
        raw = bytearray(n * 2)
        buf = array.array("H", raw)            # a bytearray initialiser is copied as raw memory
        del raw
        gc.collect()
        gc.disable()
        capture(buf, 0, pre, period, self._out)
        pre_start = self._out[0] & MASK30
        t0 = now_us()
        if click:
            self.press(True, button)
        capture(buf, pre, n - pre, period, self._out)
        post_start = self._out[0] & MASK30
        gc.enable()
        return buf, _signed30(pre_start - t0), _signed30(post_start - t0)

    # ---- files
    def read_file(self, path):
        try:
            with open(path) as f:
                return f.read()
        except OSError:
            return None

    def write_file(self, path, text, append=False):
        try:
            with open(path, "a" if append else "w") as f:
                f.write(text)
        except OSError:
            pass

    def collect(self):
        gc.collect()
