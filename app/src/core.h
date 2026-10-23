// Portable core: statistics, protocol parsing, clock sync, capture analysis, CSV.
// No Windows headers here so it can be unit-tested anywhere.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lt {

// ---------------- 30-bit Pico timer helpers ----------------
constexpr int64_t kWrap30 = int64_t(1) << 30;
// Signed difference a - b on the 30-bit wrapping timer, in [-2^29, 2^29).
int64_t delta30(int64_t a, int64_t b);

// ---------------- statistics ----------------
struct Stats {
    int n = 0;
    double mean = 0, sd = 0, min = 0, max = 0;
    double median = 0, p5 = 0, p95 = 0, p99 = 0;
};
Stats compute_stats(std::vector<double> v);
double percentile_sorted(const std::vector<double>& sorted, double p);  // p in [0,100]

// ---------------- protocol ----------------
std::vector<std::string> split_ws(const std::string& s);
bool starts_with(const std::string& s, const char* prefix);

struct Hello {
    std::string version, board;
    int cpu_mhz = 0;
};
bool parse_hello(const std::string& line, Hello& out);

struct TrialRaw {
    int seq = 0;
    int64_t t0 = 0, t1 = -1, t_sent = 0;
    int base = 0;
};
bool parse_trial(const std::string& line, TrialRaw& out);

struct Level {
    int avg = 0, min = 0, max = 0;
};
bool parse_level(const std::string& line, Level& out);

struct CaptureHeader {
    int n = 0, period_us = 0, pre = 0;
    int64_t pre_rel_us = 0, post_rel_us = 0;
};
bool parse_cap_header(const std::string& line, CaptureHeader& out);
// Appends little-endian u16 samples decoded from "D <hex>". Returns false on bad hex.
bool parse_cap_data(const std::string& line, std::vector<uint16_t>& out);
bool parse_stream(const std::string& line, std::vector<int>& out);

// ---------------- clock sync ----------------
// One PING/PONG exchange. Host times are microseconds on the PC clock.
struct Ping {
    double host_send_us = 0, host_recv_us = 0;
    int64_t pico_us = 0;  // 30-bit
    double rtt() const { return host_recv_us - host_send_us; }
};
// A mapping between the Pico's timer and the PC clock, from the best ping(s).
struct ClockMap {
    bool valid = false;
    int64_t pico_ref = 0;  // 30-bit
    double host_ref = 0;   // us
    double rtt_us = 0;     // round trip of the best ping (accuracy is about +/- rtt/2)
    double to_host(int64_t pico30) const { return host_ref + double(delta30(pico30, pico_ref)); }
};
ClockMap best_clock(const std::vector<Ping>& pings);
// Average two maps (before/after a trial) to cancel drift. Either may be invalid.
// If one is much noisier than the other, the better one is used alone.
ClockMap merge_clock(const ClockMap& a, const ClockMap& b);
// Above this round trip the clock match is too uncertain for the latency breakdown.
constexpr double kMaxSyncRttUs = 5000;

// ---------------- trials and runs ----------------
enum class Status { Ok, Timeout, Error };

struct Trial {
    int index = 0;
    Status status = Status::Ok;
    double e2e_ms = 0;          // click sent -> light change (Pico timed)
    double send_us = 0;         // time the Pico spent queueing the USB report
    int base = 0;               // sensor level just before the click
    bool has_breakdown = false; // built-in target only
    double input_ms = 0;        // click sent -> app received the mouse message
    double render_ms = 0;       // app received input -> frame presented
    double display_ms = 0;      // frame presented -> sensor saw the change
    double sync_err_ms = 0;     // +/- uncertainty of the breakdown
};

// Fill in a Trial from the Pico's raw result and (optionally) PC-side timestamps.
Trial make_trial(int index, const TrialRaw& raw, const ClockMap* clock,
                 double host_input_us, double host_present_us);

struct RunSettings {
    std::string mode;  // "target" or "external"
    std::string target_style;  // built-in target: "full screen", "small window", "large window"
    int trials = 100;
    int gap_min_ms = 250, gap_max_ms = 600;
    int hold_ms = 60;
    int timeout_ms = 500;
    int threshold_pct = 50;
    int delta = 0;
    int dir = 1;
    int button = 1;
    int dark = 0, bright = 0;
};

struct Run {
    std::string name;
    std::string started;  // "YYYY-MM-DD HH:MM:SS"
    RunSettings settings;
    std::vector<Trial> trials;
    std::string csv_path;
};

struct RunSummary {
    Stats e2e;
    int ok = 0, failed = 0;
    bool has_breakdown = false;
    double input_ms = 0, render_ms = 0, display_ms = 0;  // means over OK trials with breakdown
    double input_med = 0, render_med = 0, display_med = 0;
};
RunSummary summarize(const Run& run);
std::vector<double> ok_values(const Run& run);
std::string run_to_csv(const Run& run);

// ---------------- capture analysis ----------------
struct Capture {
    int period_us = 0;
    int pre = 0;
    int64_t pre_rel_us = 0, post_rel_us = 0;
    std::vector<uint16_t> samples;
    bool clicked = false;
    // time of sample i relative to the click (ms)
    double time_ms(size_t i) const;
};

struct ResponseResult {
    bool ok = false;
    std::string message;
    double base = 0, final_level = 0;
    bool rising = true;
    double t10_ms = 0, t50_ms = 0, t90_ms = 0;  // after the click
    double rise_ms = 0;                         // 10% -> 90%
    double overshoot_pct = 0;
};
ResponseResult analyze_response(const Capture& c);

struct FlickerResult {
    bool ok = false;
    bool flicker = false;
    std::string message;
    double mean = 0, p1 = 0, p99 = 0;
    double depth_pct = 0;  // (max-min)/(max+min)
    double freq_hz = 0;
};
FlickerResult analyze_flicker(const Capture& c);

// moving average (window w, centred) used for plotting and crossings
std::vector<double> smooth(const std::vector<uint16_t>& v, int w);

}  // namespace lt
