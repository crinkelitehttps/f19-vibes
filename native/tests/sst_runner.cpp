// Runs SingleStepTests 8088 vectors (converted by tools/sst_fetch.py)
// against the interpreter in 8086 mode.
//
// Usage: sst_runner [-v] FILE.bin...
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/cpu.h"

using namespace f19;

namespace {

struct Reader {
    std::vector<uint8_t> d;
    size_t p = 0;
    uint16_t u16() { uint16_t v; std::memcpy(&v, &d[p], 2); p += 2; return v; }
    uint32_t u32() { uint32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
    uint8_t u8() { return d[p++]; }
};

const char* kRegNames[14] = {"ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"};

uint16_t& reg_ref(Regs& r, int i) {
    static const int gpr[] = {AX, BX, CX, DX};
    if (i < 4) return r.r[gpr[i]];
    if (i == 4) return r.s[CS];
    if (i == 5) return r.s[SS];
    if (i == 6) return r.s[DS];
    if (i == 7) return r.s[ES];
    if (i == 8) return r.r[SP];
    if (i == 9) return r.r[BP];
    if (i == 10) return r.r[SI];
    if (i == 11) return r.r[DI];
    if (i == 12) return r.ip;
    return r.flags;
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    int total_fail = 0, total = 0;
    Memory mem;
    Cpu cpu(mem);
    std::string err;
    cpu.on_unimplemented = [&](const std::string& what) { err = what; };

    for (int a = 1; a < argc; a++) {
        if (!std::strcmp(argv[a], "-v")) { verbose = true; continue; }
        std::ifstream f(argv[a], std::ios::binary);
        Reader r{std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {})};
        if (r.d.empty()) { std::fprintf(stderr, "%s: cannot read\n", argv[a]); return 2; }
        uint32_t count = r.u32();
        int fails = 0, shown = 0;
        for (uint32_t t = 0; t < count; t++) {
            uint16_t nlen = r.u16();
            std::string name(reinterpret_cast<char*>(&r.d[r.p]), nlen);
            r.p += nlen;
            uint16_t fmask = r.u16();
            Regs init;
            for (int i = 0; i < 14; i++) reg_ref(init, i) = r.u16();
            std::vector<std::pair<uint32_t, uint8_t>> ram0, ram1;
            for (uint32_t n = r.u32(); n--;) { uint32_t ad = r.u32(); ram0.emplace_back(ad, r.u8()); }
            uint16_t present = r.u16();
            uint16_t fin[14];
            for (auto& v : fin) v = r.u16();
            for (uint32_t n = r.u32(); n--;) { uint32_t ad = r.u32(); ram1.emplace_back(ad, r.u8()); }

            for (auto [ad, v] : ram0) mem.write8(ad, v);
            cpu.regs = init;
            cpu.halted = false;
            err.clear();
            cpu.step();

            std::string why = err;
            for (int i = 0; i < 14 && why.empty(); i++) {
                uint16_t want = (present >> i & 1) ? fin[i] : reg_ref(init, i);
                uint16_t got = reg_ref(cpu.regs, i);
                uint16_t m = i == 13 ? fmask : 0xFFFF;
                if ((want & m) != (got & m)) {
                    char b[96];
                    std::snprintf(b, sizeof b, "%s: want %04X got %04X (mask %04X)", kRegNames[i], want, got, m);
                    why = b;
                }
            }
            // If the instruction raised an interrupt (e.g. divide error), the
            // flags it pushed carry undefined-flag values from the real
            // chip's microcode; compare that stack slot under the flags mask.
            uint32_t pushed_flags = 0xFFFFFFFF;
            if (reg_ref(cpu.regs, 8) == uint16_t(reg_ref(init, 8) - 6) && reg_ref(cpu.regs, 4) != reg_ref(init, 4))
                pushed_flags = Memory::linear(cpu.regs.s[SS], uint16_t(cpu.regs.r[SP] + 4));
            for (auto [ad, v] : ram1) {
                if (!why.empty()) break;
                uint8_t m = 0xFF;
                if (ad == pushed_flags) m = uint8_t(fmask);
                else if (ad == ((pushed_flags + 1) & 0xFFFFF)) m = uint8_t(fmask >> 8);
                if ((mem.read8(ad) & m) != (v & m)) {
                    char b[96];
                    std::snprintf(b, sizeof b, "ram[%05X]: want %02X got %02X", ad, v, mem.read8(ad));
                    why = b;
                }
            }
            if (!why.empty()) {
                fails++;
                if (verbose || shown < 3) {
                    std::printf("  FAIL %s #%u '%s': %s\n", argv[a], t, name.c_str(), why.c_str());
                    shown++;
                }
            }
            for (auto [ad, v] : ram0) mem.write8(ad, 0);
            for (auto [ad, v] : ram1) mem.write8(ad, 0);
        }
        total += count;
        total_fail += fails;
        std::printf("%-40s %6u tests %6d fail\n", argv[a], count, fails);
    }
    std::printf("TOTAL %d tests, %d failures\n", total, total_fail);
    return total_fail ? 1 : 0;
}
