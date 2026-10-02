#include "core/cpu.h"

#include <stdexcept>

namespace f19 {

namespace {

constexpr uint16_t kFlagsFixed = 0xF002;           // 8086: reserved bits read as 1
constexpr uint16_t kFlagsWritable = 0x0FD5;        // CF PF AF ZF SF TF IF DF OF

bool parity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

template <typename T> constexpr T msb() { return T(T(1) << (sizeof(T) * 8 - 1)); }

}  // namespace

// ---------------------------------------------------------------- fetch/stack

uint8_t Cpu::fetch8() {
    uint8_t v = mem_.read8(Memory::linear(regs.s[CS], regs.ip));
    regs.ip++;
    return v;
}

uint16_t Cpu::fetch16() {
    uint16_t lo = fetch8();
    return lo | (fetch8() << 8);
}

void Cpu::push(uint16_t v) {
    regs.r[SP] -= 2;
    mem_.write16(regs.s[SS], regs.r[SP], v);
}

uint16_t Cpu::pop() {
    uint16_t v = mem_.read16(regs.s[SS], regs.r[SP]);
    regs.r[SP] += 2;
    return v;
}

void Cpu::far_call(uint16_t seg, uint16_t off) {
    push(regs.s[CS]);
    push(regs.ip);
    regs.s[CS] = seg;
    regs.ip = off;
}

void Cpu::interrupt(uint8_t vector) {
    push(regs.flags);
    set_flag(IF, false);
    set_flag(TF, false);
    push(regs.s[CS]);
    push(regs.ip);
    regs.ip = mem_.read16(0, vector * 4);
    regs.s[CS] = mem_.read16(0, vector * 4 + 2);
}

bool Cpu::irq(uint8_t vector) {
    if (!flag(IF)) return false;
    halted = false;
    interrupt(vector);
    return true;
}

void Cpu::unimplemented(const char* what) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s at %04X:%04X", what, regs.s[CS], insn_ip_);
    if (on_unimplemented) on_unimplemented(buf);
    else throw std::runtime_error(buf);
}

// On the 8086 the divide-error return address is the next instruction.
void Cpu::divide_error() { interrupt(0); }

// ---------------------------------------------------------------- ModR/M

void Cpu::decode_modrm() {
    modrm_ = fetch8();
    int mod = modrm_ >> 6, rm = modrm_ & 7;
    if (mod == 3) {
        ea_is_reg_ = true;
        return;
    }
    ea_is_reg_ = false;
    uint16_t off = 0;
    int seg = DS;
    switch (rm) {
        case 0: off = regs.r[BX] + regs.r[SI]; break;
        case 1: off = regs.r[BX] + regs.r[DI]; break;
        case 2: off = regs.r[BP] + regs.r[SI]; seg = SS; break;
        case 3: off = regs.r[BP] + regs.r[DI]; seg = SS; break;
        case 4: off = regs.r[SI]; break;
        case 5: off = regs.r[DI]; break;
        case 6:
            if (mod == 0) off = fetch16();
            else { off = regs.r[BP]; seg = SS; }
            break;
        case 7: off = regs.r[BX]; break;
    }
    if (mod == 1) off += int8_t(fetch8());
    else if (mod == 2) off += fetch16();
    ea_seg_ = default_seg(seg);
    ea_off_ = off;
}

uint8_t Cpu::rm8() { return ea_is_reg_ ? regs.r8(modrm_ & 7) : mem_.read8(Memory::linear(ea_seg_, ea_off_)); }
uint16_t Cpu::rm16() { return ea_is_reg_ ? regs.r[modrm_ & 7] : mem_.read16(ea_seg_, ea_off_); }
void Cpu::set_rm8(uint8_t v) {
    if (ea_is_reg_) regs.r8(modrm_ & 7) = v;
    else mem_.write8(Memory::linear(ea_seg_, ea_off_), v);
}
void Cpu::set_rm16(uint16_t v) {
    if (ea_is_reg_) regs.r[modrm_ & 7] = v;
    else mem_.write16(ea_seg_, ea_off_, v);
}

// ---------------------------------------------------------------- ALU

template <typename T> void Cpu::set_szp(T v) {
    set_flag(ZF, v == 0);
    set_flag(SF, v & msb<T>());
    set_flag(PF, parity(uint8_t(v)));
}

template <typename T> T Cpu::alu(int op, T a, T b) {
    using W = uint32_t;
    T r = 0;
    W carry = flag(CF) ? 1 : 0;
    switch (op) {
        case 0:  // ADD
        case 2: {  // ADC
            W c = op == 2 ? carry : 0;
            W full = W(a) + W(b) + c;
            r = T(full);
            set_flag(CF, full >> (sizeof(T) * 8));
            set_flag(OF, (~(a ^ b) & (a ^ r)) & msb<T>());
            set_flag(AF, (a ^ b ^ r) & 0x10);
            break;
        }
        case 3:  // SBB
        case 5:  // SUB
        case 7: {  // CMP
            W c = op == 3 ? carry : 0;
            W full = W(a) - W(b) - c;
            r = T(full);
            set_flag(CF, W(b) + c > W(a));
            set_flag(OF, ((a ^ b) & (a ^ r)) & msb<T>());
            set_flag(AF, (a ^ b ^ r) & 0x10);
            break;
        }
        case 1: r = a | b; goto logic;
        case 4: r = a & b; goto logic;
        case 6: r = a ^ b;
        logic:
            set_flag(CF, false);
            set_flag(OF, false);
            set_flag(AF, false);  // undefined on 8086; observed 0
            break;
    }
    set_szp(r);
    return op == 7 ? a : r;
}

template <typename T> T Cpu::inc_dec(T v, bool dec) {
    T r = dec ? T(v - 1) : T(v + 1);
    set_flag(OF, dec ? v == msb<T>() : r == msb<T>());
    set_flag(AF, dec ? (v & 0xF) == 0 : (r & 0xF) == 0);
    set_szp(r);
    return r;
}

// Shifts and rotates, one bit at a time as the 8086 does: flags come from
// the final step, and counts are not masked (8086); 80186 masks to 5 bits.
template <typename T> T Cpu::shift(int op, T v, unsigned count) {
    constexpr unsigned bits = sizeof(T) * 8;
    if (count == 0) return v;
    for (unsigned i = 0; i < count; i++) {
        bool cf = flag(CF);
        switch (op) {
            case 0: {  // ROL
                bool out = v & msb<T>();
                v = T((v << 1) | out);
                set_flag(CF, out);
                set_flag(OF, bool(v & msb<T>()) != out);
                break;
            }
            case 1: {  // ROR
                bool out = v & 1;
                v = T((v >> 1) | (out ? msb<T>() : 0));
                set_flag(CF, out);
                set_flag(OF, bool(v & msb<T>()) != bool(v & (msb<T>() >> 1)));
                break;
            }
            case 2: {  // RCL
                bool out = v & msb<T>();
                v = T((v << 1) | cf);
                set_flag(CF, out);
                set_flag(OF, bool(v & msb<T>()) != out);
                break;
            }
            case 3: {  // RCR
                bool out = v & 1;
                set_flag(OF, bool(v & msb<T>()) != cf);
                v = T((v >> 1) | (cf ? msb<T>() : 0));
                set_flag(CF, out);
                break;
            }
            case 4:    // SHL/SAL
            case 6: {  // SETMO on 8086 behaves like SHL? No: handled below.
                if (op == 6) {
                    // Undocumented "SETMO": result all ones.
                    v = T(~T(0));
                    set_flag(CF, false);
                    set_flag(OF, false);
                    set_flag(AF, false);
                    set_szp(v);
                    break;
                }
                bool out = v & msb<T>();
                v = T(v << 1);
                set_flag(CF, out);
                set_flag(OF, bool(v & msb<T>()) != out);
                set_flag(AF, v & 0x10);
                set_szp(v);
                break;
            }
            case 5: {  // SHR
                bool out = v & 1;
                set_flag(OF, v & msb<T>());
                v = T(v >> 1);
                set_flag(CF, out);
                set_flag(AF, false);
                set_szp(v);
                break;
            }
            case 7: {  // SAR
                bool out = v & 1;
                v = T((v >> 1) | (v & msb<T>()));
                set_flag(CF, out);
                set_flag(OF, false);
                set_flag(AF, false);
                set_szp(v);
                break;
            }
        }
    }
    (void)bits;
    return v;
}

// ---------------------------------------------------------------- group 3

void Cpu::group3_8() {
    int op = (modrm_ >> 3) & 7;
    uint8_t v = rm8();
    switch (op) {
        case 0:
        case 1: alu<uint8_t>(4, v, fetch8()); break;  // TEST (1 is an alias)
        case 2: set_rm8(uint8_t(~v)); break;          // NOT
        case 3: {                                     // NEG
            uint8_t r = alu<uint8_t>(5, 0, v);
            set_rm8(r);
            break;
        }
        case 4: {  // MUL
            uint16_t r = uint16_t(regs.r8(0)) * v;
            regs.r[AX] = r;
            bool hi = r >> 8;
            set_flag(CF, hi);
            set_flag(OF, hi);
            set_szp<uint8_t>(uint8_t(r >> 8));
            set_flag(ZF, r == 0);
            break;
        }
        case 5: {  // IMUL
            int16_t r = int16_t(int8_t(regs.r8(0))) * int8_t(v);
            regs.r[AX] = uint16_t(r);
            bool ext = r != int16_t(int8_t(r));
            if (rep_) { /* 8086 quirk: REP negates the product */ }
            set_flag(CF, ext);
            set_flag(OF, ext);
            set_szp<uint8_t>(uint8_t(uint16_t(r) >> 8));
            break;
        }
        case 6: {  // DIV
            if (v == 0) return divide_error();
            uint16_t q = regs.r[AX] / v, r = regs.r[AX] % v;
            if (q > 0xFF) return divide_error();
            regs.r8(0) = uint8_t(q);
            regs.r8(4) = uint8_t(r);
            break;
        }
        case 7: {  // IDIV
            if (v == 0) return divide_error();
            int16_t a = int16_t(regs.r[AX]);
            int d = int8_t(v);
            int q = a / d, r = a % d;
            if (q > 127 || q < -127) return divide_error();
            if (rep_) q = -q;  // 8086 quirk: a REP prefix negates the quotient
            regs.r8(0) = uint8_t(q);
            regs.r8(4) = uint8_t(r);
            break;
        }
    }
}

void Cpu::group3_16() {
    int op = (modrm_ >> 3) & 7;
    uint16_t v = rm16();
    switch (op) {
        case 0:
        case 1: alu<uint16_t>(4, v, fetch16()); break;
        case 2: set_rm16(uint16_t(~v)); break;
        case 3: set_rm16(alu<uint16_t>(5, 0, v)); break;
        case 4: {
            uint32_t r = uint32_t(regs.r[AX]) * v;
            regs.r[AX] = uint16_t(r);
            regs.r[DX] = uint16_t(r >> 16);
            bool hi = r >> 16;
            set_flag(CF, hi);
            set_flag(OF, hi);
            set_szp<uint16_t>(uint16_t(r >> 16));
            break;
        }
        case 5: {
            int32_t r = int32_t(int16_t(regs.r[AX])) * int16_t(v);
            regs.r[AX] = uint16_t(r);
            regs.r[DX] = uint16_t(uint32_t(r) >> 16);
            bool ext = r != int32_t(int16_t(r));
            set_flag(CF, ext);
            set_flag(OF, ext);
            set_szp<uint16_t>(uint16_t(uint32_t(r) >> 16));
            break;
        }
        case 6: {
            if (v == 0) return divide_error();
            uint32_t a = (uint32_t(regs.r[DX]) << 16) | regs.r[AX];
            uint32_t q = a / v, r = a % v;
            if (q > 0xFFFF) return divide_error();
            regs.r[AX] = uint16_t(q);
            regs.r[DX] = uint16_t(r);
            break;
        }
        case 7: {
            if (v == 0) return divide_error();
            int32_t a = int32_t((uint32_t(regs.r[DX]) << 16) | regs.r[AX]);
            int32_t d = int16_t(v);
            int64_t q = int64_t(a) / d, r = int64_t(a) % d;
            if (q > 32767 || q < -32767) return divide_error();
            if (rep_) q = -q;  // 8086 quirk
            regs.r[AX] = uint16_t(q);
            regs.r[DX] = uint16_t(r);
            break;
        }
    }
}

// ---------------------------------------------------------------- strings

void Cpu::string_op(uint8_t op) {
    bool word = op & 1;
    int delta = flag(DF) ? -(word ? 2 : 1) : (word ? 2 : 1);
    uint16_t src_seg = default_seg(DS);
    auto once = [&]() -> bool {  // returns false when a REPE/REPNE condition ends the loop
        switch (op) {
            case 0xA4: case 0xA5:  // MOVS
                if (word) mem_.write16(regs.s[ES], regs.r[DI], mem_.read16(src_seg, regs.r[SI]));
                else mem_.write8(Memory::linear(regs.s[ES], regs.r[DI]), mem_.read8(Memory::linear(src_seg, regs.r[SI])));
                regs.r[SI] += delta;
                regs.r[DI] += delta;
                return true;
            case 0xA6: case 0xA7:  // CMPS
                if (word) alu<uint16_t>(7, mem_.read16(src_seg, regs.r[SI]), mem_.read16(regs.s[ES], regs.r[DI]));
                else alu<uint8_t>(7, mem_.read8(Memory::linear(src_seg, regs.r[SI])), mem_.read8(Memory::linear(regs.s[ES], regs.r[DI])));
                regs.r[SI] += delta;
                regs.r[DI] += delta;
                return false;
            case 0xAA: case 0xAB:  // STOS
                if (word) mem_.write16(regs.s[ES], regs.r[DI], regs.r[AX]);
                else mem_.write8(Memory::linear(regs.s[ES], regs.r[DI]), regs.r8(0));
                regs.r[DI] += delta;
                return true;
            case 0xAC: case 0xAD:  // LODS
                if (word) regs.r[AX] = mem_.read16(src_seg, regs.r[SI]);
                else regs.r8(0) = mem_.read8(Memory::linear(src_seg, regs.r[SI]));
                regs.r[SI] += delta;
                return true;
            case 0xAE: case 0xAF:  // SCAS
                if (word) alu<uint16_t>(7, regs.r[AX], mem_.read16(regs.s[ES], regs.r[DI]));
                else alu<uint8_t>(7, regs.r8(0), mem_.read8(Memory::linear(regs.s[ES], regs.r[DI])));
                regs.r[DI] += delta;
                return false;
            case 0x6C: case 0x6D:  // INS (80186)
                if (word) mem_.write16(regs.s[ES], regs.r[DI], in8(regs.r[DX]) | (in8(regs.r[DX] + 1) << 8));
                else mem_.write8(Memory::linear(regs.s[ES], regs.r[DI]), in8(regs.r[DX]));
                regs.r[DI] += delta;
                return true;
            case 0x6E: case 0x6F: {  // OUTS (80186)
                if (word) {
                    uint16_t v = mem_.read16(src_seg, regs.r[SI]);
                    out8(regs.r[DX], uint8_t(v));
                    out8(regs.r[DX] + 1, uint8_t(v >> 8));
                } else out8(regs.r[DX], mem_.read8(Memory::linear(src_seg, regs.r[SI])));
                regs.r[SI] += delta;
                return true;
            }
        }
        return true;
    };
    bool compares = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    if (!rep_) {
        once();
        return;
    }
    while (regs.r[CX] != 0) {
        once();
        regs.r[CX]--;
        if (compares) {
            bool z = flag(ZF);
            if (rep_ == 0xF3 && !z) break;
            if (rep_ == 0xF2 && z) break;
        }
    }
}

// ---------------------------------------------------------------- step

void Cpu::step() {
    if (halted) return;
    if (!breakpoints.empty()) {
        uint32_t lin = Memory::linear(regs.s[CS], regs.ip);
        if (breakpoints[lin]) {
            uint16_t cs = regs.s[CS], ip = regs.ip;
            on_breakpoint(lin);
            if (cs != regs.s[CS] || ip != regs.ip) return;
        }
    }
    instructions++;
    seg_override_ = -1;
    rep_ = 0;
    insn_ip_ = regs.ip;

    uint8_t op;
    for (;;) {
        op = fetch8();
        switch (op) {
            case 0x26: seg_override_ = ES; continue;
            case 0x2E: seg_override_ = CS; continue;
            case 0x36: seg_override_ = SS; continue;
            case 0x3E: seg_override_ = DS; continue;
            case 0xF0: case 0xF1: continue;          // LOCK (F1 is an alias on 8086)
            case 0xF2: case 0xF3: rep_ = op; continue;
        }
        break;
    }

    // 8088 aliases unless running as an 80186.
    if (!cpu186) {
        if (op >= 0x60 && op <= 0x6F) op += 0x10;
        else if (op == 0xC0 || op == 0xC1) op += 2;
        else if (op == 0xC8 || op == 0xC9) op += 2;
    }

    auto cond = [&](int c) -> bool {
        switch (c) {
            case 0: return flag(OF);
            case 1: return !flag(OF);
            case 2: return flag(CF);
            case 3: return !flag(CF);
            case 4: return flag(ZF);
            case 5: return !flag(ZF);
            case 6: return flag(CF) || flag(ZF);
            case 7: return !flag(CF) && !flag(ZF);
            case 8: return flag(SF);
            case 9: return !flag(SF);
            case 10: return flag(PF);
            case 11: return !flag(PF);
            case 12: return flag(SF) != flag(OF);
            case 13: return flag(SF) == flag(OF);
            case 14: return flag(ZF) || flag(SF) != flag(OF);
            default: return !flag(ZF) && flag(SF) == flag(OF);
        }
    };

    switch (op) {
        // ALU r/m forms: 00-3D except segment/prefix/BCD slots
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
            decode_modrm();
            uint8_t r = alu<uint8_t>(op >> 3, rm8(), reg8());
            if ((op >> 3) != 7) set_rm8(r);
            break;
        }
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
            decode_modrm();
            uint16_t r = alu<uint16_t>(op >> 3, rm16(), reg16());
            if ((op >> 3) != 7) set_rm16(r);
            break;
        }
        case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
            decode_modrm();
            uint8_t r = alu<uint8_t>(op >> 3, reg8(), rm8());
            if ((op >> 3) != 7) reg8() = r;
            break;
        }
        case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
            decode_modrm();
            uint16_t r = alu<uint16_t>(op >> 3, reg16(), rm16());
            if ((op >> 3) != 7) reg16() = r;
            break;
        }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            uint8_t r = alu<uint8_t>(op >> 3, regs.r8(0), fetch8());
            if ((op >> 3) != 7) regs.r8(0) = r;
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            uint16_t r = alu<uint16_t>(op >> 3, regs.r[AX], fetch16());
            if ((op >> 3) != 7) regs.r[AX] = r;
            break;
        }

        case 0x06: push(regs.s[ES]); break;
        case 0x07: regs.s[ES] = pop(); break;
        case 0x0E: push(regs.s[CS]); break;
        case 0x0F: regs.s[CS] = pop(); break;  // POP CS (8086 only)
        case 0x16: push(regs.s[SS]); break;
        case 0x17: regs.s[SS] = pop(); break;
        case 0x1E: push(regs.s[DS]); break;
        case 0x1F: regs.s[DS] = pop(); break;

        case 0x27: {  // DAA
            uint8_t al = regs.r8(0), old = al;
            bool cf = flag(CF);
            // 8088 quirk: the high-digit threshold is 0x9F when AF was set.
            uint8_t limit = flag(AF) ? 0x9F : 0x99;
            if ((al & 0xF) > 9 || flag(AF)) {
                al += 6;
                set_flag(AF, true);
            } else set_flag(AF, false);
            if (old > limit || cf) {
                al += 0x60;
                set_flag(CF, true);
            } else set_flag(CF, false);
            regs.r8(0) = al;
            set_szp(al);
            break;
        }
        case 0x2F: {  // DAS
            uint8_t al = regs.r8(0), old = al;
            bool cf = flag(CF);
            // 8088 quirk: the high-digit threshold is 0x9F when AF was set.
            uint8_t limit = flag(AF) ? 0x9F : 0x99;
            if ((al & 0xF) > 9 || flag(AF)) {
                al -= 6;
                set_flag(AF, true);
            } else set_flag(AF, false);
            if (old > limit || cf) {
                al -= 0x60;
                set_flag(CF, true);
            } else set_flag(CF, false);
            regs.r8(0) = al;
            set_szp(al);
            break;
        }
        case 0x37:    // AAA
        case 0x3F: {  // AAS
            bool adj = (regs.r8(0) & 0xF) > 9 || flag(AF);
            if (adj) {
                if (op == 0x37) {
                    regs.r8(0) += 6;   // no carry into AH on the 8088
                    regs.r8(4) += 1;
                } else {
                    regs.r8(0) -= 6;
                    regs.r8(4) -= 1;
                }
            }
            set_flag(AF, adj);
            set_flag(CF, adj);
            regs.r8(0) &= 0x0F;
            set_szp(regs.r8(0));
            break;
        }

        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            regs.r[op & 7] = inc_dec<uint16_t>(regs.r[op & 7], false);
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            regs.r[op & 7] = inc_dec<uint16_t>(regs.r[op & 7], true);
            break;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if ((op & 7) == SP) {  // 8086 pushes the decremented SP
                regs.r[SP] -= 2;
                mem_.write16(regs.s[SS], regs.r[SP], regs.r[SP]);
            } else push(regs.r[op & 7]);
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            regs.r[op & 7] = pop();
            break;

        // 80186 (only reachable with cpu186)
        case 0x60: {  // PUSHA
            uint16_t sp = regs.r[SP];
            for (int i = 0; i < 8; i++) push(i == SP ? sp : regs.r[i]);
            break;
        }
        case 0x61:  // POPA
            for (int i = 7; i >= 0; i--) {
                uint16_t v = pop();
                if (i != SP) regs.r[i] = v;
            }
            break;
        case 0x62: {  // BOUND
            decode_modrm();
            int16_t lo = int16_t(mem_.read16(ea_seg_, ea_off_)), hi = int16_t(mem_.read16(ea_seg_, ea_off_ + 2));
            int16_t v = int16_t(reg16());
            if (v < lo || v > hi) {
                regs.ip = insn_ip_;
                interrupt(5);
            }
            break;
        }
        case 0x68: push(fetch16()); break;
        case 0x6A: push(uint16_t(int8_t(fetch8()))); break;
        case 0x69:
        case 0x6B: {  // IMUL r16, r/m16, imm
            decode_modrm();
            int32_t a = int16_t(rm16());
            int32_t b = op == 0x69 ? int16_t(fetch16()) : int8_t(fetch8());
            int32_t r = a * b;
            reg16() = uint16_t(r);
            bool ext = r != int32_t(int16_t(r));
            set_flag(CF, ext);
            set_flag(OF, ext);
            break;
        }
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: string_op(op); break;

        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t d = int8_t(fetch8());
            if (cond(op & 15)) regs.ip += d;
            break;
        }

        case 0x80: case 0x82: {
            decode_modrm();
            int sub = (modrm_ >> 3) & 7;
            uint8_t r = alu<uint8_t>(sub, rm8(), fetch8());
            if (sub != 7) set_rm8(r);
            break;
        }
        case 0x81: {
            decode_modrm();
            int sub = (modrm_ >> 3) & 7;
            uint16_t r = alu<uint16_t>(sub, rm16(), fetch16());
            if (sub != 7) set_rm16(r);
            break;
        }
        case 0x83: {
            decode_modrm();
            int sub = (modrm_ >> 3) & 7;
            uint16_t r = alu<uint16_t>(sub, rm16(), uint16_t(int8_t(fetch8())));
            if (sub != 7) set_rm16(r);
            break;
        }
        case 0x84: decode_modrm(); alu<uint8_t>(4, rm8(), reg8()); break;
        case 0x85: decode_modrm(); alu<uint16_t>(4, rm16(), reg16()); break;
        case 0x86: {
            decode_modrm();
            uint8_t a = rm8(), b = reg8();
            set_rm8(b);
            reg8() = a;
            break;
        }
        case 0x87: {
            decode_modrm();
            uint16_t a = rm16(), b = reg16();
            set_rm16(b);
            reg16() = a;
            break;
        }
        case 0x88: decode_modrm(); set_rm8(reg8()); break;
        case 0x89: decode_modrm(); set_rm16(reg16()); break;
        case 0x8A: decode_modrm(); reg8() = rm8(); break;
        case 0x8B: decode_modrm(); reg16() = rm16(); break;
        case 0x8C: decode_modrm(); set_rm16(regs.s[(modrm_ >> 3) & 3]); break;
        case 0x8D:
            decode_modrm();
            if (ea_is_reg_) unimplemented("LEA with register operand");
            else reg16() = ea_off_;
            break;
        case 0x8E: decode_modrm(); regs.s[(modrm_ >> 3) & 3] = rm16(); break;
        case 0x8F: {
            decode_modrm();
            uint16_t v = pop();
            // Effective address uses SP after the pop on the 8086.
            set_rm16(v);
            break;
        }

        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t t = regs.r[AX];
            regs.r[AX] = regs.r[op & 7];
            regs.r[op & 7] = t;
            break;
        }
        case 0x98: regs.r[AX] = uint16_t(int16_t(int8_t(regs.r8(0)))); break;
        case 0x99: regs.r[DX] = (regs.r[AX] & 0x8000) ? 0xFFFF : 0; break;
        case 0x9A: {
            uint16_t off = fetch16(), seg = fetch16();
            far_call(seg, off);
            break;
        }
        case 0x9B: break;  // WAIT
        case 0x9C: push(regs.flags); break;
        case 0x9D: regs.flags = kFlagsFixed | (pop() & kFlagsWritable); break;
        case 0x9E: regs.flags = (regs.flags & 0xFF00) | (regs.r8(4) & 0xD5) | 0x02; break;
        case 0x9F: regs.r8(4) = uint8_t(regs.flags); break;

        case 0xA0: regs.r8(0) = mem_.read8(Memory::linear(default_seg(DS), fetch16())); break;
        case 0xA1: regs.r[AX] = mem_.read16(default_seg(DS), fetch16()); break;
        case 0xA2: mem_.write8(Memory::linear(default_seg(DS), fetch16()), regs.r8(0)); break;
        case 0xA3: mem_.write16(default_seg(DS), fetch16(), regs.r[AX]); break;
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            string_op(op);
            break;
        case 0xA8: alu<uint8_t>(4, regs.r8(0), fetch8()); break;
        case 0xA9: alu<uint16_t>(4, regs.r[AX], fetch16()); break;

        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            regs.r8(op & 7) = fetch8();
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            regs.r[op & 7] = fetch16();
            break;

        case 0xC0: case 0xC1: {  // 80186 shift r/m, imm8
            decode_modrm();
            unsigned n = fetch8() & 0x1F;
            int sub = (modrm_ >> 3) & 7;
            if (op == 0xC0) set_rm8(shift<uint8_t>(sub, rm8(), n));
            else set_rm16(shift<uint16_t>(sub, rm16(), n));
            break;
        }
        case 0xC2: {
            uint16_t n = fetch16();
            regs.ip = pop();
            regs.r[SP] += n;
            break;
        }
        case 0xC3: regs.ip = pop(); break;
        case 0xC4: case 0xC5: {
            decode_modrm();
            if (ea_is_reg_) {
                unimplemented("LES/LDS with register operand");
                break;
            }
            reg16() = mem_.read16(ea_seg_, ea_off_);
            regs.s[op == 0xC4 ? ES : DS] = mem_.read16(ea_seg_, uint16_t(ea_off_ + 2));
            break;
        }
        case 0xC6: decode_modrm(); set_rm8(fetch8()); break;
        case 0xC7: decode_modrm(); set_rm16(fetch16()); break;
        case 0xC8: {  // ENTER (80186)
            uint16_t size = fetch16();
            uint8_t level = fetch8() & 0x1F;
            push(regs.r[BP]);
            uint16_t frame = regs.r[SP];
            for (int i = 1; i < level; i++) {
                regs.r[BP] -= 2;
                push(mem_.read16(regs.s[SS], regs.r[BP]));
            }
            if (level) push(frame);
            regs.r[BP] = frame;
            regs.r[SP] -= size;
            break;
        }
        case 0xC9:  // LEAVE (80186)
            regs.r[SP] = regs.r[BP];
            regs.r[BP] = pop();
            break;
        case 0xCA: {
            uint16_t n = fetch16();
            regs.ip = pop();
            regs.s[CS] = pop();
            regs.r[SP] += n;
            break;
        }
        case 0xCB:
            regs.ip = pop();
            regs.s[CS] = pop();
            break;
        case 0xCC: interrupt(3); break;
        case 0xCD: interrupt(fetch8()); break;
        case 0xCE:
            if (flag(OF)) interrupt(4);
            break;
        case 0xCF:
            regs.ip = pop();
            regs.s[CS] = pop();
            regs.flags = kFlagsFixed | (pop() & kFlagsWritable);
            break;

        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            decode_modrm();
            unsigned n = (op & 2) ? regs.r8(1) : 1;
            if (cpu186) n &= 0x1F;
            int sub = (modrm_ >> 3) & 7;
            if (op & 1) set_rm16(shift<uint16_t>(sub, rm16(), n));
            else set_rm8(shift<uint8_t>(sub, rm8(), n));
            break;
        }
        case 0xD4: {  // AAM
            uint8_t base = fetch8();
            if (base == 0) {
                set_szp(uint8_t(0));  // 8088 sets SZP as for a zero result before trapping
                divide_error();
                break;
            }
            uint8_t al = regs.r8(0);
            regs.r8(4) = al / base;
            regs.r8(0) = al % base;
            set_szp(regs.r8(0));
            break;
        }
        case 0xD5: {  // AAD
            uint8_t base = fetch8();
            uint8_t r = alu<uint8_t>(0, regs.r8(0), uint8_t(regs.r8(4) * base));
            regs.r8(0) = r;
            regs.r8(4) = 0;
            break;
        }
        case 0xD6: regs.r8(0) = flag(CF) ? 0xFF : 0x00; break;  // SALC (undocumented)
        case 0xD7: regs.r8(0) = mem_.read8(Memory::linear(default_seg(DS), uint16_t(regs.r[BX] + regs.r8(0)))); break;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            decode_modrm();  // ESC: no coprocessor attached
            break;

        case 0xE0: case 0xE1: case 0xE2: {
            int8_t d = int8_t(fetch8());
            regs.r[CX]--;
            bool take = regs.r[CX] != 0;
            if (op == 0xE0) take = take && !flag(ZF);
            if (op == 0xE1) take = take && flag(ZF);
            if (take) regs.ip += d;
            break;
        }
        case 0xE3: {
            int8_t d = int8_t(fetch8());
            if (regs.r[CX] == 0) regs.ip += d;
            break;
        }
        case 0xE4: regs.r8(0) = in8(fetch8()); break;
        case 0xE5: {
            uint8_t p = fetch8();
            regs.r[AX] = in8(p) | (in8(uint8_t(p + 1)) << 8);
            break;
        }
        case 0xE6: out8(fetch8(), regs.r8(0)); break;
        case 0xE7: {
            uint8_t p = fetch8();
            out8(p, regs.r8(0));
            out8(uint8_t(p + 1), regs.r8(4));
            break;
        }
        case 0xE8: {
            int16_t d = int16_t(fetch16());
            push(regs.ip);
            regs.ip += d;
            break;
        }
        case 0xE9: {
            int16_t d = int16_t(fetch16());
            regs.ip += d;
            break;
        }
        case 0xEA: {
            uint16_t off = fetch16(), seg = fetch16();
            regs.ip = off;
            regs.s[CS] = seg;
            break;
        }
        case 0xEB: {
            int8_t d = int8_t(fetch8());
            regs.ip += d;
            break;
        }
        case 0xEC: regs.r8(0) = in8(regs.r[DX]); break;
        case 0xED: regs.r[AX] = in8(regs.r[DX]) | (in8(regs.r[DX] + 1) << 8); break;
        case 0xEE: out8(regs.r[DX], regs.r8(0)); break;
        case 0xEF:
            out8(regs.r[DX], regs.r8(0));
            out8(regs.r[DX] + 1, regs.r8(4));
            break;

        case 0xF4: halted = true; break;
        case 0xF5: set_flag(CF, !flag(CF)); break;
        case 0xF6: decode_modrm(); group3_8(); break;
        case 0xF7: decode_modrm(); group3_16(); break;
        case 0xF8: set_flag(CF, false); break;
        case 0xF9: set_flag(CF, true); break;
        case 0xFA: set_flag(IF, false); break;
        case 0xFB: set_flag(IF, true); break;
        case 0xFC: set_flag(DF, false); break;
        case 0xFD: set_flag(DF, true); break;
        case 0xFE: {
            // Native callback trap: FE 38 lo hi.
            if (mem_.read8(Memory::linear(regs.s[CS], regs.ip)) == 0x38) {
                regs.ip++;
                uint16_t id = fetch16();
                if (!on_callback) unimplemented("callback trap with no handler");
                else on_callback(id);
                break;
            }
            decode_modrm();
            int sub = (modrm_ >> 3) & 7;
            if (sub == 0) set_rm8(inc_dec<uint8_t>(rm8(), false));
            else if (sub == 1) set_rm8(inc_dec<uint8_t>(rm8(), true));
            else unimplemented("FE group, undocumented form");
            break;
        }
        case 0xFF: {
            decode_modrm();
            int sub = (modrm_ >> 3) & 7;
            switch (sub) {
                case 0: set_rm16(inc_dec<uint16_t>(rm16(), false)); break;
                case 1: set_rm16(inc_dec<uint16_t>(rm16(), true)); break;
                case 2: {
                    uint16_t target = rm16();
                    push(regs.ip);
                    regs.ip = target;
                    break;
                }
                case 3: {
                    if (ea_is_reg_) { unimplemented("CALL FAR with register operand"); break; }
                    uint16_t off = mem_.read16(ea_seg_, ea_off_), seg = mem_.read16(ea_seg_, uint16_t(ea_off_ + 2));
                    far_call(seg, off);
                    break;
                }
                case 4: regs.ip = rm16(); break;
                case 5: {
                    if (ea_is_reg_) { unimplemented("JMP FAR with register operand"); break; }
                    regs.ip = mem_.read16(ea_seg_, ea_off_);
                    regs.s[CS] = mem_.read16(ea_seg_, uint16_t(ea_off_ + 2));
                    break;
                }
                case 6:
                case 7: {  // PUSH r/m (7 is an alias on 8086)
                    if (ea_is_reg_ && (modrm_ & 7) == SP) {
                        regs.r[SP] -= 2;
                        mem_.write16(regs.s[SS], regs.r[SP], regs.r[SP]);
                    } else push(rm16());
                    break;
                }
            }
            break;
        }
        default:
            unimplemented("opcode");
            break;
    }
}

template uint8_t Cpu::alu<uint8_t>(int, uint8_t, uint8_t);
template uint16_t Cpu::alu<uint16_t>(int, uint16_t, uint16_t);

}  // namespace f19
