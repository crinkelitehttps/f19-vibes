#include "core/machine.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "core/dos.h"

namespace f19 {

namespace {
// BIOS data area offsets (segment 0x40).
constexpr uint16_t kBda = 0x40;
constexpr uint16_t kEquipment = 0x10, kMemSize = 0x13, kKbdFlags = 0x17;
constexpr uint16_t kKbdHead = 0x1A, kKbdTail = 0x1C, kKbdBufStart = 0x1E, kKbdBufEnd = 0x3E;
constexpr uint16_t kVideoMode = 0x49, kVideoCols = 0x4A, kTicks = 0x6C;
}  // namespace

Machine::Machine(std::filesystem::path game_dir) {
    // Callback 0 is reserved/invalid.
    callbacks_.push_back({[this] { stop_reason = "callback 0"; }, "invalid"});
    cpu.on_callback = [this](uint16_t id) {
        if (id >= callbacks_.size()) {
            stop_reason = "bad callback id";
            exited = true;
            return;
        }
        callbacks_[id].fn();
    };
    cpu.on_unimplemented = [this](const std::string& what) {
        stop_reason = "unimplemented: " + what;
        exited = true;
    };
    cpu.in8 = [this](uint16_t p) -> uint8_t {
        auto it = port_in.find(p);
        if (it != port_in.end()) return it->second();
        if (!unknown_ports_logged_[p]) {
            unknown_ports_logged_[p] = true;
            log("IN  port %04X (unhandled) at %04X:%04X\n", p, cpu.regs.s[CS], cpu.regs.ip);
        }
        return 0xFF;
    };
    cpu.out8 = [this](uint16_t p, uint8_t v) {
        auto it = port_out.find(p);
        if (it != port_out.end()) return it->second(v);
        if (!unknown_ports_logged_[p | 0x10000u]) {
            unknown_ports_logged_[p | 0x10000u] = true;
            log("OUT port %04X = %02X (unhandled) at %04X:%04X\n", p, v, cpu.regs.s[CS], cpu.regs.ip);
        }
    };

    // Every vector defaults to a stub that logs and IRETs.
    for (int n = 0; n < 256; n++) {
        uint16_t id = register_callback([this, n] {
            log("INT %02X (unhandled) AX=%04X\n", n, cpu.regs.r[AX]);
        }, "default int");
        set_vector(uint8_t(n), kStubSeg, make_stub(id, {0xCF}));
    }

    // BIOS data area.
    mem.write16(kBda, kEquipment, 0x0021);  // 80x25 colour, 1 floppy present? (bit0)
    mem.write16(kBda, kMemSize, 640);
    mem.write16(kBda, kKbdHead, kKbdBufStart);
    mem.write16(kBda, kKbdTail, kKbdBufStart);
    // Keyboard buffer bounds (AT BIOS). EGAME's INT 9 handler uses these to
    // walk the buffer; left at 0 it corrupts the head pointer.
    mem.write16(kBda, 0x80, kKbdBufStart);
    mem.write16(kBda, 0x82, kKbdBufEnd);
    mem.write8(Memory::linear(kBda, kVideoMode), 3);
    mem.write16(kBda, kVideoCols, 80);

    // INT 08h: BIOS timer tick -> count, INT 1Ch, EOI.
    hook_interrupt(0x08, [this] {
        uint32_t t = mem.read16(kBda, kTicks) | (uint32_t(mem.read16(kBda, kTicks + 2)) << 16);
        t++;
        mem.write16(kBda, kTicks, uint16_t(t));
        mem.write16(kBda, kTicks + 2, uint16_t(t >> 16));
        irq0_in_service_ = false;  // EOI
        // Chain to INT 1Ch: simulate "INT 1Ch" from within the handler.
        cpu.interrupt(0x1C);
        // Our stub returns via IRET after INT 1Ch's handler IRETs back here.
    }, "bios int08");
    // INT 1Ch default: IRET (the default stub already does that, silently).
    {
        uint16_t id = register_callback([] {}, "int1c");
        set_vector(0x1C, kStubSeg, make_stub(id, {0xCF}));
    }
    // INT 09h: BIOS keyboard handler. Buffers the key associated with the
    // scan code, tracks Shift/Ctrl/Alt in the BDA flags, sends EOI.
    hook_interrupt(0x09, [this] {
        uint8_t code = port60_;
        uint8_t flags = mem.read8(Memory::linear(kBda, kKbdFlags));
        bool up = code & 0x80;
        uint8_t bit = 0;
        switch (code & 0x7F) {
            case 0x2A: bit = 0x02; break;  // left shift
            case 0x36: bit = 0x01; break;  // right shift
            case 0x1D: bit = 0x04; break;  // ctrl
            case 0x38: bit = 0x08; break;  // alt
        }
        if (bit && code != 0xE0) flags = up ? (flags & ~bit) : (flags | bit);
        mem.write8(Memory::linear(kBda, kKbdFlags), flags);
        if (!up && kbd_bios_key_) bios_enqueue(kbd_bios_key_);
        kbd_bios_key_ = 0;
        irq1_in_service_ = false;
    }, "bios int09");
    hook_interrupt(0x11, [this] { cpu.regs.r[AX] = mem.read16(kBda, kEquipment); }, "int11");
    hook_interrupt(0x12, [this] { cpu.regs.r[AX] = 640; }, "int12");

    // INT 10h video: text mode on B800 (setup screens), mode bookkeeping for
    // graphics (drivers do the drawing).
    hook_interrupt(0x10, [this] { int10(); }, "int10");

    // INT 16h keyboard via the BDA ring buffer.
    hook_interrupt(0x16, [this] {
        uint8_t ah = cpu.regs.r8(4);
        uint16_t head = mem.read16(kBda, kKbdHead), tail = mem.read16(kBda, kKbdTail);
        bool empty = head == tail;
        if (ah != 0 || !empty) log("INT 16 AH=%02X empty=%d head=%04X tail=%04X\n", ah, empty, head, tail);
        switch (ah & 0xEF) {
            case 0x00: {
                uint16_t k;
                if (!key_pop(&k)) return block_and_retry();
                cpu.regs.r[AX] = k;
                break;
            }
            case 0x01:
                set_return_flag(ZF, empty);
                if (!empty) cpu.regs.r[AX] = mem.read16(kBda, head);
                break;
            case 0x02:
                cpu.regs.r8(0) = mem.read8(Memory::linear(kBda, kKbdFlags));
                break;
            default:
                log("INT 16 AH=%02X\n", ah);
        }
    }, "int16");

    // INT 1Ah time.
    hook_interrupt(0x1A, [this] {
        uint8_t ah = cpu.regs.r8(4);
        switch (ah) {
            case 0x00:
                cpu.regs.r[DX] = mem.read16(kBda, kTicks);
                cpu.regs.r[CX] = mem.read16(kBda, kTicks + 2);
                cpu.regs.r8(0) = 0;
                break;
            case 0x01:
                mem.write16(kBda, kTicks, cpu.regs.r[DX]);
                mem.write16(kBda, kTicks + 2, cpu.regs.r[CX]);
                break;
            default:
                log("INT 1A AH=%02X\n", ah);
                set_return_flag(CF, true);
        }
    }, "int1a");

    // INT 13h disk: report "no such drive / error" and log; refine from traces.
    hook_interrupt(0x13, [this] {
        log("INT 13 AX=%04X CX=%04X DX=%04X\n", cpu.regs.r[AX], cpu.regs.r[CX], cpu.regs.r[DX]);
        cpu.regs.r8(4) = 0x01;
        set_return_flag(CF, true);
    }, "int13");

    setup_ports();
    dos = std::make_unique<Dos>(*this, std::move(game_dir));
}

Machine::~Machine() = default;

namespace {
constexpr uint16_t kCursorPos = 0x50;   // 8 pages x (col, row)
constexpr uint16_t kTextSeg = 0xB800;
}  // namespace

void Machine::text_put(uint8_t ch, uint8_t attr, bool use_attr) {
    uint8_t col = mem.read8(Memory::linear(kBda, kCursorPos)), row = mem.read8(Memory::linear(kBda, kCursorPos + 1));
    uint16_t off = uint16_t((row * 80 + col) * 2);
    mem.write8(Memory::linear(kTextSeg, off), ch);
    if (use_attr) mem.write8(Memory::linear(kTextSeg, uint16_t(off + 1)), attr);
}

void Machine::text_scroll(int lines, uint8_t attr, int top, int left, int bottom, int right, bool up) {
    int height = bottom - top + 1;
    if (lines == 0 || lines > height) lines = height;
    for (int i = 0; i < height; i++) {
        int dst = up ? top + i : bottom - i;
        int src = up ? dst + lines : dst - lines;
        for (int c = left; c <= right; c++) {
            uint16_t d = uint16_t((dst * 80 + c) * 2);
            bool blank = up ? src > bottom : src < top;
            uint16_t v = blank ? uint16_t(0x20 | (attr << 8)) : mem.read16(kTextSeg, uint16_t((src * 80 + c) * 2));
            mem.write16(kTextSeg, d, v);
        }
    }
}

std::string Machine::text_screen() {
    std::string out;
    for (int r = 0; r < 25; r++) {
        for (int c = 0; c < 80; c++) {
            uint8_t ch = mem.read8(Memory::linear(kTextSeg, uint16_t((r * 80 + c) * 2)));
            // CP437 box drawing -> ASCII approximations.
            if (ch >= 0xB3 && ch <= 0xDA) ch = (ch == 0xB3 || ch == 0xBA) ? '|' : (ch == 0xC4 || ch == 0xCD) ? '-' : '+';
            else if (ch < 32 || ch >= 127) ch = ch ? '.' : ' ';
            out += char(ch);
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += '\n';
    }
    return out;
}

void Machine::tty_out(uint8_t al) {
    uint8_t col = mem.read8(Memory::linear(kBda, kCursorPos)), row = mem.read8(Memory::linear(kBda, kCursorPos + 1));
    if (al == 0x0D) col = 0;
    else if (al == 0x0A) row++;
    else if (al == 0x08) { if (col) col--; }
    else if (al == 0x07) {}
    else {
        text_put(al, 0, false);
        if (++col >= 80) { col = 0; row++; }
    }
    if (row >= 25) { text_scroll(1, 0x07, 0, 0, 24, 79, true); row = 24; }
    mem.write8(Memory::linear(kBda, kCursorPos), col);
    mem.write8(Memory::linear(kBda, kCursorPos + 1), row);
}

void Machine::int10() {
    Regs& r = cpu.regs;
    uint8_t ah = r.r8(4), al = r.r8(0);
    uint8_t col = mem.read8(Memory::linear(kBda, kCursorPos)), row = mem.read8(Memory::linear(kBda, kCursorPos + 1));
    switch (ah) {
        case 0x00:
            log("INT 10 set mode %02X\n", al);
            mem.write8(Memory::linear(kBda, kVideoMode), al & 0x7F);
            if ((al & 0x7F) <= 3 && !(al & 0x80))
                for (uint16_t i = 0; i < 4000; i += 2) mem.write16(kTextSeg, i, 0x0720);
            mem.write16(kBda, kCursorPos, 0);
            if (on_mode_set) on_mode_set(al & 0x7F);
            break;
        case 0x01: break;  // cursor shape
        case 0x02:
            mem.write8(Memory::linear(kBda, kCursorPos), r.r8(2));
            mem.write8(Memory::linear(kBda, kCursorPos + 1), r.r8(6));
            break;
        case 0x03:
            r.r8(2) = col;
            r.r8(6) = row;
            r.r[CX] = 0x0607;
            break;
        case 0x05: break;  // page
        case 0x06: case 0x07:
            text_scroll(al, r.r8(7), r.r8(5), r.r8(1), r.r8(6), r.r8(2), ah == 0x06);
            break;
        case 0x08: {
            uint16_t v = mem.read16(kTextSeg, uint16_t((row * 80 + col) * 2));
            r.r[AX] = v;
            break;
        }
        case 0x09: case 0x0A: {
            for (uint16_t i = 0; i < r.r[CX]; i++) {
                uint16_t off = uint16_t((row * 80 + col + i) * 2);
                if (off >= 4000) break;
                mem.write8(Memory::linear(kTextSeg, off), al);
                if (ah == 0x09) mem.write8(Memory::linear(kTextSeg, uint16_t(off + 1)), r.r8(3));
            }
            break;
        }
        case 0x0E:
            tty_out(al);
            break;
        case 0x0F:
            r.r8(0) = mem.read8(Memory::linear(kBda, kVideoMode));
            r.r8(4) = 80;
            r.r8(7) = 0;
            break;
        case 0x10:
            if (al == 0x10) { dac[r.r[BX] & 0xFF][0] = r.r8(6) & 0x3F; dac[r.r[BX] & 0xFF][1] = r.r8(5) & 0x3F; dac[r.r[BX] & 0xFF][2] = r.r8(1) & 0x3F; }
            else if (al == 0x12) {
                for (uint16_t i = 0; i < r.r[CX]; i++)
                    for (int k = 0; k < 3; k++)
                        dac[(r.r[BX] + i) & 0xFF][k] = mem.read8(Memory::linear(r.s[ES], uint16_t(r.r[DX] + i * 3 + k))) & 0x3F;
            } else log("INT 10 AX=%04X (palette)\n", r.r[AX]);
            break;
        case 0x1A:
            if (al == 0) { r.r8(0) = 0x1A; r.r[BX] = 0x0008; }
            break;
        case 0x12:
            if (r.r8(3) == 0x10) { r.r[BX] = 0x0003; r.r[CX] = 0x0009; }
            break;
        default:
            log("INT 10 AH=%02X AL=%02X\n", ah, al);
            break;
    }
}

void Machine::log(const char* fmt, ...) {
    if (!trace) return;
    std::fprintf(stderr, "[%10llu] ", (unsigned long long)cpu.instructions);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void Machine::add_breakpoint(uint32_t linear, BreakHandler h) {
    if (cpu.breakpoints.empty()) {
        cpu.breakpoints.assign(Memory::kSize, 0);
        cpu.on_breakpoint = [this](uint32_t lin) {
            if (breakpoints_suspended) return;
            auto it = break_handlers_.find(lin);
            if (it == break_handlers_.end()) return;
            for (auto& fn : it->second)
                if (fn()) return;
        };
    }
    cpu.breakpoints[linear & (Memory::kSize - 1)] = 1;
    break_handlers_[linear].push_back(std::move(h));
}

void Machine::clear_breakpoints_in(uint32_t lo, uint32_t hi) {
    for (auto it = break_handlers_.lower_bound(lo); it != break_handlers_.end() && it->first < hi;) {
        cpu.breakpoints[it->first] = 0;
        it = break_handlers_.erase(it);
    }
}

uint16_t Machine::register_callback(Callback cb, std::string name) {
    callbacks_.push_back({std::move(cb), std::move(name)});
    return uint16_t(callbacks_.size() - 1);
}

uint16_t Machine::make_stub(uint16_t id, std::initializer_list<uint8_t> tail) {
    uint16_t off = stub_next_;
    uint32_t a = Memory::linear(kStubSeg, off);
    mem.write8(a++, 0xFE);
    mem.write8(a++, 0x38);
    mem.write8(a++, uint8_t(id));
    mem.write8(a++, uint8_t(id >> 8));
    for (uint8_t b : tail) mem.write8(a++, b);
    stub_next_ = uint16_t(off + 4 + tail.size());
    return off;
}

void Machine::set_vector(uint8_t n, uint16_t seg, uint16_t off) {
    mem.write16(0, n * 4, off);
    mem.write16(0, n * 4 + 2, seg);
}

void Machine::hook_interrupt(uint8_t n, Callback cb, std::string name) {
    uint16_t id = register_callback(std::move(cb), std::move(name));
    set_vector(n, kStubSeg, make_stub(id, {0xCF}));
}

void Machine::set_return_flag(uint16_t f, bool on) {
    // Stack inside a handler entered by INT: [SP]=IP [SP+2]=CS [SP+4]=FLAGS.
    uint16_t sp = cpu.regs.r[SP];
    uint16_t fl = mem.read16(cpu.regs.s[SS], uint16_t(sp + 4));
    fl = on ? (fl | f) : (fl & ~f);
    mem.write16(cpu.regs.s[SS], uint16_t(sp + 4), fl);
}

void Machine::key_event(uint8_t code, uint16_t bios_key) {
    kbd_queue_.emplace_back(code, bios_key);
    cpu.halted = false;
}

void Machine::key_press(uint8_t scan, uint8_t ascii) {
    key_event(scan, uint16_t(ascii | (scan << 8)));
    key_event(uint8_t(scan | 0x80), 0);
}

void Machine::service_keyboard() {
    if (kbd_queue_.empty() || irq1_in_service_ || irq0_in_service_ || (pic_mask_ & 2) || !cpu.flag(IF)) return;
    auto [code, key] = kbd_queue_.front();
    kbd_queue_.pop_front();
    port60_ = code;
    kbd_bios_key_ = key;
    if (cpu.irq(0x09)) irq1_in_service_ = true;
}

void Machine::bios_enqueue(uint16_t key) {
    uint16_t tail = mem.read16(kBda, kKbdTail);
    uint16_t next = tail + 2;
    if (next >= kKbdBufEnd) next = kKbdBufStart;
    if (next == mem.read16(kBda, kKbdHead)) return;  // full
    mem.write16(kBda, tail, key);
    mem.write16(kBda, kKbdTail, next);
    cpu.halted = false;
}

bool Machine::key_available() const {
    return mem.read16(kBda, kKbdHead) != mem.read16(kBda, kKbdTail);
}

bool Machine::key_pop(uint16_t* key) {
    uint16_t head = mem.read16(kBda, kKbdHead);
    if (head == mem.read16(kBda, kKbdTail)) return false;
    *key = mem.read16(kBda, head);
    head += 2;
    if (head >= kKbdBufEnd) head = kKbdBufStart;
    mem.write16(kBda, kKbdHead, head);
    return true;
}

void Machine::block_and_retry() {
    cpu.regs.ip -= 4;          // back onto the FE 38 trap in the stub
    cpu.set_flag(IF, true);
    cpu.halted = true;
}

void Machine::setup_ports() {
    // PIT channel 0: counts down at 1.193182 MHz in virtual time.
    port_out[0x43] = [this](uint8_t v) {
        if ((v >> 6) != 0) return;            // only channel 0 is modelled
        if (((v >> 4) & 3) == 0) {            // counter latch command
            pit_latched_ = true;
            pit_latch_value_ = pit_count();
            pit_read_phase_ = 0;
            return;
        }
        pit_write_phase_ = 0;
        pit_read_phase_ = 0;
    };
    port_out[0x40] = [this](uint8_t v) {
        if (pit_write_phase_ == 0) {
            pit_latch_lo_ = v;
            pit_write_phase_ = 1;
        } else {
            pit_reload_ = uint16_t(pit_latch_lo_ | (v << 8));
            pit_write_phase_ = 0;
            pit_base_us_ = now_us();
            next_tick_us_ = pit_base_us_ + tick_period_us();
            log("PIT ch0 reload %u (%.1f Hz)\n", pit_reload_, 1193182.0 / (pit_reload_ ? pit_reload_ : 65536));
        }
    };
    port_in[0x40] = [this]() -> uint8_t {
        uint16_t v = pit_latched_ ? pit_latch_value_ : pit_count();
        uint8_t out = pit_read_phase_ == 0 ? uint8_t(v) : uint8_t(v >> 8);
        if (pit_read_phase_ == 1) pit_latched_ = false;
        pit_read_phase_ ^= 1;
        return out;
    };
    port_out[0x42] = [](uint8_t) {};
    port_in[0x61] = []() -> uint8_t { return 0x00; };
    port_out[0x61] = [](uint8_t) {};
    // PIC.
    // Non-specific EOI clears the highest-priority request in service.
    port_out[0x20] = [this](uint8_t v) {
        if (v != 0x20) return;
        if (irq0_in_service_) irq0_in_service_ = false;
        else irq1_in_service_ = false;
    };
    port_in[0x21] = [this]() -> uint8_t { return pic_mask_; };
    port_out[0x21] = [this](uint8_t v) { pic_mask_ = v; };
    // Keyboard controller data: last scan code.
    port_in[0x60] = [this]() -> uint8_t { return port60_; };
    port_in[0x64] = []() -> uint8_t { return 0x14; };  // controller status: no data pending
    // VGA input status (mode 13h timing: 70.086 Hz, 449 lines of 31.778 us,
    // 400 visible). Bit 0: display disabled (hblank or vblank); bit 3:
    // vertical retrace (lines 412-413).
    port_in[0x3DA] = [this]() -> uint8_t {
        // Use sub-microsecond time so line-level toggles are visible.
        uint64_t ns = cpu.instructions * 1000000 / ips_per_ms;
        uint64_t frame_ns = uint64_t(1e9 / vga_refresh_hz);
        uint64_t line_ns = frame_ns / 449;
        uint64_t t = ns % frame_ns;
        uint32_t line = uint32_t(t / line_ns);
        uint32_t in_line = uint32_t(t % line_ns);
        bool vretrace = line >= 412 && line < 414;
        bool disabled = line >= 400 || in_line >= line_ns * 4 / 5;
        return uint8_t((disabled ? 0x01 : 0) | (vretrace ? 0x08 : 0));
    };
    // CGA/sequencer writes from the drivers: accept silently.
    port_out[0x3D8] = [](uint8_t) {};
    port_out[0x3C4] = [this](uint8_t v) { seq_index_ = v; };
    port_out[0x3C5] = [this](uint8_t v) { seq_[seq_index_ & 7] = v; };
    port_in[0x3C5] = [this]() -> uint8_t { return seq_[seq_index_ & 7]; };
    // VGA DAC.
    port_out[0x3C8] = [this](uint8_t v) { dac_write_index_ = v; dac_write_phase_ = 0; };
    port_out[0x3C7] = [this](uint8_t v) { dac_read_index_ = v; dac_read_phase_ = 0; };
    port_out[0x3C9] = [this](uint8_t v) {
        dac[dac_write_index_][dac_write_phase_] = v & 0x3F;
        if (++dac_write_phase_ == 3) { dac_write_phase_ = 0; dac_write_index_++; }
    };
    port_in[0x3C9] = [this]() -> uint8_t {
        uint8_t v = dac[dac_read_index_][dac_read_phase_];
        if (++dac_read_phase_ == 3) { dac_read_phase_ = 0; dac_read_index_++; }
        return v;
    };
    // Joystick: no joystick (one-shots never time out, buttons released).
    port_in[0x201] = []() -> uint8_t { return 0xFF; };
    port_out[0x201] = [](uint8_t) {};
}

uint16_t Machine::pit_count() const {
    uint64_t ticks = (now_us() - pit_base_us_) * 1193182 / 1000000;
    uint32_t period = pit_reload_ ? pit_reload_ : 65536;
    return uint16_t(period - ticks % period);
}

void Machine::service_timer() {
    uint64_t t = now_us();
    if (t < next_tick_us_) return;
    next_tick_us_ = t + tick_period_us();
    if (irq0_in_service_ || (pic_mask_ & 1)) return;
    if (cpu.irq(0x08)) irq0_in_service_ = true;
}

void Machine::run(uint64_t budget) {
    uint64_t end = cpu.instructions + budget;
    while (!exited && cpu.instructions < end) {
        if (cpu.halted) {
            // Idle: advance virtual time to the next timer tick.
            cpu.instructions += 100;
            cpu.halted = cpu.halted && !cpu.flag(IF);
        } else {
            cpu.step();
        }
        if (cpu.instructions - last_service_ >= 64) {
            last_service_ = cpu.instructions;
            service_timer();
            service_keyboard();
        }
    }
}

}  // namespace f19
