#include "host/font.h"

#include <zlib.h>

#include <cstring>

namespace f19 {

bool load_psf(const char* path, Font& font) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) return false;
    std::vector<uint8_t> d;
    uint8_t buf[4096];
    int n;
    while ((n = gzread(gz, buf, sizeof buf)) > 0) d.insert(d.end(), buf, buf + n);
    gzclose(gz);
    if (d.size() > 4 && d[0] == 0x36 && d[1] == 0x04) {
        font.h = d[3];
        font.glyphs.assign(d.begin() + 4, d.begin() + 4 + 256 * font.h);
        return true;
    }
    if (d.size() > 32 && d[0] == 0x72 && d[1] == 0xB5 && d[2] == 0x4A && d[3] == 0x86) {
        auto u32 = [&](int o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24); };
        uint32_t hdr = u32(8), bytes = u32(20);
        font.h = int(u32(24));
        font.w = int(u32(28));
        if (font.w != 8) return false;
        font.glyphs.resize(256 * font.h);
        for (int g = 0; g < 256; g++)
            std::memcpy(&font.glyphs[g * font.h], &d[hdr + g * bytes], font.h);
        return true;
    }
    return false;
}

}  // namespace f19
