// SPDX-License-Identifier: GPL-3.0-only
#include "danmaku-strip.hpp"

#include <borealis/core/assets.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <algorithm>
#include <cstring>
#include <list>
#include <string>
#include <vector>

namespace cm0 {
namespace {
constexpr int maxWordWidth = 640;
constexpr int maxWordHeight = 52;
constexpr size_t cacheBudget = 512 * 1024;
constexpr size_t maxEntries = 96;

// Decode the single UTF-8 line used by ordinary rolling comments.
uint32_t next(std::string_view& text) {
    const auto head = uint8_t(text.front());
    const int count = head < 0x80 ? 1 : head < 0xe0 ? 2 : head < 0xf0 ? 3 : 4;
    if ((count != 1 && head < 0xc2) || head > 0xf4 || text.size() < size_t(count)) return 0;
    uint32_t code = head & (0xff >> (count == 1 ? 1 : count + 1));
    for (int i = 1; i < count; ++i) {
        const auto byte = uint8_t(text[i]);
        if ((byte & 0xc0) != 0x80) return 0;
        code = (code << 6) | (byte & 0x3f);
    }
    text.remove_prefix(count);
    constexpr uint32_t minimum[]{0, 0x20, 0x80, 0x800, 0x10000};
    return code >= minimum[count] && code <= 0x10ffff &&
        (code < 0xd800 || code > 0xdfff) ? code : 0;
}

struct Word {
    std::string text;
    int size;
    uint32_t color;
    unsigned opacity;
    int width = 0, height = 0;
    std::vector<uint16_t> pixels;
};

struct Span {
    int left = DanmakuStrip::width, right = 0;
    void include(int x0, int x1) {
        left = std::min(left, x0);
        right = std::max(right, x1);
    }
    int length() const { return std::max(0, right - left); }
};
}

struct DanmakuStrip::State {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::list<Word> words;
    size_t used = 0;
    std::array<uint8_t, maxWordWidth * maxWordHeight> mask{};
    std::vector<uint16_t> pixels = std::vector<uint16_t>(width * height);
    std::array<Span, height> rows{}, scanned[2]{};
    bool active = false;

    State() {
        if (FT_Init_FreeType(&library) == 0)
            FT_New_Face(library, BRLS_ASSET("font/switch_font.ttf"), 0, &face);
    }
    ~State() {
        if (face) FT_Done_Face(face);
        if (library) FT_Done_FreeType(library);
    }

    void rasterize(Word& word) {
        if (FT_Set_Pixel_Sizes(face, 0, word.size)) return;
        const int glyphHeight = std::min(maxWordHeight, word.size + 10);
        const int baseline = (face->size->metrics.ascender + 63) / 64 + 1;
        std::fill_n(mask.begin(), maxWordWidth * glyphHeight, 0);
        int pen = 2;
        int right = 2;
        std::string_view remaining = word.text;
        FT_UInt previous = 0;
        while (!remaining.empty()) {
            const uint32_t code = next(remaining);
            const FT_UInt index = code ? FT_Get_Char_Index(face, code) : 0;
            if (!index || FT_Load_Glyph(face, index, FT_LOAD_DEFAULT)) return;
            if (previous && FT_HAS_KERNING(face)) {
                FT_Vector kern{};
                FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kern);
                pen += kern.x / 64;
            }
            previous = index;
            const auto glyph = face->glyph;
            const int advance = (glyph->advance.x + 32) / 64;
            FT_GlyphSlot_Embolden(glyph);
            if (FT_Render_Glyph(glyph, FT_RENDER_MODE_NORMAL)) return;
            const auto& bitmap = glyph->bitmap;
            if (bitmap.width && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) return;
            const int left = pen + glyph->bitmap_left;
            const int top = baseline - glyph->bitmap_top;
            right = std::max(right, left + int(bitmap.width) + 1);
            if (right > maxWordWidth || pen + advance >= maxWordWidth) return;
            for (int y = std::max(0, -top); y < std::min(int(bitmap.rows), glyphHeight - top); ++y) {
                const auto* src = bitmap.buffer + y * bitmap.pitch;
                auto* dst = mask.data() + (top + y) * maxWordWidth;
                for (int x = std::max(0, -left); x < int(bitmap.width); ++x)
                    dst[left + x] = std::max(dst[left + x], src[x]);
            }
            pen += advance;
        }
        word.width = right;
        word.height = glyphHeight;
        word.pixels.resize(right * glyphHeight);
        const unsigned r = (word.color >> 16) & 255, g = (word.color >> 8) & 255, b = word.color & 255;
        const bool lightOutline = r * 299 + g * 587 + b * 114 < 60000;
        for (int y = 0; y < glyphHeight; ++y) {
            const auto* row = mask.data() + y * maxWordWidth;
            for (int x = 0; x < right; ++x) {
                const unsigned fill = row[x];
                unsigned edge = std::max({fill, x ? unsigned(row[x-1]) : 0u,
                                          x + 1 < right ? unsigned(row[x+1]) : 0u});
                if (y) edge = std::max(edge, unsigned(row[x-maxWordWidth]));
                if (y + 1 < glyphHeight) edge = std::max(edge, unsigned(row[x+maxWordWidth]));
                const unsigned coverage = fill * word.opacity / 255;
                const unsigned outline = (edge - fill) * word.opacity / 255;
                const unsigned halo = lightOutline ? outline : 0;
                word.pixels[y * right + x] = ((coverage + outline + 8) / 17 << 12) |
                    ((r * coverage / 255 + halo + 8) / 17 << 8) |
                    ((g * coverage / 255 + halo + 8) / 17 << 4) |
                    ((b * coverage / 255 + halo + 8) / 17);
            }
        }
    }

    const Word& get(std::string_view text, int size, uint32_t color, unsigned opacity) {
        if (auto it = std::find_if(words.begin(), words.end(), [&](const Word& w) {
                return w.size == size && w.color == color && w.opacity == opacity && w.text == text;
            }); it != words.end()) {
            words.splice(words.begin(), words, it);
            return words.front();
        }
        auto& word = words.emplace_front(Word{std::string(text), size, color, opacity});
        rasterize(word);
        used += word.pixels.size() * sizeof(uint16_t);
        while (used > cacheBudget || words.size() > maxEntries) {
            used -= words.back().pixels.size() * sizeof(uint16_t);
            words.pop_back();
        }
        return word;
    }
};

DanmakuStrip::DanmakuStrip() : state(std::make_unique<State>()) {}
DanmakuStrip::~DanmakuStrip() = default;
bool DanmakuStrip::ready() const { return state->face; }
bool DanmakuStrip::active() const { return state->active; }

bool DanmakuStrip::paint(std::string_view text, int size, uint32_t color, unsigned alpha,
                         int x, int y, const Rect& clip) {
    if (text.empty() || text.size() > 240 || size < 10 || size > 40) return false;
    auto& s = *state;
    const auto& word = s.get(text, size, color, alpha);
    if (word.pixels.empty()) return false;
    const int x0 = std::max({x, clip[0], 0}), x1 = std::min({x + word.width, clip[2], width});
    const int y0 = std::max({y, clip[1], 0}), y1 = std::min({y + word.height, clip[3], height});
    if (x0 >= x1 || y0 >= y1) return true;
    for (int row = y0; row < y1; ++row) {
        auto* dst = s.pixels.data() + row * width;
        const auto* src = word.pixels.data() + (row - y) * word.width;
        for (int col = x0; col < x1; ++col) {
            if (const auto pixel = src[col - x])
                dst[col] = pixel;
        }
        s.rows[row].include(x0, x1);
    }
    s.active = true;
    return true;
}

void DanmakuStrip::copyTo(unsigned page, uint8_t* target, unsigned pitch) {
    auto& s = *state;
    for (int y = 0; y < height; ++y) {
        const auto now = s.rows[y];
        auto dirty = s.scanned[page][y];
        if (now.length()) dirty.include(now.left, now.right);
        if (dirty.length()) {
            const auto byteOffset = dirty.left * sizeof(uint16_t);
            std::memcpy(target + y * pitch + byteOffset, s.pixels.data() + y * width + dirty.left,
                        dirty.length() * sizeof(uint16_t));
        }
        s.scanned[page][y] = now;
    }
}

void DanmakuStrip::clear() {
    auto& s = *state;
    if (!s.active) return;
    for (int y = 0; y < height; ++y) {
        const auto row = s.rows[y];
        std::fill_n(s.pixels.data() + y * width + row.left, row.length(), 0);
        s.rows[y] = {};
    }
    s.active = false;
}
}
