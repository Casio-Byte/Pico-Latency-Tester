// Windows USB-serial port with a background reader that splits lines and
// timestamps each one on arrival (for clock sync).
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

double qpc_us();  // PC clock, microseconds

struct PortInfo {
    std::string name;      // "COM5"
    std::string friendly;  // "USB Serial Device (COM5)" when available
};
std::vector<PortInfo> list_serial_ports();

struct RxLine {
    std::string text;
    double t_us = 0;  // when the bytes arrived
};

class SerialPort {
public:
    ~SerialPort();
    bool open(const std::string& name);
    void close();
    bool is_open() const { return h_ != nullptr && !broken_; }
    const std::string& name() const { return name_; }
    // Write; returns the PC time just before the bytes were handed to the driver.
    bool write(const std::string& s, double* t_send_us = nullptr);
    // Wait for the next line that is not consumed by the line filter.
    bool next_line(RxLine& out, int timeout_ms);
    void clear_lines();
    // Lines starting with "L " are passed here instead of the queue (live stream).
    void set_stream_sink(std::function<void(const std::string&)> f);
    // Raw mode (for the MicroPython raw REPL): bytes are collected unsplit.
    void set_raw(bool on);
    // Read raw bytes until `until` appears (returned data includes it) or timeout.
    bool read_until(const std::string& until, std::string& out, int timeout_ms);
    bool write_raw(const std::string& bytes);
    std::string why_broken() const { return why_; }

private:
    void reader();
    void* h_ = nullptr;
    std::string name_;
    std::thread rx_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> broken_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<RxLine> q_;
    std::mutex wm_;
    std::mutex sink_m_;
    std::function<void(const std::string&)> sink_;
    std::atomic<bool> raw_{false};
    std::string rawbuf_;
    std::string why_;
};
