#include "device.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <algorithm>

using namespace lt;

bool Device::send(const std::string& line, double* t_us) {
    if (!sp_.write(line + "\n", t_us)) {
        last_error = "Write to " + sp_.name() + " failed (unplugged?)";
        ok_ = false;
        return false;
    }
    return true;
}

bool Device::wait_prefix(const char* prefix, RxLine& out, int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        RxLine l;
        if (!sp_.next_line(l, int(left))) {
            if (!sp_.is_open()) {
                last_error = "Pico disconnected" + (sp_.why_broken().empty() ? "" : " (" + sp_.why_broken() + ")");
                ok_ = false;
                return false;
            }
            continue;
        }
        if (starts_with(l.text, prefix)) {
            out = std::move(l);
            return true;
        }
        if (starts_with(l.text, "ERR")) {
            last_error = "Pico: " + l.text;
            return false;
        }
        if (starts_with(l.text, "Traceback") || starts_with(l.text, ">>>")) {
            last_error = "Pico program is not running (REPL). Unplug and replug it.";
            return false;
        }
        // anything else is stale output - ignore
    }
    last_error = std::string("No reply to ") + prefix + " (timeout)";
    return false;
}

bool Device::hello(int timeout_ms) {
    sp_.clear_lines();
    if (!send("") || !send("HELLO")) return false;
    RxLine l;
    if (!wait_prefix("HELLO", l, timeout_ms)) return false;
    if (!parse_hello(l.text, info)) {
        last_error = "Unexpected reply: " + l.text;
        return false;
    }
    return true;
}

bool Device::connect(const std::string& port) {
    std::lock_guard<std::recursive_mutex> g(m_);
    disconnect();
    if (!sp_.open(port)) {
        last_error = "Could not open " + port + " (in use by Thonny?)";
        return false;
    }
    ok_ = true;
    if (!hello(800)) {
        sp_.close();
        ok_ = false;
        if (last_error.find("timeout") != std::string::npos)
            last_error = port + " did not answer like the latency tester";
        return false;
    }
    return true;
}

bool Device::auto_connect() {
    std::lock_guard<std::recursive_mutex> g(m_);
    auto ports = list_serial_ports();
    if (ports.empty()) {
        last_error = "No serial ports found. Is the Pico plugged in?";
        return false;
    }
    for (auto& p : ports)
        if (connect(p.name)) return true;
    last_error = "No latency tester found on " + std::to_string(ports.size()) + " port(s)";
    return false;
}

void Device::disconnect() {
    std::lock_guard<std::recursive_mutex> g(m_);
    streaming_ = false;
    sp_.set_stream_sink(nullptr);
    sp_.close();
    ok_ = false;
}

bool Device::ping(Ping& out) {
    std::lock_guard<std::recursive_mutex> g(m_);
    // Each ping carries an id; a late reply to an earlier ping must never be
    // mistaken for this one (that would skew the clock match badly).
    if (streaming_) {  // any command would stop the live stream
        last_error = "Busy streaming";
        return false;
    }
    unsigned id = ++ping_id_;
    double ts = 0;
    if (!send("PING " + std::to_string(id), &ts)) return false;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < end) {
        RxLine l;
        int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
        if (!wait_prefix("P ", l, std::max(1, left))) return false;
        auto w = split_ws(l.text);
        if (w.size() < 3 || std::strtoul(w[2].c_str(), nullptr, 10) != id) continue;  // stale reply
        out.host_send_us = ts;
        out.host_recv_us = l.t_us;
        out.pico_us = std::strtoll(w[1].c_str(), nullptr, 10);
        return true;
    }
    last_error = "No reply to PING (timeout)";
    return false;
}

ClockMap Device::sync(int n) {
    std::lock_guard<std::recursive_mutex> g(m_);
    std::vector<Ping> pings;
    for (int i = 0; i < n; i++) {
        Ping p;
        if (ping(p)) pings.push_back(p);
    }
    return best_clock(pings);
}

bool Device::level(Level& out) {
    std::lock_guard<std::recursive_mutex> g(m_);
    if (!send("LEVEL")) return false;
    RxLine l;
    if (!wait_prefix("LEVEL", l, 500)) return false;
    return parse_level(l.text, out);
}

bool Device::cfg(const std::string& kv) {
    std::lock_guard<std::recursive_mutex> g(m_);
    if (!send("CFG " + kv)) return false;
    RxLine l;
    return wait_prefix("OK", l, 500);
}

bool Device::trial(TrialRaw& out, int timeout_ms) {
    std::lock_guard<std::recursive_mutex> g(m_);
    if (!send("TRIAL")) return false;
    RxLine l;
    if (!wait_prefix("R ", l, timeout_ms + 1500)) return false;
    if (!parse_trial(l.text, out)) {
        last_error = "Bad trial reply: " + l.text;
        return false;
    }
    return true;
}

bool Device::capture(int n, int period_us, int pre, bool click, Capture& out) {
    std::lock_guard<std::recursive_mutex> g(m_);
    char cmd[96];
    std::snprintf(cmd, sizeof cmd, "CAP %d %d %d %d", n, period_us, pre, click ? 1 : 0);
    if (!send(cmd)) return false;
    RxLine l;
    int window_ms = int((long long)n * period_us / 1000) + 3000;
    if (!wait_prefix("CAP ", l, window_ms)) return false;
    CaptureHeader h;
    if (!parse_cap_header(l.text, h)) {
        last_error = "Bad capture header: " + l.text;
        return false;
    }
    out = Capture{};
    out.period_us = h.period_us;
    out.pre = h.pre;
    out.pre_rel_us = h.pre_rel_us;
    out.post_rel_us = h.post_rel_us;
    out.clicked = click;
    out.samples.reserve(size_t(h.n));
    while (true) {
        RxLine d;
        if (!sp_.next_line(d, 3000)) {
            last_error = "Capture data stopped arriving";
            return false;
        }
        if (d.text == "CAPEND") break;
        if (!parse_cap_data(d.text, out.samples)) {
            last_error = "Corrupt capture data";
            return false;
        }
    }
    if (int(out.samples.size()) != h.n) {
        last_error = "Capture size mismatch";
        return false;
    }
    return true;
}

bool Device::stream_start(int hz, std::function<void(const std::string&)> sink) {
    std::lock_guard<std::recursive_mutex> g(m_);
    sp_.set_stream_sink(std::move(sink));
    streaming_ = send("STREAM " + std::to_string(hz));
    return streaming_;
}

bool Device::stream_stop() {
    std::lock_guard<std::recursive_mutex> g(m_);
    streaming_ = false;
    bool ok = send("STOP");
    RxLine l;
    if (ok) ok = wait_prefix("OK", l, 800);
    sp_.set_stream_sink(nullptr);
    return ok;
}

bool Device::led(bool on) {
    std::lock_guard<std::recursive_mutex> g(m_);
    if (!send(on ? "LED 1" : "LED 0")) return false;
    RxLine l;
    return wait_prefix("OK", l, 500);
}

bool Device::exit_to_repl() {
    std::lock_guard<std::recursive_mutex> g(m_);
    send("EXIT");
    RxLine l;
    bool ok = wait_prefix("BYE", l, 800);
    disconnect();
    return ok;
}
