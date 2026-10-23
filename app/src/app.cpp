// Latency Tester - Windows app for the Pico click-to-photon latency tester.
// Dear ImGui + ImPlot on Direct3D 11.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core.h"
#include "device.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"
#include "installer.h"
#include "target.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace lt;

static const char* kAppVersion = "2.0";

// ---------------------------------------------------------------- D3D11
static ID3D11Device* g_d3d = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_sc = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resize_w = 0, g_resize_h = 0;
static HWND g_hwnd = nullptr;

static void create_rtv() {
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (bb) {
        g_d3d->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}
static void release_rtv() {
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
}
static bool create_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_sc, &g_d3d, &fl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_sc, &g_d3d, &fl, &g_ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

// ---------------------------------------------------------------- state
static Device g_dev;
static Target g_target;

static std::mutex g_m;  // guards everything in this block
static std::string g_status = "Starting...";
static float g_progress = -1.f;
static std::deque<std::string> g_log;
static Run g_run;
static bool g_run_active = false;
static std::vector<Run> g_history;
static std::vector<bool> g_hist_sel;
static Capture g_cap;
static int g_cap_kind = 0;  // 1 response, 2 flicker
static ResponseResult g_resp;
static FlickerResult g_flick;
static std::string g_cap_label;
struct SensorCheck {
    bool valid = false;
    int dark = 0, bright = 0, noise = 0;
    std::string verdict;
    int quality = 0;  // 0 bad, 1 weak, 2 good
};
static SensorCheck g_check;
static bool g_connected = false;
static Hello g_info;
static std::string g_port;
static std::string g_last_port;
static std::string g_fw_warning;

static std::mutex g_live_m;
static std::vector<float> g_live;  // last few seconds of the live stream
static std::atomic<bool> g_live_on{false};

static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_cancel{false};
static std::atomic<int> g_countdown{0};
static std::string g_job_name;
static std::thread g_job;
static std::wstring g_results_dir;
static int g_target_mode = Target::kFullScreen;

constexpr UINT WM_APP_TARGET = WM_APP + 1;
constexpr UINT WM_APP_JOBDONE = WM_APP + 2;

// ---------------------------------------------------------------- helpers
static std::string now_str(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[64];
    std::strftime(b, sizeof b, fmt, &tm);
    return b;
}

static void log(const std::string& s) {
    std::lock_guard<std::mutex> g(g_m);
    g_log.push_back("[" + now_str("%H:%M:%S") + "] " + s);
    if (g_log.size() > 400) g_log.pop_front();
}
static void status(const std::string& s, float progress = -1.f) {
    std::lock_guard<std::mutex> g(g_m);
    g_status = s;
    g_progress = progress;
}
static void fail(const std::string& s) {
    status(s);
    log(s);
}

static std::string w2u(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring u2w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

static void init_results_dir() {
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        g_results_dir = std::wstring(docs) + L"\\LatencyTester";
        CoTaskMemFree(docs);
    } else {
        wchar_t buf[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, buf);
        g_results_dir = std::wstring(buf) + L"\\LatencyTester";
    }
    CreateDirectoryW(g_results_dir.c_str(), nullptr);
}

static std::string save_text(const std::string& base, const std::string& text) {
    std::wstring path = g_results_dir + L"\\" + u2w(base);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return {};
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return w2u(path);
}

// ---- small settings file (Documents\LatencyTester\settings.ini)
static void save_settings() {
    char b[160];
    std::snprintf(b, sizeof b, "target_mode=%d\r\nwin_x=%d\r\nwin_y=%d\r\n", g_target_mode, g_target.win_x,
                  g_target.win_y);
    std::wstring path = g_results_dir + L"\\settings.ini";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fputs(b, f);
    fclose(f);
}
static void load_settings() {
    std::wstring path = g_results_dir + L"\\settings.ini";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (std::sscanf(line, "target_mode=%d", &v) == 1 && v >= 0 && v <= 2) g_target_mode = v;
        if (std::sscanf(line, "win_x=%d", &v) == 1) g_target.win_x = v;
        if (std::sscanf(line, "win_y=%d", &v) == 1) g_target.win_y = v;
    }
    fclose(f);
}

static void open_results_folder() { ShellExecuteW(nullptr, L"open", g_results_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

static bool sleep_cancel(int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (g_cancel || g_target.abort_requested) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !(g_cancel || g_target.abort_requested);
}

static void refresh_conn() {
    std::lock_guard<std::mutex> g(g_m);
    g_connected = g_dev.connected();
    if (g_connected) {
        g_info = g_dev.info;
        g_port = g_dev.port();
        g_last_port = g_port;
    }
}

static void start_job(const std::string& name, std::function<void()> fn) {
    if (g_busy) return;
    if (g_job.joinable()) g_job.join();
    g_busy = true;
    g_cancel = false;
    g_job_name = name;
    g_job = std::thread([fn] {
        fn();
        refresh_conn();
        g_countdown = 0;
        g_busy = false;
        PostMessageW(g_hwnd, WM_APP_JOBDONE, 0, 0);
    });
}

static bool ensure_connected() {
    if (g_dev.connected()) return true;
    status("Looking for the Pico...");
    std::string lp;
    {
        std::lock_guard<std::mutex> g(g_m);
        lp = g_last_port;
    }
    bool ok = (!lp.empty() && g_dev.connect(lp)) || g_dev.auto_connect();
    refresh_conn();
    if (!ok) {
        fail("Not connected: " + g_dev.last_error);
        return false;
    }
    log("Connected on " + g_dev.port());
    return true;
}

// ---- target control from the worker thread (the window lives on the UI thread)
static bool target_open() {
    PostMessageW(g_hwnd, WM_APP_TARGET, 1, 0);
    for (int i = 0; i < 300 && !g_target.visible(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!g_target.visible()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    return true;
}
static void target_close() {
    PostMessageW(g_hwnd, WM_APP_TARGET, 0, 0);
    for (int i = 0; i < 300 && g_target.visible(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
static void target_white(bool w) { PostMessageW(g_target.hwnd(), Target::kMsgSetColor, w ? 1 : 0, 0); }

// ---------------------------------------------------------------- settings
struct TestCfg {
    int trials = 100;
    int warmup = 3;
    int gap_min = 250, gap_max = 600;
    int hold = 60;
    int timeout = 500;
    int threshold = 50;  // % of black->white step (target)
    int button = 1;      // 1 left, 2 right
    int countdown = 5;   // external
    int dir = 0;         // external: 0 either, 1 brighter, -1 darker
    int delta = 0;       // external: 0 = auto
};
static TestCfg g_cfg_target;
static TestCfg g_cfg_ext = [] {
    TestCfg c;
    c.trials = 50;
    c.gap_min = 400;
    c.gap_max = 900;
    c.hold = 40;
    c.warmup = 1;
    return c;
}();
static int g_cap_source = 0;  // 0 built-in target, 1 screen as-is
static int g_cap_window = 2;  // index into kWindows
static const int kWindowsMs[] = {20, 40, 80, 160};

// ---------------------------------------------------------------- jobs
static void calibrate_target(SensorCheck& sc) {
    Level a{}, b{};
    target_white(false);
    sleep_cancel(400);
    g_dev.level(a);
    target_white(true);
    sleep_cancel(400);
    g_dev.level(b);
    target_white(false);
    sleep_cancel(300);
    sc.valid = true;
    sc.dark = a.avg;
    sc.bright = b.avg;
    sc.noise = std::max(a.max - a.min, b.max - b.min);
    int step = sc.bright - sc.dark;
    if (step < 80) {
        sc.quality = 0;
        sc.verdict = "Too little change - put the sensor flat on the screen, facing it";
    } else if (step < 400 || sc.noise * 4 > step) {
        sc.quality = 1;
        sc.verdict = "Usable but weak - shade it from room light / press it flatter";
    } else {
        sc.quality = 2;
        sc.verdict = "Good";
    }
}

static void begin_run(const std::string& mode, const TestCfg& c, int delta, int dir, const SensorCheck* sc) {
    Run r;
    r.settings.mode = mode;
    r.settings.trials = c.trials;
    r.settings.gap_min_ms = c.gap_min;
    r.settings.gap_max_ms = c.gap_max;
    r.settings.hold_ms = c.hold;
    r.settings.timeout_ms = c.timeout;
    r.settings.threshold_pct = c.threshold;
    r.settings.delta = delta;
    r.settings.dir = dir;
    r.settings.button = c.button;
    if (sc) {
        r.settings.dark = sc->dark;
        r.settings.bright = sc->bright;
    }
    r.started = now_str("%Y-%m-%d %H:%M:%S");
    if (mode == "target") {
        r.settings.target_style = Target::mode_name(g_target_mode);
        r.name = std::string(g_target_mode == Target::kFullScreen ? "Full screen " : "Windowed ") +
                 now_str("%H:%M:%S");
    } else {
        r.name = "Game/external " + now_str("%H:%M:%S");
    }
    std::lock_guard<std::mutex> g(g_m);
    g_run = r;
    g_run_active = true;
}

static void end_run(bool stopped_early = false) {
    Run copy;
    {
        std::lock_guard<std::mutex> g(g_m);
        g_run_active = false;
        copy = g_run;
    }
    if (copy.trials.empty()) return;
    auto csv = run_to_csv(copy);
    copy.csv_path = save_text(now_str("%Y%m%d_%H%M%S_") + copy.settings.mode + ".csv", csv);
    auto s = summarize(copy);
    char b[200];
    std::snprintf(b, sizeof b, "%s: median %.2f ms, mean %.2f ms over %d clicks (%d failed)",
                  stopped_early ? "Stopped early" : "Done", s.e2e.median, s.e2e.mean, s.ok, s.failed);
    status(b);
    log(std::string(b) + (copy.csv_path.empty() ? "" : " - saved " + copy.csv_path));
    std::lock_guard<std::mutex> g(g_m);
    g_run.csv_path = copy.csv_path;
    g_history.push_back(copy);
    g_hist_sel.push_back(false);
}

static void push_trial(const Trial& t, int done, int total) {
    char b[160];
    if (t.status == Status::Ok)
        std::snprintf(b, sizeof b, "Click %d / %d:  %.2f ms", done, total, t.e2e_ms);
    else
        std::snprintf(b, sizeof b, "Click %d / %d:  no change seen (timeout)", done, total);
    std::lock_guard<std::mutex> g(g_m);
    g_run.trials.push_back(t);
    g_status = b;
    g_progress = float(done) / float(total);
}

static std::string cfg_string(int dir, int delta, const TestCfg& c) {
    char b[160];
    std::snprintf(b, sizeof b, "dir=%d delta=%d timeout=%d hold=%d confirm=3 button=%d", dir, delta,
                  c.timeout * 1000, c.hold, c.button);
    return b;
}

static void job_target_test(TestCfg c) {
    if (!ensure_connected()) return;
    status(std::string("Opening the target (") + Target::mode_name(g_target_mode) + ")...");
    if (!target_open()) {
        fail("Could not open the target window");
        return;
    }
    status("Checking the sensor...");
    SensorCheck sc;
    calibrate_target(sc);
    {
        std::lock_guard<std::mutex> g(g_m);
        g_check = sc;
    }
    if (sc.quality == 0) {
        target_close();
        char b[200];
        std::snprintf(b, sizeof b, "Sensor sees too little change (black %d, white %d). Put it flat on the screen.",
                      sc.dark, sc.bright);
        fail(b);
        return;
    }
    int delta = std::max(20, (sc.bright - sc.dark) * c.threshold / 100);
    if (!g_dev.cfg(cfg_string(1, delta, c))) {
        target_close();
        fail(g_dev.last_error);
        return;
    }
    begin_run("target", c, delta, 1, &sc);
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> gap(c.gap_min, std::max(c.gap_min, c.gap_max));
    int total = c.warmup + c.trials, fails_in_row = 0;
    for (int i = 0; i < total; i++) {
        if (!sleep_cancel(gap(rng))) break;
        ClockMap a = g_dev.sync(6);
        int before = g_target.last_click().count;
        TrialRaw raw;
        if (!g_dev.trial(raw, c.timeout)) {
            log("Click failed: " + g_dev.last_error);
            if (!g_dev.connected() || ++fails_in_row >= 5) break;
            continue;
        }
        Target::Click ck;
        for (int k = 0; k < 40; k++) {
            ck = g_target.last_click();
            if (ck.count > before) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ClockMap b = g_dev.sync(4);
        ClockMap clk = merge_clock(a, b);
        bool got = ck.count > before;
        if (i < c.warmup) {
            status("Warm-up click " + std::to_string(i + 1));
            continue;
        }
        Trial t = make_trial(i - c.warmup + 1, raw, &clk, got ? ck.t_input_us : 0, got ? ck.t_present_us : 0);
        if (!got && t.status == Status::Ok)
            log("Click " + std::to_string(t.index) + ": the target window did not receive the click");
        fails_in_row = t.status == Status::Ok ? 0 : fails_in_row + 1;
        push_trial(t, i - c.warmup + 1, c.trials);
        if (fails_in_row >= 5) {
            log("Stopped: 5 clicks in a row with no change seen");
            break;
        }
    }
    target_close();
    bool stopped = g_cancel || g_target.abort_requested;
    if (g_target.abort_requested) log("Stopped with Esc");
    end_run(stopped);
}

static void job_place_window() {
    PostMessageW(g_hwnd, WM_APP_TARGET, 2, 0);
    for (int i = 0; i < 300 && !g_target.visible(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    status("Drag the white window over the sensor, then press Enter in it");
    while (g_target.visible() && !g_cancel) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (g_target.visible()) target_close();
    save_settings();
    status("Target window position saved");
}

static bool countdown(int secs, const std::string& what) {
    for (int s = secs; s > 0; s--) {
        g_countdown = s;
        status(what + " - starting in " + std::to_string(s) + "...");
        g_dev.led(true);
        if (!sleep_cancel(120)) return false;
        g_dev.led(false);
        if (!sleep_cancel(880)) return false;
    }
    g_countdown = 0;
    return true;
}

static void blink_done() {
    for (int i = 0; i < 3; i++) {
        g_dev.led(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        g_dev.led(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
}

static void job_external_test(TestCfg c) {
    if (!ensure_connected()) return;
    if (!countdown(c.countdown, "Switch to your game")) {
        status("Cancelled");
        return;
    }
    Level l{};
    if (!g_dev.level(l)) {
        fail(g_dev.last_error);
        return;
    }
    int noise = l.max - l.min;
    int delta = c.delta > 0 ? c.delta : std::max(25, noise * 3 + 15);
    log("External test: sensor " + std::to_string(l.avg) + ", noise " + std::to_string(noise) + ", trigger at +/-" +
        std::to_string(delta));
    if (!g_dev.cfg(cfg_string(c.dir, delta, c))) {
        fail(g_dev.last_error);
        return;
    }
    SensorCheck sc;
    sc.dark = l.avg;
    begin_run("external", c, delta, c.dir, &sc);
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> gap(c.gap_min, std::max(c.gap_min, c.gap_max));
    int total = c.warmup + c.trials, fails_in_row = 0;
    for (int i = 0; i < total; i++) {
        if (!sleep_cancel(gap(rng))) break;
        TrialRaw raw;
        if (!g_dev.trial(raw, c.timeout)) {
            log("Click failed: " + g_dev.last_error);
            if (!g_dev.connected() || ++fails_in_row >= 5) break;
            continue;
        }
        if (i < c.warmup) continue;
        Trial t = make_trial(i - c.warmup + 1, raw, nullptr, 0, 0);
        fails_in_row = t.status == Status::Ok ? 0 : fails_in_row + 1;
        push_trial(t, i - c.warmup + 1, c.trials);
        if (fails_in_row >= 8) {
            log("Stopped: 8 clicks in a row with no change seen. Does the click make the screen change?");
            break;
        }
    }
    blink_done();
    end_run(g_cancel);
}

static void job_sensor_check() {
    if (!ensure_connected()) return;
    status("Checking the sensor on the built-in target...");
    if (!target_open()) {
        fail("Could not open the target window");
        return;
    }
    SensorCheck sc;
    calibrate_target(sc);
    target_close();
    {
        std::lock_guard<std::mutex> g(g_m);
        g_check = sc;
    }
    status("Sensor check: " + sc.verdict);
}

static void job_capture(bool flicker, bool use_target, int window_ms) {
    if (!ensure_connected()) return;
    Capture c;
    bool ok;
    if (use_target) {
        status("Opening the target...");
        if (!target_open()) {
            fail("Could not open the target window");
            return;
        }
    } else if (!countdown(3, flicker ? "Point the sensor at the screen" : "Switch to the window to test")) {
        status("Cancelled");
        return;
    }
    if (flicker) {
        if (use_target) {
            target_white(true);
            sleep_cancel(500);
        }
        status("Capturing light output (200 ms)...");
        ok = g_dev.capture(25000, 8, 0, false, c);
        if (use_target) target_white(false);
    } else {
        int n = 20000;
        int period = std::max(2, window_ms * 1000 / n);
        n = std::min(30000, window_ms * 1000 / period);
        if (use_target) {
            target_white(false);
            sleep_cancel(300);
        }
        status("Capturing the click response...");
        ok = g_dev.capture(n, period, n / 40, true, c);
    }
    if (use_target) target_close();
    if (!ok) {
        fail("Capture failed: " + g_dev.last_error);
        return;
    }
    std::lock_guard<std::mutex> g(g_m);
    g_cap = c;
    g_cap_kind = flicker ? 2 : 1;
    g_cap_label = std::string(flicker ? "Flicker" : "Response") +
                  (use_target ? std::string(" (") + Target::mode_name(g_target_mode) + ") " : " (your screen) ") +
                  now_str("%H:%M:%S");
    if (flicker) {
        g_flick = analyze_flicker(c);
        g_status = g_flick.message;
    } else {
        g_resp = analyze_response(c);
        char b[200];
        if (g_resp.ok)
            std::snprintf(b, sizeof b, "Response: 50%% at %.2f ms, rise 10-90%% %.2f ms", g_resp.t50_ms, g_resp.rise_ms);
        else
            std::snprintf(b, sizeof b, "%s", g_resp.message.c_str());
        g_status = b;
    }
    g_progress = -1;
}

static void job_live() {
    if (!ensure_connected()) return;
    {
        std::lock_guard<std::mutex> g(g_live_m);
        g_live.clear();
    }
    g_live_on = true;
    status("Live sensor view running");
    bool ok = g_dev.stream_start(500, [](const std::string& line) {
        std::vector<int> v;
        if (!parse_stream(line, v)) return;
        std::lock_guard<std::mutex> g(g_live_m);
        for (int x : v) g_live.push_back(float(x));
        if (g_live.size() > 2500) g_live.erase(g_live.begin(), g_live.begin() + (g_live.size() - 2500));
    });
    if (ok)
        while (!g_cancel && g_dev.connected()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_dev.stream_stop();
    g_live_on = false;
    status("Live view stopped");
}

static void job_connect(std::string port) {
    status("Connecting...");
    bool ok = port.empty() ? g_dev.auto_connect() : g_dev.connect(port);
    refresh_conn();
    if (!ok) {
        fail("Not connected: " + g_dev.last_error);
        return;
    }
    std::string msg = "Connected on " + g_dev.port() + ": " + g_dev.info.board + ", firmware " + g_dev.info.version;
    status(msg);
    log(msg);
    std::lock_guard<std::mutex> g(g_m);
    g_fw_warning = g_dev.info.version != bundled_firmware_version()
                       ? std::string("Pico firmware ") + g_dev.info.version + " differs from this app's " +
                             bundled_firmware_version() + " - use Install / update firmware."
                       : "";
}

static void job_install() {
    std::string port;
    if (g_dev.connected()) {
        port = g_dev.port();
        status("Stopping the tester program on " + port + "...");
        g_dev.exit_to_repl();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    refresh_conn();
    std::string err;
    bool ok = install_firmware(port, [](const std::string& s, float p) { status(s, p); }, err);
    if (!ok) {
        fail("Install failed: " + err);
        return;
    }
    log("Firmware " + std::string(bundled_firmware_version()) + " installed. Waiting for the Pico to restart...");
    status("Waiting for the Pico to restart...");
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    for (int i = 0; i < 10 && !g_cancel; i++) {
        if ((!port.empty() && g_dev.connect(port)) || g_dev.auto_connect()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    refresh_conn();
    if (g_dev.connected()) {
        status("Firmware installed - connected on " + g_dev.port());
        log("Connected after install: firmware " + g_dev.info.version);
        std::lock_guard<std::mutex> g(g_m);
        g_fw_warning.clear();
    } else {
        fail("Installed, but the Pico did not reconnect - unplug it and plug it back in, then press Connect.");
    }
}

static void job_exit_repl() {
    if (!g_dev.connected()) return;
    g_dev.exit_to_repl();
    refresh_conn();
    {
        std::lock_guard<std::mutex> g(g_m);
        g_last_port.clear();  // don't auto-reconnect: Thonny needs the port
    }
    status("The Pico is now at the MicroPython prompt - Thonny can connect. Replug it to restart the tester.");
    log("Exited to REPL");
}

// ---------------------------------------------------------------- UI helpers
static ImFont* g_font = nullptr;
static ImFont* g_font_big = nullptr;
static ImFont* g_font_huge = nullptr;
static float g_dpi = 1.0f;

static const ImVec4 kAccent(0.20f, 0.65f, 1.00f, 1.0f);
static const ImVec4 kGood(0.30f, 0.85f, 0.45f, 1.0f);
static const ImVec4 kWarn(1.00f, 0.75f, 0.20f, 1.0f);
static const ImVec4 kBad(1.00f, 0.35f, 0.35f, 1.0f);
static const ImVec4 kInputCol(0.35f, 0.60f, 1.00f, 1.0f);
static const ImVec4 kRenderCol(0.85f, 0.55f, 1.00f, 1.0f);
static const ImVec4 kDisplayCol(1.00f, 0.70f, 0.25f, 1.0f);

static void help_marker(const char* t) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void card(const char* label, const char* value, const ImVec4* col = nullptr, float w = 0) {
    if (w <= 0) w = 120 * g_dpi;
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImGui::PushFont(g_font_big);
    if (col)
        ImGui::TextColored(*col, "%s", value);
    else
        ImGui::TextUnformatted(value);
    ImGui::PopFont();
    ImGui::EndGroup();
    float used = ImGui::GetItemRectSize().x;
    ImGui::SameLine(0, std::max(16.f * g_dpi, w - used));
}

static bool big_button(const char* label, bool enabled, const ImVec4& col = kAccent, bool room_for_help = false) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x * 0.55f, col.y * 0.55f, col.z * 0.55f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 0.7f, col.y * 0.7f, col.z * 0.7f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    float w = room_for_help ? -(ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x + 2) : -1;
    bool r = ImGui::Button(label, ImVec2(w, 40 * g_dpi));
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    return r;
}

static void get_display_run(const char* mode, Run& out, bool& has) {
    std::lock_guard<std::mutex> g(g_m);
    has = false;
    if (g_run.settings.mode == mode && (g_run_active || !g_run.trials.empty())) {
        out = g_run;
        has = true;
        return;
    }
    for (auto it = g_history.rbegin(); it != g_history.rend(); ++it)
        if (it->settings.mode == mode) {
            out = *it;
            has = true;
            return;
        }
}

static void draw_results(const Run& run) {
    auto s = summarize(run);
    std::vector<double> v = ok_values(run);
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(run.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", run.started.c_str());
    if (!run.csv_path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open results folder")) open_results_folder();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", run.csv_path.c_str());
    }
    if (s.ok == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled(s.failed ? "No successful clicks yet - the sensor did not see the screen change."
                                     : "Waiting for the first click...");
        return;
    }
    char b[64];
    ImGui::Spacing();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Median");
    ImGui::PushFont(g_font_huge);
    ImGui::TextColored(kAccent, "%.2f", s.e2e.median);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("ms");
    ImGui::EndGroup();
    ImGui::SameLine(0, 32 * g_dpi);
    ImGui::BeginGroup();
    std::snprintf(b, sizeof b, "%.2f", s.e2e.mean);
    card("Mean", b);
    std::snprintf(b, sizeof b, "%.2f", s.e2e.p95);
    card("95th pct", b);
    std::snprintf(b, sizeof b, "%.2f", s.e2e.min);
    card("Min", b, &kGood);
    std::snprintf(b, sizeof b, "%.2f", s.e2e.max);
    card("Max", b, &kWarn);
    ImGui::NewLine();
    std::snprintf(b, sizeof b, "%.2f", s.e2e.sd);
    card("Std dev", b);
    std::snprintf(b, sizeof b, "%.2f", s.e2e.p99);
    card("99th pct", b);
    std::snprintf(b, sizeof b, "%d", s.ok);
    card("Clicks", b);
    std::snprintf(b, sizeof b, "%d", s.failed);
    card("Failed", b, s.failed ? &kBad : nullptr);
    ImGui::NewLine();
    ImGui::EndGroup();

    if (s.has_breakdown) {
        ImGui::SeparatorText("Where the time goes (mean)");
        ImGui::TextColored(kInputCol, "USB + Windows input %.2f ms", s.input_ms);
        ImGui::SameLine(0, 24 * g_dpi);
        ImGui::TextColored(kRenderCol, "App render %.2f ms", s.render_ms);
        ImGui::SameLine(0, 24 * g_dpi);
        ImGui::TextColored(kDisplayCol, "Display (compositor + scan-out + pixels) %.2f ms", s.display_ms);
        help_marker(
            "USB + Windows input: from the Pico sending the click to this app receiving the mouse message.\n"
            "App render: from receiving it to presenting the white frame.\n"
            "Display: from presenting the frame to the sensor seeing the screen change - the compositor, "
            "waiting for the next scan-out, and the panel's pixel response.\n\n"
            "The split relies on matching the Pico's clock to the PC's over USB, so each part is accurate to "
            "about +/- half a millisecond. The total is exact (timed by the Pico alone).");
        if (s.input_ms < 0 || s.render_ms < 0 || s.display_ms < 0)
            ImGui::TextDisabled("A part below zero is within the +/- 0.5 ms clock-matching uncertainty - treat it as ~0.");
        if (ImPlot::BeginPlot("##breakdown", ImVec2(-1, 70 * g_dpi),
                              ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoGridLines, ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, std::max(1.0, s.input_ms + s.render_ms + s.display_ms) * 1.05,
                                    ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_East, ImPlotLegendFlags_Outside);
            // stacked bar, drawn as cumulative bars back-to-front
            double in = std::max(0.0, s.input_ms), re = std::max(0.0, s.render_ms), di = std::max(0.0, s.display_ms);
            double cum3[1] = {in + re + di}, cum2[1] = {in + re}, cum1[1] = {in};
            ImPlot::SetNextFillStyle(kDisplayCol);
            ImPlot::PlotBars("Display", cum3, 1, 0.6, 0, ImPlotBarsFlags_Horizontal);
            ImPlot::SetNextFillStyle(kRenderCol);
            ImPlot::PlotBars("Render", cum2, 1, 0.6, 0, ImPlotBarsFlags_Horizontal);
            ImPlot::SetNextFillStyle(kInputCol);
            ImPlot::PlotBars("Input", cum1, 1, 0.6, 0, ImPlotBarsFlags_Horizontal);
            ImPlot::EndPlot();
        }
    }

    if (!s.has_breakdown && run.settings.mode == "target") {
        double best = 1e9;
        for (auto& t : run.trials)
            if (t.sync_err_ms > 0) best = std::min(best, t.sync_err_ms * 2);
        ImGui::Spacing();
        if (best < 1e9)
            ImGui::TextDisabled("Time breakdown unavailable: the USB round trip to the Pico was too irregular to "
                                "split the time (best %.1f ms, needs under %.0f ms). The total above is still exact.",
                                best, kMaxSyncRttUs / 1000);
    }
    ImGui::SeparatorText("Distribution");
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    float h = std::max(180.f * g_dpi, ImGui::GetContentRegionAvail().y - 8 * g_dpi);
    if (ImPlot::BeginPlot("Histogram", ImVec2(half, h), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Latency (ms)", "Clicks", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        int bins = std::max(8, std::min(60, int(std::sqrt(double(v.size())) * 2)));
        ImPlot::SetNextFillStyle(kAccent, 0.75f);
        ImPlot::PlotHistogram("Latency", v.data(), int(v.size()), bins);
        double med[1] = {s.e2e.median};
        ImPlot::SetNextLineStyle(kWarn, 2);
        ImPlot::PlotInfLines("Median", med, 1);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    if (ImPlot::BeginPlot("Every click", ImVec2(half, h))) {
        ImPlot::SetupAxes("Click #", "Latency (ms)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        std::vector<double> xs, ys, fx, bx, in_y, re_y;
        for (auto& t : run.trials) {
            if (t.status == Status::Ok) {
                xs.push_back(t.index);
                ys.push_back(t.e2e_ms);
                if (t.has_breakdown) {
                    bx.push_back(t.index);
                    in_y.push_back(t.input_ms);
                    re_y.push_back(t.input_ms + t.render_ms);
                }
            } else {
                fx.push_back(t.index);
            }
        }
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.5f * g_dpi, kAccent, 0);
        ImPlot::SetNextLineStyle(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f));
        ImPlot::PlotLine("End-to-end", xs.data(), ys.data(), int(xs.size()));
        if (!bx.empty()) {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 2 * g_dpi, kInputCol, 0);
            ImPlot::SetNextLineStyle(ImVec4(kInputCol.x, kInputCol.y, kInputCol.z, 0.5f), 1);
            ImPlot::PlotLine("Input part", bx.data(), in_y.data(), int(bx.size()));
            ImPlot::SetNextLineStyle(ImVec4(kRenderCol.x, kRenderCol.y, kRenderCol.z, 0.5f), 1);
            ImPlot::PlotLine("Input + render", bx.data(), re_y.data(), int(bx.size()));
        }
        if (!fx.empty()) {
            std::vector<double> fy(fx.size(), s.e2e.min);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Cross, 5 * g_dpi, kBad, 2, kBad);
            ImPlot::PlotScatter("Failed", fx.data(), fy.data(), int(fx.size()));
        }
        ImPlot::EndPlot();
    }
}

static void settings_common(TestCfg& c, bool external) {
    ImGui::PushItemWidth(150 * g_dpi);
    ImGui::InputInt("Clicks", &c.trials, 10, 50);
    c.trials = std::max(1, std::min(100000, c.trials));
    ImGui::InputInt("Warm-up clicks", &c.warmup, 1, 1);
    c.warmup = std::max(0, std::min(50, c.warmup));
    help_marker("Clicks done first and not counted, to wake up the GPU, USB and power saving.");
    ImGui::DragIntRange2("Gap (ms)", &c.gap_min, &c.gap_max, 5, 100, 5000);
    help_marker("Random pause between clicks, so clicks don't line up with the display's refresh cycle.");
    ImGui::SliderInt("Hold (ms)", &c.hold, 10, 500);
    help_marker("How long the mouse button is held down for each click.");
    ImGui::SliderInt("Timeout (ms)", &c.timeout, 50, 2000);
    const char* buttons[] = {"Left", "Right"};
    int bi = c.button - 1;
    ImGui::Combo("Mouse button", &bi, buttons, 2);
    c.button = bi + 1;
    if (!external) {
        ImGui::SliderInt("Trigger level (%)", &c.threshold, 5, 95);
        help_marker("The click counts as 'seen' when the brightness passes this percentage of the way from black "
                    "to white. 50% is standard; lower catches the very first change.");
    } else {
        ImGui::SliderInt("Countdown (s)", &c.countdown, 1, 30);
        const char* dirs[] = {"Brighter or darker", "Brighter", "Darker"};
        int di = c.dir == 0 ? 0 : (c.dir > 0 ? 1 : 2);
        ImGui::Combo("Detect", &di, dirs, 3);
        c.dir = di == 0 ? 0 : (di == 1 ? 1 : -1);
        ImGui::InputInt("Trigger size", &c.delta, 5, 50);
        c.delta = std::max(0, std::min(3000, c.delta));
        help_marker("How far the sensor reading (0-4095) must move to count as a change. 0 = automatic "
                    "(based on the noise just before the run). Raise it if random flicker triggers it; lower "
                    "it if small muzzle flashes are missed.");
    }
    ImGui::PopItemWidth();
}

static bool can_run() { return !g_busy && g_connected; }

static void target_mode_ui() {
    const char* modes[] = {"Full screen", "Small window (480x360)", "Large window (960x720)"};
    ImGui::SetNextItemWidth(200 * g_dpi);
    if (ImGui::Combo("Target", &g_target_mode, modes, 3)) save_settings();
    help_marker("Full screen is the fastest path to the display. A window goes through the Windows desktop "
                "compositor like most apps and windowed games - run both and compare them in History to see "
                "what windowed mode costs on your PC.");
    if (g_target_mode != Target::kFullScreen) {
        ImGui::BeginDisabled(g_busy);
        if (ImGui::Button("Place window over the sensor")) start_job("Place window", [] { job_place_window(); });
        ImGui::EndDisabled();
        help_marker("Opens the target window in white so you can drag it over the sensor. Press Enter in "
                    "it when done. The position is remembered.");
    }
}

static void tab_target() {
    ImGui::BeginChild("set", ImVec2(330 * g_dpi, 0), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Click-to-photon (built-in target)");
    ImGui::TextWrapped("Measures your whole PC: USB, Windows, this app, the GPU and the monitor. The target goes "
                       "black, and turns white each time the Pico clicks.");
    ImGui::Spacing();
    settings_common(g_cfg_target, false);
    target_mode_ui();
    ImGui::Spacing();
    if (big_button("Start test", can_run())) {
        TestCfg c = g_cfg_target;
        start_job("Click-to-photon", [c] { job_target_test(c); });
    }
    ImGui::Spacing();
    ImGui::TextWrapped(g_target_mode == Target::kFullScreen
                           ? "Put the sensor flat on the monitor (anywhere - the whole screen flashes) and keep the "
                             "mouse still. Press Esc to stop early."
                           : "Put the sensor flat on the monitor and place the window over it first. Keep the mouse "
                             "still. Press Esc to stop early.");
    {
        std::lock_guard<std::mutex> g(g_m);
        if (g_check.valid) {
            ImGui::Spacing();
            ImGui::TextDisabled("Last sensor check: black %d, white %d", g_check.dark, g_check.bright);
            ImGui::TextColored(g_check.quality == 2 ? kGood : (g_check.quality == 1 ? kWarn : kBad), "%s",
                               g_check.verdict.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("res", ImVec2(0, 0), ImGuiChildFlags_Borders);
    Run r;
    bool has;
    get_display_run("target", r, has);
    if (has)
        draw_results(r);
    else
        ImGui::TextDisabled("Results appear here.");
    ImGui::EndChild();
}

static void tab_external() {
    ImGui::BeginChild("set", ImVec2(330 * g_dpi, 0), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Game / any app");
    ImGui::TextWrapped("Measures latency inside a game or any program. The Pico clicks your real mouse button, so "
                       "whatever is under the pointer gets clicked - e.g. a gun firing.");
    ImGui::Spacing();
    settings_common(g_cfg_ext, true);
    ImGui::Spacing();
    if (big_button("Start test", can_run(), kWarn)) {
        TestCfg c = g_cfg_ext;
        start_job("Game test", [c] { job_external_test(c); });
    }
    ImGui::Spacing();
    ImGui::TextWrapped("1. Put the sensor where the screen changes on a click (muzzle flash, a menu that "
                       "lights up...).\n2. Press Start, then switch to the game during the countdown. The "
                       "Pico's LED blinks each second.\n3. Results build up here; the LED blinks 3 times at "
                       "the end.");
    int cd = g_countdown;
    if (cd > 0 && g_busy) {
        ImGui::PushFont(g_font_huge);
        ImGui::TextColored(kWarn, "%d", cd);
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("res", ImVec2(0, 0), ImGuiChildFlags_Borders);
    Run r;
    bool has;
    get_display_run("external", r, has);
    if (has)
        draw_results(r);
    else
        ImGui::TextDisabled("Results appear here.");
    ImGui::EndChild();
}

static void tab_capture() {
    ImGui::BeginChild("set", ImVec2(330 * g_dpi, 0), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Response & flicker scope");
    ImGui::TextWrapped("Records the sensor at up to 500,000 samples per second to show exactly how the screen "
                       "changes.");
    ImGui::Spacing();
    ImGui::RadioButton("Built-in target (black/white)", &g_cap_source, 0);
    ImGui::RadioButton("Your screen as it is", &g_cap_source, 1);
    if (g_cap_source == 0) target_mode_ui();
    const char* wins[] = {"20 ms", "40 ms", "80 ms", "160 ms"};
    ImGui::SetNextItemWidth(150 * g_dpi);
    ImGui::Combo("Response window", &g_cap_window, wins, 4);
    help_marker("How long to record after the click. Must be longer than the click-to-photon latency.");
    ImGui::Spacing();
    if (big_button("Capture click response", can_run(), kAccent, true)) {
        bool tgt = g_cap_source == 0;
        int w = kWindowsMs[g_cap_window];
        start_job("Response capture", [tgt, w] { job_capture(false, tgt, w); });
    }
    help_marker("One click, recorded in detail: when the change starts, its midpoint, and how long the "
                "pixels take to go from 10% to 90% (the panel's response time for this transition).");
    if (big_button("Capture flicker (200 ms)", can_run(), kAccent, true)) {
        bool tgt = g_cap_source == 0;
        start_job("Flicker capture", [tgt] { job_capture(true, tgt, 0); });
    }
    help_marker("Records steady light output. Shows PWM backlight flicker frequency and depth, or confirms "
                "a flicker-free backlight.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("res", ImVec2(0, 0), ImGuiChildFlags_Borders);
    Capture c;
    int kind;
    ResponseResult rr;
    FlickerResult fr;
    std::string label;
    {
        std::lock_guard<std::mutex> g(g_m);
        c = g_cap;
        kind = g_cap_kind;
        rr = g_resp;
        fr = g_flick;
        label = g_cap_label;
    }
    if (!kind) {
        ImGui::TextDisabled("Captures appear here.");
        ImGui::EndChild();
        return;
    }
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopFont();
    char b[64];
    if (kind == 1) {
        if (rr.ok) {
            std::snprintf(b, sizeof b, "%.2f ms", rr.t10_ms);
            card("First change (10%)", b, nullptr, 170 * g_dpi);
            std::snprintf(b, sizeof b, "%.2f ms", rr.t50_ms);
            card("Midpoint (50%)", b, &kAccent, 170 * g_dpi);
            std::snprintf(b, sizeof b, "%.2f ms", rr.t90_ms);
            card("Settled (90%)", b, nullptr, 170 * g_dpi);
            std::snprintf(b, sizeof b, "%.2f ms", rr.rise_ms);
            card("Pixel response 10-90%", b, &kWarn, 190 * g_dpi);
            ImGui::NewLine();
            if (rr.overshoot_pct > 3) ImGui::TextColored(kWarn, "Overshoot %.0f%% (overdrive)", rr.overshoot_pct);
        } else {
            ImGui::TextColored(kWarn, "%s", rr.message.c_str());
        }
    } else {
        if (fr.ok) {
            std::snprintf(b, sizeof b, "%s", fr.flicker ? "Yes" : "No");
            card("Flicker", b, fr.flicker ? &kWarn : &kGood, 130 * g_dpi);
            if (fr.flicker) {
                std::snprintf(b, sizeof b, "%.0f Hz", fr.freq_hz);
                card("Frequency", b, nullptr, 150 * g_dpi);
            }
            std::snprintf(b, sizeof b, "%.1f %%", fr.depth_pct);
            card("Modulation", b, nullptr, 150 * g_dpi);
            ImGui::NewLine();
            ImGui::TextDisabled("%s", fr.message.c_str());
        } else {
            ImGui::TextColored(kWarn, "%s", fr.message.c_str());
        }
    }
    if (ImGui::Button("Save capture as CSV")) {
        std::string txt = "time_ms,level\n";
        char line[64];
        for (size_t i = 0; i < c.samples.size(); i++) {
            std::snprintf(line, sizeof line, "%.4f,%u\n", c.time_ms(i), unsigned(c.samples[i]));
            txt += line;
        }
        auto p = save_text(now_str("%Y%m%d_%H%M%S_") + (kind == 1 ? "response" : "flicker") + ".csv", txt);
        log(p.empty() ? "Could not save the capture" : "Saved " + p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open results folder")) open_results_folder();
    // plot (decimated for speed)
    size_t n = c.samples.size();
    size_t step = std::max<size_t>(1, n / 6000);
    std::vector<double> xs, ys;
    auto sm = smooth(c.samples, kind == 1 ? 5 : 1);
    for (size_t i = 0; i < n; i += step) {
        xs.push_back(c.time_ms(i));
        ys.push_back(sm[i]);
    }
    if (ImPlot::BeginPlot("##cap", ImVec2(-1, -1))) {
        ImPlot::SetupAxes(kind == 1 ? "Time after click (ms)" : "Time (ms)", "Sensor level (0-4095)",
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetNextLineStyle(kAccent, 1.5f);
        ImPlot::PlotLine("Light", xs.data(), ys.data(), int(xs.size()));
        if (kind == 1) {
            double zero[1] = {0};
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.5f), 1);
            ImPlot::PlotInfLines("Click", zero, 1);
            if (rr.ok) {
                double m[3] = {rr.t10_ms, rr.t50_ms, rr.t90_ms};
                ImPlot::SetNextLineStyle(kWarn, 1);
                ImPlot::PlotInfLines("10/50/90%", m, 3);
                ImPlot::TagX(rr.t50_ms, kWarn, "%.2f", rr.t50_ms);
            }
        } else if (fr.ok) {
            double lv[1] = {fr.mean};
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.4f), 1);
            ImPlot::PlotInfLines("Mean", lv, 1, ImPlotInfLinesFlags_Horizontal);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndChild();
}

static void tab_sensor() {
    ImGui::BeginChild("set", ImVec2(330 * g_dpi, 0), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Sensor");
    ImGui::TextWrapped("Wiring: ALS-PT19  + to 3V3 (pin 36),  - to AGND (pin 33),  OUT to GP26 (pin 31).");
    ImGui::Spacing();
    if (big_button("Sensor check", can_run(), kAccent, true)) start_job("Sensor check", [] { job_sensor_check(); });
    help_marker("Shows full-screen black, then white, and measures what the sensor sees.");
    {
        std::lock_guard<std::mutex> g(g_m);
        if (g_check.valid) {
            ImGui::Text("Black: %d   White: %d", g_check.dark, g_check.bright);
            ImGui::Text("Step: %d   Noise: %d", g_check.bright - g_check.dark, g_check.noise);
            ImGui::TextColored(g_check.quality == 2 ? kGood : (g_check.quality == 1 ? kWarn : kBad), "%s",
                               g_check.verdict.c_str());
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Live view");
    if (!g_live_on) {
        if (big_button("Start live view", can_run())) start_job("Live view", [] { job_live(); });
    } else {
        if (big_button("Stop live view", true, kBad)) g_cancel = true;
    }
    ImGui::TextWrapped("Use it to position the sensor: the reading should jump when the screen under it "
                       "changes. Higher = brighter.");
    ImGui::Spacing();
    ImGui::SeparatorText("Pico LED");
    ImGui::BeginDisabled(!can_run());
    if (ImGui::Button("LED on")) start_job("LED", [] { g_dev.led(true); });
    ImGui::SameLine();
    if (ImGui::Button("LED off")) start_job("LED", [] { g_dev.led(false); });
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("plot", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::vector<float> live;
    {
        std::lock_guard<std::mutex> g(g_live_m);
        live = g_live;
    }
    float cur = live.empty() ? 0 : live.back();
    ImGui::TextDisabled("Current reading");
    ImGui::PushFont(g_font_huge);
    ImGui::Text("%4.0f", cur);
    ImGui::PopFont();
    ImGui::ProgressBar(cur / 4095.f, ImVec2(-1, 10 * g_dpi), "");
    if (ImPlot::BeginPlot("##live", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("Seconds", "Level", 0, 0);
        ImPlot::SetupAxisLimits(ImAxis_X1, -5, 0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 4095, ImPlotCond_Once);
        std::vector<float> xs(live.size());
        for (size_t i = 0; i < live.size(); i++) xs[i] = -float(live.size() - 1 - i) / 500.f;
        ImPlot::SetNextLineStyle(kAccent, 1.5f);
        ImPlot::PlotLine("Sensor", xs.data(), live.data(), int(live.size()));
        ImPlot::EndPlot();
    }
    ImGui::EndChild();
}

static void tab_history() {
    std::vector<Run> hist;
    std::vector<bool> sel;
    {
        std::lock_guard<std::mutex> g(g_m);
        hist = g_history;
        sel = g_hist_sel;
    }
    if (hist.empty()) {
        ImGui::TextDisabled("Finished runs appear here, and are saved as CSV files in Documents\\LatencyTester.");
        if (ImGui::Button("Open results folder")) open_results_folder();
        return;
    }
    ImGui::TextDisabled("Tick runs to compare them.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open results folder")) open_results_folder();
    float table_h = std::min(ImGui::GetContentRegionAvail().y * 0.45f, (hist.size() + 1.5f) * ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("hist", 10,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(-1, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 28 * g_dpi);
        ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Started", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Clicks", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Median", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Mean", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("95th", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Std dev", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Failed", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80 * g_dpi);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < hist.size(); i++) {
            auto s = summarize(hist[i]);
            ImGui::TableNextRow();
            ImGui::PushID(int(i));
            ImGui::TableNextColumn();
            bool b = sel[i];
            if (ImGui::Checkbox("##s", &b)) {
                std::lock_guard<std::mutex> g(g_m);
                if (i < g_hist_sel.size()) g_hist_sel[i] = b;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hist[i].name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hist[i].started.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.ok);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.e2e.median);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.e2e.mean);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.e2e.p95);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.e2e.sd);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.failed);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                std::lock_guard<std::mutex> g(g_m);
                if (i < g_history.size()) {
                    g_history.erase(g_history.begin() + long(i));
                    g_hist_sel.erase(g_hist_sel.begin() + long(i));
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    std::vector<const Run*> chosen;
    for (size_t i = 0; i < hist.size(); i++)
        if (sel[i]) chosen.push_back(&hist[i]);
    if (chosen.empty()) {
        ImGui::TextDisabled("Tick two or more runs above to compare their distributions.");
        return;
    }
    ImGui::SeparatorText("Comparison");
    double lo = 1e9, hi = 0;
    std::vector<std::vector<double>> vals;
    for (auto* r : chosen) {
        vals.push_back(ok_values(*r));
        for (double x : vals.back()) {
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
    }
    double base_med = compute_stats(vals[0]).median;
    for (size_t k = 0; k < chosen.size(); k++) {
        auto st = compute_stats(vals[k]);
        ImGui::Text("%-28s median %.2f ms", chosen[k]->name.c_str(), st.median);
        if (k > 0) {
            ImGui::SameLine();
            double d = st.median - base_med;
            ImGui::TextColored(d < 0 ? kGood : kBad, "(%+.2f ms vs first)", d);
        }
    }
    if (hi <= lo) hi = lo + 1;
    if (ImPlot::BeginPlot("##cmp", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("Latency (ms)", "Relative frequency", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        for (size_t k = 0; k < chosen.size(); k++) {
            ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.45f);
            ImPlot::PlotHistogram(chosen[k]->name.c_str(), vals[k].data(), int(vals[k].size()), 40, 1.0,
                                  ImPlotRange(lo, hi), ImPlotHistogramFlags_Density);
        }
        ImPlot::EndPlot();
    }
}

static int g_tab_request = -1;

static void draw_ui() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);

    // ---- header
    bool connected;
    Hello info;
    std::string port, fw_warn;
    std::vector<std::string> logcopy;
    std::string st;
    float prog;
    {
        std::lock_guard<std::mutex> g(g_m);
        connected = g_connected;
        info = g_info;
        port = g_port;
        fw_warn = g_fw_warning;
        st = g_status;
        prog = g_progress;
    }
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted("Latency Tester");
    ImGui::PopFont();
    ImGui::SameLine(0, 24 * g_dpi);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 6 * g_dpi;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.65f), r,
                                                ImGui::GetColorU32(connected ? kGood : kBad));
    ImGui::Dummy(ImVec2(r * 2, r));
    ImGui::SameLine();
    if (connected)
        ImGui::Text("%s  -  %s, firmware %s, %d MHz", port.c_str(), info.board.c_str(), info.version.c_str(),
                    info.cpu_mhz);
    else
        ImGui::TextDisabled("Not connected");
    float bw = 0;
    const char* b1 = connected ? "Disconnect" : "Connect";
    bw = ImGui::CalcTextSize(b1).x + ImGui::CalcTextSize("Install / update firmware").x +
         ImGui::CalcTextSize("Exit to REPL").x + ImGui::GetStyle().FramePadding.x * 6 + ImGui::GetStyle().ItemSpacing.x * 3;
    ImGui::SameLine(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
    ImGui::BeginDisabled(g_busy);
    if (ImGui::Button(b1)) {
        if (connected)
            start_job("Disconnect", [] {
                g_dev.disconnect();
                {
                    std::lock_guard<std::mutex> g(g_m);
                    g_last_port.clear();  // stay disconnected until Connect is pressed
                }
                status("Disconnected");
            });
        else
            start_job("Connect", [] { job_connect(""); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Install / update firmware")) ImGui::OpenPopup("Install firmware");
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected);
    if (ImGui::Button("Exit to REPL")) ImGui::OpenPopup("Exit to REPL?");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (ImGui::BeginPopupModal("Install firmware", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Writes tester firmware %s to the Pico.", bundled_firmware_version());
        ImGui::BulletText("The Pico must already have MicroPython on it (see the README).");
        ImGui::BulletText("Close Thonny first.");
        ImGui::BulletText("Existing main.py and lib/usb on the Pico are replaced.");
        if (ImGui::Button("Install", ImVec2(140 * g_dpi, 0))) {
            start_job("Install firmware", [] { job_install(); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140 * g_dpi, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Exit to REPL?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Stops the tester program so Thonny can connect to the Pico.\n"
                               "Unplug and replug the Pico to start the tester again.");
        if (ImGui::Button("Exit to REPL", ImVec2(140 * g_dpi, 0))) {
            start_job("Exit", [] { job_exit_repl(); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140 * g_dpi, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!fw_warn.empty()) ImGui::TextColored(kWarn, "%s", fw_warn.c_str());

    // ---- tabs
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("body", ImVec2(0, -footer));
    if (ImGui::BeginTabBar("tabs")) {
        auto flag = [](int i) { return g_tab_request == i ? ImGuiTabItemFlags_SetSelected : 0; };
        if (ImGui::BeginTabItem("Click-to-photon", nullptr, flag(0))) {
            tab_target();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game / any app", nullptr, flag(1))) {
            tab_external();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Response & flicker", nullptr, flag(2))) {
            tab_capture();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sensor", nullptr, flag(3))) {
            tab_sensor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("History", nullptr, flag(4))) {
            tab_history();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log", nullptr, flag(5))) {
            std::deque<std::string> lg;
            {
                std::lock_guard<std::mutex> g(g_m);
                lg = g_log;
            }
            ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (auto& l : lg) ImGui::TextUnformatted(l.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        g_tab_request = -1;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // ---- footer
    ImGui::Separator();
    if (g_busy) {
        if (ImGui::Button("Stop", ImVec2(90 * g_dpi, 0))) g_cancel = true;
        ImGui::SameLine();
    }
    if (prog >= 0 && g_busy) {
        ImGui::ProgressBar(prog, ImVec2(220 * g_dpi, 0));
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(st.c_str());
    ImGui::TextDisabled("Latency Tester %s  |  F11: full screen  |  results: %s", kAppVersion,
                        w2u(g_results_dir).c_str());
    ImGui::End();
}

// ---------------------------------------------------------------- window
static bool g_app_fullscreen = false;
static WINDOWPLACEMENT g_prev_place{sizeof(WINDOWPLACEMENT)};

static void toggle_app_fullscreen() {
    DWORD style = DWORD(GetWindowLongPtrW(g_hwnd, GWL_STYLE));
    if (!g_app_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (GetWindowPlacement(g_hwnd, &g_prev_place) &&
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, style & ~DWORD(WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_app_fullscreen = true;
        }
    } else {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_prev_place);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_app_fullscreen = false;
    }
}

static LRESULT WINAPI wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, wp, lp)) return true;
    switch (m) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_APP_TARGET:
            if (wp == 1)
                g_target.show(g_hwnd, g_target_mode);
            else if (wp == 2)
                g_target.show(g_hwnd, g_target_mode, true);  // drag into place
            else
                g_target.hide();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_F11) {
                toggle_app_fullscreen();
                return 0;
            }
            break;
        case WM_APP_JOBDONE:
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = LONG(900 * g_dpi);
            mmi->ptMinTrackSize.y = LONG(600 * g_dpi);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void set_dpi_awareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    using Fn = BOOL(WINAPI*)(HANDLE);
    auto fn = u ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDpiAwarenessContext"))) : nullptr;
    if (fn)
        fn(reinterpret_cast<HANDLE>(-4));  // PER_MONITOR_AWARE_V2
    else
        SetProcessDPIAware();
}

static void setup_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.TabRounding = 5;
    s.PopupRounding = 6;
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(9, 7);
    s.WindowPadding = ImVec2(12, 10);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.08f, 0.095f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.105f, 0.125f, 1);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.27f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.17f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.45f, 0.75f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.38f, 0.65f, 1);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.35f, 0.58f, 0.6f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.25f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.30f, 0.40f, 1);
    s.ScaleAllSizes(g_dpi);
    ImPlot::StyleColorsDark();
    ImPlot::GetStyle().PlotPadding = ImVec2(8 * g_dpi, 8 * g_dpi);
}

static void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::string fonts = w2u(windir) + "\\Fonts\\";
    auto exists = [](const std::string& p) { return GetFileAttributesW(u2w(p).c_str()) != INVALID_FILE_ATTRIBUTES; };
    std::string reg = fonts + "segoeui.ttf", bold = fonts + "seguisb.ttf";
    if (!exists(bold)) bold = fonts + "segoeuib.ttf";
    if (exists(reg)) {
        g_font = io.Fonts->AddFontFromFileTTF(reg.c_str(), 17.f * g_dpi);
        g_font_big = io.Fonts->AddFontFromFileTTF(exists(bold) ? bold.c_str() : reg.c_str(), 22.f * g_dpi);
        g_font_huge = io.Fonts->AddFontFromFileTTF(exists(bold) ? bold.c_str() : reg.c_str(), 46.f * g_dpi);
    }
    if (!g_font || !g_font_big || !g_font_huge) {
        io.Fonts->Clear();
        ImFontConfig cfg;
        cfg.SizePixels = 15.f * g_dpi;
        g_font = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = 20.f * g_dpi;
        g_font_big = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = 40.f * g_dpi;
        g_font_huge = io.Fonts->AddFontDefault(&cfg);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    set_dpi_awareness();
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    init_results_dir();
    load_settings();

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"LatencyTesterMain";
    RegisterClassExW(&wc);
    HDC sdc = GetDC(nullptr);
    g_dpi = std::max(1.0f, float(GetDeviceCaps(sdc, LOGPIXELSX)) / 96.f);
    ReleaseDC(nullptr, sdc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Latency Tester", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           int(1280 * g_dpi), int(820 * g_dpi), nullptr, nullptr, inst, nullptr);
    if (!create_d3d(g_hwnd)) {
        MessageBoxW(nullptr, L"Could not start Direct3D 11.", L"Latency Tester", MB_ICONERROR);
        return 1;
    }
    g_target.init(g_d3d, g_ctx, inst);
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = nullptr;
    load_fonts();
    setup_style();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d3d, g_ctx);

    log("Latency Tester " + std::string(kAppVersion) + " started. Results folder: " + w2u(g_results_dir));
    // Keepalive: tells the Pico the app is there (its LED stops blinking) while idle.
    std::atomic<bool> quit{false};
    std::thread keepalive([&quit] {
        while (!quit) {
            for (int i = 0; i < 20 && !quit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!quit && !g_busy && g_dev.connected()) {
                Ping p;
                g_dev.ping(p);
            }
        }
    });
    start_job("Connect", [] { job_connect(""); });

    bool done = false;
    auto last_retry = std::chrono::steady_clock::now();
    while (!done) {
        // While the target is up, block on messages so clicks are handled the
        // instant they arrive, and don't spend any GPU time on the UI.
        if (g_target.visible()) {
            MSG msg;
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) {
                done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_target.visible()) continue;

        // reconnect quietly if the Pico went away (e.g. replugged)
        std::string lp;
        {
            std::lock_guard<std::mutex> g(g_m);
            lp = g_last_port;
        }
        if (!g_busy && !g_connected && !lp.empty() &&
            std::chrono::steady_clock::now() - last_retry > std::chrono::seconds(3)) {
            last_retry = std::chrono::steady_clock::now();
            for (auto& pi : list_serial_ports())
                if (pi.name == lp) {
                    start_job("Reconnect", [lp] {
                        if (g_dev.connect(lp)) {
                            refresh_conn();
                            log("Reconnected on " + lp);
                            status("Reconnected on " + lp);
                        }
                    });
                    break;
                }
        }
        {
            std::lock_guard<std::mutex> g(g_m);
            if (g_connected && !g_dev.connected() && !g_busy) {
                g_connected = false;
                g_status = "Pico disconnected";
            }
        }

        if (g_resize_w) {
            release_rtv();
            g_sc->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }
        // During a game test, keep the UI cheap so it doesn't disturb the game.
        if (g_busy && g_job_name == "Game test" && GetForegroundWindow() != g_hwnd) Sleep(100);
        if (IsIconic(g_hwnd)) {
            Sleep(50);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(g_font);
        draw_ui();
        ImGui::PopFont();
        ImGui::Render();
        const float clear[4] = {0.075f, 0.08f, 0.095f, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    save_settings();
    g_cancel = true;
    quit = true;
    if (keepalive.joinable()) keepalive.join();
    if (g_job.joinable()) g_job.join();
    if (g_dev.connected()) g_dev.disconnect();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    release_rtv();
    if (g_sc) g_sc->Release();
    if (g_ctx) g_ctx->Release();
    if (g_d3d) g_d3d->Release();
    DestroyWindow(g_hwnd);
    timeEndPeriod(1);
    return 0;
}
