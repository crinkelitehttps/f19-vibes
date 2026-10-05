#include "host/manual.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#ifdef F19_HAVE_POPPLER
#include <poppler-document.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>
#endif

namespace f19 {

namespace {

// Clipboard layout (pixels in the image).
constexpr uint32_t kBoard = 0xFF7A5230, kBoardEdge = 0xFF4E321C, kPaper = 0xFFFFFFFF, kShadow = 0xFF5A3C22;
constexpr uint32_t kClip = 0xFFB4B8BC, kClipEdge = 0xFF6E7378, kInk = 0xFF202428;
constexpr int kPaperX = 36, kPaperY = 120, kPaperW = Manual::kWidth - 2 * kPaperX, kPaperH = Manual::kHeight - kPaperY - 36;
constexpr int kClipW = 640, kClipY0 = 30, kClipY1 = 200;
// Where the page goes: below the clip, inside a margin.
constexpr int kTextX = kPaperX + 24, kTextY = kClipY1 + 10, kTextW = kPaperW - 48, kTextH = kPaperY + kPaperH - 24 - kTextY;

// Text pages: the console font at 3x (title 4x), 83 columns by 28 lines.
constexpr int kTextScale = 3, kTitleScale = 4, kLabelScale = 4;
constexpr uint32_t kTitleInk = 0xFF1C3A6E, kStripe = 0xFFEDF0F4;

void fill(std::vector<uint32_t>& img, int x0, int y0, int x1, int y1, uint32_t c) {
    x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, Manual::kWidth), y1 = std::min(y1, Manual::kHeight);
    for (int y = y0; y < y1; y++) std::fill(&img[size_t(y) * Manual::kWidth + x0], &img[size_t(y) * Manual::kWidth + x1], c);
}

// `text` with its top-left at (x, y), `s` pixels per font pixel.
void draw_text(std::vector<uint32_t>& img, const Font& font, const std::string& text, int x, int y, int s, uint32_t c) {
    if (font.glyphs.empty()) return;
    for (unsigned char ch : text) {
        for (int r = 0; r < font.h; r++) {
            const uint8_t bits = font.glyphs[ch * font.h + r];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) fill(img, x + col * s, y + r * s, x + (col + 1) * s, y + (r + 1) * s, c);
        }
        x += font.w * s;
    }
}

// The clipboard without the page: board, paper and clip with its label.
void draw_board(std::vector<uint32_t>& img, const Font& font, const std::string& label) {
    const int W = Manual::kWidth, H = Manual::kHeight;
    img.assign(size_t(W) * H, kBoardEdge);
    fill(img, 8, 8, W - 8, H - 8, kBoard);
    fill(img, kPaperX + 10, kPaperY + 10, kPaperX + kPaperW + 10, kPaperY + kPaperH + 10, kShadow);
    fill(img, kPaperX, kPaperY, kPaperX + kPaperW, kPaperY + kPaperH, kPaper);
    const int cx0 = (W - kClipW) / 2;
    fill(img, cx0, kClipY0, cx0 + kClipW, kClipY1, kClipEdge);
    fill(img, cx0 + 6, kClipY0 + 6, cx0 + kClipW - 6, kClipY1 - 6, kClip);
    const int lw = int(label.size()) * font.w * kLabelScale, lh = font.h * kLabelScale;
    draw_text(img, font, label, (W - lw) / 2, (kClipY0 + kClipY1 - lh) / 2, kLabelScale, kInk);
}

struct TextPage {
    std::string title;
    std::vector<std::string> lines;
};

int text_rows(const Font& font) {
    return (kTextH - font.h * kTitleScale - font.h * kTextScale / 2) / (font.h * kTextScale);
}

void draw_text_page(std::vector<uint32_t>& img, const Font& font, const TextPage& page) {
    draw_text(img, font, page.title, kTextX, kTextY, kTitleScale, kTitleInk);
    const int lh = font.h * kTextScale, y0 = kTextY + font.h * kTitleScale + lh / 2;
    for (size_t i = 0; i < page.lines.size(); i++) {
        const int y = y0 + int(i) * lh;
        if (i % 2 && !page.lines[i].empty()) fill(img, kTextX - 8, y, kTextX + kTextW + 8, y + lh, kStripe);
        draw_text(img, font, page.lines[i].substr(0, Manual::kTextColumns), kTextX, y, kTextScale, kInk);
    }
}

}  // namespace

std::string find_manual(const std::string& explicit_path, const std::string& game_dir) {
    if (!explicit_path.empty()) return explicit_path;
    namespace fs = std::filesystem;
    for (const std::string& dir : {game_dir, std::string(".")}) {
        std::vector<fs::path> pdfs;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == ".pdf" && e.is_regular_file(ec)) pdfs.push_back(e.path());
        }
        std::sort(pdfs.begin(), pdfs.end());
        if (!pdfs.empty()) return pdfs.front().string();
    }
    return {};
}

struct Manual::Impl {
    Font font;
    std::vector<TextPage> text;
#ifdef F19_HAVE_POPPLER
    std::unique_ptr<poppler::document> doc;
#endif
    int pdf_pages = 0;
    mutable std::mutex mu;
    std::condition_variable cv;
    std::map<int, std::shared_ptr<const Image>> cache;
    int want = -1;
    bool stop = false;
    std::thread worker;

    int pages() const { return int(text.size()) + pdf_pages; }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void run() {
        std::unique_lock<std::mutex> lk(mu);
        while (!stop) {
            int target = -1;
            for (int d : {0, 1, -1, 2}) {
                int pg = want + d;
                if (want >= 0 && pg >= 0 && pg < pages() && !cache.count(pg)) {
                    target = pg;
                    break;
                }
            }
            if (target < 0) {
                cv.wait(lk);
                continue;
            }
            lk.unlock();
            auto img = render(target);
            lk.lock();
            cache[target] = std::move(img);
            for (auto it = cache.begin(); it != cache.end();)
                it = std::abs(it->first - want) > 3 ? cache.erase(it) : std::next(it);
        }
    }

    std::shared_ptr<const Image> render(int page) const {
        auto img = std::make_shared<Image>();
        img->page = page;
        const int nt = int(text.size());
        if (page < nt) {
            draw_board(img->argb, font, "CONTROLS " + std::to_string(page + 1) + "/" + std::to_string(nt));
            draw_text_page(img->argb, font, text[page]);
            return img;
        }
        const int index = page - nt;
        draw_board(img->argb, font, "MANUAL " + std::to_string(index + 1) + "/" + std::to_string(pdf_pages));
#ifdef F19_HAVE_POPPLER
        std::unique_ptr<poppler::page> pg(doc->create_page(index));
        if (!pg) return img;
        poppler::page_renderer r;
        r.set_render_hints(poppler::page_renderer::antialiasing | poppler::page_renderer::text_antialiasing |
                           poppler::page_renderer::text_hinting);
        r.set_image_format(poppler::image::format_argb32);
        const poppler::rectf box = pg->page_rect();
        const double pw = box.width(), ph = box.height();
        if (pw <= 0 || ph <= 0) return img;

        // Trim the margins: the printed area, found on a coarse render.
        double x0 = 0, y0 = 0, x1 = pw, y1 = ph;
        {
            const double dpi = 36;
            poppler::image probe = r.render_page(pg.get(), dpi, dpi);
            if (probe.is_valid() && probe.format() == poppler::image::format_argb32) {
                const int w = probe.width(), h = probe.height();
                int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
                for (int y = 1; y < h - 1; y++) {
                    auto row = reinterpret_cast<const uint32_t*>(probe.const_data() + size_t(y) * probe.bytes_per_row());
                    for (int x = 1; x < w - 1; x++) {
                        uint32_t c = row[x];
                        if (std::min({c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF}) < 200) {
                            bx0 = std::min(bx0, x), bx1 = std::max(bx1, x);
                            by0 = std::min(by0, y), by1 = std::max(by1, y);
                        }
                    }
                }
                if (bx1 > bx0 && by1 > by0) {
                    const double k = 72.0 / dpi, pad = 8;
                    x0 = std::max(0.0, bx0 * k - pad), x1 = std::min(pw, (bx1 + 1) * k + pad);
                    y0 = std::max(0.0, by0 * k - pad), y1 = std::min(ph, (by1 + 1) * k + pad);
                }
            }
        }

        // Fit the printed area into the paper's text area.
        const double s = std::min(kTextW / (x1 - x0), kTextH / (y1 - y0));  // pixels per point
        const int rx = int(x0 * s), ry = int(y0 * s);
        const int rw = std::min(int((x1 - x0) * s), kTextW), rh = std::min(int((y1 - y0) * s), kTextH);
        poppler::image pi = r.render_page(pg.get(), s * 72, s * 72, rx, ry, rw, rh);
        if (!pi.is_valid() || pi.format() != poppler::image::format_argb32) return img;
        const int ox = kTextX + (kTextW - pi.width()) / 2, oy = kTextY + (kTextH - pi.height()) / 2;
        for (int y = 0; y < pi.height(); y++) {
            auto row = reinterpret_cast<const uint32_t*>(pi.const_data() + size_t(y) * pi.bytes_per_row());
            uint32_t* dst = &img->argb[size_t(oy + y) * kWidth + ox];
            for (int x = 0; x < pi.width(); x++) dst[x] = row[x] | 0xFF000000u;
        }
#endif
        return img;
    }
};

Manual::Manual() : p_(std::make_unique<Impl>()) {}
Manual::~Manual() = default;

void Manual::set_font(const Font& font) { p_->font = font; }

void Manual::add_section(const Section& s) {
    // Groups (runs of lines between empty ones) stay on one page if they fit.
    const int rows = text_rows(p_->font);
    std::vector<std::vector<std::string>> groups(1);
    for (const std::string& l : s.lines) {
        if (l.empty()) {
            if (!groups.back().empty()) groups.emplace_back();
        } else {
            groups.back().push_back(l);
        }
    }
    bool first = true;
    auto new_page = [&] {
        p_->text.push_back(TextPage{first ? s.title : s.title + " (cont.)", {}});
        first = false;
    };
    new_page();
    for (const auto& g : groups) {
        if (g.empty()) continue;
        auto* lines = &p_->text.back().lines;
        const int used = int(lines->size()), need = int(g.size()) + (used ? 1 : 0);
        if (used && used + need > rows && int(g.size()) <= rows) {
            new_page();
            lines = &p_->text.back().lines;
        } else if (used) {
            lines->push_back("");
        }
        for (const std::string& l : g) {
            if (int(lines->size()) >= rows) {
                new_page();
                lines = &p_->text.back().lines;
            }
            lines->push_back(l);
        }
    }
}

int Manual::pages() const { return p_->pages(); }
int Manual::text_pages() const { return int(p_->text.size()); }

bool Manual::open(const std::string& path) {
#ifdef F19_HAVE_POPPLER
    std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(path));
    if (!doc || doc->is_locked() || doc->pages() <= 0) {
        std::fprintf(stderr, "warning: manual: cannot read %s\n", path.c_str());
        return false;
    }
    p_->pdf_pages = doc->pages();
    p_->doc = std::move(doc);
    std::fprintf(stderr, "manual: %s (%d pages)\n", path.c_str(), p_->pdf_pages);
    return true;
#else
    std::fprintf(stderr, "warning: manual: this build has no PDF support (poppler-cpp); cannot show %s\n", path.c_str());
    return false;
#endif
}

void Manual::request(int page) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->want = page;
    }
    if (!p_->worker.joinable()) p_->worker = std::thread([this] { p_->run(); });
    p_->cv.notify_all();
}

std::shared_ptr<const Manual::Image> Manual::get(int page) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->cache.find(page);
    return it != p_->cache.end() ? it->second : nullptr;
}

}  // namespace f19
