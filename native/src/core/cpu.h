// x86-16 real-mode interpreter (8086/8088, optional 80186 extensions).
//
// Exact 8086 semantics are the default, validated against the
// SingleStepTests 8088 hardware vectors (tests/sst_runner.cpp). With
// `cpu186` set, opcodes that are aliases on the 8088 (60-6F, C0/C1, C8/C9)
// become the 80186 instructions instead.
//
// Native code is reached through a trap: the otherwise-undocumented encoding
// FE 38 lo hi calls `on_callback(lo | hi << 8)` and continues after it.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace f19 {

static_assert(std::endian::native == std::endian::little, "register file assumes little-endian host");

class Memory {
public:
    static constexpr uint32_t kSize = 0x100000;  // 1 MB, addresses wrap like an 8088
    Memory() : bytes_(kSize, 0) {}

    static uint32_t linear(uint16_t seg, uint16_t off) { return ((uint32_t(seg) << 4) + off) & (kSize - 1); }
    uint8_t read8(uint32_t a) const { return bytes_[a & (kSize - 1)]; }
    void write8(uint32_t a, uint8_t v) {
        a &= kSize - 1;
        if (journal) journal->emplace_back(a, bytes_[a]);
        if (a - watch_lo < watch_len) watch_mask[a - watch_lo] = watch_tag;
        bytes_[a] = v;
    }
    // Write tagging: writes to [watch_lo, watch_lo + watch_len) store
    // `watch_tag` in watch_mask (who last wrote each byte).
    uint32_t watch_lo = 0, watch_len = 0;
    uint8_t* watch_mask = nullptr;
    uint8_t watch_tag = 0;
    // When set, every write appends (address, previous value).
    std::vector<std::pair<uint32_t, uint8_t>>* journal = nullptr;
    // Word access within a segment wraps the offset at 64K, as on the 8086.
    uint16_t read16(uint16_t seg, uint16_t off) const {
        return read8(linear(seg, off)) | (read8(linear(seg, uint16_t(off + 1))) << 8);
    }
    void write16(uint16_t seg, uint16_t off, uint16_t v) {
        write8(linear(seg, off), uint8_t(v));
        write8(linear(seg, uint16_t(off + 1)), uint8_t(v >> 8));
    }
    uint8_t* data() { return bytes_.data(); }

private:
    std::vector<uint8_t> bytes_;
};

enum Reg16 { AX, CX, DX, BX, SP, BP, SI, DI };
enum SReg { ES, CS, SS, DS };
enum Flag : uint16_t {
    CF = 1 << 0, PF = 1 << 2, AF = 1 << 4, ZF = 1 << 6, SF = 1 << 7,
    TF = 1 << 8, IF = 1 << 9, DF = 1 << 10, OF = 1 << 11,
};

struct Regs {
    uint16_t r[8] = {};   // AX CX DX BX SP BP SI DI
    uint16_t s[4] = {};   // ES CS SS DS
    uint16_t ip = 0;
    uint16_t flags = 0xF002;  // 8086: bits 12-15 and 1 read as 1

    uint8_t& r8(int i) { return reinterpret_cast<uint8_t*>(r)[(i & 3) * 2 + (i >> 2)]; }
};

class Cpu {
public:
    explicit Cpu(Memory& mem) : mem_(mem) {}

    Regs regs;
    bool cpu186 = false;
    bool halted = false;
    uint64_t instructions = 0;

    // Hooks. Defaults: ports read 0xFF, writes ignored; callbacks fail.
    std::function<uint8_t(uint16_t port)> in8 = [](uint16_t) { return uint8_t(0xFF); };
    std::function<void(uint16_t port, uint8_t v)> out8 = [](uint16_t, uint8_t) {};
    std::function<void(uint16_t id)> on_callback;
    // Called on an opcode the interpreter does not implement; default throws.
    std::function<void(const std::string& what)> on_unimplemented;

    // Execution breakpoints: when `breakpoints` is non-empty and the byte for
    // the linear address of CS:IP is non-zero, `on_breakpoint` runs before
    // the instruction. If it changes CS:IP, the instruction is skipped
    // (lets native code replace a routine: it can emulate RETF).
    std::vector<uint8_t> breakpoints;
    std::function<void(uint32_t linear)> on_breakpoint;

    // Execute one instruction (including its prefixes, and a whole REP loop).
    void step();
    // Deliver a hardware interrupt if IF is set; returns whether it was taken.
    bool irq(uint8_t vector);
    // Software-style interrupt entry (push flags/CS/IP, clear IF/TF, vector).
    void interrupt(uint8_t vector);

    // Helpers for HLE code.
    void push(uint16_t v);
    uint16_t pop();
    bool flag(uint16_t f) const { return regs.flags & f; }
    void set_flag(uint16_t f, bool on) { regs.flags = on ? (regs.flags | f) : (regs.flags & ~f); }
    void far_call(uint16_t seg, uint16_t off);   // push CS:IP, jump
    Memory& mem() { return mem_; }

private:
    Memory& mem_;

    // Per-instruction decode state.
    int seg_override_ = -1;
    int rep_ = 0;          // 0, 0xF2 (REPNE) or 0xF3 (REPE)
    uint16_t insn_ip_ = 0; // IP of the first prefix/opcode byte
    uint8_t modrm_ = 0;
    bool ea_is_reg_ = false;
    uint16_t ea_seg_ = 0, ea_off_ = 0;

    uint8_t fetch8();
    uint16_t fetch16();
    void decode_modrm();
    uint16_t default_seg(uint16_t s) const { return seg_override_ >= 0 ? regs.s[seg_override_] : regs.s[s]; }

    uint8_t rm8();
    uint16_t rm16();
    void set_rm8(uint8_t v);
    void set_rm16(uint16_t v);
    uint8_t& reg8() { return regs.r8((modrm_ >> 3) & 7); }
    uint16_t& reg16() { return regs.r[(modrm_ >> 3) & 7]; }

    // ALU. op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP
    template <typename T> T alu(int op, T a, T b);
    template <typename T> T inc_dec(T v, bool dec);
    template <typename T> T shift(int op, T v, unsigned count);
    template <typename T> void set_szp(T v);
    void group3_8();
    void group3_16();
    void string_op(uint8_t op);
    void unimplemented(const char* what);
    void divide_error();
};

}  // namespace f19
