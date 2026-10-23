#!/bin/sh
# Cross-compiles LatencyTester.exe for 64-bit Windows using Zig's clang (pip install ziglang).
set -e
cd "$(dirname "$0")"
I=third_party/imgui; P=third_party/implot
CXX="python3 -m ziglang c++ -target x86_64-windows-gnu -std=c++17 -O2"
mkdir -p build/obj
for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp \
         $I/backends/imgui_impl_win32.cpp $I/backends/imgui_impl_dx11.cpp $P/implot.cpp $P/implot_items.cpp; do
  o=build/obj/$(basename $f .cpp).o
  [ -f $o ] || $CXX -I$I -I$I/backends -I$P -c $f -o $o
done
python3 tools/embed.py
for f in src/app.cpp src/core.cpp src/device.cpp src/serial_win.cpp src/target.cpp src/installer.cpp; do
  $CXX -Wall -Wno-unused-function -I$I -I$I/backends -I$P -Isrc -c $f -o build/obj/app_$(basename $f .cpp).o &
done
wait
$CXX build/obj/*.o -o build/LatencyTester.exe -static -s -Wl,--subsystem,windows \
  -ld3d11 -ld3dcompiler_47 -ldxgi -lgdi32 -ldwmapi -lwinmm -lshell32 -lole32 -luuid -ladvapi32 -luser32
ls -la build/LatencyTester.exe
