// Headless runner for bring-up: boots a DOS program from the game directory,
// logs DOS/BIOS service calls, and dumps the VGA (mode 13h) screen.
//
// Usage: f19trace GAMEDIR [-p PROGRAM] [-a ARGS] [-n MILLIONS] [-t] [-s OUT.ppm] [-k KEYS]
//   -k KEYS: keys to type, one per emulated second ('\n' = Enter).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/dos.h"
#include "core/machine.h"

using namespace f19;

static void screenshot(Machine& m, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n320 200\n255\n");
    for (int i = 0; i < 64000; i++) {
        uint8_t c = m.mem.read8(0xA0000 + i);
        uint8_t rgb[3] = {uint8_t(m.dac[c][0] * 255 / 63), uint8_t(m.dac[c][1] * 255 / 63), uint8_t(m.dac[c][2] * 255 / 63)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s GAMEDIR [-p PROGRAM] [-a ARGS] [-n MILLIONS] [-t] [-s OUT.ppm] [-k KEYS]\n", argv[0]);
        return 2;
    }
    std::string program = "F19.COM", args, shot, keys;
    double millions = 50;
    bool trace = false;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "-p") && i + 1 < argc) program = argv[++i];
        else if (!std::strcmp(argv[i], "-a") && i + 1 < argc) args = argv[++i];
        else if (!std::strcmp(argv[i], "-n") && i + 1 < argc) millions = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "-t")) trace = true;
        else if (!std::strcmp(argv[i], "-s") && i + 1 < argc) shot = argv[++i];
        else if (!std::strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
    }
    Machine m(argv[1]);
    m.trace = trace;
    if (!m.dos->start_program(program, args)) {
        std::fprintf(stderr, "cannot start %s\n", program.c_str());
        return 1;
    }
    uint64_t total = uint64_t(millions * 1e6);
    uint64_t slice = uint64_t(m.ips_per_ms) * 1000;  // one emulated second
    size_t k = 0;
    while (!m.exited && m.cpu.instructions < total) {
        m.run(slice);
        if (k < keys.size()) {
            char c = keys[k++];
            if (c == '\n') m.key_press(0x1C, 0x0D);
            else if (c == ' ') m.key_press(0x39, ' ');
            else m.key_press(0, uint8_t(c));
        }
    }
    std::fprintf(stderr, "stopped after %llu instructions (%.1f s emulated): %s\n",
                 (unsigned long long)m.cpu.instructions, m.now_us() / 1e6,
                 m.stop_reason.empty() ? "budget reached" : m.stop_reason.c_str());
    std::fprintf(stderr, "CPU at %04X:%04X\n", m.cpu.regs.s[CS], m.cpu.regs.ip);
    if (!shot.empty()) screenshot(m, shot.c_str());
    return 0;
}
