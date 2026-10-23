#include "target.h"

#include "serial_win.h"  // qpc_us

static Target* g_self = nullptr;

static LRESULT CALLBACK target_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_self) return g_self->proc(h, m, wp, lp);
    return DefWindowProcW(h, m, wp, lp);
}

bool Target::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, HINSTANCE inst) {
    dev_ = dev;
    ctx_ = ctx;
    g_self = this;
    WNDCLASSW wc{};
    wc.lpfnWndProc = target_wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LatencyTesterTarget";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);  // no CS_DBLCLKS: every click is a plain button-down
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, L"LatencyTesterTarget", L"Latency Tester - target", WS_POPUP, 0, 0, 64,
                            64, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev)) &&
        SUCCEEDED(dxdev->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory5), (void**)&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            tearing_ = allow;
    }
    if (f5) f5->Release();
    if (adapter) adapter->Release();
    if (dxdev) dxdev->Release();
    return true;
}

void Target::create_swapchain() {
    release_swapchain();
    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* f2 = nullptr;
    dev_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    dxdev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&f2);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = UINT(w_);
    d.Height = UINT(h_);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(f2->CreateSwapChainForHwnd(dev_, hwnd_, &d, nullptr, nullptr, &sc_))) {
        // fall back to no tearing
        tearing_ = false;
        d.Flags = 0;
        f2->CreateSwapChainForHwnd(dev_, hwnd_, &d, nullptr, nullptr, &sc_);
    }
    if (sc_) {
        f2->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        ID3D11Texture2D* bb = nullptr;
        sc_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        if (bb) {
            dev_->CreateRenderTargetView(bb, nullptr, &rtv_);
            bb->Release();
        }
    }
    IDXGIDevice1* d1 = nullptr;
    if (SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1))) {
        d1->SetMaximumFrameLatency(1);
        d1->Release();
    }
    f2->Release();
    adapter->Release();
    dxdev->Release();
}

void Target::release_swapchain() {
    if (rtv_) rtv_->Release();
    rtv_ = nullptr;
    if (sc_) sc_->Release();
    sc_ = nullptr;
}

void Target::render(bool white) {
    if (!sc_ || !rtv_) return;
    const float black[4] = {0, 0, 0, 1};
    const float wht[4] = {1, 1, 1, 1};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, white ? wht : black);
    sc_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

const char* Target::mode_name(int m) {
    switch (m) {
        case kSmallWindow: return "small window";
        case kLargeWindow: return "large window";
        default: return "full screen";
    }
}

void Target::show(HWND near_window, int mode, bool placing) {
    mode_ = mode;
    placing_ = placing;
    owner_ = near_window;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(near_window, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT mon = mi.rcMonitor;
    if (mode == kFullScreen) {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP);
        SetWindowTextW(hwnd_, L"Latency Tester - target");
        w_ = mon.right - mon.left;
        h_ = mon.bottom - mon.top;
        SetWindowPos(hwnd_, HWND_TOPMOST, mon.left, mon.top, w_, h_, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    } else {
        // normal window with a title bar (so it can be dragged), always on top
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
        SetWindowTextW(hwnd_, placing ? L"Drag this over the sensor, then press Enter"
                                      : L"Latency Tester - target (Esc stops)");
        w_ = mode == kSmallWindow ? 480 : 960;
        h_ = mode == kSmallWindow ? 360 : 720;
        RECT r{0, 0, w_, h_};
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOPMOST);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int x = win_x, y = win_y;
        RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (x < vs.left || y < vs.top || x + ww > vs.right || y + wh > vs.bottom) {  // first time / off-screen
            x = mon.left + ((mon.right - mon.left) - ww) / 2;
            y = mon.top + ((mon.bottom - mon.top) - wh) / 2;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, ww, wh, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }
    create_swapchain();
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    abort_requested = false;
    if (!placing) {
        // Keep the pointer on the target so the Pico's clicks land here.
        RECT cr;
        GetClientRect(hwnd_, &cr);
        POINT tl{cr.left, cr.top}, br{cr.right, cr.bottom};
        ClientToScreen(hwnd_, &tl);
        ClientToScreen(hwnd_, &br);
        RECT clip{tl.x, tl.y, br.x, br.y};
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        ClipCursor(&clip);
    }
    render(placing);
    render(placing);
    visible_ = true;
}

void Target::hide() {
    ClipCursor(nullptr);
    release_swapchain();
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    if (owner_) SetForegroundWindow(owner_);
}

void Target::set_white(bool white) { render(white); }

Target::Click Target::last_click() {
    std::lock_guard<std::mutex> g(m_);
    return click_;
}

LRESULT Target::proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            if (placing_) return 0;
            double t_in = qpc_us();
            render(true);
            double t_p = qpc_us();
            std::lock_guard<std::mutex> g(m_);
            click_.count++;
            click_.t_input_us = t_in;
            click_.t_present_us = t_p;
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (!placing_) render(false);
            return 0;
        case kMsgSetColor:
            render(wp != 0);
            return 0;
        case WM_KEYDOWN:
            if (placing_ && (wp == VK_RETURN || wp == VK_ESCAPE)) {
                hide();
                return 0;
            }
            if (wp == VK_ESCAPE) abort_requested = true;
            return 0;
        case WM_MOVE:
            if (mode_ != kFullScreen && visible_) {
                RECT wr;
                GetWindowRect(h, &wr);
                win_x = wr.left;
                win_y = wr.top;
            }
            break;
        case WM_SETCURSOR:
            if (placing_ || LOWORD(lp) != HTCLIENT) break;  // normal cursor for dragging
            SetCursor(nullptr);
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            if (placing_) render(true);
            return 0;
        }
        case WM_CONTEXTMENU:
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            if (placing_) {
                hide();
                return 0;
            }
            abort_requested = true;
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
