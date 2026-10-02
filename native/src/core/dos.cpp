#include "core/dos.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>

#include "core/machine.h"

namespace f19 {

namespace fs = std::filesystem;

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

// DOS 8.3 wildcard match ("*.WLD", "ROSTER.???").
bool wild_match(const std::string& pat, const std::string& name) {
    auto split = [](const std::string& s) {
        auto dot = s.find('.');
        return std::pair{s.substr(0, dot), dot == std::string::npos ? std::string() : s.substr(dot + 1)};
    };
    auto part = [](std::string p, const std::string& n, size_t len) {
        std::string pp;
        for (char c : p) {
            if (c == '*') { while (pp.size() < len) pp += '?'; break; }
            pp += c;
        }
        while (pp.size() < len) pp += ' ';
        std::string nn = n;
        while (nn.size() < len) nn += ' ';
        for (size_t i = 0; i < len; i++)
            if (pp[i] != '?' && pp[i] != nn[i]) return false;
        return true;
    };
    auto [pb, pe] = split(upper(pat));
    auto [nb, ne] = split(upper(name));
    return part(pb, nb, 8) && part(pe, ne, 3);
}

}  // namespace

Dos::Dos(Machine& m, fs::path dir) : game_dir(std::move(dir)), m_(m), cpu_(m.cpu) {
    // Single free arena.
    Memory& mem = m_.mem;
    uint32_t a = Memory::linear(kFirstMcb, 0);
    mem.write8(a, 'Z');
    mem.write16(kFirstMcb, 1, 0);
    mem.write16(kFirstMcb, 3, uint16_t(kMemTop - kFirstMcb - 1));

    m_.hook_interrupt(0x21, [this] { int21(); }, "dos int21");
    m_.hook_interrupt(0x20, [this] { terminate(0); }, "dos int20");
}

Dos::~Dos() {
    for (auto& [h, f] : files_) std::fclose(f);
}

// ---------------------------------------------------------------- memory

uint16_t Dos::alloc(uint16_t paras, uint16_t owner, uint16_t* largest) {
    Memory& mem = m_.mem;
    uint16_t best = 0;
    for (uint16_t mcb = kFirstMcb;;) {
        uint8_t type = mem.read8(Memory::linear(mcb, 0));
        uint16_t own = mem.read16(mcb, 1), size = mem.read16(mcb, 3);
        if (own == 0) {
            best = std::max(best, size);
            if (size >= paras) {
                if (size > paras) {  // split
                    uint16_t next = uint16_t(mcb + 1 + paras);
                    mem.write8(Memory::linear(next, 0), type);
                    mem.write16(next, 1, 0);
                    mem.write16(next, 3, uint16_t(size - paras - 1));
                    mem.write8(Memory::linear(mcb, 0), 'M');
                    mem.write16(mcb, 3, paras);
                }
                mem.write16(mcb, 1, owner);
                return uint16_t(mcb + 1);
            }
        }
        if (type == 'Z') break;
        mcb = uint16_t(mcb + 1 + size);
    }
    if (largest) *largest = best;
    return 0;
}

void Dos::coalesce() {
    Memory& mem = m_.mem;
    for (uint16_t mcb = kFirstMcb;;) {
        uint8_t type = mem.read8(Memory::linear(mcb, 0));
        if (type == 'Z') break;
        uint16_t size = mem.read16(mcb, 3);
        uint16_t next = uint16_t(mcb + 1 + size);
        if (mem.read16(mcb, 1) == 0 && mem.read16(next, 1) == 0) {
            mem.write8(Memory::linear(mcb, 0), mem.read8(Memory::linear(next, 0)));
            mem.write16(mcb, 3, uint16_t(size + 1 + mem.read16(next, 3)));
            continue;
        }
        mcb = next;
    }
}

bool Dos::free_block(uint16_t seg) {
    uint16_t mcb = uint16_t(seg - 1);
    uint8_t t = m_.mem.read8(Memory::linear(mcb, 0));
    if (t != 'M' && t != 'Z') return false;
    m_.mem.write16(mcb, 1, 0);
    coalesce();
    return true;
}

bool Dos::resize(uint16_t seg, uint16_t paras, uint16_t* largest) {
    Memory& mem = m_.mem;
    uint16_t mcb = uint16_t(seg - 1);
    uint8_t type = mem.read8(Memory::linear(mcb, 0));
    if (type != 'M' && type != 'Z') return false;
    uint16_t size = mem.read16(mcb, 3);
    uint16_t owner = mem.read16(mcb, 1);
    // Merge any free blocks that follow, then split to the requested size.
    for (;;) {
        if (mem.read8(Memory::linear(mcb, 0)) == 'Z') break;
        uint16_t next = uint16_t(mcb + 1 + size);
        if (mem.read16(next, 1) != 0) break;
        mem.write8(Memory::linear(mcb, 0), mem.read8(Memory::linear(next, 0)));
        size = uint16_t(size + 1 + mem.read16(next, 3));
        mem.write16(mcb, 3, size);
    }
    if (paras > size) {
        if (largest) *largest = size;
        return false;
    }
    if (paras < size) {
        uint16_t next = uint16_t(mcb + 1 + paras);
        mem.write8(Memory::linear(next, 0), mem.read8(Memory::linear(mcb, 0)));
        mem.write16(next, 1, 0);
        mem.write16(next, 3, uint16_t(size - paras - 1));
        mem.write8(Memory::linear(mcb, 0), 'M');
        mem.write16(mcb, 3, paras);
    }
    mem.write16(mcb, 1, owner);
    coalesce();
    return true;
}

void Dos::free_owned_by(uint16_t psp) {
    Memory& mem = m_.mem;
    for (uint16_t mcb = kFirstMcb;;) {
        if (mem.read16(mcb, 1) == psp) mem.write16(mcb, 1, 0);
        if (mem.read8(Memory::linear(mcb, 0)) == 'Z') break;
        mcb = uint16_t(mcb + 1 + mem.read16(mcb, 3));
    }
    coalesce();
}

// ---------------------------------------------------------------- processes

uint16_t Dos::build_env(const std::string& program_path) {
    std::string env = std::string("COMSPEC=C:\\COMMAND.COM") + '\0' + "PATH=C:\\" + '\0' + '\0';
    env += '\x01';
    env += '\0';
    env += program_path + '\0';
    uint16_t paras = uint16_t((env.size() + 15) / 16);
    uint16_t seg = alloc(paras, 0xFFFF);
    for (size_t i = 0; i < env.size(); i++) m_.mem.write8(Memory::linear(seg, uint16_t(i)), uint8_t(env[i]));
    return seg;
}

uint16_t Dos::make_psp(uint16_t seg, uint16_t mem_end, uint16_t env, uint16_t parent, const std::string& tail) {
    Memory& mem = m_.mem;
    for (int i = 0; i < 256; i++) mem.write8(Memory::linear(seg, uint16_t(i)), 0);
    mem.write8(Memory::linear(seg, 0), 0xCD);
    mem.write8(Memory::linear(seg, 1), 0x20);
    mem.write16(seg, 2, mem_end);
    for (int v = 0; v < 3; v++) {  // INT 22h/23h/24h vectors
        mem.write16(seg, uint16_t(0x0A + 4 * v), mem.read16(0, uint16_t((0x22 + v) * 4)));
        mem.write16(seg, uint16_t(0x0C + 4 * v), mem.read16(0, uint16_t((0x22 + v) * 4 + 2)));
    }
    mem.write16(seg, 0x16, parent);
    const uint8_t jft[20] = {1, 1, 1, 0, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 20; i++) mem.write8(Memory::linear(seg, uint16_t(0x18 + i)), jft[i]);
    mem.write16(seg, 0x2C, env);
    mem.write16(seg, 0x32, 20);
    mem.write16(seg, 0x34, 0x18);
    mem.write16(seg, 0x36, seg);
    mem.write8(Memory::linear(seg, 0x50), 0xCD);
    mem.write8(Memory::linear(seg, 0x51), 0x21);
    mem.write8(Memory::linear(seg, 0x52), 0xCB);
    for (int i = 0; i < 11; i++) {  // blank FCBs
        mem.write8(Memory::linear(seg, uint16_t(0x5D + i)), ' ');
        mem.write8(Memory::linear(seg, uint16_t(0x6D + i)), ' ');
    }
    size_t n = std::min<size_t>(tail.size(), 126);
    mem.write8(Memory::linear(seg, 0x80), uint8_t(n));
    for (size_t i = 0; i < n; i++) mem.write8(Memory::linear(seg, uint16_t(0x81 + i)), uint8_t(tail[i]));
    mem.write8(Memory::linear(seg, uint16_t(0x81 + n)), 0x0D);
    return seg;
}

uint32_t Dos::image_paras(const std::vector<uint8_t>& f, bool is_exe, uint16_t* min_extra, uint16_t* max_extra) {
    if (!is_exe) {
        *min_extra = 0x1000;
        *max_extra = 0xFFFF;
        return uint32_t((f.size() + 0x100 + 15) / 16);
    }
    auto w = [&](int o) { return uint16_t(f[o] | (f[o + 1] << 8)); };
    uint32_t total = w(4) * 512u - (w(2) ? 512u - w(2) : 0u);
    uint32_t image = total - w(8) * 16u;
    *min_extra = w(0x0A);
    *max_extra = w(0x0C);
    return (image + 15) / 16;
}

bool Dos::load_image(const std::vector<uint8_t>& f, bool is_exe, uint16_t load_seg, uint16_t reloc_seg,
                     uint16_t* cs, uint16_t* ip, uint16_t* ss, uint16_t* sp) {
    Memory& mem = m_.mem;
    if (!is_exe) {
        for (size_t i = 0; i < f.size(); i++) mem.write8(Memory::linear(load_seg, 0) + uint32_t(i), f[i]);
        return true;
    }
    auto w = [&](size_t o) { return uint16_t(f[o] | (f[o + 1] << 8)); };
    uint32_t hdr = w(8) * 16u;
    uint32_t total = w(4) * 512u - (w(2) ? 512u - w(2) : 0u);
    if (total > f.size()) total = uint32_t(f.size());
    uint32_t base = Memory::linear(load_seg, 0);
    for (uint32_t i = hdr; i < total; i++) mem.write8(base + (i - hdr), f[i]);
    uint16_t nrel = w(6), relo = w(0x18);
    for (uint16_t i = 0; i < nrel; i++) {
        uint16_t off = w(relo + 4 * i), seg = w(relo + 4 * i + 2);
        uint16_t tseg = uint16_t(load_seg + seg);
        mem.write16(tseg, off, uint16_t(mem.read16(tseg, off) + reloc_seg));
    }
    if (cs) {
        *ss = uint16_t(w(0x0E) + reloc_seg);
        *sp = w(0x10);
        *ip = w(0x14);
        *cs = uint16_t(w(0x16) + reloc_seg);
    }
    return true;
}

static bool read_file(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool Dos::start_program(const std::string& dos_name, const std::string& args) {
    fs::path p = resolve(dos_name, true);
    std::vector<uint8_t> file;
    if (p.empty() || !read_file(p, file)) return false;
    bool is_exe = file.size() > 2 && file[0] == 'M' && file[1] == 'Z';
    uint16_t env = build_env("C:\\" + upper(dos_name));
    uint16_t largest = 0;
    alloc(0xFFFF, 0, &largest);
    uint16_t seg = alloc(largest, 0xFFFF);
    uint16_t psp = make_psp(seg, uint16_t(seg + largest), env, seg, args.empty() ? "" : " " + args);
    m_.mem.write16(uint16_t(env - 1), 1, psp);
    m_.mem.write16(uint16_t(seg - 1), 1, psp);
    current_psp = psp;
    dta_seg_ = psp;
    dta_off_ = 0x80;
    Regs& r = cpu_.regs;
    r = Regs{};
    r.s[DS] = r.s[ES] = psp;
    if (is_exe) {
        load_image(file, true, uint16_t(psp + 0x10), uint16_t(psp + 0x10), &r.s[CS], &r.ip, &r.s[SS], &r.r[SP]);
    } else {
        load_image(file, false, uint16_t(psp + 0x10), 0, nullptr, nullptr, nullptr, nullptr);
        r.s[CS] = r.s[SS] = psp;
        r.ip = 0x100;
        r.r[SP] = 0xFFFE;
        m_.mem.write16(psp, 0xFFFE, 0);  // RET -> INT 20h at PSP:0
    }
    r.flags = 0xF202;
    m_.log("DOS start %s psp=%04X entry %04X:%04X\n", dos_name.c_str(), psp, r.s[CS], r.ip);
    return true;
}

void Dos::exec(uint8_t mode) {
    Regs& r = cpu_.regs;
    std::string name = read_string(r.s[DS], r.r[DX]);
    fs::path p = resolve(name, true);
    std::vector<uint8_t> file;
    if (p.empty() || !read_file(p, file)) {
        m_.log("DOS exec %s: not found\n", name.c_str());
        return fail(2);
    }
    bool is_exe = file.size() > 2 && file[0] == 'M' && file[1] == 'Z';
    Memory& mem = m_.mem;
    uint16_t pb_seg = r.s[ES], pb_off = r.r[BX];

    if (mode == 3) {  // load overlay
        uint16_t load_seg = mem.read16(pb_seg, pb_off), reloc = mem.read16(pb_seg, uint16_t(pb_off + 2));
        m_.log("DOS load overlay %s at %04X reloc %04X\n", name.c_str(), load_seg, reloc);
        load_image(file, is_exe, load_seg, reloc, nullptr, nullptr, nullptr, nullptr);
        m_.notify_overlay_loaded(upper(name), load_seg);
        return ok();
    }
    if (mode != 0) {
        m_.log("DOS exec mode %d unsupported\n", mode);
        return fail(1);
    }

    uint16_t env_param = mem.read16(pb_seg, pb_off);
    uint16_t tail_off = mem.read16(pb_seg, uint16_t(pb_off + 2)), tail_seg = mem.read16(pb_seg, uint16_t(pb_off + 4));
    uint8_t tail_len = mem.read8(Memory::linear(tail_seg, tail_off));
    std::string tail;
    for (int i = 0; i < tail_len; i++) tail += char(mem.read8(Memory::linear(tail_seg, uint16_t(tail_off + 1 + i))));

    uint16_t min_extra, max_extra;
    uint32_t img = image_paras(file, is_exe, &min_extra, &max_extra);
    uint32_t need_min = img + 0x10 + min_extra;
    uint32_t want = std::min<uint32_t>(0xFFFF, img + 0x10 + max_extra);
    uint16_t env = env_param ? env_param : build_env("C:\\" + upper(name));
    uint16_t largest = 0;
    alloc(0xFFFF, 0, &largest);
    uint32_t paras = std::min<uint32_t>(want, largest);
    if (paras < need_min) {
        m_.log("DOS exec %s: out of memory (need %u, largest %u)\n", name.c_str(), need_min, largest);
        return fail(8);
    }
    uint16_t seg = alloc(uint16_t(paras), 0xFFFF);
    uint16_t psp = make_psp(seg, uint16_t(seg + paras), env, current_psp, tail);
    if (!env_param) mem.write16(uint16_t(env - 1), 1, psp);
    mem.write16(uint16_t(seg - 1), 1, psp);

    Process proc{r, current_psp};
    procs_.push_back(proc);
    current_psp = psp;
    dta_seg_ = psp;
    dta_off_ = 0x80;

    m_.log("DOS exec %s '%s' psp=%04X paras=%u\n", name.c_str(), tail.c_str(), psp, paras);
    Regs nr;
    nr.s[DS] = nr.s[ES] = psp;
    if (is_exe) {
        load_image(file, true, uint16_t(psp + 0x10), uint16_t(psp + 0x10), &nr.s[CS], &nr.ip, &nr.s[SS], &nr.r[SP]);
    } else {
        load_image(file, false, uint16_t(psp + 0x10), 0, nullptr, nullptr, nullptr, nullptr);
        nr.s[CS] = nr.s[SS] = psp;
        nr.ip = 0x100;
        nr.r[SP] = 0xFFFE;
        mem.write16(psp, 0xFFFE, 0);
    }
    nr.flags = 0xF202;
    r = nr;
}

void Dos::terminate(uint8_t code) {
    m_.log("DOS terminate psp=%04X code=%02X\n", current_psp, code);
    last_return_code_ = code;
    free_owned_by(current_psp);
    if (procs_.empty()) {
        m_.exited = true;
        m_.exit_code = code;
        m_.stop_reason = "program exited";
        return;
    }
    Process p = procs_.back();
    procs_.pop_back();
    current_psp = p.psp;
    dta_seg_ = current_psp;
    dta_off_ = 0x80;
    cpu_.regs = p.parent_regs;
    m_.set_return_flag(CF, false);
}

// ---------------------------------------------------------------- files

std::string Dos::read_string(uint16_t seg, uint16_t off) {
    std::string s;
    for (;;) {
        char c = char(m_.mem.read8(Memory::linear(seg, off++)));
        if (!c) break;
        s += c;
    }
    return s;
}

fs::path Dos::resolve(const std::string& dos_path, bool must_exist) {
    std::string p = dos_path;
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    fs::path cur = game_dir;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        std::string comp = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? p.size() + 1 : slash + 1;
        if (comp.empty() || comp == ".") continue;
        fs::path found;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(cur, ec))
            if (upper(e.path().filename().string()) == upper(comp)) { found = e.path(); break; }
        if (found.empty()) {
            if (must_exist || start <= p.size()) return {};
            return cur / upper(comp);  // new file in an existing directory
        }
        cur = found;
    }
    return cur;
}

void Dos::find_fill_dta() {
    Memory& mem = m_.mem;
    const std::string& name = find_results_[find_pos_++];
    fs::path p = game_dir / name;
    std::error_code ec;
    uint32_t size = uint32_t(fs::file_size(p, ec));
    for (int i = 0; i < 43; i++) mem.write8(Memory::linear(dta_seg_, uint16_t(dta_off_ + i)), 0);
    mem.write8(Memory::linear(dta_seg_, uint16_t(dta_off_ + 0x15)), 0x20);
    mem.write16(dta_seg_, uint16_t(dta_off_ + 0x16), 0);
    mem.write16(dta_seg_, uint16_t(dta_off_ + 0x18), (8 << 9) | (10 << 5) | 14);  // 1988-10-14
    mem.write16(dta_seg_, uint16_t(dta_off_ + 0x1A), uint16_t(size));
    mem.write16(dta_seg_, uint16_t(dta_off_ + 0x1C), uint16_t(size >> 16));
    std::string up = upper(name);
    for (size_t i = 0; i < up.size() && i < 12; i++) mem.write8(Memory::linear(dta_seg_, uint16_t(dta_off_ + 0x1E + i)), uint8_t(up[i]));
}

// ---------------------------------------------------------------- INT 21h

void Dos::fail(uint16_t err) {
    cpu_.regs.r[AX] = err;
    m_.set_return_flag(CF, true);
}
void Dos::ok() { m_.set_return_flag(CF, false); }

void Dos::int21() {
    Regs& r = cpu_.regs;
    Memory& mem = m_.mem;
    uint8_t ah = r.r8(4), al = r.r8(0);
    switch (ah) {
        case 0x00: terminate(0); return;
        case 0x01: case 0x07: case 0x08: {  // read char (01 echoes)
            uint16_t k;
            if (pending_scan_) { r.r8(0) = pending_scan_; pending_scan_ = 0; return; }
            if (!m_.key_pop(&k)) return m_.block_and_retry();
            r.r8(0) = uint8_t(k);
            if (!r.r8(0)) pending_scan_ = uint8_t(k >> 8);  // extended key: scan code next
            m_.log("DOS read char %02X\n", r.r8(0));
            return;
        }
        case 0x02: m_.tty_out(r.r8(2)); return;
        case 0x06:
            if (r.r8(2) == 0xFF) {
                uint16_t k;
                if (pending_scan_) { r.r8(0) = pending_scan_; pending_scan_ = 0; m_.set_return_flag(ZF, false); return; }
                if (!m_.key_pop(&k)) { r.r8(0) = 0; m_.set_return_flag(ZF, true); return; }
                r.r8(0) = uint8_t(k);
                if (!r.r8(0)) pending_scan_ = uint8_t(k >> 8);
                m_.set_return_flag(ZF, false);
            } else m_.tty_out(r.r8(2));
            return;
        case 0x0C: {  // flush buffer, then run function AL
            uint16_t k;
            while (m_.key_pop(&k)) {}
            pending_scan_ = 0;
            if (al == 0x01 || al == 0x06 || al == 0x07 || al == 0x08) { r.r8(4) = al; int21(); }
            return;
        }
        case 0x09: {
            std::string s;
            for (uint16_t o = r.r[DX];; o++) {
                char c = char(mem.read8(Memory::linear(r.s[DS], o)));
                if (c == '$' || s.size() > 512) break;
                s += c;
            }
            m_.log("DOS print \"%s\"\n", s.c_str());
            for (char c : s) m_.tty_out(uint8_t(c));
            return;
        }
        case 0x0B: r.r8(0) = (m_.key_available() || pending_scan_) ? 0xFF : 0x00; return;
        case 0x0E: r.r8(0) = 3; return;     // select disk: 3 drives
        case 0x19: r.r8(0) = 2; return;     // current disk C:
        case 0x1A: dta_seg_ = r.s[DS]; dta_off_ = r.r[DX]; return;
        case 0x25: m_.set_vector(al, r.s[DS], r.r[DX]); m_.log("DOS set vector %02X = %04X:%04X\n", al, r.s[DS], r.r[DX]); return;
        case 0x2A: {
            std::time_t t = std::time(nullptr);
            std::tm tm = *std::localtime(&t);
            r.r[CX] = uint16_t(tm.tm_year + 1900);
            r.r8(6) = uint8_t(tm.tm_mon + 1);
            r.r8(2) = uint8_t(tm.tm_mday);
            r.r8(0) = uint8_t(tm.tm_wday);
            return;
        }
        case 0x2C: {
            std::time_t t = std::time(nullptr);
            std::tm tm = *std::localtime(&t);
            r.r8(5) = uint8_t(tm.tm_hour);
            r.r8(1) = uint8_t(tm.tm_min);
            r.r8(6) = uint8_t(tm.tm_sec);
            r.r8(2) = uint8_t((m_.now_us() / 10000) % 100);
            return;
        }
        case 0x30: r.r[AX] = 0x1E03; r.r[BX] = 0; r.r[CX] = 0; return;  // DOS 3.30
        case 0x33: if (al == 0) r.r8(2) = 0; return;
        case 0x35: r.r[BX] = mem.read16(0, al * 4); r.s[ES] = mem.read16(0, al * 4 + 2); return;
        case 0x36: r.r[AX] = 4; r.r[BX] = 0x4000; r.r[CX] = 512; r.r[DX] = 0xFFFF; return;
        case 0x3B: return ok();
        case 0x3C:
        case 0x3D: {
            std::string name = read_string(r.s[DS], r.r[DX]);
            fs::path p = resolve(name, ah == 0x3D);
            std::FILE* f = nullptr;
            if (!p.empty()) {
                if (ah == 0x3C) f = std::fopen(p.c_str(), "w+b");
                else f = std::fopen(p.c_str(), (al & 3) == 0 ? "rb" : "r+b");
            }
            if (!f) {
                m_.log("DOS %s %s: not found\n", ah == 0x3C ? "create" : "open", name.c_str());
                return fail(2);
            }
            uint16_t h = 5;
            while (files_.count(h)) h++;
            files_[h] = f;
            file_names_[h] = name;
            r.r[AX] = h;
            m_.log("DOS %s %s -> %u\n", ah == 0x3C ? "create" : "open", name.c_str(), h);
            return ok();
        }
        case 0x3E: {
            auto it = files_.find(r.r[BX]);
            if (it == files_.end()) return r.r[BX] < 5 ? ok() : fail(6);
            std::fclose(it->second);
            files_.erase(it);
            file_names_.erase(r.r[BX]);
            return ok();
        }
        case 0x3F: {
            auto it = files_.find(r.r[BX]);
            if (it == files_.end()) {
                if (r.r[BX] < 5) { r.r[AX] = 0; return ok(); }
                return fail(6);
            }
            std::vector<uint8_t> buf(r.r[CX]);
            size_t n = std::fread(buf.data(), 1, buf.size(), it->second);
            for (size_t i = 0; i < n; i++) mem.write8(Memory::linear(r.s[DS], uint16_t(r.r[DX] + i)), buf[i]);
            r.r[AX] = uint16_t(n);
            return ok();
        }
        case 0x40: {
            if (r.r[BX] == 1 || r.r[BX] == 2) {
                std::string s;
                for (uint16_t i = 0; i < r.r[CX]; i++) s += char(mem.read8(Memory::linear(r.s[DS], uint16_t(r.r[DX] + i))));
                m_.log("DOS write(%u) \"%s\"\n", r.r[BX], s.c_str());
                for (char c : s) m_.tty_out(uint8_t(c));
                r.r[AX] = r.r[CX];
                return ok();
            }
            auto it = files_.find(r.r[BX]);
            if (it == files_.end()) return fail(6);
            std::vector<uint8_t> buf(r.r[CX]);
            for (size_t i = 0; i < buf.size(); i++) buf[i] = mem.read8(Memory::linear(r.s[DS], uint16_t(r.r[DX] + i)));
            r.r[AX] = uint16_t(std::fwrite(buf.data(), 1, buf.size(), it->second));
            return ok();
        }
        case 0x41: {
            fs::path p = resolve(read_string(r.s[DS], r.r[DX]), true);
            std::error_code ec;
            if (p.empty() || !fs::remove(p, ec)) return fail(2);
            return ok();
        }
        case 0x42: {
            auto it = files_.find(r.r[BX]);
            if (it == files_.end()) return fail(6);
            long off = long(int32_t((uint32_t(r.r[CX]) << 16) | r.r[DX]));
            if (std::fseek(it->second, off, al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END)) return fail(25);
            long pos = std::ftell(it->second);
            r.r[AX] = uint16_t(pos);
            r.r[DX] = uint16_t(pos >> 16);
            return ok();
        }
        case 0x43: {
            fs::path p = resolve(read_string(r.s[DS], r.r[DX]), true);
            if (p.empty()) return fail(2);
            r.r[CX] = 0x20;
            return ok();
        }
        case 0x44:
            if (al == 0x00) {
                r.r[DX] = r.r[BX] < 5 ? 0x80D3 : 0x0002;
                return ok();
            }
            if (al == 0x08) { r.r[AX] = 1; return ok(); }  // fixed media
            m_.log("DOS ioctl %02X\n", al);
            return fail(1);
        case 0x47: mem.write8(Memory::linear(r.s[DS], r.r[SI]), 0); return ok();
        case 0x48: {
            uint16_t largest = 0;
            uint16_t seg = alloc(r.r[BX], current_psp, &largest);
            m_.log("DOS alloc %u paras -> %04X\n", r.r[BX], seg);
            if (!seg) { r.r[BX] = largest; return fail(8); }
            r.r[AX] = seg;
            return ok();
        }
        case 0x49:
            m_.log("DOS free %04X\n", r.s[ES]);
            return free_block(r.s[ES]) ? ok() : fail(9);
        case 0x4A: {
            uint16_t largest = 0;
            m_.log("DOS resize %04X to %u paras\n", r.s[ES], r.r[BX]);
            if (!resize(r.s[ES], r.r[BX], &largest)) { r.r[BX] = largest; return fail(8); }
            return ok();
        }
        case 0x4B: exec(al); return;
        case 0x4C: terminate(al); return;
        case 0x4D: r.r[AX] = last_return_code_; return;
        case 0x4E: {
            std::string pat = read_string(r.s[DS], r.r[DX]);
            auto slash = pat.find_last_of("\\:/");
            if (slash != std::string::npos) pat = pat.substr(slash + 1);
            find_results_.clear();
            find_pos_ = 0;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(game_dir, ec))
                if (e.is_regular_file() && wild_match(pat, e.path().filename().string()))
                    find_results_.push_back(e.path().filename().string());
            std::sort(find_results_.begin(), find_results_.end());
            m_.log("DOS findfirst %s -> %zu\n", pat.c_str(), find_results_.size());
            if (find_results_.empty()) return fail(18);
            find_fill_dta();
            return ok();
        }
        case 0x4F:
            if (find_pos_ >= find_results_.size()) return fail(18);
            find_fill_dta();
            return ok();
        case 0x50: current_psp = r.r[BX]; return;
        case 0x51: case 0x62: r.r[BX] = current_psp; return;
        case 0x57:
            if (al == 0) { r.r[CX] = 0; r.r[DX] = (8 << 9) | (10 << 5) | 14; }
            return ok();
        default:
            m_.log("DOS AH=%02X AL=%02X (unhandled)\n", ah, al);
            return fail(1);
    }
}

}  // namespace f19
