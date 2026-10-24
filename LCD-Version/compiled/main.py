# Latency tester - LCD version (no PC app needed).
# Waveshare RP2040-LCD-0.96 + Adafruit ALS-PT19 on GP26 + one push button GP16->GND (pins 21 + 23).
#
# Hold the button while plugging in = maintenance mode (no USB mouse, Thonny can
# connect). BOOTSEL works too.
import machine
import rp2
import time
from machine import Pin

machine.freq(200_000_000)

import lt_display
import lt_hw

btn = Pin(16, Pin.IN, Pin.PULL_UP)
time.sleep_ms(50)
maintenance = btn.value() == 0 or rp2.bootsel_button()

tft = lt_display.TFT()
fb = tft.fb
if maintenance:
    fb.fill(0)
    fb.text("MAINTENANCE MODE", 16, 20, lt_display.rgb(255, 210, 0))
    fb.text("USB mouse off", 28, 36, 0xFFFF)
    fb.text("Thonny can connect", 8, 52, lt_display.rgb(120, 120, 130))
    tft.flush()
    raise SystemExit

try:
    hw = lt_hw.HW(button_pin=16, sensor_adc=26)
except RuntimeError as e:
    fb.fill(0)
    fb.text("Needs an RP2040", 20, 30, lt_display.rgb(255, 70, 70))
    tft.flush()
    raise SystemExit

fb.fill(0)
fb.text("LATENCY TESTER", 24, 28, lt_display.rgb(0, 200, 255))
fb.text("starting USB...", 20, 44, lt_display.rgb(120, 120, 130))
tft.flush()
hw.start_usb()

import lt_app

try:
    flip = 0
    try:
        import json
        with open(lt_app.SETTINGS_FILE) as f:
            flip = json.loads(f.read()).get("flip", 0)
    except Exception:
        pass
    if flip:
        tft.set_flip(True)
    lt_app.run(hw, tft, lt_display.rgb)
except Exception as e:
    import sys
    hw.press(False)
    fb.fill(0)
    fb.text("ERROR", 60, 4, lt_display.rgb(255, 70, 70))
    msg = repr(e)
    for i in range(6):
        fb.text(msg[i * 20:(i + 1) * 20], 0, 18 + i * 10, 0xFFFF)
    tft.flush()
    sys.print_exception(e)
