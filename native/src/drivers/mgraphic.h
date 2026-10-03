// Native replacement for MGRAPHIC.EXE, F-19's VGA/MCGA (mode 13h) driver.
//
// The original driver stays loaded; its state lives where the original keeps
// it (driver data segment D = load segment, code segment C = entry segment),
// so native and original routines can be mixed while the port is in
// progress. Each native slot reproduces the original's memory effects and
// register side effects (the 3D engine calls some slots from assembly).
//
// Verify mode runs both versions on every call and reports differences: the
// native result is computed from a snapshot, the original then runs to its
// RETF, and memory + registers are compared. Execution continues from the
// original's result.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace f19 {

class Machine;

class MGraphicNative {
public:
    explicit MGraphicNative(Machine& m);

    bool verify = false;          // compare every native call against the original
    bool enabled = true;          // false: install nothing (original driver only)
    uint64_t verify_every = 1;    // verify 1 in N calls (others run native only)

    struct SlotStats { uint64_t calls = 0, verified = 0, mismatches = 0; std::vector<std::string> examples; };
    std::map<int, SlotStats> stats;
    std::string report() const;

    // For the high-resolution renderer.
    std::function<bool()> is_world_call;   // decides the write tag for a call
    std::function<void()> on_flip;         // before slot 44 copies page 1 -> 0
    std::vector<uint64_t> flip_times_us;   // emulated time of each flip
    uint16_t current_color() const { return color(); }
    uint16_t page_seg(int i) const { return page(uint16_t(i)); }
    uint16_t current_origin() const { return origin(); }
    uint16_t current_draw_seg() const { return draw_seg(); }
    int current_slot() const { return current_slot_; }
    bool attached() const { return cs_ != 0; }

private:
    Machine& m_;
    uint16_t ds_ = 0;  // driver data segment (load segment)
    uint16_t cs_ = 0;  // driver code segment (relocated header word +0x18)

    using Fn = void (MGraphicNative::*)();
    std::map<int, Fn> impl_;
    void attach(uint16_t load_seg);
    bool dispatch(int slot);
    void run_verified(int slot, Fn fn);

    // Memory helpers.
    uint16_t c16(uint16_t off) const;
    void c16(uint16_t off, uint16_t v);
    uint8_t c8(uint16_t off) const;
    void c8(uint16_t off, uint8_t v);
    uint16_t d16(uint16_t off) const;
    void d16(uint16_t off, uint16_t v);
    uint8_t d8(uint16_t off) const;
    void d8(uint16_t off, uint8_t v);
    uint16_t arg(int i) const;     // i-th word after the far return address
    void retf();
    uint16_t row(uint16_t y) const { return c16(uint16_t(0x0C + 2 * y)); }  // cs:row table
    uint16_t page(uint16_t i) const { return c16(uint16_t(0x67F + 2 * i)); }
    uint16_t draw_seg() const { return c16(0x19C); }
    uint16_t origin() const { return c16(0x19E); }
    uint8_t color() const { return d8(0x1B7A); }

    // Slots.
    void s_retf();
    void s_const();               // constant getters (value chosen by slot)
    int current_slot_ = -1;
    void s11_gauge();
    void s12_select_page1();
    void s13_select_page_ax();
    void s14_select_page();
    void s15_set_draw_seg();
    void s16_get_draw_seg();
    void s24_origin_zero();
    void s26_set_origin();
    void s27_set_origin_ax();
    void s28_const5580();
    void s29_const1950();
    void s30_get_origin();
    void s31_line();
    void s32_color_ah();
    void s33_color();
    void s36_plot();
    void s37_spans();
    void s38_set_cc_ax();
    void s39_set_ce_ax();
    void s41_recolor_rect();
    void s42_copy_rect();
    void s43_clear_screen();
    void s44_flip();
    void s45_get_flip_flag();
    void s46_flicker();
    void s47_char_width();
    void s48_copy_page_from();
    void s51_copy_row();
    void s56_page_es();
    void s57_set_page();
    void s58_row_offset_di();
    void s59_clear_es();
    void s62_offset_xy();
    void s63_const3();
    void s64_set_cc();
    void s65_set_ce();
    void s69_screen_off();
    void s70_screen_on();
    void s73_blit_ptr();
    void s74_blit();
    void s71_blit_clip_ptr();
    void s72_blit_clip();
    void s75_set_page();
    void s79_set_shake();

    // Text (slots 1-10).
    void s1_text_vclip();
    void s2_text_lclip();
    void s3_text_rclip();
    void s4_text();
    void s6_text_clip();
    void s5_text_w();
    void s7_text_vclip_w();
    void s8_text_lclip_w();
    void s9_text_clip_w();
    void s10_text_rclip_w();
    struct TextRegs { uint16_t ax, bx, cx, dx, si, di; };
    // Inner text routine with BX = string (SS), BP = param block (SS).
    // `ret` = address after the setup call (left in CX); clips in order
    // left, right, vertical as selected; `clip_rets` = their return
    // addresses (left in AX on a clip exit).
    void text_inner(TextRegs& t, uint16_t bp, uint16_t setup_ret, bool lclip, bool rclip, bool vclip,
                    const uint16_t clip_rets[3]);
    void text_wrapper(void (MGraphicNative::*inner)());

    // Shared bodies.
    void plot_current();              // cs:02df + stosb
    void blit_body(uint16_t bp);      // cs:07d5 with param block at SS:bp
    void blit_clip_body(uint16_t bp); // cs:0734
};

}  // namespace f19
