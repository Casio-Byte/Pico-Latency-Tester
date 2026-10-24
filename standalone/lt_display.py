# Driver for the Waveshare RP2040-LCD-0.96 built-in screen:
# 160x80 ST7735S IPS on SPI1. Pins: DC=GP8 CS=GP9 CLK=GP10 DIN=GP11 RST=GP12 BL=GP25.
import framebuf
import time
from machine import PWM, SPI, Pin


def rgb(r, g, b):
    """RGB565, byte-swapped for the panel (framebuf stores little-endian)."""
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((c & 0xFF) << 8) | (c >> 8)


class TFT:
    W = 160
    H = 80

    def __init__(self, flip=False, bgr=True, hz=40_000_000):
        # SPI first, then the control pins (re-claims GP8 from SPI1's default MISO)
        self.spi = SPI(1, baudrate=hz, polarity=0, phase=0, sck=Pin(10), mosi=Pin(11))
        self.dc = Pin(8, Pin.OUT, value=1)
        self.cs = Pin(9, Pin.OUT, value=1)
        self.rst = Pin(12, Pin.OUT, value=1)
        self.bl = PWM(Pin(25))
        self.bl.freq(1000)
        self.bl.duty_u16(0)
        self.rst(0)
        time.sleep_ms(10)
        self.rst(1)
        time.sleep_ms(120)
        self._cmd(0x01)                      # software reset
        time.sleep_ms(150)
        self._cmd(0x11)                      # sleep out
        time.sleep_ms(120)
        self._cmd(0x21)                      # inversion on (IPS panel)
        self._cmd(0x3A, b"\x05")             # RGB565
        self.set_flip(flip, bgr)
        self._cmd(0x29)                      # display on
        # panel sits at column 1, row 26 of the controller's memory
        self._wx = bytes([0, 1, 0, self.W])
        self._wy = bytes([0, 26, 0, 26 + self.H - 1])
        self.buf = bytearray(self.W * self.H * 2)
        self.fb = framebuf.FrameBuffer(self.buf, self.W, self.H, framebuf.RGB565)
        self.flush()
        self.backlight(70)

    def set_flip(self, flip, bgr=True):
        mad = (0x60 if flip else 0xA0) | (0x08 if bgr else 0x00)
        self._cmd(0x36, bytes([mad]))

    def backlight(self, pct):
        self.bl.duty_u16(int(max(0, min(100, pct)) * 655.35))

    def _cmd(self, c, data=None):
        self.cs(0)
        self.dc(0)
        self.spi.write(bytes([c]))
        if data:
            self.dc(1)
            self.spi.write(data)
        self.cs(1)

    def flush(self):
        self._cmd(0x2A, self._wx)
        self._cmd(0x2B, self._wy)
        self.cs(0)
        self.dc(0)
        self.spi.write(b"\x2c")
        self.dc(1)
        self.spi.write(self.buf)
        self.cs(1)
