// The clipboard in the cockpit: pages of text (the controls) followed by
// the game manual (a PDF the user supplies).
//
// Pages are rendered on a background thread (the PDF with poppler, trimmed
// to its printed area) and laid onto a clipboard image (board, paper, clip
// with the page number), so the renderer only uploads finished images. The
// page being read and its neighbours are kept ready.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "host/font.h"

namespace f19 {

class Manual {
public:
    // Clipboard image size (pixels).
    static constexpr int kWidth = 2120, kHeight = 1716;
    // Text pages: characters per line.
    static constexpr int kTextColumns = 83;
    struct Image {
        int page = -1;
        std::vector<uint32_t> argb;  // kWidth x kHeight, 0xAARRGGBB, top row first
    };
    // A section of text pages: a title and lines (kTextColumns wide). An
    // empty line separates groups; a page break avoids splitting one.
    struct Section {
        std::string title;
        std::vector<std::string> lines;
    };

    Manual();
    ~Manual();
    // Set up before the first request(): the font for text and the label,
    // the text sections, then the PDF.
    void set_font(const Font& font);
    void add_section(const Section& s);
    // Open the PDF; false (with a message) if it cannot be read or this
    // build has no PDF support.
    bool open(const std::string& path);
    bool ok() const { return pages() > 0; }
    int pages() const;       // text pages + PDF pages
    int text_pages() const;  // the PDF's first page follows these
    // Page `page` (0-based) is wanted: render it first, then its neighbours.
    void request(int page);
    // The clipboard image for `page`, if it is ready.
    std::shared_ptr<const Image> get(int page) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The manual's PDF: `explicit_path` if given, else the first *.pdf in
// `game_dir`, then in the current directory. Empty if none.
std::string find_manual(const std::string& explicit_path, const std::string& game_dir);

}  // namespace f19
