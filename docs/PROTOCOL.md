# Serial protocol

The Pico appears as a USB serial port alongside the USB mouse. Commands are
ASCII lines ending in `\n`; replies are lines starting with a tag (the Pico
sends `\r\n` line endings). The app must assert DTR, and must never set the
port to 1200 baud (that reboots the Pico into its bootloader).

Times are the Pico's microsecond timer masked to 30 bits (wraps every
2^30 us, about 17.9 minutes). Sensor values are 12-bit ADC counts (0-4095,
higher = brighter).

| Command | Reply | Notes |
|---|---|---|
| `HELLO` | `HELLO lattest <version> <board> <cpu_mhz>` | Board name has spaces replaced by `_` |
| `PING <id>` | `P <pico_us> <id>` | Clock sync. The id lets the host ignore late replies |
| `LEVEL` | `LEVEL <avg> <min> <max>` | 256 samples, 2 us apart |
| `CFG key=val ...` | `OK` | See settings below |
| `TRIAL` | `R <seq> <t0> <t1> <t_sent> <base>` | One click. `t1 = -1` on timeout |
| `CAP <n> <period_us> <pre> <click>` | `CAP ...`, `D ...` lines, `CAPEND` | High-speed capture, below |
| `STREAM <hz>` | `L <v> <v> ...` every ~20 ms | Live view; any next command stops it |
| `STOP` | `OK` | Ends a stream |
| `LED <0\|1>` | `OK` | Onboard LED |
| `EXIT` | `BYE` | Stops the program, leaving the MicroPython REPL |
| anything else | `ERR <message>` | |

## TRIAL

1. Baseline = average of 64 samples.
2. `t0` = timer; mouse button down; `t_sent` = timer after the report is queued.
3. Spin until the sensor is `delta` counts away from the baseline in direction
   `dir` for `confirm` consecutive samples; `t1` = time of the first of them.
4. Reply `R`, then hold the button for `hold` ms in total and release it.

Latency = `t1 - t0` (mod 2^30).

## CFG keys

| Key | Default | Meaning |
|---|---|---|
| `dir` | 1 | +1 brighter, -1 darker, 0 either |
| `delta` | 200 | ADC counts away from the baseline that count as a change |
| `timeout` | 500000 | us to wait for a change |
| `hold` | 60 | ms the button is held |
| `confirm` | 3 | consecutive samples over the threshold |
| `button` | 1 | 1 left, 2 right |

## CAP

Captures `n` samples (max 30000) every `period_us` (min 2). The first `pre`
samples are taken before the click (if `click` is 1), the rest after it.

```
CAP <n> <period> <pre> <pre_rel_us> <post_rel_us>
D <hex>          (repeated; 12-bit samples as little-endian u16, 512 per line)
CAPEND
```

Sample `i` was taken at, relative to the click:

- `i < pre`: `pre_rel_us + i * period`
- `i >= pre`: `post_rel_us + (i - pre) * period`
