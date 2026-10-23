# Latency tester - standalone version (no PC app).
# One button + the 160x80 screen on a Waveshare RP2040-LCD-0.96.
#
# Button:  short press = start / next / stop      hold (0.7 s) = menu / select / back
#
# All hardware access goes through `hw` (lt_hw.HW on the board), so this file
# also runs against a simulated board for testing.
import json

VERSION = "3.0"
SETTINGS_FILE = "settings.json"
HISTORY_FILE = "history.csv"
LAST_FILE = "last_run.csv"

W = 160
H = 80
LONG_MS = 700

CLICK_OPTS = (20, 50, 100, 200, 500)
COUNTDOWN_OPTS = (0, 3, 5, 10)
DETECT_OPTS = (("Either", 0), ("Brighter", 1), ("Darker", -1))
TRIGGER_OPTS = (("Auto", 0), ("Small", 40), ("Medium", 150), ("Large", 500))
HOLD_OPTS = (40, 60, 100, 200)
GAP_OPTS = (("Fast", 150, 300), ("Normal", 250, 600), ("Slow", 500, 1000))
BUTTON_OPTS = ("Left", "Right")
TIMEOUT_US = 500000
CONFIRM = 3
WARMUP = 2

DEFAULTS = {"clicks": 2, "countdown": 2, "detect": 0, "trigger": 0, "hold": 1, "gap": 1,
            "button": 0, "flip": 0, "last_median": -1.0, "runs": 0}


# ---------------------------------------------------------------- stats
def pct(sorted_v, p):
    n = len(sorted_v)
    if n == 0:
        return 0.0
    if n == 1:
        return sorted_v[0]
    pos = p / 100 * (n - 1)
    i = int(pos)
    if i >= n - 1:
        return sorted_v[-1]
    return sorted_v[i] + (sorted_v[i + 1] - sorted_v[i]) * (pos - i)


def stats(v):
    s = sorted(v)
    n = len(s)
    if not n:
        return None
    mean = sum(s) / n
    sd = (sum((x - mean) ** 2 for x in s) / (n - 1)) ** 0.5 if n > 1 else 0.0
    return {"n": n, "mean": mean, "sd": sd, "min": s[0], "max": s[-1], "med": pct(s, 50),
            "p95": pct(s, 95), "p99": pct(s, 99)}


def delta30(a, b):
    d = (a - b) & 0x3FFFFFFF
    if d >= 0x20000000:
        d -= 0x40000000
    return d


def analyze_response(samples, period, pre, pre_rel, post_rel):
    """10/50/90% crossing times (ms after the click) and rise time."""
    n = len(samples)

    def t_ms(i):
        if i < pre:
            return (pre_rel + i * period) / 1000
        return (post_rel + (i - pre) * period) / 1000

    base = sum(samples[:pre]) / pre if pre > 4 else sum(samples[:20]) / 20
    tail = samples[n - n // 7:]
    final = sum(tail) / len(tail)
    step = final - base
    if abs(step) < 30:
        return None
    w = 5
    r = {"rising": step > 0, "base": base, "final": final}
    want = [0.1, 0.5, 0.9]
    got = []
    acc = sum(samples[pre:pre + w])
    for i in range(pre, n - w):
        f = (acc / w - base) / step
        if f >= want[len(got)]:
            got.append(t_ms(i + w // 2))
            if len(got) == 3:
                break
        acc += samples[i + w] - samples[i]
    if len(got) < 3:
        return None
    r["t10"], r["t50"], r["t90"] = got
    r["rise"] = got[2] - got[0]
    return r


def analyze_flicker(samples, period):
    s = sorted(samples)
    lo = pct(s, 1)
    hi = pct(s, 99)
    mean = sum(samples) / len(samples)
    if hi + lo <= 0:
        return {"ok": False, "msg": "Sensor reads 0"}
    depth = (hi - lo) / (hi + lo) * 100
    pp = hi - lo
    if pp < 24 or depth < 1.5:
        return {"ok": True, "flicker": False, "depth": depth, "hz": 0}
    up = mean + pp * 0.15
    dn = mean - pp * 0.15
    above = samples[0] > mean
    count = 0
    first = last = 0
    for i in range(1, len(samples)):
        v = samples[i]
        if not above and v > up:
            above = True
            if count == 0:
                first = i
            last = i
            count += 1
        elif above and v < dn:
            above = False
    if count >= 3 and last > first:
        hz = (count - 1) / ((last - first) * period / 1e6)
        return {"ok": True, "flicker": True, "depth": depth, "hz": hz}
    return {"ok": True, "flicker": False, "depth": depth, "hz": 0}


# ---------------------------------------------------------------- button
class Button:
    """Turns the raw button into 'short' and 'long' events. A long event fires
    while still held (after LONG_MS); its release is swallowed."""

    def __init__(self, hw):
        self.hw = hw
        self.down_at = None
        self.long_sent = False
        self.armed = not hw.pressed()        # ignore a press held from before

    def poll(self):
        hw = self.hw
        p = hw.pressed()
        now = hw.ms()
        if not self.armed:
            if not p:
                self.armed = True
            return None
        if p:
            if self.down_at is None:
                self.down_at = now
            elif not self.long_sent and hw.diff_ms(now, self.down_at) >= LONG_MS:
                self.long_sent = True
                return "long"
            return None
        if self.down_at is not None:
            was_long = self.long_sent
            self.down_at = None
            self.long_sent = False
            hw.sleep_ms(20)                  # debounce release
            if not was_long:
                return "short"
        return None

    def held(self):
        return self.down_at is not None

    def reset(self):
        self.down_at = None
        self.long_sent = False
        self.armed = not self.hw.pressed()


# ---------------------------------------------------------------- app
class App:
    def __init__(self, hw, tft, rgb):
        self.hw = hw
        self.tft = tft
        self.fb = tft.fb
        self.btn = Button(hw)
        self.WHITE = rgb(255, 255, 255)
        self.GREY = rgb(120, 120, 130)
        self.DARK = rgb(40, 42, 50)
        self.CYAN = rgb(0, 200, 255)
        self.YELLOW = rgb(255, 210, 0)
        self.GREEN = rgb(0, 230, 90)
        self.RED = rgb(255, 70, 70)
        self.ORANGE = rgb(255, 140, 0)
        self.BLUE = rgb(40, 110, 230)
        self.BLACK = 0
        import framebuf
        self._sbuf = bytearray(8 * 8)
        self._sfb = framebuf.FrameBuffer(self._sbuf, 64, 8, framebuf.MONO_HLSB)
        self.cfg = dict(DEFAULTS)
        self.load_settings()
        self.last = []          # latencies of the last run
        self.last_fails = 0
        self.last_info = ""

    # ---------------- settings / files
    def load_settings(self):
        txt = self.hw.read_file(SETTINGS_FILE)
        if txt:
            try:
                d = json.loads(txt)
                for k in DEFAULTS:
                    if k in d:
                        self.cfg[k] = d[k]
            except ValueError:
                pass

    def save_settings(self):
        self.hw.write_file(SETTINGS_FILE, json.dumps(self.cfg))

    def clicks(self):
        return CLICK_OPTS[self.cfg["clicks"] % len(CLICK_OPTS)]

    # ---------------- drawing helpers
    def clear(self):
        self.fb.fill(0)

    def text(self, s, x, y, c=None):
        self.fb.text(s[:20], x, y, self.WHITE if c is None else c)

    def ctext(self, s, y, c=None):
        s = s[:20]
        self.fb.text(s, (W - len(s) * 8) // 2, y, self.WHITE if c is None else c)

    def big(self, s, y, c, scale=0, x=None):
        s = s[:8]
        if not scale:
            scale = 3 if len(s) * 24 <= W else 2
        sfb = self._sfb
        sfb.fill(0)
        sfb.text(s, 0, 0, 1)
        if x is None:
            x = (W - len(s) * 8 * scale) // 2
        fr = self.fb.fill_rect
        for yy in range(8):
            for xx in range(len(s) * 8):
                if sfb.pixel(xx, yy):
                    fr(x + xx * scale, y + yy * scale, scale, scale, c)

    def header(self, s, c=None):
        self.fb.fill_rect(0, 0, W, 11, self.DARK)
        self.text(s, 2, 2, self.CYAN if c is None else c)

    def footer(self, s, c=None):
        self.ctext(s, H - 9, self.GREY if c is None else c)

    def bar(self, frac, y=H - 3, c=None):
        frac = max(0.0, min(1.0, frac))
        self.fb.fill_rect(0, y, W, 3, self.DARK)
        self.fb.fill_rect(0, y, int(W * frac), 3, self.CYAN if c is None else c)

    def show(self):
        self.tft.flush()

    def message(self, title, l1="", l2="", c=None, wait=True):
        self.clear()
        self.header(title, c)
        self.ctext(l1, 26)
        self.ctext(l2, 42, self.GREY)
        self.footer("press = ok")
        self.show()
        if wait:
            self.wait_event()

    # ---------------- events
    def wait_event(self, timeout_ms=None, idle=None):
        """Block until 'short' or 'long'. idle() is called ~every 50 ms."""
        hw = self.hw
        t0 = hw.ms()
        last_idle = t0
        while True:
            e = self.btn.poll()
            if e:
                return e
            now = hw.ms()
            if timeout_ms is not None and hw.diff_ms(now, t0) >= timeout_ms:
                return None
            if idle and hw.diff_ms(now, last_idle) >= 50:
                last_idle = now
                idle()
            hw.sleep_ms(10)

    def sleep_check(self, ms):
        """Sleep, but return True early if the button is pressed (to stop)."""
        hw = self.hw
        end = hw.ms() + ms
        while hw.diff_ms(end, hw.ms()) > 0:
            if hw.pressed():
                return True
            hw.sleep_ms(5)
        return False

    # ---------------- screens
    def home(self):
        while True:
            self.clear()
            self.header("LATENCY TESTER")
            self.text("v" + VERSION, W - 34, 2, self.GREY)
            self.ctext("Press: start %d" % self.clicks(), 18)
            self.ctext("Hold: menu", 30, self.GREY)
            if self.cfg["last_median"] >= 0:
                self.ctext("Last %.2f ms" % self.cfg["last_median"], 48, self.YELLOW)
            usb = self.hw.usb_ready()
            self.fb.fill_rect(W - 6, H - 6, 4, 4, self.GREEN if usb else self.RED)
            self.text("USB" if usb else "no USB", 2, H - 9, self.GREY)
            self.show()
            e = self.wait_event(timeout_ms=1000)
            if e == "short":
                self.run_auto()
            elif e == "long":
                self.menu()

    def menu_items(self):
        c = self.cfg
        return [
            ("Start run", self.run_auto),
            ("Manual test", self.run_manual),
            ("Response scope", self.scope),
            ("Flicker test", self.flicker),
            ("Sensor view", self.sensor_view),
            ("Last results", self.results),
            ("History", self.history),
            ("Clicks: %d" % self.clicks(), ("clicks", len(CLICK_OPTS))),
            ("Countdown: %ds" % COUNTDOWN_OPTS[c["countdown"]], ("countdown", len(COUNTDOWN_OPTS))),
            ("Detect: " + DETECT_OPTS[c["detect"]][0], ("detect", len(DETECT_OPTS))),
            ("Trigger: " + TRIGGER_OPTS[c["trigger"]][0], ("trigger", len(TRIGGER_OPTS))),
            ("Hold: %dms" % HOLD_OPTS[c["hold"]], ("hold", len(HOLD_OPTS))),
            ("Gap: " + GAP_OPTS[c["gap"]][0], ("gap", len(GAP_OPTS))),
            ("Mouse: " + BUTTON_OPTS[c["button"]], ("button", len(BUTTON_OPTS))),
            ("Flip screen", self.flip_screen),
            ("Back", None),
        ]

    def menu(self):
        sel = 0
        while True:
            items = self.menu_items()
            sel %= len(items)
            self.clear()
            self.header("MENU")
            self.text("%d/%d" % (sel + 1, len(items)), W - 40, 2, self.GREY)
            top = min(max(0, sel - 2), len(items) - 5)
            for row in range(5):
                i = top + row
                if i >= len(items):
                    break
                y = 13 + row * 11
                if i == sel:
                    self.fb.fill_rect(0, y - 1, W, 10, self.BLUE)
                self.text(items[i][0], 4, y, self.WHITE if i == sel else self.GREY)
            self.footer("press=next hold=ok")
            self.show()
            e = self.wait_event(timeout_ms=30000)
            if e is None:
                return
            if e == "short":
                sel += 1
                continue
            label, act = items[sel]
            if act is None:
                return
            if isinstance(act, tuple):
                key, n = act
                self.cfg[key] = (self.cfg[key] + 1) % n
                self.save_settings()
                continue
            act()
            self.btn.reset()
            if label == "Start run":
                return

    def flip_screen(self):
        self.cfg["flip"] = 0 if self.cfg["flip"] else 1
        self.tft.set_flip(bool(self.cfg["flip"]))
        self.save_settings()

    # ---------------- runs
    def countdown(self, what):
        secs = COUNTDOWN_OPTS[self.cfg["countdown"]]
        for s in range(secs, 0, -1):
            self.clear()
            self.header(what)
            self.ctext("Switch to the target", 14, self.GREY)
            self.big(str(s), 28, self.YELLOW)
            self.footer("press = cancel")
            self.show()
            if self.sleep_check(1000):
                self.wait_release()
                return False
        return True

    def wait_release(self):
        while self.hw.pressed():
            self.hw.sleep_ms(10)
        self.hw.sleep_ms(30)
        self.btn.reset()

    def detection(self, hold_ms):
        """Work out the trigger size and direction. Auto: one calibration click
        held for 300 ms measures how much the screen changes; the trigger is
        half of that. Falls back to the sensor noise if the change is small."""
        hw = self.hw
        avg, lo, hi = hw.level()
        noise = hi - lo
        dirn = DETECT_OPTS[self.cfg["detect"]][1]
        fixed = TRIGGER_OPTS[self.cfg["trigger"]][1]
        if fixed:
            return fixed, dirn, "trigger %d" % fixed
        btn = self.cfg["button"] + 1
        base = hw.adc_avg(6)
        hw.press(True, btn)
        top = base
        bot = base
        end = hw.ms() + 300
        while hw.diff_ms(end, hw.ms()) > 0:
            v = hw.adc_avg(3)
            if v > top:
                top = v
            if v < bot:
                bot = v
        hw.press(False, btn)
        hw.sleep_ms(300)
        up = top - base
        down = base - bot
        need = max(60, noise * 4)
        if dirn >= 0 and up >= need and (dirn == 1 or up >= down):
            return max(20, up // 2), 1, "brighter, 50%%=%d" % (up // 2)
        if dirn <= 0 and down >= need:
            return max(20, down // 2), -1, "darker, 50%%=%d" % (down // 2)
        return max(25, noise * 3 + 15), dirn, "noise %d" % noise

    def run_auto(self):
        hw = self.hw
        self.wait_release()
        if not hw.usb_ready():
            self.message("NO USB", "Plug into the PC", "(mouse not ready)", self.RED)
            return
        if not self.countdown("STARTING"):
            return
        self.clear()
        self.header("CALIBRATING")
        self.ctext("Measuring the screen", 30, self.GREY)
        self.show()
        hold = HOLD_OPTS[self.cfg["hold"]]
        btn = self.cfg["button"] + 1
        delta, dirn, info = self.detection(hold)
        n = self.clicks()
        gap = GAP_OPTS[self.cfg["gap"]]
        res = []
        fails = 0
        in_row = 0
        stopped = False
        for i in range(WARMUP + n):
            if self.sleep_check(hw.rand(gap[1], gap[2])):
                stopped = True
                break
            base, t0, t1, ts = hw.trial(delta, dirn, TIMEOUT_US, CONFIRM, btn)
            rem = hold - delta30(hw.now_us(), t0) // 1000
            if rem > 0:
                hw.sleep_ms(rem)
            hw.press(False, btn)
            if i < WARMUP:
                continue
            if t1 < 0:
                fails += 1
                in_row += 1
            else:
                in_row = 0
                res.append(delta30(t1, t0) / 1000)
            self.draw_running(res, fails, i - WARMUP + 1, n, t1 < 0)
            if in_row >= 8:
                self.message("NO CHANGE SEEN", "8 clicks in a row", "Check sensor/target", self.RED)
                stopped = True
                break
        if stopped:
            self.wait_release()
        self.finish(res, fails, info, stopped)

    def draw_running(self, res, fails, done, n, failed):
        self.clear()
        self.header("RUN %d/%d" % (done, n))
        if fails:
            self.text("x%d" % fails, W - 32, 2, self.RED)
        if failed:
            self.big("--", 16, self.RED)
        elif res:
            self.big("%.1f" % res[-1], 16, self.WHITE)
        if res:
            s = sorted(res)
            self.ctext("med %.2f ms" % pct(s, 50), 47, self.YELLOW)
        self.ctext("press = stop", 62, self.GREY)
        self.bar(done / n)
        self.show()

    def finish(self, res, fails, info, stopped):
        self.last = res
        self.last_fails = fails
        self.last_info = info
        st = stats(res)
        if st:
            self.cfg["last_median"] = round(st["med"], 2)
            self.cfg["runs"] += 1
            self.save_settings()
            run = self.cfg["runs"]
            self.hw.write_file(LAST_FILE, "click,latency_ms\n" + "".join(
                "%d,%.3f\n" % (i + 1, v) for i, v in enumerate(res)))
            if self.hw.read_file(HISTORY_FILE) is None:
                self.hw.write_file(HISTORY_FILE, "run,clicks,failed,median,mean,sd,min,max,p95,detect\n")
            self.hw.write_file(HISTORY_FILE, "%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s\n" % (
                run, st["n"], fails, st["med"], st["mean"], st["sd"], st["min"], st["max"], st["p95"],
                info.replace(",", ";")), append=True)
        self.results()

    def results(self):
        res = self.last
        st = stats(res)
        if not st:
            self.message("NO RESULTS", "Run a test first" if not self.last_fails else "No clicks seen")
            return
        page = 0
        while True:
            self.clear()
            if page == 0:
                self.header("MEDIAN  (ms)", self.GREEN)
                self.big("%.2f" % st["med"], 14, self.GREEN)
                self.text("avg %.2f sd %.2f" % (st["mean"], st["sd"]), 2, 42)
                self.text("min %.1f max %.1f" % (st["min"], st["max"]), 2, 52, self.GREY)
                self.text("n%d fail%d" % (st["n"], self.last_fails), 2, 70, self.GREY)
            elif page == 1:
                self.header("DISTRIBUTION")
                self.histogram(res, st)
            elif page == 2:
                self.header("EVERY CLICK")
                self.scatter(res, st)
            else:
                self.header("DETAIL")
                self.text("95th  %.2f ms" % st["p95"], 2, 15)
                self.text("99th  %.2f ms" % st["p99"], 2, 26)
                self.text("min   %.2f ms" % st["min"], 2, 37)
                self.text("max   %.2f ms" % st["max"], 2, 48)
                self.text(self.last_info[:20], 2, 60, self.GREY)
            self.text("%d/4" % (page + 1), W - 26, 70 if page == 0 else 2, self.GREY)
            self.show()
            e = self.wait_event(timeout_ms=60000)
            if e != "short":
                return
            page = (page + 1) % 4

    def histogram(self, res, st):
        lo = st["min"]
        hi = st["max"]
        if hi - lo < 0.5:
            hi = lo + 0.5
        nb = 32
        bins = [0] * nb
        for v in res:
            b = int((v - lo) / (hi - lo) * nb)
            bins[min(nb - 1, b)] += 1
        top = max(bins)
        x0, y0, h = 0, 64, 50
        bw = W // nb
        for i, c in enumerate(bins):
            bh = int(c / top * h)
            if bh:
                self.fb.fill_rect(x0 + i * bw, y0 - bh, bw - 1, bh, self.CYAN)
        mx = int((st["med"] - lo) / (hi - lo) * (W - 1))
        self.fb.vline(mx, 13, 52, self.YELLOW)
        self.text("%.1f" % lo, 0, 70, self.GREY)
        s = "%.1f" % hi
        self.text(s, W - len(s) * 8, 70, self.GREY)

    def scatter(self, res, st):
        lo = st["min"]
        hi = st["max"]
        if hi - lo < 0.5:
            hi = lo + 0.5
        n = len(res)
        y0, h = 66, 52
        my = y0 - int((st["med"] - lo) / (hi - lo) * h)
        self.fb.hline(0, my, W, self.DARK)
        for i, v in enumerate(res):
            x = int(i * (W - 1) / max(1, n - 1))
            y = y0 - int((v - lo) / (hi - lo) * h)
            self.fb.fill_rect(x, y, 2, 2, self.CYAN)
        self.text("%.1f-%.1f ms" % (lo, hi), 2, 70, self.GREY)

    # ---------------- manual mode: the button is the mouse
    def run_manual(self):
        hw = self.hw
        self.wait_release()
        if not hw.usb_ready():
            self.message("NO USB", "Plug into the PC", "", self.RED)
            return
        avg, lo, hi = hw.level()
        fixed = TRIGGER_OPTS[self.cfg["trigger"]][1]
        delta = fixed if fixed else max(25, (hi - lo) * 3 + 15)
        dirn = DETECT_OPTS[self.cfg["detect"]][1]
        btn = self.cfg["button"] + 1
        res = []
        fails = 0
        failed = False
        while True:
            self.clear()
            self.header("MANUAL  %d" % len(res))
            if failed:
                self.big("--", 16, self.RED)
            elif res:
                self.big("%.1f" % res[-1], 16, self.WHITE)
            else:
                self.ctext("Press = click", 22)
            if res:
                self.ctext("med %.2f ms" % pct(sorted(res), 50), 50, self.YELLOW)
            self.footer("hold 2s = finish")
            self.show()
            r = None
            while r is None:
                r = hw.manual_trial(delta, dirn, TIMEOUT_US, CONFIRM, btn, 20000)
            base, t0, t1, ts = r
            failed = t1 < 0
            if failed:
                fails += 1
            else:
                res.append(delta30(t1, t0) / 1000)
            t_down = hw.ms()
            long_hold = False
            while hw.pressed():
                if hw.diff_ms(hw.ms(), t_down) > 2000:
                    long_hold = True
                hw.sleep_ms(5)
            hw.press(False, btn)
            hw.sleep_ms(30)
            if long_hold:
                if not failed and res:
                    res.pop()                # the finishing press isn't a measurement
                elif failed:
                    fails -= 1
                break
        self.btn.reset()
        if res:
            self.finish(res, fails, "manual", False)

    # ---------------- response scope
    def scope(self):
        hw = self.hw
        self.wait_release()
        if not hw.usb_ready():
            self.message("NO USB", "Plug into the PC", "", self.RED)
            return
        while True:
            if not self.countdown("RESPONSE"):
                return
            self.clear()
            self.header("CAPTURING")
            self.show()
            n, period, pre = 16000, 5, 400
            btn = self.cfg["button"] + 1
            buf, pre_rel, post_rel = hw.capture(n, period, pre, True, btn)
            hw.sleep_ms(HOLD_OPTS[self.cfg["hold"]])
            hw.press(False, btn)
            r = analyze_response(buf, period, pre, pre_rel, post_rel)
            self.clear()
            if r:
                self.header("50%% %.1f rise %.1f" % (r["t50"], r["rise"]))
            else:
                self.header("NO CLEAR CHANGE", self.RED)
            self.wave(buf, period, pre, pre_rel, post_rel, r)
            self.show()
            del buf
            hw.collect()
            e = self.wait_event(timeout_ms=120000)
            if e != "short":
                return

    def wave(self, buf, period, pre, pre_rel, post_rel, r=None, t_from=None, t_to=None):
        """Plot samples (time on x) into the area below the header."""
        n = len(buf)

        def t_ms(i):
            if i < pre:
                return (pre_rel + i * period) / 1000
            return (post_rel + (i - pre) * period) / 1000

        a = t_ms(0) if t_from is None else t_from
        b = t_ms(n - 1) if t_to is None else t_to
        lo = 4095
        hi = 0
        for v in buf:
            if v < lo:
                lo = v
            if v > hi:
                hi = v
        if hi - lo < 20:
            hi = lo + 20
        y0, h = 68, 54
        col = [None] * W
        for i in range(0, n, 2):
            t = t_ms(i)
            if t < a or t > b:
                continue
            x = int((t - a) / (b - a) * (W - 1))
            y = y0 - int((buf[i] - lo) / (hi - lo) * h)
            c = col[x]
            if c is None:
                col[x] = [y, y]
            else:
                if y < c[0]:
                    c[0] = y
                if y > c[1]:
                    c[1] = y
        for x in range(W):
            c = col[x]
            if c:
                self.fb.vline(x, c[0], c[1] - c[0] + 1, self.CYAN)
        if a < 0 < b:
            x = int((0 - a) / (b - a) * (W - 1))
            self.fb.vline(x, 13, 56, self.GREY)
        if r:
            x = int((r["t50"] - a) / (b - a) * (W - 1))
            self.fb.vline(x, 13, 56, self.YELLOW)
        self.text("%.0f" % a, 0, 71, self.GREY)
        s = "%.0fms" % b
        self.text(s, W - len(s) * 8, 71, self.GREY)

    # ---------------- flicker
    def flicker(self):
        hw = self.hw
        self.wait_release()
        while True:
            self.clear()
            self.header("FLICKER TEST")
            self.ctext("Sensor on a bright", 22, self.GREY)
            self.ctext("part of the screen", 32, self.GREY)
            self.ctext("Capturing...", 50)
            self.show()
            n, period = 16000, 12
            buf, pre_rel, post_rel = hw.capture(n, period, 0, False, 1)
            r = analyze_flicker(buf, period)
            self.clear()
            if not r["ok"]:
                self.header(r["msg"], self.RED)
            elif r["flicker"]:
                self.header("%.0f Hz  %.0f%% PWM" % (r["hz"], r["depth"]), self.ORANGE)
            else:
                self.header("NO FLICKER %.1f%%" % r["depth"], self.GREEN)
            self.wave(buf, period, 0, pre_rel, post_rel, None, 0, 20)
            self.show()
            del buf
            hw.collect()
            e = self.wait_event(timeout_ms=120000)
            if e != "short":
                return

    # ---------------- live sensor
    def sensor_view(self):
        hw = self.hw
        self.wait_release()
        hist = []
        while True:
            hist.append(hw.adc_avg(6))
            if len(hist) > W:
                hist.pop(0)
            if len(hist) % 3 == 0:
                lo = min(hist)
                hi = max(hist)
                span = max(40, hi - lo)
                mid = (hi + lo) / 2
                self.clear()
                self.header("SENSOR  %d" % hist[-1])
                self.text("0-4095", W - 50, 2, self.GREY)
                for x in range(1, len(hist)):
                    y1 = 41 - int((hist[x - 1] - mid) / span * 52)
                    y2 = 41 - int((hist[x] - mid) / span * 52)
                    self.fb.line(x - 1, y1, x, y2, self.CYAN)
                self.footer("press = back")
                self.show()
            e = self.btn.poll()
            if e:
                return
            hw.sleep_ms(10)

    # ---------------- history
    def history(self):
        txt = self.hw.read_file(HISTORY_FILE)
        rows = []
        if txt:
            for line in txt.split("\n")[1:]:
                p = line.split(",")
                if len(p) >= 9:
                    rows.append(p)
        if not rows:
            self.message("HISTORY", "No runs yet")
            return
        rows.reverse()
        top = 0
        while True:
            self.clear()
            self.header("HISTORY  %d runs" % len(rows))
            for i in range(5):
                if top + i >= len(rows):
                    break
                p = rows[top + i]
                self.text("#%-3s %6.2f n%s" % (p[0], float(p[3]), p[1]), 2, 14 + i * 11,
                          self.WHITE if i == 0 else self.GREY)
            self.footer("press=more hold=back")
            self.show()
            e = self.wait_event(timeout_ms=60000)
            if e != "short":
                return
            top = top + 5 if top + 5 < len(rows) else 0


def run(hw, tft, rgb):
    app = App(hw, tft, rgb)
    app.home()
