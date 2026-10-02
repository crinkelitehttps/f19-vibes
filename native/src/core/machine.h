// A minimal PC around the interpreter: memory map, interrupt vector stubs
// into native code, PIT/PIC timing, keyboard buffer, I/O port dispatch.
// DOS and BIOS services are implemented natively (dos.cpp, bios.cpp).
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/cpu.h"

namespace f19 {

class Dos;

// Segment holding the native-callback stubs (one per vector / service).
constexpr uint16_t kStubSeg = 0xF000;

class Machine {
public:
    explicit Machine(std::filesystem::path game_dir);
    ~Machine();

    Memory mem;
    Cpu cpu{mem};
    std::unique_ptr<Dos> dos;

    // Virtual time: instructions per millisecond of emulated time.
    uint32_t ips_per_ms = 4000;
    uint64_t now_us() const { return cpu.instructions * 1000 / ips_per_ms; }

    // Logging of service calls (DOS/BIOS); set by the host.
    bool trace = false;
    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    // Native callbacks. register_callback returns an id; make_stub writes
    // FE 38 id + `tail` bytes into the stub segment and returns its offset.
    using Callback = std::function<void()>;
    uint16_t register_callback(Callback cb, std::string name);
    uint16_t make_stub(uint16_t id, std::initializer_list<uint8_t> tail);
    void set_vector(uint8_t n, uint16_t seg, uint16_t off);
    // Point interrupt n at a native handler ending in IRET.
    void hook_interrupt(uint8_t n, Callback cb, std::string name);
    // From inside an interrupt handler: change flags the IRET will restore.
    void set_return_flag(uint16_t f, bool on);

    // I/O ports: devices register handlers; unknown ports are logged once.
    std::map<uint16_t, std::function<uint8_t()>> port_in;
    std::map<uint16_t, std::function<void(uint8_t)>> port_out;

    // VGA DAC (6-bit RGB per entry) - used by the emulated-hardware path.
    uint8_t dac[256][3] = {};
    // Text mode (B800) as a printable string, for headless bring-up.
    std::string text_screen();
    std::function<void(uint8_t mode)> on_mode_set;
    // Keyboard: host pushes (scan, ascii); BIOS INT 16h consumes.
    void key_press(uint8_t scan, uint8_t ascii);
    // Take the next key from the BIOS buffer (ascii | scan << 8), if any.
    bool key_pop(uint16_t* key);
    bool key_available() const;
    // Inside a native interrupt handler: make the INT re-execute later
    // (blocking calls), letting timer interrupts run meanwhile.
    void block_and_retry();

    // Run until `budget` instructions have executed or the program exits.
    void run(uint64_t budget);
    bool exited = false;
    int exit_code = 0;
    std::string stop_reason;

private:
    struct CallbackEntry { Callback fn; std::string name; };
    std::vector<CallbackEntry> callbacks_;
    uint8_t dac_write_index_ = 0, dac_read_index_ = 0;
    int dac_write_phase_ = 0, dac_read_phase_ = 0;
    uint16_t stub_next_ = 0x0100;

    // PIT channel 0 / PIC.
    uint16_t pit_reload_ = 0;       // 0 means 65536
    int pit_write_phase_ = 0;
    uint8_t pit_latch_lo_ = 0;
    uint64_t next_tick_us_ = 0;
    bool irq0_in_service_ = false;
    uint8_t pic_mask_ = 0;
    uint64_t tick_period_us() const { return uint64_t(pit_reload_ ? pit_reload_ : 65536) * 1000000 / 1193182; }
    void service_timer();

    std::map<uint32_t, bool> unknown_ports_logged_;   // key: port | 0x10000 for OUT
    void setup_ports();
    void int10();
    void text_put(uint8_t ch, uint8_t attr, bool use_attr);
    void text_scroll(int lines, uint8_t attr, int top, int left, int bottom, int right, bool up);
};

}  // namespace f19
