#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lt {

int64_t delta30(int64_t a, int64_t b) {
    int64_t d = (a - b) & (kWrap30 - 1);
    if (d >= kWrap30 / 2) d -= kWrap30;
    return d;
}

double percentile_sorted(const std::vector<double>& s, double p) {
    if (s.empty()) return 0;
    if (s.size() == 1) return s[0];
    double pos = (p / 100.0) * double(s.size() - 1);
    size_t i = size_t(std::floor(pos));
    if (i >= s.size() - 1) return s.back();
    double f = pos - double(i);
    return s[i] + (s[i + 1] - s[i]) * f;
}

Stats compute_stats(std::vector<double> v) {
    Stats st;
    st.n = int(v.size());
    if (v.empty()) return st;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    st.mean = sum / double(v.size());
    double ss = 0;
    for (double x : v) ss += (x - st.mean) * (x - st.mean);
    st.sd = v.size() > 1 ? std::sqrt(ss / double(v.size() - 1)) : 0.0;
    st.min = v.front();
    st.max = v.back();
    st.median = percentile_sorted(v, 50);
    st.p5 = percentile_sorted(v, 5);
    st.p95 = percentile_sorted(v, 95);
    st.p99 = percentile_sorted(v, 99);
    return st;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string w;
    while (is >> w) out.push_back(w);
    return out;
}

bool starts_with(const std::string& s, const char* prefix) {
    size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static bool to_i64(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (!end || *end != 0) return false;
    out = v;
    return true;
}

static bool to_int(const std::string& s, int& out) {
    int64_t v;
    if (!to_i64(s, v)) return false;
    out = int(v);
    return true;
}

bool parse_hello(const std::string& line, Hello& out) {
    auto w = split_ws(line);
    if (w.size() < 5 || w[0] != "HELLO" || w[1] != "lattest") return false;
    out.version = w[2];
    out.board = w[3];
    for (auto& c : out.board) if (c == '_') c = ' ';
    return to_int(w[4], out.cpu_mhz);
}

bool parse_trial(const std::string& line, TrialRaw& out) {
    auto w = split_ws(line);
    if (w.size() < 6 || w[0] != "R") return false;
    return to_int(w[1], out.seq) && to_i64(w[2], out.t0) && to_i64(w[3], out.t1) &&
           to_i64(w[4], out.t_sent) && to_int(w[5], out.base);
}

bool parse_level(const std::string& line, Level& out) {
    auto w = split_ws(line);
    if (w.size() < 4 || w[0] != "LEVEL") return false;
    return to_int(w[1], out.avg) && to_int(w[2], out.min) && to_int(w[3], out.max);
}

bool parse_cap_header(const std::string& line, CaptureHeader& out) {
    auto w = split_ws(line);
    if (w.size() < 6 || w[0] != "CAP") return false;
    return to_int(w[1], out.n) && to_int(w[2], out.period_us) && to_int(w[3], out.pre) &&
           to_i64(w[4], out.pre_rel_us) && to_i64(w[5], out.post_rel_us);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_cap_data(const std::string& line, std::vector<uint16_t>& out) {
    if (!starts_with(line, "D ")) return false;
    size_t i = 2;
    size_t len = line.size();
    while (len > i && (line[len - 1] == ' ' || line[len - 1] == '\r')) len--;
    if ((len - i) % 4 != 0) return false;
    for (; i + 3 < len; i += 4) {
        int a = hexval(line[i]), b = hexval(line[i + 1]), c = hexval(line[i + 2]), d = hexval(line[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        int lo = a * 16 + b, hi = c * 16 + d;
        out.push_back(uint16_t(lo | (hi << 8)));
    }
    return true;
}

bool parse_stream(const std::string& line, std::vector<int>& out) {
    if (!starts_with(line, "L ")) return false;
    auto w = split_ws(line);
    for (size_t i = 1; i < w.size(); i++) {
        int v;
        if (to_int(w[i], v)) out.push_back(v);
    }
    return true;
}

ClockMap best_clock(const std::vector<Ping>& pings) {
    ClockMap m;
    const Ping* best = nullptr;
    for (auto& p : pings) {
        if (p.rtt() <= 0) continue;
        if (!best || p.rtt() < best->rtt()) best = &p;
    }
    if (!best) return m;
    m.valid = true;
    m.pico_ref = best->pico_us;
    m.host_ref = (best->host_send_us + best->host_recv_us) / 2.0;
    m.rtt_us = best->rtt();
    return m;
}

ClockMap merge_clock(const ClockMap& a, const ClockMap& b) {
    if (!a.valid) return b;
    if (!b.valid) return a;
    if (a.rtt_us > 2 * b.rtt_us + 500) return b;
    if (b.rtt_us > 2 * a.rtt_us + 500) return a;
    // Express b's reference in a's frame, average the offsets.
    ClockMap m = a;
    double b_in_a = a.to_host(b.pico_ref);
    double off = (b.host_ref - b_in_a) / 2.0;
    m.host_ref = a.host_ref + off;
    m.rtt_us = std::max(a.rtt_us, b.rtt_us);
    return m;
}

Trial make_trial(int index, const TrialRaw& raw, const ClockMap* clock, double host_input_us,
                 double host_present_us) {
    Trial t;
    t.index = index;
    t.base = raw.base;
    t.send_us = double(delta30(raw.t_sent, raw.t0));
    if (raw.t1 < 0) {
        t.status = Status::Timeout;
        return t;
    }
    t.e2e_ms = double(delta30(raw.t1, raw.t0)) / 1000.0;
    if (t.e2e_ms < 0) {
        t.status = Status::Error;
        return t;
    }
    if (clock && clock->valid) t.sync_err_ms = clock->rtt_us / 2000.0;
    if (clock && clock->valid && clock->rtt_us <= kMaxSyncRttUs && host_input_us > 0 && host_present_us > 0) {
        double h_t0 = clock->to_host(raw.t0);
        double h_t1 = clock->to_host(raw.t1);
        t.input_ms = (host_input_us - h_t0) / 1000.0;
        t.render_ms = (host_present_us - host_input_us) / 1000.0;
        t.display_ms = (h_t1 - host_present_us) / 1000.0;
        t.sync_err_ms = clock->rtt_us / 2000.0;
        t.has_breakdown = true;
    }
    return t;
}

std::vector<double> ok_values(const Run& run) {
    std::vector<double> v;
    for (auto& t : run.trials)
        if (t.status == Status::Ok) v.push_back(t.e2e_ms);
    return v;
}

RunSummary summarize(const Run& run) {
    RunSummary s;
    std::vector<double> in, re, di;
    for (auto& t : run.trials) {
        if (t.status != Status::Ok) {
            s.failed++;
            continue;
        }
        s.ok++;
        if (t.has_breakdown) {
            in.push_back(t.input_ms);
            re.push_back(t.render_ms);
            di.push_back(t.display_ms);
        }
    }
    s.e2e = compute_stats(ok_values(run));
    if (!in.empty()) {
        s.has_breakdown = true;
        auto a = compute_stats(in), b = compute_stats(re), c = compute_stats(di);
        s.input_ms = a.mean;
        s.render_ms = b.mean;
        s.display_ms = c.mean;
        s.input_med = a.median;
        s.render_med = b.median;
        s.display_med = c.median;
    }
    return s;
}

static const char* status_str(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Timeout: return "timeout";
        default: return "error";
    }
}

std::string run_to_csv(const Run& run) {
    std::ostringstream o;
    auto s = summarize(run);
    char buf[512];
    o << "# Latency Tester run: " << run.name << "\n";
    o << "# Started: " << run.started << "\n";
    o << "# Mode: " << run.settings.mode;
    if (!run.settings.target_style.empty()) o << " (" << run.settings.target_style << ")";
    o << "\n";
    std::snprintf(buf, sizeof buf,
                  "# Settings: trials=%d gap=%d-%dms hold=%dms timeout=%dms threshold=%d%% delta=%d dir=%d button=%d dark=%d bright=%d\n",
                  run.settings.trials, run.settings.gap_min_ms, run.settings.gap_max_ms, run.settings.hold_ms,
                  run.settings.timeout_ms, run.settings.threshold_pct, run.settings.delta, run.settings.dir,
                  run.settings.button, run.settings.dark, run.settings.bright);
    o << buf;
    std::snprintf(buf, sizeof buf,
                  "# Result: n=%d failed=%d mean=%.3f median=%.3f sd=%.3f min=%.3f max=%.3f p95=%.3f p99=%.3f ms\n",
                  s.ok, s.failed, s.e2e.mean, s.e2e.median, s.e2e.sd, s.e2e.min, s.e2e.max, s.e2e.p95, s.e2e.p99);
    o << buf;
    if (s.has_breakdown) {
        std::snprintf(buf, sizeof buf, "# Breakdown (mean): input=%.3f render=%.3f display=%.3f ms\n", s.input_ms,
                      s.render_ms, s.display_ms);
        o << buf;
    }
    o << "trial,status,e2e_ms,input_ms,render_ms,display_ms,sync_err_ms,send_us,sensor_base\n";
    for (auto& t : run.trials) {
        if (t.has_breakdown)
            std::snprintf(buf, sizeof buf, "%d,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%d\n", t.index, status_str(t.status),
                          t.e2e_ms, t.input_ms, t.render_ms, t.display_ms, t.sync_err_ms, t.send_us, t.base);
        else
            std::snprintf(buf, sizeof buf, "%d,%s,%.3f,,,,,%.0f,%d\n", t.index, status_str(t.status),
                          t.status == Status::Ok ? t.e2e_ms : 0.0, t.send_us, t.base);
        o << buf;
    }
    return o.str();
}

// ---------------- capture analysis ----------------
double Capture::time_ms(size_t i) const {
    if (int(i) < pre) return (double(pre_rel_us) + double(i) * period_us) / 1000.0;
    return (double(post_rel_us) + double(i - size_t(pre)) * period_us) / 1000.0;
}

std::vector<double> smooth(const std::vector<uint16_t>& v, int w) {
    std::vector<double> out(v.size());
    if (v.empty()) return out;
    if (w < 1) w = 1;
    int h = w / 2;
    double sum = 0;
    int n = int(v.size());
    // prefix sums for O(n)
    std::vector<double> ps(n + 1, 0);
    for (int i = 0; i < n; i++) ps[i + 1] = ps[i] + v[i];
    for (int i = 0; i < n; i++) {
        int a = std::max(0, i - h), b = std::min(n, i + h + 1);
        out[i] = (ps[b] - ps[a]) / double(b - a);
    }
    (void)sum;
    return out;
}

static double mean_range(const std::vector<double>& v, size_t a, size_t b) {
    if (b <= a) return 0;
    double s = 0;
    for (size_t i = a; i < b; i++) s += v[i];
    return s / double(b - a);
}

ResponseResult analyze_response(const Capture& c) {
    ResponseResult r;
    size_t n = c.samples.size();
    if (n < 50) {
        r.message = "Capture too short";
        return r;
    }
    int w = std::max(1, std::min(9, int(20 / std::max(1, c.period_us)) | 1));
    auto s = smooth(c.samples, w);
    // baseline: pre-click samples, or the first 2% after the click
    size_t b_end = c.pre > 8 ? size_t(c.pre) : std::max<size_t>(8, n / 50);
    r.base = mean_range(s, 0, b_end);
    // final: last 15%
    r.final_level = mean_range(s, n - n * 15 / 100, n);
    double step = r.final_level - r.base;
    if (std::fabs(step) < 30) {
        r.message = "No clear change in brightness (step < 30 counts)";
        return r;
    }
    r.rising = step > 0;
    auto frac = [&](size_t i) { return (s[i] - r.base) / step; };
    size_t start = size_t(c.pre);
    double t10 = -1, t50 = -1, t90 = -1;
    for (size_t i = start; i < n; i++) {
        double f = frac(i);
        if (t10 < 0 && f >= 0.10) t10 = c.time_ms(i);
        if (t50 < 0 && f >= 0.50) t50 = c.time_ms(i);
        if (t90 < 0 && f >= 0.90) {
            t90 = c.time_ms(i);
            break;
        }
    }
    if (t10 < 0 || t50 < 0 || t90 < 0) {
        r.message = "Transition not complete inside the capture window - capture longer";
        return r;
    }
    double peak = 0;
    for (size_t i = start; i < n; i++) peak = std::max(peak, frac(i));
    r.overshoot_pct = std::max(0.0, (peak - 1.0) * 100.0);
    r.t10_ms = t10;
    r.t50_ms = t50;
    r.t90_ms = t90;
    r.rise_ms = t90 - t10;
    r.ok = true;
    return r;
}

FlickerResult analyze_flicker(const Capture& c) {
    FlickerResult r;
    size_t n = c.samples.size();
    if (n < 100) {
        r.message = "Capture too short";
        return r;
    }
    std::vector<double> v(c.samples.begin(), c.samples.end());
    std::vector<double> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    r.p1 = percentile_sorted(sorted, 1);
    r.p99 = percentile_sorted(sorted, 99);
    double sum = 0;
    for (double x : v) sum += x;
    r.mean = sum / double(n);
    r.ok = true;
    if (r.p99 + r.p1 <= 0) {
        r.message = "Sensor reads zero - is it on a bright screen?";
        r.ok = false;
        return r;
    }
    r.depth_pct = (r.p99 - r.p1) / (r.p99 + r.p1) * 100.0;
    double pp = r.p99 - r.p1;
    if (pp < 24 || r.depth_pct < 1.5) {
        r.flicker = false;
        r.message = "No flicker detected (flat light output)";
        return r;
    }
    // count rising crossings of the mean with hysteresis
    double hi = r.mean + pp * 0.15, lo = r.mean - pp * 0.15;
    bool above = v[0] > r.mean;
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = 1; i < n; i++) {
        if (!above && v[i] > hi) {
            above = true;
            if (crossings == 0) first = i;
            last = i;
            crossings++;
        } else if (above && v[i] < lo) {
            above = false;
        }
    }
    if (crossings >= 3 && last > first) {
        double span_s = double(last - first) * c.period_us / 1e6;
        r.freq_hz = double(crossings - 1) / span_s;
        r.flicker = true;
        char buf[128];
        std::snprintf(buf, sizeof buf, "Flicker: %.0f Hz, %.0f%% modulation", r.freq_hz, r.depth_pct);
        r.message = buf;
    } else {
        r.flicker = false;
        r.message = "Brightness varies but no regular flicker found";
    }
    return r;
}

}  // namespace lt
