// Serial diagnostic: ping a (real or simulated) tester on a COM port and print round trips.
// Usage: serial_probe.exe COM7
#include <windows.h>

#include <cstdio>
#include <string>

#include "serial_win.h"

int main(int argc, char** argv) {
    std::string port = argc > 1 ? argv[1] : "COM7";
    SerialPort sp;
    if (!sp.open(port)) {
        std::printf("open %s failed\n", port.c_str());
        return 1;
    }
    for (int i = 0; i < 20; i++) {
        double t = 0;
        bool w = sp.write("PING " + std::to_string(i) + "\n", &t);
        RxLine l;
        bool r = sp.next_line(l, 500);
        std::printf("%2d write=%d read=%d open=%d rtt=%7.2f ms  '%s'  %s\n", i, w, r, sp.is_open(),
                    r ? (l.t_us - t) / 1000 : -1.0, r ? l.text.c_str() : "", sp.why_broken().c_str());
        std::fflush(stdout);
        Sleep(100);
    }
    return 0;
}
