# Changelog

## v3.1 - standalone (October 2026)

Hardware: Waveshare RP2040-LCD-0.96 + Adafruit ALS-PT19 + one 6x6 mm button.

| From | To |
|---|---|
| ALS-PT19 `+` / `-` / `OUT` | 3V3 (pin 36) / AGND (pin 33) / GP26 (pin 31) |
| Button | GP16 (pin 21) and GND (pin 23) - soldered straight onto the board |

- **Button moved** from GP15 (pin 20) to GP16 (pin 21), with GND on pin 23.
  Maintenance mode (hold the button while plugging in) follows it.
- **Manual test removed.**
- **Flicker test fixed:** it ran the board out of memory analysing the 16,000
  samples and crashed, most visibly when no flicker was found. It now uses a
  fixed buffer made at start-up and a small histogram instead of a sorted
  copy. It also has a countdown, holds the mouse button so the target turns
  white, and reports "TOO DARK" when the sensor sees no light.
- **Countdown before every test** (run, response scope, flicker), and a click
  wherever the test needs the screen to change.
- **Sensor view** redraws continuously; a press sends a click so you can watch
  the reading jump; hold goes back.
- **History:** browse every run, open one for median / average / spread / 95th
  / fails, and *Clear history* (hold again to confirm).
- **Trigger defaults to Small** (the most sensitive fixed trigger). Boards
  with older settings switch once; later choices are kept.
- An out-of-memory error in any menu item now shows a message instead of
  crashing.
- **3D-printed case** (`standalone/case/`): screen window and button hole in
  the lid, sensor pocket under the board looking out of the back, board on a
  ledge with gaps for solder joints, push-fit lid, USB-C slot.
- Simulator covers 84 steps, including the flicker test on flickering, steady
  and dark screens, sensor-view clicks and clearing history.

## v3.0 - standalone

First version without a PC app: one-button menus, automatic runs with
calibration, results pages (median, histogram, every click, detail), response
scope, flicker test, sensor view, history, settings saved on the board.

## v2.1 - PC app

Windows app (Dear ImGui + D3D11) for a Pico / Pico W with the sensor and the
onboard LED: built-in full-screen or windowed target (F11 to toggle), latency
breakdown via clock sync, scope, history, firmware installer.

---

None of the versions has been run on real hardware yet - they compile for the
RP2040 and are tested against simulated boards. Results and issues welcome.
