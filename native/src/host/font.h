// A PC console font (PSF1/PSF2, 8 pixels wide), for text mode and the
// clipboard's controls pages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f19 {

struct Font {
    int w = 8, h = 16;
    std::vector<uint8_t> glyphs;  // 256 * h bytes, one row per byte (bit 7 = left)
};

// Load a PSF1/PSF2 console font (gzip or plain).
bool load_psf(const char* path, Font& font);
// The same from memory.
bool load_psf(const uint8_t* data, size_t size, Font& font);
// The font built into f19 (native/data/default8x16.psfu.gz, from kbd).
bool load_builtin_font(Font& font);

}  // namespace f19
