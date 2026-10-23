// Installs the tester firmware onto a Pico that runs MicroPython, through the
// MicroPython raw REPL over USB serial (the same mechanism Thonny uses).
#pragma once
#include <functional>
#include <string>

// Finds a port with a MicroPython REPL (or uses `port` if given) and uploads
// main.py plus the USB mouse library, then soft-resets the Pico.
// progress(text, 0..1) is called from the calling thread.
bool install_firmware(const std::string& port, const std::function<void(const std::string&, float)>& progress,
                      std::string& err);

// Returns the first port where a MicroPython REPL answers, or "".
std::string find_micropython_port();

const char* bundled_firmware_version();
