// RGB8 images, 2D overlay drawing (lines, rectangles, bitmap text), PNG encoding
// and base64 — everything needed to hand a rendered picture to an LLM.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGB, row-major, top row first

    Image() = default;
    Image(int w, int h, Rgb fill = {});

    void set(int x, int y, Rgb c);
    void blend(int x, int y, Rgb c, float alpha);
    Rgb get(int x, int y) const;
};

void blit(Image& dst, const Image& src, int x, int y);
void fillRect(Image& img, int x, int y, int w, int h, Rgb c, float alpha = 1.0f);
void drawLine(Image& img, float x0, float y0, float x1, float y1, Rgb c, float thickness = 1.0f);
// 5x7 bitmap font. Lowercase is drawn as uppercase. Returns the drawn width.
int drawText(Image& img, int x, int y, const std::string& text, Rgb c, int scale = 2);
int textWidth(const std::string& text, int scale = 2);
inline int textHeight(int scale = 2) { return 7 * scale; }
// Text on a translucent dark backing box for legibility over geometry.
void drawLabel(Image& img, int x, int y, const std::string& text, Rgb fg, Rgb bg, int scale = 2, float bgAlpha = 0.75f);

std::vector<uint8_t> encodePng(const Image& img);
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);
std::string base64Encode(const std::vector<uint8_t>& data);
bool writeFile(const std::string& path, const std::vector<uint8_t>& data);

}  // namespace sf
