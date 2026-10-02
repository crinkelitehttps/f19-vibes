#include "drivers/mgraphic.h"

#include <cstdio>
#include <cstring>

#include "core/machine.h"

namespace f19 {

namespace {
constexpr uint16_t kRowTable = 0x0C;
}

MGraphicNative::MGraphicNative(Machine& m) : m_(m) {
    impl_ = {
        {1, &MGraphicNative::s1_text_vclip},
        {2, &MGraphicNative::s2_text_lclip},
        {3, &MGraphicNative::s3_text_rclip},
        {4, &MGraphicNative::s4_text},
        {5, &MGraphicNative::s5_text_w},
        {6, &MGraphicNative::s6_text_clip},
        {7, &MGraphicNative::s7_text_vclip_w},
        {8, &MGraphicNative::s8_text_lclip_w},
        {9, &MGraphicNative::s9_text_clip_w},
        {10, &MGraphicNative::s10_text_rclip_w},
        {11, &MGraphicNative::s11_gauge},
        {12, &MGraphicNative::s12_select_page1},
        {13, &MGraphicNative::s13_select_page_ax},
        {14, &MGraphicNative::s14_select_page},
        {15, &MGraphicNative::s15_set_draw_seg},
        {16, &MGraphicNative::s16_get_draw_seg},
        {17, &MGraphicNative::s73_blit_ptr},
        {18, &MGraphicNative::s74_blit},
        {19, &MGraphicNative::s71_blit_clip_ptr},
        {20, &MGraphicNative::s72_blit_clip},
        {21, &MGraphicNative::s_retf},
        {22, &MGraphicNative::s_retf},
        {23, &MGraphicNative::s_const},
        {24, &MGraphicNative::s24_origin_zero},
        {25, &MGraphicNative::s24_origin_zero},
        {26, &MGraphicNative::s26_set_origin},
        {27, &MGraphicNative::s27_set_origin_ax},
        {28, &MGraphicNative::s28_const5580},
        {29, &MGraphicNative::s29_const1950},
        {30, &MGraphicNative::s30_get_origin},
        {31, &MGraphicNative::s31_line},
        {32, &MGraphicNative::s32_color_ah},
        {33, &MGraphicNative::s33_color},
        {34, &MGraphicNative::s_retf},
        {35, &MGraphicNative::s_retf},
        {36, &MGraphicNative::s36_plot},
        {37, &MGraphicNative::s37_spans},
        {38, &MGraphicNative::s38_set_cc_ax},
        {39, &MGraphicNative::s39_set_ce_ax},
        {40, &MGraphicNative::s37_spans},
        {41, &MGraphicNative::s41_recolor_rect},
        {42, &MGraphicNative::s42_copy_rect},
        {43, &MGraphicNative::s43_clear_screen},
        {44, &MGraphicNative::s44_flip},
        {45, &MGraphicNative::s45_get_flip_flag},
        {46, &MGraphicNative::s46_flicker},
        {47, &MGraphicNative::s47_char_width},
        {48, &MGraphicNative::s48_copy_page_from},
        {49, &MGraphicNative::s_const},
        {51, &MGraphicNative::s51_copy_row},
        {52, &MGraphicNative::s51_copy_row},
        {53, &MGraphicNative::s_retf},
        {54, &MGraphicNative::s_retf},
        {55, &MGraphicNative::s_retf},
        {56, &MGraphicNative::s56_page_es},
        {57, &MGraphicNative::s57_set_page},
        {58, &MGraphicNative::s58_row_offset_di},
        {59, &MGraphicNative::s59_clear_es},
        {61, &MGraphicNative::s_retf},
        {62, &MGraphicNative::s62_offset_xy},
        {63, &MGraphicNative::s63_const3},
        {64, &MGraphicNative::s64_set_cc},
        {65, &MGraphicNative::s65_set_ce},
        {66, &MGraphicNative::s_const},
        {67, &MGraphicNative::s_const},
        {69, &MGraphicNative::s69_screen_off},
        {70, &MGraphicNative::s70_screen_on},
        {71, &MGraphicNative::s71_blit_clip_ptr},
        {72, &MGraphicNative::s72_blit_clip},
        {73, &MGraphicNative::s73_blit_ptr},
        {74, &MGraphicNative::s74_blit},
        {75, &MGraphicNative::s75_set_page},
        {76, &MGraphicNative::s_const},
        {77, &MGraphicNative::s_const},
        {78, &MGraphicNative::s_const},
        {79, &MGraphicNative::s79_set_shake},
        {80, &MGraphicNative::s_retf},
        {81, &MGraphicNative::s_retf},
        {82, &MGraphicNative::s_retf},
        {83, &MGraphicNative::s_retf},
    };
    m_.overlay_listeners.push_back([this](const std::string& name, uint16_t seg) {
        if (enabled && name == "MGRAPHIC.EXE") attach(seg);
    });
}

void MGraphicNative::attach(uint16_t load_seg) {
    ds_ = load_seg;
    cs_ = m_.mem.read16(load_seg, 0x18);  // relocated at load
    uint16_t n = m_.mem.read16(load_seg, 0x22);
    uint8_t first = m_.mem.read8(Memory::linear(load_seg, 0x1C));
    std::map<uint32_t, int> seen;
    for (int i = 0; i < n; i++) {
        int slot = first + i;
        if (!impl_.count(slot)) continue;
        uint32_t lin = Memory::linear(cs_, m_.mem.read16(load_seg, uint16_t(0x24 + 2 * i)));
        if (seen.count(lin)) continue;  // aliases share one routine
        seen[lin] = slot;
        m_.add_breakpoint(lin, [this, slot] { return dispatch(slot); });
    }
    m_.log("native MGRAPHIC attached: data %04X code %04X, %zu entry points\n", ds_, cs_, seen.size());
}

// ------------------------------------------------------------ helpers

uint16_t MGraphicNative::c16(uint16_t off) const { return m_.mem.read16(cs_, off); }
void MGraphicNative::c16(uint16_t off, uint16_t v) { m_.mem.write16(cs_, off, v); }
uint8_t MGraphicNative::c8(uint16_t off) const { return m_.mem.read8(Memory::linear(cs_, off)); }
void MGraphicNative::c8(uint16_t off, uint8_t v) { m_.mem.write8(Memory::linear(cs_, off), v); }
uint16_t MGraphicNative::d16(uint16_t off) const { return m_.mem.read16(ds_, off); }
void MGraphicNative::d16(uint16_t off, uint16_t v) { m_.mem.write16(ds_, off, v); }
uint8_t MGraphicNative::d8(uint16_t off) const { return m_.mem.read8(Memory::linear(ds_, off)); }
void MGraphicNative::d8(uint16_t off, uint8_t v) { m_.mem.write8(Memory::linear(ds_, off), v); }

uint16_t MGraphicNative::arg(int i) const {
    const Regs& r = m_.cpu.regs;
    return m_.mem.read16(r.s[SS], uint16_t(r.r[SP] + 4 + 2 * i));
}

void MGraphicNative::retf() {
    Regs& r = m_.cpu.regs;
    r.ip = m_.cpu.pop();
    r.s[CS] = m_.cpu.pop();
}

// ------------------------------------------------------------ dispatch / verify

bool MGraphicNative::dispatch(int slot) {
    auto it = impl_.find(slot);
    if (it == impl_.end()) return false;
    SlotStats& st = stats[slot];
    st.calls++;
    current_slot_ = slot;
    if (slot == 44 && on_flip) on_flip();
    uint8_t tag = (is_world_call && is_world_call()) ? 1 : 0;
    m_.mem.watch_tag = tag;
    if (verify && (st.calls % verify_every) == 0) {
        run_verified(slot, it->second);
    } else {
        (this->*(it->second))();
    }
    m_.mem.watch_tag = 0;
    return true;
}

void MGraphicNative::run_verified(int slot, Fn fn) {
    Memory& mem = m_.mem;
    Cpu& cpu = m_.cpu;
    SlotStats& st = stats[slot];
    st.verified++;
    using Journal = std::vector<std::pair<uint32_t, uint8_t>>;

    // Native run, journalled, then undone.
    Regs before = cpu.regs;
    Journal jn;
    mem.journal = &jn;
    (this->*fn)();
    mem.journal = nullptr;
    Regs native = cpu.regs;
    std::map<uint32_t, uint8_t> native_vals;
    for (auto& [a, old] : jn) native_vals[a] = mem.read8(a);
    for (auto it = jn.rbegin(); it != jn.rend(); ++it) mem.data()[it->first] = it->second;

    // Original run from the same state until it returns.
    cpu.regs = before;
    uint16_t ret_ip = mem.read16(before.s[SS], before.r[SP]);
    uint16_t ret_cs = mem.read16(before.s[SS], uint16_t(before.r[SP] + 2));
    uint16_t ret_sp = uint16_t(before.r[SP] + 4);
    Journal jo;
    mem.journal = &jo;
    m_.breakpoints_suspended = true;
    bool returned = false;
    for (int n = 0; n < 20000000; n++) {
        cpu.step();
        if (cpu.regs.s[CS] == ret_cs && cpu.regs.ip == ret_ip && cpu.regs.r[SP] == ret_sp) {
            returned = true;
            break;
        }
    }
    m_.breakpoints_suspended = false;
    mem.journal = nullptr;

    // Pre-call value of every address either side wrote (first journal entry).
    std::map<uint32_t, uint8_t> pre;
    for (auto& [a, old] : jo) pre.emplace(a, old);
    for (auto& [a, old] : jn) pre.emplace(a, old);

    std::string why;
    char b[200];
    if (!returned) why = "original did not return";
    static const char* names[] = {"AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"};
    for (int i = 0; returned && i < 8; i++) {
        if (native.r[i] != cpu.regs.r[i]) {
            std::snprintf(b, sizeof b, "%s native %04X original %04X; ", names[i], native.r[i], cpu.regs.r[i]);
            why += b;
        }
    }
    static const char* snames[] = {"ES", "CS", "SS", "DS"};
    for (int i = 0; i < 4; i++)
        if (native.s[i] != cpu.regs.s[i]) {
            std::snprintf(b, sizeof b, "%s native %04X original %04X; ", snames[i], native.s[i], cpu.regs.s[i]);
            why += b;
        }
    if (native.ip != cpu.regs.ip) why += "IP differs; ";
    if ((native.flags ^ cpu.regs.flags) & DF) why += "DF differs; ";

    // Compare every written address, ignoring free stack space just below SP
    // (the original pushes temporaries there, the native code does not).
    uint32_t stack_hi = Memory::linear(cpu.regs.s[SS], cpu.regs.r[SP]);
    uint32_t stack_lo = stack_hi >= 0x200 ? stack_hi - 0x200 : 0;
    int diffs = 0;
    uint32_t first = 0;
    uint8_t first_n = 0, first_o = 0;
    for (auto& [a, old] : pre) {
        if (a >= stack_lo && a < stack_hi) continue;
        auto nv = native_vals.find(a);
        uint8_t n = nv != native_vals.end() ? nv->second : old;
        uint8_t o = mem.read8(a);
        if (n != o) {
            if (!diffs) { first = a; first_n = n; first_o = o; }
            diffs++;
        }
    }
    if (diffs) {
        std::snprintf(b, sizeof b, "memory: %d bytes differ, first %05X native %02X original %02X; ", diffs, first, first_n, first_o);
        why += b;
    }
    if (!why.empty()) {
        st.mismatches++;
        if (st.examples.size() < 3) {
            std::snprintf(b, sizeof b, "[AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X args %04X %04X %04X %04X] ",
                          before.r[AX], before.r[BX], before.r[CX], before.r[DX], before.r[SI], before.r[DI], before.r[BP],
                          mem.read16(before.s[SS], uint16_t(before.r[SP] + 4)), mem.read16(before.s[SS], uint16_t(before.r[SP] + 6)),
                          mem.read16(before.s[SS], uint16_t(before.r[SP] + 8)), mem.read16(before.s[SS], uint16_t(before.r[SP] + 10)));
            st.examples.push_back(b + why);
        }
    }
    // Execution continues from the original's result.
}

std::string MGraphicNative::report() const {
    std::string out;
    char b[160];
    for (auto& [slot, st] : stats) {
        std::snprintf(b, sizeof b, "slot %2d: %9llu calls, %9llu verified, %6llu mismatches\n", slot,
                      (unsigned long long)st.calls, (unsigned long long)st.verified, (unsigned long long)st.mismatches);
        out += b;
        for (auto& e : st.examples) out += "    " + e + "\n";
    }
    return out;
}

// ------------------------------------------------------------ slots

void MGraphicNative::s_retf() { retf(); }

void MGraphicNative::s_const() {
    static const std::map<int, uint16_t> where = {{23, 0x1D0}, {49, 0x1D2}, {66, 0x1D4}, {67, 0x1D6},
                                                  {76, 0x1D8}, {77, 0x1DA}, {78, 0x1DC}};
    m_.cpu.regs.r[AX] = c16(where.at(current_slot_));
    retf();
}

// cs:0613 - vertical bar gauge (inputs SI, BX, DL, CL).
void MGraphicNative::s11_gauge() {
    Regs& r = m_.cpu.regs;
    Memory& mem = m_.mem;
    uint16_t es = draw_seg();
    bool down = r.r[SI] == 0;          // STD unless SI != 0
    uint16_t bx = uint16_t(r.r[BX] - 1);
    uint16_t si = r.r[SI];
    if (int8_t(r.r8(2)) >= 1) bx += 0x14;
    uint8_t cl = r.r8(1);
    if (cl) { si += 4; bx++; }
    uint8_t al = 0x0F;
    uint16_t dx = d16(uint16_t(si + 0x1C52));
    uint8_t ch = 0x0A;
    uint16_t di = r.r[DI];
    auto stosb = [&] {
        mem.write8(Memory::linear(es, di), al);
        di = down ? uint16_t(di - 1) : uint16_t(di + 1);
    };
    for (;;) {
        if (bx < d16(uint16_t(si + 0x1C5A))) break;
        if (!(bx > d16(uint16_t(si + 0x1C62)))) {
            di = uint16_t(row(bx) + dx);
            if (ch == 5) { stosb(); stosb(); }
            else if (ch == 0x0A) { stosb(); stosb(); if (!cl) stosb(); }
            else stosb();
        }
        bx -= 2;
        if (ch == 0x0A) ch = 0;
        ch++;
    }
    r.r[AX] = 0x0F0F;
    r.r[BX] = bx;
    r.r8(5) = ch;  // CH
    r.r[DX] = dx;
    r.r[SI] = si;
    r.r[DI] = di;
    m_.cpu.set_flag(DF, false);
    retf();
}

void MGraphicNative::s12_select_page1() {
    m_.cpu.regs.r[AX] = page(1);
    c16(0x19C, m_.cpu.regs.r[AX]);
    retf();
}

void MGraphicNative::s13_select_page_ax() {
    Regs& r = m_.cpu.regs;
    r.r[AX] = page(r.r[AX]);
    c16(0x19C, r.r[AX]);
    retf();
}

void MGraphicNative::s14_select_page() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    r.r[AX] = page(arg(0));
    c16(0x19C, r.r[AX]);
    retf();
}

void MGraphicNative::s15_set_draw_seg() { c16(0x19C, m_.cpu.regs.r[AX]); retf(); }
void MGraphicNative::s16_get_draw_seg() { m_.cpu.regs.r[AX] = draw_seg(); retf(); }
void MGraphicNative::s24_origin_zero() { c16(0x19E, 0); retf(); }
void MGraphicNative::s26_set_origin() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    r.r[AX] = arg(0);
    c16(0x19E, r.r[AX]);
    retf();
}
void MGraphicNative::s27_set_origin_ax() { c16(0x19E, m_.cpu.regs.r[AX]); retf(); }
void MGraphicNative::s28_const5580() { m_.cpu.regs.r[AX] = 0x5580; retf(); }
void MGraphicNative::s29_const1950() { m_.cpu.regs.r[AX] = 0x1950; retf(); }
void MGraphicNative::s30_get_origin() { m_.cpu.regs.r[AX] = origin(); retf(); }

// cs:02df then STOSB: plot the current colour at the current point.
void MGraphicNative::plot_current() {
    Regs& r = m_.cpu.regs;
    r.r8(0) = color();
    r.r[DI] = uint16_t(row(d16(0x1B70)) + origin() + d16(0x1B6E));
    m_.mem.write8(Memory::linear(draw_seg(), r.r[DI]), r.r8(0));
    r.r[DI]++;
}

// cs:02f7 - line (AX,BX)-(CX,DX) in the current colour.
void MGraphicNative::s31_line() {
    Regs& r = m_.cpu.regs;
    uint16_t ax = r.r[AX], bx = r.r[BX], cx = r.r[CX], dx = r.r[DX];
    bool equal_x = ax == cx;
    if (ax > cx) { std::swap(ax, cx); std::swap(dx, bx); }
    d16(0x1B6E, ax);
    d16(0x1B70, bx);
    d16(0x1B72, cx);
    d16(0x1B74, dx);
    r.r[AX] = ax; r.r[BX] = bx; r.r[CX] = cx; r.r[DX] = dx;
    if (equal_x && bx == dx) {  // single point
        plot_current();
        retf();
        return;
    }
    uint16_t si = 1, bp = 0x140;
    cx = uint16_t(cx - ax);
    dx = uint16_t(dx - bx);
    if (int16_t(dx) < 0) { bp = uint16_t(-bp); dx = uint16_t(-dx); }
    if (cx < dx) { std::swap(bp, si); std::swap(dx, cx); }
    d16(0x1B76, cx);
    d16(0x1B78, dx);
    plot_current();
    r.r[DI]--;  // plot_current includes the STOSB; the loop below starts with it
    uint16_t es = draw_seg();
    uint8_t al = r.r8(0);
    uint16_t di = r.r[DI];
    bx = d16(0x1B78);
    cx = d16(0x1B76);
    dx = uint16_t(-uint16_t((cx + 1) >> 1));
    si--;
    for (;;) {
        m_.mem.write8(Memory::linear(es, di), al);
        di++;
        cx--;
        if (int16_t(cx) < 0) break;
        di += si;
        dx += bx;
        if (int16_t(dx) < 0) continue;
        dx -= d16(0x1B76);
        di += bp;
    }
    r.r[BX] = bx; r.r[CX] = cx; r.r[DX] = dx; r.r[SI] = si; r.r[BP] = bp; r.r[DI] = di;
    retf();
}

void MGraphicNative::s32_color_ah() {
    Regs& r = m_.cpu.regs;
    d8(0x1B7A, r.r8(4));
    r.r[DX] = ds_;
    retf();
}

void MGraphicNative::s33_color() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    r.r8(4) = uint8_t(arg(0));
    d8(0x1B7A, r.r8(4));
    r.r[DX] = ds_;
    retf();
}

void MGraphicNative::s36_plot() {
    Regs& r = m_.cpu.regs;
    r.r[AX] = ds_;
    plot_current();
    retf();
}

// cs:082a - fill polygon spans for rows AX..CX; SS:BX left x table,
// SS:BX+0x1B8 right x table (one word per row).
void MGraphicNative::s37_spans() {
    Regs& r = m_.cpu.regs;
    Memory& mem = m_.mem;
    uint16_t ss = r.s[SS];
    r.r[DX] = ds_;
    if (int16_t(r.r[AX]) < 0) { retf(); return; }
    uint16_t es = draw_seg();
    uint16_t dx = uint16_t(r.r[AX] << 1);
    uint16_t si = uint16_t(r.r[CX] << 1);
    uint16_t bx = r.r[BX];
    uint8_t al = color();
    uint16_t ax = uint16_t(al | (al << 8));
    uint16_t bp = r.r[BP], cx = r.r[CX], di = r.r[DI];
    uint16_t org = origin();
    for (;;) {
        bp = mem.read16(ss, uint16_t(bx + si));
        cx = mem.read16(ss, uint16_t(bx + si + 0x1B8));
        bool draw = true;
        if (cx < bp) draw = false;
        else if (cx == bp && (cx == 0 || cx == 0x13F)) draw = false;
        if (draw) {
            cx = uint16_t(cx - bp + 1);
            di = uint16_t(c16(uint16_t(si + kRowTable)) + org + bp);
            bool done = false;
            if (di & 1) {
                mem.write8(Memory::linear(es, di), al);
                di++;
                cx--;
                done = cx == 0;
            }
            if (!done) {
                bool odd = cx & 1;
                cx >>= 1;
                for (; cx; cx--) { mem.write16(es, di, ax); di += 2; }
                if (odd) { mem.write8(Memory::linear(es, di), al); di++; }
            }
        }
        si -= 2;
        if (!(int16_t(si) >= int16_t(dx))) break;
    }
    r.r[AX] = ax; r.r[CX] = cx; r.r[DX] = dx; r.r[SI] = si; r.r[BP] = bp; r.r[DI] = di;
    retf();
}

void MGraphicNative::s38_set_cc_ax() { d16(0xCC, m_.cpu.regs.r[AX]); m_.cpu.regs.r[DX] = ds_; retf(); }
void MGraphicNative::s39_set_ce_ax() { d16(0xCE, m_.cpu.regs.r[AX]); m_.cpu.regs.r[DX] = ds_; retf(); }
void MGraphicNative::s64_set_cc() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    r.r[AX] = arg(0);
    d16(0xCC, r.r[AX]);
    r.r[DX] = ds_;
    retf();
}
void MGraphicNative::s65_set_ce() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    r.r[AX] = arg(0);
    d16(0xCE, r.r[AX]);
    r.r[DX] = ds_;
    retf();
}

// cs:08f9 - replace colour `from` with `to` in a rectangle of a page.
void MGraphicNative::s41_recolor_rect() {
    Regs& r = m_.cpu.regs;
    Memory& mem = m_.mem;
    uint16_t pidx = mem.read16(r.s[SS], arg(0));
    uint16_t es = page(pidx);
    uint16_t si = arg(2), cx = uint16_t(arg(4) - si + 1);
    si = uint16_t(si << 1);
    uint16_t x1 = arg(1), bx = uint16_t(arg(3) - x1 + 1);
    uint8_t from = uint8_t(arg(5)), to = uint8_t(arg(6));
    do {
        uint16_t di = uint16_t(x1 + c16(uint16_t(si + kRowTable)));
        uint16_t n = bx;
        do {
            uint32_t a = Memory::linear(es, di);
            if (mem.read8(a) == from) mem.write8(a, to);
            di++;
        } while (--n);
        si += 2;
    } while (--cx);
    r.r[AX] = uint16_t(to | (from << 8));
    r.r[BX] = bx;
    r.r[CX] = 0;
    r.r[DX] = ds_;
    retf();
}

// cs:08a2 - copy a rectangle between pages.
void MGraphicNative::s42_copy_rect() {
    Regs& r = m_.cpu.regs;
    Memory& mem = m_.mem;
    uint16_t src = page(arg(0)), sx = arg(1), sy2 = uint16_t(arg(2) << 1);
    uint16_t dst = page(arg(3)), dx = arg(4), dy2 = uint16_t(arg(5) << 1);
    uint16_t w = arg(6), cx = arg(7);
    do {
        uint16_t s = uint16_t(c16(uint16_t(sy2 + kRowTable)) + sx);
        uint16_t d = uint16_t(c16(uint16_t(dy2 + kRowTable)) + dx);
        for (uint16_t n = w; n; n--) mem.write8(Memory::linear(dst, d++), mem.read8(Memory::linear(src, s++)));
        sy2 += 2;
        dy2 += 2;
    } while (--cx);
    r.r[BX] = w;
    r.r[CX] = 0;
    r.r[DX] = dx;
    retf();
}

void MGraphicNative::s59_clear_es() {
    Regs& r = m_.cpu.regs;
    for (uint32_t i = 0; i < 0xFA00; i += 2) m_.mem.write16(r.s[ES], uint16_t(i), 0);
    r.r[AX] = 0;
    r.r[CX] = 0;
    r.r[DI] = 0xFA00;
    retf();
}

void MGraphicNative::s43_clear_screen() {
    Regs& r = m_.cpu.regs;
    for (uint32_t i = 0; i < 0xFA00; i += 2) m_.mem.write16(0xA000, uint16_t(i), 0);
    r.r[AX] = 0;
    r.r[CX] = 0;
    retf();
}

void MGraphicNative::s44_flip() {
    Regs& r = m_.cpu.regs;
    c8(0x1A0, 1);
    uint16_t src = page(1), dst = page(0);
    for (uint32_t i = 0; i < 0xFA00; i += 2) m_.mem.write16(dst, uint16_t(i), m_.mem.read16(src, uint16_t(i)));
    r.r[CX] = 0;
    retf();
}

void MGraphicNative::s45_get_flip_flag() { m_.cpu.regs.r8(0) = c8(0x1A0); retf(); }

void MGraphicNative::s48_copy_page_from() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    uint16_t src = arg(0), dst = draw_seg();
    for (uint32_t i = 0; i < 0xFA00; i += 2) m_.mem.write16(dst, uint16_t(i), m_.mem.read16(src, uint16_t(i)));
    r.r[CX] = 0;
    retf();
}

// cs:09b8 - flicker 9 palette entries (lights) and optional screen shake.
void MGraphicNative::s46_flicker() {
    Regs& r = m_.cpu.regs;
    uint16_t ax = d16(0x1C9A);
    ax = uint16_t(ax * 5 + 1);
    d16(0x1C9A, ax);
    uint16_t bx = ax & 3;
    bx = d8(uint16_t(bx + 0x1C96));
    uint16_t si = uint16_t(0x1B83 + 3 * bx);
    uint8_t ch = d8(si), cl = d8(uint16_t(si + 1)), bh = d8(uint16_t(si + 2));
    uint8_t bl = 0x8D;
    do {
        m_.cpu.out8(0x3C8, bl);
        m_.cpu.out8(0x3C9, ch);
        m_.cpu.out8(0x3C9, cl);
        m_.cpu.out8(0x3C9, bh);
        bl = uint8_t(bl + 0x10);
    } while (bl != 0x1D);
    uint16_t dx = 0x3C9;
    uint8_t ah = uint8_t(ax >> 8), al = bh;  // AL holds the last byte sent to the DAC
    uint8_t shake = c8(0x9AC);
    if (shake) {
        ah &= 3;
        c8(0x9AC, --shake);
        if (!shake) ah = 0;
        dx = 0x3D4;
        al = 0x0D;
        m_.cpu.out8(0x3D4, al);
        m_.cpu.out8(0x3D5, ah);
    }
    r.r[AX] = uint16_t(al | (ah << 8));
    r.r[BX] = uint16_t(bl | (bh << 8));
    r.r[CX] = uint16_t(cl | (ch << 8));
    r.r[DX] = dx;
    retf();
}

void MGraphicNative::s47_char_width() {
    Regs& r = m_.cpu.regs;
    uint16_t font = arg(1), ch = arg(0);
    r.r[BX] = d16(uint16_t(0xE2 + 2 * font));
    if (ch >= 0x80) r.r[AX] = 0;
    else r.r[AX] = d8(uint16_t(r.r[BX] + ch));
    retf();
}

void MGraphicNative::s51_copy_row() {
    Regs& r = m_.cpu.regs;
    for (int i = 0; i < 0x140; i++)
        m_.mem.write8(Memory::linear(r.s[ES], uint16_t(r.r[DI] + i)), m_.mem.read8(Memory::linear(r.s[SS], uint16_t(r.r[BP] + i))));
    r.r[AX] = r.s[SS];
    r.r[SI] = uint16_t(r.r[BP] + 0x140);
    r.r[DI] = uint16_t(r.r[DI] + 0x140);
    r.r[CX] = 0;
    retf();
}

void MGraphicNative::s56_page_es() {
    Regs& r = m_.cpu.regs;
    r.r[SI] = uint16_t(r.r[SI] << 1);
    r.s[ES] = c16(uint16_t(r.r[SI] + 0x67F));
    retf();
}

void MGraphicNative::s57_set_page() {
    Regs& r = m_.cpu.regs;
    r.r[AX] = arg(1);
    r.r[BX] = uint16_t(arg(0) << 1);
    c16(uint16_t(r.r[BX] + 0x67F), r.r[AX]);
    retf();
}

void MGraphicNative::s75_set_page() {
    Regs& r = m_.cpu.regs;
    r.r[AX] = arg(0);
    r.r[BX] = uint16_t(arg(1) << 1);
    c16(uint16_t(r.r[BX] + 0x67F), r.r[AX]);
    retf();
}

void MGraphicNative::s58_row_offset_di() {
    Regs& r = m_.cpu.regs;
    r.r[DI] = uint16_t(r.r[DI] << 1);
    r.r[AX] = c16(uint16_t(r.r[DI] + kRowTable));
    retf();
}

void MGraphicNative::s62_offset_xy() {
    Regs& r = m_.cpu.regs;
    r.r[AX] = arg(0);
    r.r[BX] = uint16_t(arg(1) << 1);
    r.r[AX] = uint16_t(r.r[AX] + c16(uint16_t(r.r[BX] + kRowTable)));
    retf();
}

void MGraphicNative::s63_const3() { m_.cpu.regs.r[AX] = 3; retf(); }

// cs:0228 / cs:0242 - screen off / on at vertical retrace (the native
// version does not wait).
void MGraphicNative::s69_screen_off() {
    Regs& r = m_.cpu.regs;
    m_.cpu.out8(0x3D8, 0x02);
    m_.cpu.out8(0x3C4, 0x01);
    uint8_t v = uint8_t(m_.cpu.in8(0x3C5) | 0x20);
    m_.cpu.out8(0x3C5, v);
    r.r8(0) = v;
    r.r[DX] = 0x3C5;
    retf();
}

void MGraphicNative::s70_screen_on() {
    Regs& r = m_.cpu.regs;
    m_.cpu.out8(0x3D8, 0x0A);
    m_.cpu.out8(0x3C4, 0x01);
    uint8_t v = uint8_t(m_.cpu.in8(0x3C5) & 0xDF);
    m_.cpu.out8(0x3C5, v);
    r.r8(0) = v;
    r.r[DX] = 0x3C5;
    retf();
}

void MGraphicNative::s79_set_shake() {
    Regs& r = m_.cpu.regs;
    r.r[BX] = r.r[SP];
    r.r8(0) = uint8_t(arg(0));
    c8(0x9AC, r.r8(0));
    retf();
}

// cs:07d5 - sprite blit, colour 0 transparent. Param block at SS:bp:
// +0 src seg, +2 src x, +4 src y, +6 dst page, +8 dst x, +A dst y, +C w, +E h.
void MGraphicNative::blit_body(uint16_t bp) {
    Regs& r = m_.cpu.regs;
    Memory& mem = m_.mem;
    uint16_t ss = r.s[SS];
    uint16_t src = mem.read16(ss, bp);
    uint16_t dst = page(mem.read16(ss, uint16_t(bp + 6)));
    uint16_t w = mem.read16(ss, uint16_t(bp + 0xC)), cx = mem.read16(ss, uint16_t(bp + 0xE));
    uint16_t dx = mem.read16(ss, uint16_t(bp + 8));
    uint16_t si = uint16_t(mem.read16(ss, uint16_t(bp + 4)) << 1);
    uint16_t di = uint16_t(mem.read16(ss, uint16_t(bp + 0xA)) << 1);
    uint16_t sx = mem.read16(ss, uint16_t(bp + 2));
    uint8_t al = uint8_t(src);
    do {
        uint16_t s = uint16_t(c16(uint16_t(si + kRowTable)) + sx);
        uint16_t d = uint16_t(c16(uint16_t(di + kRowTable)) + dx);
        uint16_t n = w;
        do {
            al = mem.read8(Memory::linear(src, s++));
            if (al) mem.write8(Memory::linear(dst, d), al);
            d++;
        } while (--n);
        di += 2;
        si += 2;
    } while (--cx);
    r.r[AX] = uint16_t((src & 0xFF00) | al);
    r.r[BX] = w;
    r.r[CX] = 0;
    r.r[DX] = dx;
    r.r[SI] = si;
    r.r[DI] = di;
}

void MGraphicNative::s74_blit() {
    blit_body(m_.cpu.regs.r[BP]);
    retf();
}

void MGraphicNative::s73_blit_ptr() {
    Regs& r = m_.cpu.regs;
    uint16_t si = r.r[SI], di = r.r[DI];
    blit_body(arg(0));
    r.r[SI] = si;
    r.r[DI] = di;
    retf();
}

// cs:0734 - clipped blit. Clip window in the param block:
// +10 top, +12 bottom, +14 left, +16 right. w/h are restored afterwards;
// the x/y and source x/y fields stay adjusted, as in the original.
void MGraphicNative::blit_clip_body(uint16_t bp) {
    Regs& r = m_.cpu.regs;
    Memory& mem = m_.mem;
    uint16_t ss = r.s[SS];
    auto P = [&](int o) { return mem.read16(ss, uint16_t(bp + o)); };
    auto S = [&](int o, uint16_t v) { mem.write16(ss, uint16_t(bp + o), v); };
    uint16_t saved_w = P(0xC), saved_h = P(0xE);
    uint16_t ax, bx, cx;
    bool drawn = false;
    do {
        cx = P(0x14);
        ax = P(8);
        if (int16_t(ax) < int16_t(cx)) {
            bx = uint16_t(ax + P(0xC));
            if (!(int16_t(bx) >= int16_t(cx))) break;
            cx = uint16_t(cx - ax);
            S(8, uint16_t(P(8) + cx));
            S(2, uint16_t(P(2) + cx));
            S(0xC, uint16_t(P(0xC) - cx));
        } else {
            bx = uint16_t(ax + P(0xC));
        }
        cx = P(0x16);
        if (int16_t(bx) > int16_t(cx)) {
            if (int16_t(ax) > int16_t(cx)) break;
            bx = uint16_t(bx - cx);
            S(0xC, uint16_t(P(0xC) - bx));
        }
        cx = P(0x10);
        ax = P(0xA);
        if (int16_t(ax) < int16_t(cx)) {
            bx = uint16_t(ax + P(0xE));
            if (!(int16_t(bx) >= int16_t(cx))) break;
            cx = uint16_t(cx - ax);
            S(0xA, uint16_t(P(0xA) + cx));
            S(4, uint16_t(P(4) + cx));
            S(0xE, uint16_t(P(0xE) - cx));
        } else {
            bx = uint16_t(ax + P(0xE));
        }
        cx = P(0x12);
        if (int16_t(bx) > int16_t(cx)) {
            if (int16_t(ax) > int16_t(cx)) break;
            bx = uint16_t(bx - cx);
            S(0xE, uint16_t(P(0xE) - bx));
        }
        blit_body(bp);
        drawn = true;
    } while (false);
    if (!drawn) {
        r.r[AX] = ax;
        r.r[BX] = bx;
        r.r[CX] = cx;
    }
    S(0xE, saved_h);
    S(0xC, saved_w);
}

void MGraphicNative::s72_blit_clip() {
    blit_clip_body(m_.cpu.regs.r[BP]);
    retf();
}

void MGraphicNative::s71_blit_clip_ptr() {
    Regs& r = m_.cpu.regs;
    uint16_t si = r.r[SI], di = r.r[DI];
    blit_clip_body(arg(0));
    r.r[SI] = si;
    r.r[DI] = di;
    retf();
}

// ------------------------------------------------------------ text

// cs:05b9 setup, cs:03e3 left clip, cs:041f right clip, cs:0468 vertical
// clip, cs:04ac render. Param block (SS:bp): +0 page, +2 flags (bit 0:
// opaque), +4 fg, +6 bg, +8 x, +A y, +C font, +E clip top, +10 clip bottom,
// +12 clip left, +14 clip right. String bytes >= 0x80 set the colour.
void MGraphicNative::text_inner(TextRegs& t, uint16_t bp, uint16_t setup_ret, bool lclip, bool rclip, bool vclip,
                                const uint16_t clip_rets[3]) {
    Memory& mem = m_.mem;
    uint16_t ss = m_.cpu.regs.s[SS];
    auto P8 = [&](int o) { return mem.read8(Memory::linear(ss, uint16_t(bp + o))); };
    auto P16 = [&](int o) { return mem.read16(ss, uint16_t(bp + o)); };
    auto S8 = [&](uint16_t off) { return mem.read8(Memory::linear(ss, off)); };
    auto lo = [](uint16_t v) { return uint8_t(v); };
    auto hi = [](uint16_t v) { return uint8_t(v >> 8); };
    auto mk = [](uint8_t l, uint8_t h) { return uint16_t(l | (h << 8)); };

    // Setup.
    t.cx = setup_ret;
    if (S8(t.bx) == 0) return;
    t.ax = ds_;
    t.di = uint16_t(P16(0) << 1);
    uint16_t es = c16(uint16_t(t.di + 0x67F));
    d8(0x1C45, 0xFF);
    d16(0x1C4E, 0);
    d8(0x1C4B, 0);
    t.di = uint16_t(P16(0xC) << 1);
    t.ax = d16(uint16_t(t.di + 0xEE));
    d16(0x1C48, t.ax);
    t.ax = mk(d8(uint16_t(t.di + 0x106)), hi(t.ax));
    d8(0x1C4A, lo(t.ax));
    d8(0x1C44, lo(t.ax));
    d8(0x1C4C, lo(t.ax));
    t.ax = d16(uint16_t(t.di + 0xE2));
    d16(0x1C50, t.ax);
    t.ax = P16(2) & 1;
    t.di = uint16_t(t.di + t.ax);
    t.ax = mk(lo(t.ax), d8(uint16_t(t.di + 0xFA)));
    d8(0x1C4D, hi(t.ax));

    // Left clip.
    if (lclip && d8(0x1C4A) != 0xFF) {
        t.ax = P16(0x12);
        t.dx = P16(8);
        if (t.dx < t.ax) {
            mem.write16(ss, uint16_t(bp + 8), t.ax);
            t.ax = uint16_t(t.ax - t.dx);
            uint8_t w = d8(0x1C4A);
            uint8_t q = uint8_t(t.ax / w), rem = uint8_t(t.ax % w);
            t.ax = mk(q, rem);
            d8(0x1C4B, rem);
            d8(0x1C44, uint8_t(d8(0x1C44) - rem));
            uint8_t al = uint8_t(q + 1);
            for (;;) {
                al--;
                t.ax = mk(al, hi(t.ax));
                if (al == 0) break;
                t.bx++;
                if (S8(t.bx) == 0) { t.ax = clip_rets[0]; return; }
            }
        }
    }
    // Right clip.
    if (rclip && d8(0x1C4A) != 0xFF) {
        t.cx = P16(0x14);
        t.dx = P16(8);
        if (t.dx >= t.cx) { t.ax = clip_rets[1]; return; }
        t.di = 0xFFFF;
        do { t.di++; } while (S8(uint16_t(t.bx + t.di)) != 0);
        t.ax = uint16_t(lo(t.di) * d8(0x1C4A));
        t.ax = uint16_t(t.ax + t.dx - 1);
        if (t.ax > t.cx) {
            t.ax = uint16_t(t.ax - t.cx);
            uint8_t cl = d8(0x1C4A);
            uint8_t q = uint8_t(t.ax / cl), rem = uint8_t(t.ax % cl);
            t.ax = mk(q, rem);
            cl = uint8_t(cl - rem);
            t.cx = mk(cl, hi(t.cx));
            d8(0x1C4C, cl);
            t.dx = mk(uint8_t(lo(t.di) - q), hi(t.di));
            d8(0x1C45, lo(t.dx));
        } else {
            t.ax = uint16_t(t.ax - t.cx);
        }
    }
    // Vertical clip.
    if (vclip) {
        uint8_t cl = P8(0xE), ch = P8(0x10);
        uint8_t dl = P8(0xA);
        uint8_t dh = uint8_t(d8(0x1C4D) - 1 + dl);
        t.cx = mk(cl, ch);
        t.dx = mk(dl, dh);
        if (dl > ch || dh < cl) { t.ax = clip_rets[2]; return; }
        if (dl < cl) {
            uint8_t al = uint8_t(dh - cl + 1);
            t.ax = mk(al, hi(t.ax));
            d8(0x1C4D, al);
            mem.write8(Memory::linear(ss, uint16_t(bp + 0xA)), cl);
            cl = uint8_t((cl - dl) << 1);
            d8(0x1C4E, cl);
            t.cx = mk(cl, ch);
        }
        if (dh > ch) {
            ch = uint8_t(ch - dl + 1);
            d8(0x1C4D, ch);
            t.cx = mk(lo(t.cx), ch);
        }
    }

    // Render.
    t.ax = mk(lo(t.ax), P8(4));
    d8(0x1B7A, hi(t.ax));
    t.dx = ds_;
    t.di = d16(0x1C4E);
    t.di = d16(uint16_t(t.di + 0x112));
    t.di = uint16_t(t.di + d16(0x1C48));
    d16(0x1C46, t.di);
    t.di = uint16_t(P16(0xA) << 1);
    t.si = t.bx;
    for (;;) {
        if (d8(0x1C45) == 1) d8(0x1C44, d8(0x1C4C));
        uint8_t al = S8(t.si);
        t.si++;
        t.ax = mk(al, hi(t.ax));
        if (al == 0) return;
        if (al & 0x80) {
            al &= 0x7F;
            mem.write8(Memory::linear(ss, uint16_t(bp + 4)), al);
            t.ax = mk(al, al);
            d8(0x1B7A, al);
            t.dx = ds_;
            d8(0x1C44, 0);
        } else {
            uint16_t save_si = t.si, save_di = t.di;
            t.ax = al;
            t.si = t.ax;
            t.dx = mk(P8(4), P8(6));
            if (d8(0x1C4A) == 0xFF) {
                t.bx = d16(0x1C50);
                t.ax = mk(d8(uint16_t(t.bx + t.si)), hi(t.ax));
                d8(0x1C44, lo(t.ax));
            }
            bool opaque = P8(2) == 1;
            uint8_t ch = d8(0x1C4D);
            do {
                t.bx = d16(d16(0x1C46) + t.si);
                t.bx = uint16_t((t.bx >> 8) | (t.bx << 8));
                uint8_t cl = d8(0x1C4B);
                t.bx = uint16_t(cl >= 16 ? 0 : t.bx << cl);
                uint16_t row_di = t.di;
                t.di = uint16_t(c16(uint16_t(t.di + kRowTable)) + P16(8));
                cl = d8(0x1C44);
                if (!opaque) {
                    cl--;
                    while (cl) {
                        bool carry = t.bx & 0x8000;
                        t.bx = uint16_t(t.bx << 1);
                        if (carry) mem.write8(Memory::linear(es, t.di), lo(t.dx));
                        t.di++;
                        cl--;
                    }
                } else {
                    while (cl) {
                        t.ax = t.dx;
                        bool carry = t.bx & 0x8000;
                        t.bx = uint16_t(t.bx << 1);
                        if (!carry) t.ax = mk(hi(t.ax), lo(t.ax));
                        mem.write8(Memory::linear(es, t.di), lo(t.ax));
                        t.di++;
                        cl--;
                    }
                }
                t.cx = mk(cl, ch);
                t.di = uint16_t(row_di + 2);
                t.si = uint16_t(t.si + 0x100);
                ch--;
                t.cx = mk(cl, ch);
            } while (ch);
            t.si = save_si;
            t.di = save_di;
        }
        uint8_t n = uint8_t(d8(0x1C45) - 1);
        d8(0x1C45, n);
        if (n == 0) return;
        d8(0x1C4B, 0);
        t.ax = d8(0x1C44);
        mem.write16(ss, uint16_t(bp + 8), uint16_t(P16(8) + t.ax));
        t.ax = mk(d8(0x1C4A), 0);
        d8(0x1C44, lo(t.ax));
    }
}

namespace {
const uint16_t kRetsSlot6[3] = {0x3D1, 0x3D4, 0x3D7};
const uint16_t kRetsSlot2[3] = {0x3E0, 0, 0};
const uint16_t kRetsSlot3[3] = {0, 0x41C, 0};
const uint16_t kRetsSlot1[3] = {0, 0, 0x465};
const uint16_t kRetsNone[3] = {0, 0, 0};
}  // namespace

#define F19_TEXT_INNER(name, ret, l, rc, v, rets)                                              \
    void MGraphicNative::name() {                                                              \
        Regs& r = m_.cpu.regs;                                                                  \
        TextRegs t{r.r[AX], r.r[BX], r.r[CX], r.r[DX], r.r[SI], r.r[DI]};                       \
        text_inner(t, r.r[BP], ret, l, rc, v, rets);                                            \
        r.r[AX] = t.ax; r.r[BX] = t.bx; r.r[CX] = t.cx; r.r[DX] = t.dx; r.r[SI] = t.si; r.r[DI] = t.di; \
        retf();                                                                                 \
    }
F19_TEXT_INNER(s1_text_vclip, 0x462, false, false, true, kRetsSlot1)
F19_TEXT_INNER(s2_text_lclip, 0x3DD, true, false, false, kRetsSlot2)
F19_TEXT_INNER(s3_text_rclip, 0x419, false, true, false, kRetsSlot3)
F19_TEXT_INNER(s4_text, 0x4AC, false, false, false, kRetsNone)
F19_TEXT_INNER(s6_text_clip, 0x3CE, true, true, true, kRetsSlot6)
#undef F19_TEXT_INNER

// Wrappers: push bp; mov bp,sp; push si; push di; bx = arg1; bp = arg0;
// call far inner; pop di; pop si; pop bp; retf.
void MGraphicNative::text_wrapper(void (MGraphicNative::*inner)()) {
    Regs& r = m_.cpu.regs;
    uint16_t bp = r.r[BP], si = r.r[SI], di = r.r[DI];
    uint16_t a0 = arg(0), a1 = arg(1);
    r.r[BX] = a1;
    r.r[BP] = a0;
    // Run the inner routine as if far-called: give it a return frame.
    uint16_t ret_ip = r.ip, ret_cs = r.s[CS];
    m_.cpu.push(ret_cs);
    m_.cpu.push(ret_ip);
    (this->*inner)();  // pops the frame via retf()
    r.r[BP] = bp;
    r.r[SI] = si;
    r.r[DI] = di;
    retf();
}

void MGraphicNative::s5_text_w() { text_wrapper(&MGraphicNative::s4_text); }
void MGraphicNative::s7_text_vclip_w() { text_wrapper(&MGraphicNative::s1_text_vclip); }
void MGraphicNative::s8_text_lclip_w() { text_wrapper(&MGraphicNative::s2_text_lclip); }
void MGraphicNative::s9_text_clip_w() { text_wrapper(&MGraphicNative::s6_text_clip); }
void MGraphicNative::s10_text_rclip_w() { text_wrapper(&MGraphicNative::s3_text_rclip); }

}  // namespace f19
