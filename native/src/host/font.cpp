#include "host/font.h"

#include "builtin_font.h"

#include <zlib.h>

#include <cstring>

namespace f19 {

namespace {

bool parse_psf(const std::vector<uint8_t>& d, Font& font) {
    if (d.size() > 4 && d[0] == 0x36 && d[1] == 0x04) {
        font.h = d[3];
        if (d.size() < 4 + 256 * size_t(font.h)) return false;
        font.glyphs.assign(d.begin() + 4, d.begin() + 4 + 256 * font.h);
        return true;
    }
    if (d.size() > 32 && d[0] == 0x72 && d[1] == 0xB5 && d[2] == 0x4A && d[3] == 0x86) {
        auto u32 = [&](int o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24); };
        uint32_t hdr = u32(8), bytes = u32(20);
        font.h = int(u32(24));
        font.w = int(u32(28));
        if (font.w != 8 || d.size() < hdr + 256 * size_t(bytes)) return false;
        font.glyphs.resize(256 * font.h);
        for (int g = 0; g < 256; g++)
            std::memcpy(&font.glyphs[g * font.h], &d[hdr + g * bytes], font.h);
        return true;
    }
    return false;
}

}  // namespace

bool load_psf(const char* path, Font& font) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) return false;
    std::vector<uint8_t> d;
    uint8_t buf[4096];
    int n;
    while ((n = gzread(gz, buf, sizeof buf)) > 0) d.insert(d.end(), buf, buf + n);
    gzclose(gz);
    return parse_psf(d, font);
}

bool load_psf(const uint8_t* data, size_t size, Font& font) {
    if (size < 2 || data[0] != 0x1f || data[1] != 0x8b) return parse_psf(std::vector<uint8_t>(data, data + size), font);
    z_stream z{};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = uInt(size);
    std::vector<uint8_t> d;
    uint8_t buf[4096];
    int r;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        d.insert(d.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (r == Z_OK);
    inflateEnd(&z);
    return r == Z_STREAM_END && parse_psf(d, font);
}

bool load_builtin_font(Font& font) { return load_psf(kBuiltinFont, sizeof kBuiltinFont, font); }

}  // namespace f19
