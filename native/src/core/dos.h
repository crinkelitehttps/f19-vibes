// High-level emulation of the DOS services F-19 uses (INT 21h, INT 20h).
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/cpu.h"

namespace f19 {

class Machine;

class Dos {
public:
    Dos(Machine& m, std::filesystem::path game_dir);
    ~Dos();

    // Load a program (COM or MZ) and set the CPU up to run it as the
    // top-level process, as COMMAND.COM would. Returns false on error.
    bool start_program(const std::string& dos_name, const std::string& args);

    std::filesystem::path game_dir;
    uint16_t current_psp = 0;

private:
    Machine& m_;
    Cpu& cpu_;

    // Memory control blocks: arena from kFirstMcb to 0x9FFF.
    static constexpr uint16_t kFirstMcb = 0x0100;
    static constexpr uint16_t kMemTop = 0xA000;
    uint16_t alloc(uint16_t paras, uint16_t owner, uint16_t* largest = nullptr);
    bool free_block(uint16_t seg);
    bool resize(uint16_t seg, uint16_t paras, uint16_t* largest);
    void free_owned_by(uint16_t psp);
    void coalesce();

    // Processes.
    struct Process {
        Regs parent_regs;     // parent state at its EXEC call (inside the INT 21h stub)
        uint16_t psp;
    };
    std::vector<Process> procs_;
    uint8_t last_return_code_ = 0;
    uint8_t pending_scan_ = 0;   // second byte of an extended key for AH=01/06/07/08
    uint16_t build_env(const std::string& program_path);
    uint16_t make_psp(uint16_t seg, uint16_t mem_end, uint16_t env, uint16_t parent, const std::string& tail);
    // Load image file into memory. For MZ: relocates by `reloc_seg`; returns
    // entry CS:IP / SS:SP via out params. `load_seg` is where the image goes.
    bool load_image(const std::vector<uint8_t>& file, bool is_exe, uint16_t load_seg, uint16_t reloc_seg,
                    uint16_t* cs, uint16_t* ip, uint16_t* ss, uint16_t* sp);
    uint32_t image_paras(const std::vector<uint8_t>& file, bool is_exe, uint16_t* min_extra, uint16_t* max_extra);
    void exec(uint8_t mode);
    void terminate(uint8_t code);

    // Files.
    std::map<uint16_t, std::FILE*> files_;
    std::map<uint16_t, std::string> file_names_;
    uint16_t dta_seg_ = 0, dta_off_ = 0x80;
    std::vector<std::string> find_results_;
    size_t find_pos_ = 0;
    std::string read_string(uint16_t seg, uint16_t off);
    std::filesystem::path resolve(const std::string& dos_path, bool must_exist);
    void find_fill_dta();

    void int21();
    void fail(uint16_t err);
    void ok();
};

}  // namespace f19
