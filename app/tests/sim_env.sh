#!/bin/bash
# Resets the simulated test bench: virtual serial pair, fake Pico, Wine COM7 link.
# Then start the app separately with:  wine64 build/LatencyTester.exe
set -u
T=${SIM_DIR:-/tmp/latency-sim}; mkdir -p $T
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$T/wineprefix} DISPLAY=:55
pkill -f '[f]ake_pico.py' 2>/dev/null
pkill -f '[L]atencyTester.exe' 2>/dev/null
pkill -f 'socat -d -d pty' 2>/dev/null
sleep 1
pgrep -x Xvfb >/dev/null || (Xvfb :55 -screen 0 1600x1000x24 +extension GLX >$T/xvfb.log 2>&1 &)
rm -f $T/pico_dev $T/pico_host
(socat -d -d pty,raw,echo=0,link=$T/pico_dev pty,raw,echo=0,link=$T/pico_host >$T/socat.log 2>&1 &)
for i in $(seq 50); do [ -e $T/pico_host ] && break; sleep 0.1; done
ln -sf $T/pico_host $WINEPREFIX/dosdevices/com7
timeout 60 /usr/lib/wine/wine64 reg add 'HKLM\HARDWARE\DEVICEMAP\SERIALCOMM' /v '\Device\Serial6' /t REG_SZ /d COM7 /f >/dev/null 2>&1
rm -f /tmp/fakepico.log
cd "$(dirname "$0")/.."
(python3 -I tests/fake_pico.py $T/pico_dev --fsroot $T/fakefs >$T/fake.out 2>&1 &)
for i in $(seq 50); do grep -q "fake pico up" /tmp/fakepico.log 2>/dev/null && break; sleep 0.1; done
echo "bench ready: $(readlink $T/pico_host)"
