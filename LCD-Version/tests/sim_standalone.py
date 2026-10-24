"""Runs the LCD-version latency tester (lt_app.py) on a simulated board.

- virtual time (no real waiting), scripted button presses
- a model PC screen: goes white a random 12-24 ms after each mouse-down,
  black again after mouse-up, with a 2 ms pixel transition, or a PWM backlight
- a Python framebuf with MicroPython's own 8x8 font, so screenshots match the device

Usage: python sim_standalone.py FONT_H OUT_DIR
  FONT_H = path to micropython/extmod/font_petme128_8x8.h
"""
import json
import os
import random
import re
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

# ---------------------------------------------------------------- framebuf
FONT = None


def load_font(path):
    global FONT
    src = open(path).read()
    body = src[src.index("{") + 1: src.rindex("}")]
    body = re.sub(r"//[^\n]*", "", body)
    FONT = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]


class FrameBuffer:
    def __init__(self, buf, w, h, fmt):
        self.w, self.h, self.fmt = w, h, fmt
        self.p = [[0] * w for _ in range(h)]
        self.texts = []

    def fill(self, c):
        for row in self.p:
            for x in range(self.w):
                row[x] = c
        self.texts = []

    def pixel(self, x, y, c=None):
        if 0 <= x < self.w and 0 <= y < self.h:
            if c is None:
                return self.p[y][x]
            self.p[y][x] = c
        return None if c is not None else 0

    def fill_rect(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.h, y + h)):
            row = self.p[yy]
            for xx in range(max(0, x), min(self.w, x + w)):
                row[xx] = c

    def rect(self, x, y, w, h, c):
        self.hline(x, y, w, c)
        self.hline(x, y + h - 1, w, c)
        self.vline(x, y, h, c)
        self.vline(x + w - 1, y, h, c)

    def hline(self, x, y, w, c):
        self.fill_rect(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.fill_rect(x, y, 1, h, c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.pixel(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def text(self, s, x, y, c=1):
        self.texts.append(s)
        for ch in s:
            o = ord(ch)
            if o < 32 or o > 127:
                o = 127
            for j in range(8):
                col = FONT[(o - 32) * 8 + j]
                for yy in range(8):
                    if col & 1:
                        self.pixel(x + j, y + yy, c)
                    col >>= 1
            x += 8


fbmod = types.ModuleType("framebuf")
fbmod.FrameBuffer = FrameBuffer
fbmod.RGB565 = 1
fbmod.MONO_HLSB = 3
sys.modules["framebuf"] = fbmod

import lt_app  # noqa: E402


def rgb(r, g, b):
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((c & 0xFF) << 8) | (c >> 8)


def unrgb(c):
    c = ((c & 0xFF) << 8) | (c >> 8)
    return ((c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3)


class StopSim(Exception):
    pass


# ---------------------------------------------------------------- fake board
class FakeHW:
    BLACK, WHITE = 150, 3300

    def __init__(self, seed=1):
        self.rng = random.Random(seed)
        self.t = 0                      # virtual microseconds
        self.presses = []               # (start_us, end_us) of the physical button
        self.events = [(0, False)]      # (effective_us, white) screen changes
        self._ev_t = [0]
        self.files = {}
        self.scene = "target"           # or "pwm" or "dead"
        self.lat = []                   # true latencies generated (ms)
        self.mouse_down = False
        self.on_tick = None

    # time
    def _adv(self, us):
        self.t += us
        if self.on_tick:
            self.on_tick()
        if self.t - getattr(self, "progress_t", 0) > 300_000_000:
            raise StopSim("stalled")

    def now_us(self):
        return self.t & 0x3FFFFFFF

    def ms(self):
        return self.t // 1000

    def diff_ms(self, a, b):
        return a - b

    def sleep_ms(self, n):
        self._adv(n * 1000)

    def rand(self, a, b):
        return self.rng.randint(a, b)

    def collect(self):
        pass

    # button
    def pressed(self):
        return any(a <= self.t < b for a, b in self.presses)

    def push(self, dur_ms, after_ms=30):
        a = self.t + after_ms * 1000
        self.presses.append((a, a + dur_ms * 1000))

    # screen model
    def level_at(self, t):
        if self.scene == "dead":
            return 600
        if self.scene == "dark":
            return 40
        if self.scene == "pwm":
            return 2400 + (600 if (t // 2083) % 2 else -600)
        import bisect
        i = bisect.bisect_right(self._ev_t, t) - 1
        if i < 0:
            return self.BLACK
        start, white = self.events[i]
        cur = self.WHITE if white else self.BLACK
        prev = (self.WHITE if self.events[i - 1][1] else self.BLACK) if i > 0 else self.BLACK
        f = min(1.0, (t - start) / 2000.0)
        return int(prev + (cur - prev) * f)

    def adc_now(self):
        return self.level_at(self.t) + self.rng.randint(-4, 4)

    def adc_avg(self, shift):
        n = 1 << shift
        s = 0
        for _ in range(n):
            s += self.adc_now()
            self.t += 2
        return s // n

    def level(self):
        v = []
        for _ in range(256):
            v.append(self.adc_now())
            self.t += 2
        return sum(v) // 256, min(v), max(v)

    # USB mouse
    def usb_ready(self):
        return True

    def press(self, down, button=1):
        if down == self.mouse_down:
            return
        self.mouse_down = down
        lat = self.rng.uniform(12, 24) * 1000
        if down:
            self.lat.append(lat / 1000)
        self.events.append((self.t + int(lat), down))
        self.events.sort()
        self._ev_t = [e[0] for e in self.events]

    def _wait_change(self, base, delta, dirn, t0, timeout, confirm):
        while True:
            if self.t - t0 > timeout:
                return -1
            v = self.adc_now()
            if (dirn >= 0 and v > base + delta) or (dirn <= 0 and v < base - delta):
                first = self.t
                self.t += 2 * confirm
                return first & 0x3FFFFFFF
            self.t += 10

    def trial(self, delta, dirn, timeout_us, confirm, button):
        base = self.adc_avg(6)
        t0 = self.t
        self.press(True, button)
        self.t += 40
        ts = self.t
        t1 = self._wait_change(base, delta, dirn, t0, timeout_us, confirm)
        return base, t0 & 0x3FFFFFFF, t1, ts & 0x3FFFFFFF

    def capture(self, n, period, pre, click, button):
        import array
        buf = array.array("H", [0] * n)
        t_pre = self.t
        for i in range(pre):
            buf[i] = self.adc_now()
            self.t += period
        t0 = self.t
        if click:
            self.press(True, button)
        t_post = self.t
        for i in range(pre, n):
            buf[i] = max(0, self.adc_now())
            self.t += period
        return buf, t_pre - t0, t_post - t0

    # files
    def read_file(self, path):
        return self.files.get(path)

    def write_file(self, path, text, append=False):
        self.files[path] = (self.files.get(path, "") if append else "") + text


class FakeTFT:
    W, H = 160, 80

    def __init__(self, hw, script, outdir):
        self.fb = FrameBuffer(None, 160, 80, 1)
        self.hw = hw
        self.script = script
        self.outdir = outdir
        self.frames = 0
        self.flip = False
        self.shots = []
        self.log = []

    def set_flip(self, flip, bgr=True):
        self.flip = flip

    def screen_text(self):
        return " | ".join(self.fb.texts)

    def save(self, name):
        from PIL import Image
        img = Image.new("RGB", (160, 80))
        px = img.load()
        for y in range(80):
            for x in range(160):
                px[x, y] = unrgb(self.fb.p[y][x])
        if self.flip:
            img = img.rotate(180)
        img = img.resize((480, 240), Image.NEAREST)
        path = os.path.join(self.outdir, name + ".png")
        img.save(path)
        self.shots.append(path)

    def flush(self):
        self.frames += 1
        txt = self.screen_text()
        if not self.log or self.log[-1][1] != txt:
            self.log.append((self.hw.t // 1000, txt))
        self.script.on_frame(self, txt)


class Script:
    """Steps: (substring to wait for on screen, action(s), screenshot name or None).
    action: 'short', 'long', ('hold', ms), ('scene', name), 'stop', None, or a list.
    The screen is re-checked on every frame and whenever virtual time moves."""

    def __init__(self, hw, steps):
        self.hw = hw
        self.steps = list(steps)
        self.i = 0
        self.busy_until = 0
        self.tft = None
        self.txt = ""

    def on_frame(self, tft, txt):
        self.tft = tft
        self.txt = txt
        self.check()

    def check(self):
        if self.tft is None:
            return
        if self.i >= len(self.steps):
            raise StopSim()
        hw = self.hw
        if hw.pressed() or hw.t < self.busy_until:
            return
        want, action, shot = self.steps[self.i]
        if want not in self.txt:
            return
        if shot:
            self.tft.save(shot)
        self.i += 1
        hw.progress_t = hw.t
        acts = action if isinstance(action, list) else [action]
        for act in acts:
            if act == "short":
                hw.push(120)
                self.busy_until = hw.t + 300_000
            elif act == "long":
                hw.push(1000)
                self.busy_until = hw.t + 1200_000
            elif isinstance(act, tuple) and act[0] == "hold":
                hw.push(act[1])
                self.busy_until = hw.t + (act[1] + 200) * 1000
            elif isinstance(act, tuple) and act[0] == "scene":
                hw.scene = act[1]
            elif act == "stop":
                raise StopSim()


def main():
    load_font(sys.argv[1])
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    hw = FakeHW()
    def menu_to(cur, target, total=15):
        out = []
        k = cur
        while k != target:
            out.append(("%d/%d" % (k, total), "short", None))
            k = k % total + 1
        return out

    steps = [
        ("Press: start", "short", "01_home"),
        ("Switch to the target", None, "02_countdown"),
        ("RUN 40/100", None, "03_running"),
        ("MEDIAN", "short", "04_results_median"),
        ("DISTRIBUTION", "short", "05_results_histogram"),
        ("EVERY CLICK", "short", "06_results_every_click"),
        ("DETAIL", "long", "07_results_detail"),
        ("Press: start", "long", None),
        ("1/15", None, "08_menu"),
    ]
    steps += menu_to(1, 10)
    steps += [("Trigger: Small", None, "08b_menu_trigger_default")]
    steps += menu_to(10, 7)
    steps += [("7/15", "long", "09_menu_clicks"),
              ("Clicks: 200", None, "10_menu_clicks_changed")]
    steps += menu_to(7, 2)
    steps += [("2/15", "long", None),
              ("Switch to the target", None, "13_scope_countdown"),
              ("50%", "long", "14_response_scope")]
    # flicker: PWM screen, then a plain screen (no flicker), then a dark one
    steps += [("2/15", [("scene", "pwm"), "short"], None), ("3/15", "long", None),
              ("Switch to the target", None, None),
              ("Hz", [("scene", "target"), "short"], "15_flicker"),
              ("NO FLICKER", [("scene", "dark"), "short"], "15b_flicker_none"),
              ("TOO DARK", "long", "15c_flicker_dark"),
              ("3/15", ("scene", "target"), None)]
    steps += menu_to(3, 4)
    steps += [("4/15", "long", None), ("SENSOR", "short", None),
              ("CLICK", None, "16_sensor_view"),
              ("SENSOR", "long", None)]
    steps += menu_to(4, 6)
    steps += [("6/15", "long", None),
              ("HISTORY  1 runs", "long", "17_history"),
              ("RUN #1", "short", "17b_history_run"),
              ("HISTORY  1 runs", "short", None),
              ("Clear history", "long", "17c_history_clear_selected"),
              ("CLEAR HISTORY?", "long", "17d_history_confirm"),
              ("HISTORY  0 runs", "short", "17e_history_cleared"),
              ("HISTORY  0 runs", "long", None)]
    steps += menu_to(6, 14)
    steps += [("14/15", "long", None), ("14/15", "long", "18_flipped_menu")]
    steps += menu_to(14, 15)
    steps += [("15/15", "long", None),
              ("Press: start 200", [("scene", "dead"), "short"], "19_home_after"),
              ("NO CHANGE SEEN", "short", "20_no_change"),
              ("NO RESULTS", "short", None),
              ("Press: start", "stop", None)]
    script = Script(hw, steps)
    tft = FakeTFT(hw, script, outdir)
    hw.on_tick = script.check
    try:
        lt_app.run(hw, tft, rgb)
    except StopSim as e:
        if str(e):
            print("STOPPED:", e, "waiting for step", script.i, steps[script.i] if script.i < len(steps) else "")
    print("script steps done: %d/%d" % (script.i, len(steps)))
    print("virtual time: %.1f s, frames: %d" % (hw.t / 1e6, tft.frames))
    for t, txt in tft.log[-6:]:
        print("  %7d ms  %s" % (t, txt))
    hist = hw.files.get(lt_app.HISTORY_FILE, "")
    print("history.csv:\n" + hist)
    print("settings:", hw.files.get(lt_app.SETTINGS_FILE))
    true = sorted(hw.lat)
    print("model latency (all clicks) median %.2f ms" % true[len(true) // 2])
    json.dump([t for t in tft.log], open(os.path.join(outdir, "screen_log.json"), "w"), indent=0)
    return 0 if script.i == len(steps) else 1


if __name__ == "__main__":
    sys.exit(main())
