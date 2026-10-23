// Unit tests for the portable core. Build: g++ -std=c++17 -I../src test_core.cpp ../src/core.cpp
#include <cmath>
#include <cstdio>
#include <string>

#include "core.h"

using namespace lt;
static int fails = 0, checks = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
        }                                                                \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= (tol))

static void test_delta30() {
    CHECK(delta30(100, 50) == 50);
    CHECK(delta30(50, 100) == -50);
    // wrap: a just after wrap, b just before
    CHECK(delta30(10, kWrap30 - 10) == 20);
    CHECK(delta30(kWrap30 - 10, 10) == -20);
}

static void test_stats() {
    auto s = compute_stats({1, 2, 3, 4, 5});
    CHECK(s.n == 5);
    NEAR(s.mean, 3, 1e-9);
    NEAR(s.median, 3, 1e-9);
    NEAR(s.min, 1, 1e-9);
    NEAR(s.max, 5, 1e-9);
    NEAR(s.sd, std::sqrt(2.5), 1e-9);
    NEAR(s.p95, 4.8, 1e-9);
    auto e = compute_stats({});
    CHECK(e.n == 0);
    auto one = compute_stats({7});
    NEAR(one.median, 7, 1e-9);
    NEAR(one.sd, 0, 1e-9);
}

static void test_parse() {
    Hello h;
    CHECK(parse_hello("HELLO lattest 2.0 Raspberry_Pi_Pico_W_with_RP2040 200", h));
    CHECK(h.version == "2.0");
    CHECK(h.cpu_mhz == 200);
    CHECK(h.board == "Raspberry Pi Pico W with RP2040");
    CHECK(!parse_hello("HELLO other 1 x 1", h));

    TrialRaw t;
    CHECK(parse_trial("R 5 1000 13500 1060 812", t));
    CHECK(t.seq == 5 && t.t0 == 1000 && t.t1 == 13500 && t.t_sent == 1060 && t.base == 812);
    CHECK(parse_trial("R 6 1000 -1 1060 812", t));
    CHECK(t.t1 == -1);
    CHECK(!parse_trial("R 6 1000", t));
    CHECK(!parse_trial("R x 1 2 3 4", t));

    Level l;
    CHECK(parse_level("LEVEL 512 500 530", l));
    CHECK(l.avg == 512 && l.min == 500 && l.max == 530);

    CaptureHeader ch;
    CHECK(parse_cap_header("CAP 1000 4 100 -412 37", ch));
    CHECK(ch.n == 1000 && ch.period_us == 4 && ch.pre == 100 && ch.pre_rel_us == -412 && ch.post_rel_us == 37);

    std::vector<uint16_t> d;
    CHECK(parse_cap_data("D 0100ff0f", d));  // 0x0001, 0x0fff
    CHECK(d.size() == 2 && d[0] == 1 && d[1] == 0x0fff);
    CHECK(!parse_cap_data("D 01z0", d));
    CHECK(parse_cap_data("D 0200\r", d));
    CHECK(d.size() == 3 && d[2] == 2);

    std::vector<int> sv;
    CHECK(parse_stream("L 1 2 3", sv));
    CHECK(sv.size() == 3 && sv[2] == 3);
}

static void test_clock() {
    // Pico clock = host clock + 1,000,000 us, symmetric 1 ms RTT for best ping
    std::vector<Ping> pings = {
        {5000, 8000, 1000000 + 6000},   // rtt 3000, skewed
        {10000, 11000, 1000000 + 10500},  // rtt 1000, symmetric -> best
        {20000, 22000, 1000000 + 21000},
    };
    auto m = best_clock(pings);
    CHECK(m.valid);
    NEAR(m.rtt_us, 1000, 1e-9);
    NEAR(m.to_host(1000000 + 30000), 30000, 1e-6);
    // across the 30-bit wrap
    std::vector<Ping> wp = {{0, 1000, kWrap30 - 500}};
    auto w = best_clock(wp);
    NEAR(w.to_host(1000), 2000, 1e-6);  // 1500 us after the ping midpoint (500)
    // merge cancels offset error
    ClockMap a = m, b = m;
    a.host_ref += 200;
    b.host_ref -= 200;
    auto mm = merge_clock(a, b);
    NEAR(mm.to_host(1000000 + 30000), 30000, 1e-6);
    ClockMap inv;
    CHECK(merge_clock(inv, m).valid);
}

static void test_trial() {
    TrialRaw raw;
    raw.t0 = 1000000;
    raw.t_sent = 1000080;
    raw.t1 = 1012500;  // 12.5 ms
    raw.base = 400;
    std::vector<Ping> p = {{0, 1000, 1000000 - 9500}};  // host 500 <-> pico 990500 => offset 990000
    auto clock = best_clock(p);
    // host: t0 at 10000, input at 11500 (1.5 ms), present at 12500 (1 ms render), t1 at 22500 -> display 10 ms
    auto t = make_trial(1, raw, &clock, 11500, 12500);
    CHECK(t.status == Status::Ok);
    NEAR(t.e2e_ms, 12.5, 1e-9);
    NEAR(t.send_us, 80, 1e-9);
    CHECK(t.has_breakdown);
    NEAR(t.input_ms, 1.5, 1e-9);
    NEAR(t.render_ms, 1.0, 1e-9);
    NEAR(t.display_ms, 10.0, 1e-9);
    NEAR(t.input_ms + t.render_ms + t.display_ms, t.e2e_ms, 1e-9);
    NEAR(t.sync_err_ms, 0.5, 1e-9);

    // a noisy sync (20 ms round trip) must not produce a breakdown
    std::vector<Ping> noisy = {{0, 20000, 1000000 - 0}};
    auto nclk = best_clock(noisy);
    auto tn = make_trial(9, raw, &nclk, 11500, 12500);
    CHECK(tn.status == Status::Ok && !tn.has_breakdown);
    NEAR(tn.sync_err_ms, 10.0, 1e-9);
    // merge prefers the clean map when the other is much worse
    auto mg = merge_clock(nclk, clock);
    NEAR(mg.rtt_us, 1000, 1e-9);

    raw.t1 = -1;
    auto to = make_trial(2, raw, nullptr, 0, 0);
    CHECK(to.status == Status::Timeout);

    Run run;
    run.name = "t";
    run.settings.mode = "target";
    run.trials = {t, to};
    auto s = summarize(run);
    CHECK(s.ok == 1 && s.failed == 1 && s.has_breakdown);
    auto csv = run_to_csv(run);
    CHECK(csv.find("trial,status,e2e_ms") != std::string::npos);
    CHECK(csv.find("1,ok,12.500,1.500,1.000,10.000") != std::string::npos);
    CHECK(csv.find("2,timeout") != std::string::npos);
}

static Capture synth_step(double delay_ms, double rise_ms, int period_us, int pre, double lo, double hi) {
    Capture c;
    c.period_us = period_us;
    c.pre = pre;
    c.pre_rel_us = -int64_t(pre) * period_us;
    c.post_rel_us = 0;
    int n = 20000;
    for (int i = 0; i < n; i++) {
        double t = c.time_ms(size_t(i));
        double f = 0;
        if (t > delay_ms) f = std::min(1.0, (t - delay_ms) / rise_ms);
        c.samples.push_back(uint16_t(lo + (hi - lo) * f + ((i * 7) % 5) - 2));  // small noise
    }
    return c;
}

static void test_response() {
    auto c = synth_step(10.0, 4.0, 4, 500, 200, 3000);
    auto r = analyze_response(c);
    CHECK(r.ok);
    CHECK(r.rising);
    // linear ramp: 10% at 10.4, 50% at 12, 90% at 13.6 ms
    NEAR(r.t10_ms, 10.4, 0.05);
    NEAR(r.t50_ms, 12.0, 0.05);
    NEAR(r.t90_ms, 13.6, 0.05);
    NEAR(r.rise_ms, 3.2, 0.08);
    auto f = synth_step(5.0, 2.0, 4, 500, 3000, 200);  // falling
    auto rf = analyze_response(f);
    CHECK(rf.ok && !rf.rising);
    NEAR(rf.t50_ms, 6.0, 0.05);
    auto flat = synth_step(1000, 1, 4, 500, 500, 500);
    CHECK(!analyze_response(flat).ok);
}

static void test_flicker() {
    Capture c;
    c.period_us = 8;
    for (int i = 0; i < 25000; i++) {
        double t = i * 8e-6;
        double sq = std::fmod(t * 240.0, 1.0) < 0.5 ? 1.0 : 0.0;  // 240 Hz PWM
        c.samples.push_back(uint16_t(1000 + 2000 * sq));
    }
    auto r = analyze_flicker(c);
    CHECK(r.ok && r.flicker);
    NEAR(r.freq_hz, 240, 2);
    NEAR(r.depth_pct, 50, 1);
    Capture flat;
    flat.period_us = 8;
    for (int i = 0; i < 5000; i++) flat.samples.push_back(uint16_t(2000 + (i % 3)));
    auto rf = analyze_flicker(flat);
    CHECK(rf.ok && !rf.flicker);
}

int main() {
    test_delta30();
    test_stats();
    test_parse();
    test_clock();
    test_trial();
    test_response();
    test_flicker();
    std::printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
