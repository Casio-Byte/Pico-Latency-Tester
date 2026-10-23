#!/bin/sh
# Pre-compiles the modules for the Pico (pip install mpy-cross). The .mpy files
# load faster and need less RAM than compiling the .py files on the board.
set -e
cd "$(dirname "$0")"
mkdir -p compiled
for m in lt_app lt_hw lt_display; do
  python3 -m mpy_cross -march=armv6m $m.py -o compiled/$m.mpy
done
cp main.py compiled/main.py
ls -la compiled
