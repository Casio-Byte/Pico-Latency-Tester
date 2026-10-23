// Talks to the Pico firmware over the USB serial port. All calls block; call
// them from a worker thread. Calls are serialised internally.
#pragma once
#include <functional>
#include <mutex>
#include <string>

#include "core.h"
#include "serial_win.h"

class Device {
public:
    bool connect(const std::string& port);
    bool auto_connect();  // tries every serial port
    void disconnect();
    bool connected() const { return sp_.is_open() && ok_; }
    std::string port() const { return sp_.name(); }
    lt::Hello info;
    std::string last_error;

    bool ping(lt::Ping& out);
    lt::ClockMap sync(int n);  // n pings, best one
    bool level(lt::Level& out);
    bool cfg(const std::string& kv);
    bool trial(lt::TrialRaw& out, int timeout_ms);
    bool capture(int n, int period_us, int pre, bool click, lt::Capture& out);
    bool stream_start(int hz, std::function<void(const std::string&)> sink);
    bool stream_stop();
    bool led(bool on);
    bool exit_to_repl();

private:
    bool hello(int timeout_ms);
    bool send(const std::string& line, double* t_us = nullptr);
    bool wait_prefix(const char* prefix, RxLine& out, int timeout_ms);
    SerialPort sp_;
    std::recursive_mutex m_;
    bool ok_ = false;
    unsigned ping_id_ = 0;
    bool streaming_ = false;
};
