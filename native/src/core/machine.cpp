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
    hook_interrupt(0x11, [this] { cpu.regs.r[AX] = mem.read16(kBda, kEquipment); }, "int11");
    hook_interrupt(0x12, [this] { cpu.regs.r[AX] = 640; }, "int12");

    // INT 10h video (minimal; drivers do the real work).
    hook_interrupt(0x10, [this] {
        uint8_t ah = cpu.regs.r8(4), al = cpu.regs.r8(0);
        switch (ah) {
            case 0x00:
                log("INT 10 set mode %02X\n", al);
                mem.write8(Memory::linear(kBda, kVideoMode), al & 0x7F);
                break;
            case 0x0F:
                cpu.regs.r8(0) = mem.read8(Memory::linear(kBda, kVideoMode));
                cpu.regs.r8(4) = 80;
                cpu.regs.r8(7) = 0;
                break;
            case 0x1A:  // display combination: VGA colour
                if (al == 0) { cpu.regs.r8(0) = 0x1A; cpu.regs.r[BX] = 0x0008; }
                break;
            case 0x12:  // EGA info: 256K, colour
                if (cpu.regs.r8(3) == 0x10) { cpu.regs.r[BX] = 0x0003; cpu.regs.r[CX] = 0x0009; }
                break;
            case 0x0E: case 0x09: case 0x0A:
                log("INT 10 char %02X '%c'\n", al, al >= 32 && al < 127 ? al : '.');
                break;
            default:
                log("INT 10 AH=%02X AL=%02X\n", ah, al);
                break;
        }
    }, "int10");

    // INT 16h keyboard via the BDA ring buffer.
    hook_interrupt(0x16, [this] {
        uint8_t ah = cpu.regs.r8(4);
        uint16_t head = mem.read16(kBda, kKbdHead), tail = mem.read16(kBda, kKbdTail);
        bool empty = head == tail;
        switch (ah & 0xEF) {
            case 0x00:
                if (empty) {
                    // Blocking read: rewind to re-execute INT 16h (the stub's trap)
                    // after time passes, letting interrupts run.
                    cpu.regs.ip -= 4;
                    cpu.set_flag(IF, true);
                    cpu.halted = true;
                    return;
                }
                cpu.regs.r[AX] = mem.read16(kBda, head);
                head += 2;
                if (head >= kKbdBufEnd) head = kKbdBufStart;
                mem.write16(kBda, kKbdHead, head);
                break;
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

void Machine::log(const char* fmt, ...) {
    if (!trace) return;
    std::fprintf(stderr, "[%10llu] ", (unsigned long long)cpu.instructions);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
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

void Machine::key_press(uint8_t scan, uint8_t ascii) {
    uint16_t tail = mem.read16(kBda, kKbdTail);
    uint16_t next = tail + 2;
    if (next >= kKbdBufEnd) next = kKbdBufStart;
    if (next == mem.read16(kBda, kKbdHead)) return;  // full
    mem.write16(kBda, tail, uint16_t(ascii | (scan << 8)));
    mem.write16(kBda, kKbdTail, next);
    cpu.halted = false;
}

void Machine::setup_ports() {
    // PIT.
    port_out[0x43] = [this](uint8_t v) {
        if ((v >> 6) == 0) pit_write_phase_ = 0;  // channel 0 mode set
        log("PIT mode %02X\n", v);
    };
    port_out[0x40] = [this](uint8_t v) {
        if (pit_write_phase_ == 0) {
            pit_latch_lo_ = v;
            pit_write_phase_ = 1;
        } else {
            pit_reload_ = uint16_t(pit_latch_lo_ | (v << 8));
            pit_write_phase_ = 0;
            log("PIT ch0 reload %u (%.1f Hz)\n", pit_reload_, 1193182.0 / (pit_reload_ ? pit_reload_ : 65536));
        }
    };
    port_in[0x40] = [this]() -> uint8_t { return uint8_t(now_us()); };
    port_out[0x42] = [](uint8_t) {};
    port_in[0x61] = []() -> uint8_t { return 0x00; };
    port_out[0x61] = [](uint8_t) {};
    // PIC.
    port_out[0x20] = [this](uint8_t v) { if (v == 0x20) irq0_in_service_ = false; };
    port_in[0x21] = [this]() -> uint8_t { return pic_mask_; };
    port_out[0x21] = [this](uint8_t v) { pic_mask_ = v; };
    // Keyboard controller data: last scan code.
    port_in[0x60] = []() -> uint8_t { return 0; };
    // VGA input status: toggle retrace (bit 3) and display enable (bit 0).
    port_in[0x3DA] = [this]() -> uint8_t {
        uint64_t t = now_us() % 14286;  // ~70 Hz frame
        return t < 1000 ? 0x09 : 0x00;
    };
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
        if ((cpu.instructions & 63) == 0) service_timer();
    }
}

}  // namespace f19
