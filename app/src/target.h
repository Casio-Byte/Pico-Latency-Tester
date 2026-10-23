// Full-screen black/white flash target. Turns white the instant a mouse button
// goes down and records when the click arrived and when the frame was presented.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>

#include <atomic>
#include <mutex>

class Target {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, HINSTANCE inst);
    // Target styles. Windowed modes go through the desktop compositor, so
    // comparing them with full screen shows what windowed mode costs.
    enum Mode { kFullScreen = 0, kSmallWindow = 1, kLargeWindow = 2 };
    static const char* mode_name(int m);
    // Main thread only. placing = let the user drag the window over the sensor
    // (white, normal cursor, Enter/Esc to finish).
    void show(HWND near_window, int mode, bool placing = false);
    void hide();
    void set_white(bool white);
    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }
    bool tearing() const { return tearing_; }

    struct Click {
        int count = 0;
        double t_input_us = 0;    // mouse-down message handled
        double t_present_us = 0;  // white frame presented
    };
    Click last_click();
    std::atomic<bool> abort_requested{false};
    // last window position (screen coords) for the windowed modes; -1 = centred
    int win_x = -1, win_y = -1;

    // Messages other threads can post to hwnd():
    static constexpr UINT kMsgSetColor = WM_APP + 10;  // wParam 0/1

    LRESULT proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

private:
    void create_swapchain();
    void release_swapchain();
    void render(bool white);
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain1* sc_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    HWND hwnd_ = nullptr;
    bool tearing_ = false;
    std::atomic<bool> visible_{false};
    int w_ = 0, h_ = 0;
    int mode_ = kFullScreen;
    HWND owner_ = nullptr;
    bool placing_ = false;
    std::mutex m_;
    Click click_;
};
