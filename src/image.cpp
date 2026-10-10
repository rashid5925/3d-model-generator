#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>


namespace sf {

Image::Image(int w, int h, Rgb fill) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 3) {
    for (size_t i = 0; i < pixels.size(); i += 3) {
        pixels[i] = fill.r;
        pixels[i + 1] = fill.g;
        pixels[i + 2] = fill.b;
    }
}

void Image::set(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    size_t i = (static_cast<size_t>(y) * width + x) * 3;
    pixels[i] = c.r;
    pixels[i + 1] = c.g;
    pixels[i + 2] = c.b;
}

void Image::blend(int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    size_t i = (static_cast<size_t>(y) * width + x) * 3;
    a = std::clamp(a, 0.0f, 1.0f);
    pixels[i] = static_cast<uint8_t>(pixels[i] + (c.r - pixels[i]) * a);
    pixels[i + 1] = static_cast<uint8_t>(pixels[i + 1] + (c.g - pixels[i + 1]) * a);
    pixels[i + 2] = static_cast<uint8_t>(pixels[i + 2] + (c.b - pixels[i + 2]) * a);
}

Rgb Image::get(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return {};
    size_t i = (static_cast<size_t>(y) * width + x) * 3;
    return {pixels[i], pixels[i + 1], pixels[i + 2]};
}

void blit(Image& dst, const Image& src, int ox, int oy) {
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x) dst.set(ox + x, oy + y, src.get(x, y));
}

void fillRect(Image& img, int x, int y, int w, int h, Rgb c, float alpha) {
    for (int yy = std::max(0, y); yy < std::min(img.height, y + h); ++yy)
        for (int xx = std::max(0, x); xx < std::min(img.width, x + w); ++xx) {
            if (alpha >= 1.0f) img.set(xx, yy, c);
            else img.blend(xx, yy, c, alpha);
        }
}

void drawLine(Image& img, float x0, float y0, float x1, float y1, Rgb c, float thickness) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    int steps = std::max(1, static_cast<int>(std::ceil(len * 2)));
    float r = thickness * 0.5f;
    for (int s = 0; s <= steps; ++s) {
        float t = static_cast<float>(s) / steps;
        float px = x0 + dx * t, py = y0 + dy * t;
        for (int yy = static_cast<int>(std::floor(py - r)); yy <= static_cast<int>(std::ceil(py + r)); ++yy)
            for (int xx = static_cast<int>(std::floor(px - r)); xx <= static_cast<int>(std::ceil(px + r)); ++xx) {
                float ddx = xx + 0.5f - px, ddy = yy + 0.5f - py;
                float d = std::sqrt(ddx * ddx + ddy * ddy);
                float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                if (cov > 0) img.blend(xx, yy, c, cov);
            }
    }
}

namespace {

struct Glyph {
    char ch;
    uint8_t rows[7];
};

const Glyph kFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},                      {'.', {0, 0, 0, 0, 0, 0x0C, 0x0C}},
    {',', {0, 0, 0, 0, 0x0C, 0x04, 0x08}},             {':', {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0}},
    {'-', {0, 0, 0, 0x1F, 0, 0, 0}},                   {'+', {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'/', {0, 0x01, 0x02, 0x04, 0x08, 0x10, 0}},       {'=', {0, 0, 0x1F, 0, 0x1F, 0, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 0x1F}},                   {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}}, {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {'*', {0, 0x04, 0x15, 0x0E, 0x15, 0x04, 0}},       {'\'', {0x04, 0x04, 0x08, 0, 0, 0, 0}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04}},    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'^', {0x04, 0x0A, 0x11, 0, 0, 0, 0}},
    {'"', {0x0A, 0x0A, 0, 0, 0, 0, 0}},                {'|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
};

const Glyph* findGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const auto& g : kFont)
        if (g.ch == c) return &g;
    for (const auto& g : kFont)
        if (g.ch == '?') return &g;
    return nullptr;
}

}  // namespace

int textWidth(const std::string& text, int scale) {
    return text.empty() ? 0 : static_cast<int>(text.size()) * 6 * scale - scale;
}

int drawText(Image& img, int x, int y, const std::string& text, Rgb c, int scale) {
    int cx = x;
    for (char ch : text) {
        const Glyph* g = findGlyph(ch);
        if (g) {
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (g->rows[row] & (0x10 >> col)) fillRect(img, cx + col * scale, y + row * scale, scale, scale, c);
        }
        cx += 6 * scale;
    }
    return cx - x;
}

void drawLabel(Image& img, int x, int y, const std::string& text, Rgb fg, Rgb bg, int scale, float bgAlpha) {
    int pad = scale * 2;
    fillRect(img, x - pad, y - pad, textWidth(text, scale) + 2 * pad, textHeight(scale) + 2 * pad, bg, bgAlpha);
    drawText(img, x, y, text, fg, scale);
}

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

// Minimal DEFLATE encoder (RFC 1951): LZ77 with hash chains + the fixed Huffman
// code, wrapped in a zlib stream (RFC 1950). No external zlib needed, which keeps
// release binaries free of dependencies.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void bits(uint32_t value, int count) {  // LSB-first
        buf_ |= static_cast<uint64_t>(value) << n_;
        n_ += count;
        while (n_ >= 8) {
            out_.push_back(static_cast<uint8_t>(buf_));
            buf_ >>= 8;
            n_ -= 8;
        }
    }
    void huffman(uint32_t code, int len) {  // Huffman codes go MSB-first
        uint32_t rev = 0;
        for (int i = 0; i < len; ++i) rev |= ((code >> i) & 1u) << (len - 1 - i);
        bits(rev, len);
    }
    void flush() {
        if (n_ > 0) out_.push_back(static_cast<uint8_t>(buf_));
        buf_ = 0;
        n_ = 0;
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t buf_ = 0;
    int n_ = 0;
};

void fixedLiteral(BitWriter& w, int sym) {
    if (sym < 144) w.huffman(0x30 + sym, 8);
    else if (sym < 256) w.huffman(0x190 + (sym - 144), 9);
    else if (sym < 280) w.huffman(sym - 256, 7);
    else w.huffman(0xC0 + (sym - 280), 8);
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void emitMatch(BitWriter& w, int len, int dist) {
    int li = 28;
    while (kLenBase[li] > len) --li;
    fixedLiteral(w, 257 + li);
    w.bits(static_cast<uint32_t>(len - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) --di;
    w.huffman(static_cast<uint32_t>(di), 5);
    w.bits(static_cast<uint32_t>(dist - kDistBase[di]), kDistExtra[di]);
}

std::vector<uint8_t> zlibCompress(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out{0x78, 0x01};
    BitWriter w(out);
    w.bits(1, 1);  // BFINAL
    w.bits(1, 2);  // BTYPE = fixed Huffman

    const int kWindow = 32768, kHashBits = 15, kMaxChain = 48, kMinMatch = 3, kMaxMatch = 258;
    std::vector<int32_t> head(1 << kHashBits, -1), prev(kWindow, -1);
    const size_t n = raw.size();
    auto hashAt = [&](size_t i) {
        uint32_t v = (static_cast<uint32_t>(raw[i]) << 16) | (static_cast<uint32_t>(raw[i + 1]) << 8) | raw[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        uint32_t h = hashAt(i);
        prev[i % kWindow] = head[h];
        head[h] = static_cast<int32_t>(i);
    };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + kMinMatch <= n) {
            int32_t cand = head[hashAt(i)];
            int chain = kMaxChain;
            size_t maxLen = std::min<size_t>(kMaxMatch, n - i);
            while (cand >= 0 && chain-- > 0 && i - static_cast<size_t>(cand) <= static_cast<size_t>(kWindow - 1)) {
                const uint8_t* a = raw.data() + cand;
                const uint8_t* b = raw.data() + i;
                if (a[bestLen] == b[bestLen]) {
                    size_t l = 0;
                    while (l < maxLen && a[l] == b[l]) ++l;
                    if (static_cast<int>(l) > bestLen) {
                        bestLen = static_cast<int>(l);
                        bestDist = static_cast<int>(i - static_cast<size_t>(cand));
                        if (l == maxLen) break;
                    }
                }
                cand = prev[static_cast<size_t>(cand) % kWindow];
            }
        }
        if (bestLen >= kMinMatch) {
            emitMatch(w, bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) insert(i + k);
            i += bestLen;
        } else {
            fixedLiteral(w, raw[i]);
            insert(i);
            ++i;
        }
    }
    fixedLiteral(w, 256);  // end of block
    w.flush();

    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(out, (b << 16) | a);
    return out;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::vector<uint8_t> encodePng(const Image& img) {
    const int bpp = 3;
    size_t stride = static_cast<size_t>(img.width) * bpp;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * img.height);
    std::vector<uint8_t> cand[3];
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + y * stride;
        const uint8_t* prev = y > 0 ? row - stride : nullptr;
        // Pick the filter (None, Sub, Up) with the smallest sum of absolute values.
        for (auto& c : cand) c.assign(stride, 0);
        for (size_t i = 0; i < stride; ++i) {
            uint8_t left = i >= bpp ? row[i - bpp] : 0;
            uint8_t up = prev ? prev[i] : 0;
            cand[0][i] = row[i];
            cand[1][i] = static_cast<uint8_t>(row[i] - left);
            cand[2][i] = static_cast<uint8_t>(row[i] - up);
        }
        int best = 0;
        long bestScore = -1;
        for (int f = 0; f < 3; ++f) {
            long score = 0;
            for (uint8_t v : cand[f]) score += v < 128 ? v : 256 - v;
            if (bestScore < 0 || score < bestScore) {
                bestScore = score;
                best = f;
            }
        }
        raw.push_back(static_cast<uint8_t>(best == 0 ? 0 : best == 1 ? 1 : 2));
        raw.insert(raw.end(), cand[best].begin(), cand[best].end());
    }

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(img.width));
    put32(ihdr, static_cast<uint32_t>(img.height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", zlibCompress(raw));
    chunk(png, "IEND", {});
    return png;
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t v = data[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

}  // namespace sf
