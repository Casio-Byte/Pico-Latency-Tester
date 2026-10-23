#include "installer.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "firmware_files.h"
#include "serial_win.h"

const char* bundled_firmware_version() { return kFirmwareVersion; }

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::string b64(const unsigned char* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = unsigned(d[i]) << 16;
        if (i + 1 < n) v |= unsigned(d[i + 1]) << 8;
        if (i + 2 < n) v |= unsigned(d[i + 2]);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

namespace {
class RawRepl {
public:
    explicit RawRepl(SerialPort& sp) : sp_(sp) {}
    std::string error;

    bool enter() {
        sp_.set_raw(true);
        // Ctrl-C twice stops a running program (the tester disables Ctrl-C, so
        // the caller sends EXIT first), Ctrl-A enters the raw REPL.
        sp_.write_raw("\r\x03\x03");
        sleep_ms(150);
        sp_.set_raw(true);  // drop whatever was printed
        sp_.write_raw("\r\x01");
        std::string got;
        if (!sp_.read_until("raw REPL; CTRL-B to exit\r\n>", got, 2500)) {
            error = "Pico did not enter the MicroPython raw REPL";
            return false;
        }
        return true;
    }

    // Runs code; returns false (with error) if it raised.
    bool exec(const std::string& code, std::string* output = nullptr, int timeout_ms = 5000) {
        for (size_t i = 0; i < code.size(); i += 256) {
            if (!sp_.write_raw(code.substr(i, 256))) {
                error = "Write failed";
                return false;
            }
        }
        if (!sp_.write_raw("\x04")) {
            error = "Write failed";
            return false;
        }
        std::string got;
        if (!sp_.read_until("OK", got, 2000)) {
            error = "Raw REPL did not accept the code";
            return false;
        }
        std::string out, errs, prompt;
        if (!sp_.read_until("\x04", out, timeout_ms) || !sp_.read_until("\x04", errs, 2000) ||
            !sp_.read_until(">", prompt, 2000)) {
            error = "Raw REPL stopped responding";
            return false;
        }
        out.pop_back();
        errs.pop_back();
        if (output) *output = out;
        if (!errs.empty()) {
            error = "Pico error: " + errs;
            return false;
        }
        return true;
    }

    void exit_and_reset() {
        sp_.write_raw("\x02");  // back to the normal REPL
        sleep_ms(100);
        sp_.write_raw("\x04");  // soft reset -> runs the new main.py
        sleep_ms(300);
    }

private:
    SerialPort& sp_;
};
}  // namespace

std::string find_micropython_port() {
    for (auto& p : list_serial_ports()) {
        SerialPort sp;
        if (!sp.open(p.name)) continue;
        sp.set_raw(true);
        sp.write_raw("\r\x03\x03");
        sleep_ms(150);
        sp.set_raw(true);
        sp.write_raw("\r\x02\r");
        std::string got;
        if (sp.read_until(">>> ", got, 800)) return p.name;
    }
    return "";
}

bool install_firmware(const std::string& port_in, const std::function<void(const std::string&, float)>& progress,
                      std::string& err) {
    std::string port = port_in;
    if (port.empty()) {
        progress("Looking for a Pico running MicroPython...", 0.0f);
        port = find_micropython_port();
        if (port.empty()) {
            err = "No Pico with MicroPython found. Install MicroPython on it first (see README), close Thonny, "
                  "and plug it in.";
            return false;
        }
    }
    SerialPort sp;
    if (!sp.open(port)) {
        err = "Could not open " + port + " - close Thonny and try again.";
        return false;
    }
    RawRepl repl(sp);
    progress("Connecting to MicroPython on " + port + "...", 0.02f);
    if (!repl.enter()) {
        err = repl.error;
        return false;
    }
    std::string out;
    if (!repl.exec("import sys\nprint(sys.implementation._machine)\n", &out)) {
        err = repl.error;
        return false;
    }
    if (out.find("RP2040") == std::string::npos) {
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        err = "This board is not a Pico / Pico W with the RP2040 chip (found: " + out +
              "). The Pico 2 (RP2350) is not supported yet.";
        return false;
    }
    if (!repl.exec("import os\nfor d in ('lib','lib/usb','lib/usb/device'):\n"
                   "    try:\n        os.mkdir(d)\n    except OSError:\n        pass\n"
                   "from binascii import a2b_base64 as _a\n")) {
        err = repl.error;
        return false;
    }
    size_t total = 0, done = 0;
    for (int i = 0; i < kFirmwareFileCount; i++) total += kFirmwareFiles[i].size;
    for (int i = 0; i < kFirmwareFileCount; i++) {
        const auto& f = kFirmwareFiles[i];
        std::string path = f.path;
        progress("Writing " + path, 0.05f + 0.9f * float(done) / float(total));
        // write to a temp name first so a failed upload never leaves half a main.py
        std::string tmp = path + ".tmp";
        if (!repl.exec("_f=open('" + tmp + "','wb')\n")) {
            err = repl.error;
            return false;
        }
        const size_t chunk = 1536;
        for (size_t o = 0; o < f.size; o += chunk) {
            size_t n = std::min(chunk, f.size - o);
            if (!repl.exec("_f.write(_a('" + b64(f.data + o, n) + "'))\n")) {
                err = repl.error;
                return false;
            }
            done += n;
            progress("Writing " + path, 0.05f + 0.9f * float(done) / float(total));
        }
        std::string size_out;
        if (!repl.exec("_f.close()\nimport os\ntry:\n    os.remove('" + path +
                           "')\nexcept OSError:\n    pass\nos.rename('" + tmp + "','" + path + "')\nprint(os.stat('" +
                           path + "')[6])\n",
                       &size_out)) {
            err = repl.error;
            return false;
        }
        if (std::strtoul(size_out.c_str(), nullptr, 10) != f.size) {
            err = "Size check failed for " + path;
            return false;
        }
    }
    progress("Restarting the Pico...", 0.97f);
    repl.exit_and_reset();
    sp.close();
    progress("Done", 1.0f);
    return true;
}
