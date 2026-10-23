#define WIN32_LEAN_AND_MEAN
#include "serial_win.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

double qpc_us() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1e6 / double(f.QuadPart);
}

// Wine's serial driver crashes when a pending WaitCommEvent is cancelled, so under
// Wine (used for automated testing) the reader polls instead.
static bool running_under_wine() {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
}

std::vector<PortInfo> list_serial_ports() {
    std::vector<PortInfo> out;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            char vname[256];
            BYTE data[256];
            DWORD vlen = sizeof vname, dlen = sizeof data, type = 0;
            LONG r = RegEnumValueA(key, i, vname, &vlen, nullptr, &type, data, &dlen);
            if (r != ERROR_SUCCESS) break;
            if (type == REG_SZ) {
                PortInfo p;
                p.name.assign(reinterpret_cast<char*>(data), strnlen(reinterpret_cast<char*>(data), dlen));
                std::string dev = vname;
                p.friendly = p.name + (dev.find("USB") != std::string::npos || dev.find("usb") != std::string::npos
                                           ? "  (USB serial)"
                                           : "");
                out.push_back(p);
            }
        }
        RegCloseKey(key);
    }
    std::sort(out.begin(), out.end(), [](const PortInfo& a, const PortInfo& b) {
        auto num = [](const std::string& s) { return atoi(s.c_str() + (s.size() > 3 ? 3 : 0)); };
        return num(a.name) < num(b.name);
    });
    return out;
}

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string& name) {
    close();
    std::string path = "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = 115200;  // ignored by USB CDC, but never 1200 (that resets the Pico)
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;  // MicroPython only sends when DTR is set
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    SetCommState(h, &dcb);
    COMMTIMEOUTS to{};
    // ReadFile returns at once with whatever is buffered (documented special case);
    // the reader sleeps in WaitCommEvent(EV_RXCHAR) until bytes arrive, so replies
    // are timestamped the moment the driver has them.
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(h, &to);
    SetupComm(h, 1 << 16, 1 << 14);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    h_ = h;
    name_ = name;
    stop_ = false;
    broken_ = false;
    rx_ = std::thread([this] { reader(); });
    return true;
}

void SerialPort::close() {
    stop_ = true;
    if (h_) CancelIoEx(static_cast<HANDLE>(h_), nullptr);
    if (rx_.joinable()) rx_.join();
    if (h_) CloseHandle(static_cast<HANDLE>(h_));
    h_ = nullptr;
    clear_lines();
}

bool SerialPort::write(const std::string& s, double* t_send_us) {
    if (!h_) return false;
    std::lock_guard<std::mutex> g(wm_);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    if (t_send_us) *t_send_us = qpc_us();
    BOOL ok = WriteFile(static_cast<HANDLE>(h_), s.data(), DWORD(s.size()), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(static_cast<HANDLE>(h_), &ov, &written, FALSE);
        else {
            CancelIoEx(static_cast<HANDLE>(h_), &ov);
            GetOverlappedResult(static_cast<HANDLE>(h_), &ov, &written, TRUE);
            ok = FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    if (!ok) {
        why_ = "WriteFile error " + std::to_string(GetLastError());
        broken_ = true;
    }
    return ok && written == s.size();
}

void SerialPort::reader() {
    HANDLE h = static_cast<HANDLE>(h_);
    OVERLAPPED evo{}, rdo{};
    evo.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    rdo.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    SetCommMask(h, EV_RXCHAR | EV_ERR);
    std::string acc;
    char buf[4096];

    auto fail = [&](const char* what, DWORD e) {
        why_ = std::string(what) + " error " + std::to_string(e);
        broken_ = true;
    };
    // Read whatever is buffered right now (returns immediately with these timeouts).
    auto read_now = [&](DWORD& got) -> bool {
        got = 0;
        ResetEvent(rdo.hEvent);
        if (ReadFile(h, buf, sizeof buf, &got, &rdo)) return true;
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            fail("ReadFile", e);
            return false;
        }
        if (WaitForSingleObject(rdo.hEvent, 1000) != WAIT_OBJECT_0) {
            CancelIoEx(h, &rdo);
            if (WaitForSingleObject(rdo.hEvent, 1000) != WAIT_OBJECT_0) {
                fail("ReadFile stuck", 0);
                return false;
            }
            got = 0;
            return true;
        }
        if (!GetOverlappedResult(h, &rdo, &got, FALSE)) {
            e = GetLastError();
            if (e == ERROR_OPERATION_ABORTED) return true;
            fail("Read", e);
            return false;
        }
        return true;
    };
    auto process = [&](DWORD got, double t) {
        if (raw_) {
            {
                std::lock_guard<std::mutex> g(m_);
                rawbuf_.append(buf, got);
            }
            cv_.notify_all();
            return;
        }
        acc.append(buf, got);
        size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, pos);
            acc.erase(0, pos + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            if (line.size() > 2 && line[0] == 'L' && line[1] == ' ') {
                std::lock_guard<std::mutex> g(sink_m_);
                if (sink_) {
                    sink_(line);
                    continue;
                }
            }
            {
                std::lock_guard<std::mutex> g(m_);
                q_.push_back({std::move(line), t});
                if (q_.size() > 20000) q_.pop_front();
            }
            cv_.notify_all();
        }
        if (acc.size() > (1 << 20)) acc.clear();
    };

    DWORD mask = 0;
    bool wait_pending = false;
    bool poll_mode = running_under_wine();
    int idle = 0;
    while (!stop_ && poll_mode) {  // fallback: poll every millisecond
        DWORD got = 0;
        if (!read_now(got)) break;
        if (got > 0) {
            process(got, qpc_us());
            idle = 0;
            continue;
        }
        Sleep(1);
        if (++idle >= 500) {
            idle = 0;
            DWORD errs = 0;
            COMSTAT cs{};
            if (!ClearCommError(h, &errs, &cs) && !stop_) {
                fail("ClearCommError", GetLastError());
                break;
            }
        }
    }
    while (!stop_ && !poll_mode) {
        if (!wait_pending) {
            // drain everything already buffered
            DWORD got = 0;
            if (!read_now(got)) break;
            if (got > 0) {
                process(got, qpc_us());  // stamped when the bytes reached us
                continue;
            }
            // sleep until the driver signals a received character
            ResetEvent(evo.hEvent);
            mask = 0;
            if (WaitCommEvent(h, &mask, &evo)) continue;
            DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                // driver without event support: switch to polling
                poll_mode = true;
                while (!stop_) {
                    DWORD g2 = 0;
                    if (!read_now(g2)) break;
                    if (g2 > 0)
                        process(g2, qpc_us());
                    else
                        Sleep(1);
                }
                break;
            }
            wait_pending = true;
        }
        if (WaitForSingleObject(evo.hEvent, 500) == WAIT_OBJECT_0) {
            wait_pending = false;
            DWORD dummy = 0;
            if (!GetOverlappedResult(h, &evo, &dummy, FALSE)) {
                DWORD e = GetLastError();
                if (stop_) break;
                if (e == ERROR_OPERATION_ABORTED) continue;
                fail("WaitCommEvent result", e);
                break;
            }
            continue;
        }
        // quiet for 0.5 s: check the device is still there (a removed USB device errors here)
        DWORD errs = 0;
        COMSTAT cs{};
        if (!ClearCommError(h, &errs, &cs) && !stop_) {
            fail("ClearCommError", GetLastError());
            break;
        }
    }
    if (wait_pending) {  // the OS must be finished with mask/evo before they go away
        CancelIoEx(h, &evo);
        WaitForSingleObject(evo.hEvent, 1000);
    }
    CloseHandle(evo.hEvent);
    CloseHandle(rdo.hEvent);
    cv_.notify_all();
}

bool SerialPort::next_line(RxLine& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !q_.empty() || broken_; }))
        return false;
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
}

void SerialPort::clear_lines() {
    std::lock_guard<std::mutex> g(m_);
    q_.clear();
}

void SerialPort::set_stream_sink(std::function<void(const std::string&)> f) {
    std::lock_guard<std::mutex> g(sink_m_);
    sink_ = std::move(f);
}

void SerialPort::set_raw(bool on) {
    std::lock_guard<std::mutex> g(m_);
    raw_ = on;
    rawbuf_.clear();
}

bool SerialPort::read_until(const std::string& until, std::string& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    size_t pos = std::string::npos;
    bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        pos = rawbuf_.find(until);
        return pos != std::string::npos || broken_;
    });
    if (!ok || pos == std::string::npos) {
        out = rawbuf_;
        return false;
    }
    out = rawbuf_.substr(0, pos + until.size());
    rawbuf_.erase(0, pos + until.size());
    return true;
}

bool SerialPort::write_raw(const std::string& bytes) { return write(bytes, nullptr); }
