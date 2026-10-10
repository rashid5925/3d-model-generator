// Portable software rasterizer: the fallback when no GPU/Vulkan driver exists
// (headless servers, CI). 2x supersampled, multi-threaded over horizontal bands.
#include <algorithm>
#include <cmath>
#include <thread>

#include "render/renderer.hpp"
#include "render/shading.hpp"

namespace sf {

namespace {

struct ClipVert {
    Vec4 clip;
    Vec3 pos, normal, color;
};

ClipVert lerpClip(const ClipVert& a, const ClipVert& b, float t) {
    return {{a.clip.x + (b.clip.x - a.clip.x) * t, a.clip.y + (b.clip.y - a.clip.y) * t,
             a.clip.z + (b.clip.z - a.clip.z) * t, a.clip.w + (b.clip.w - a.clip.w) * t},
            lerp(a.pos, b.pos, t),
            lerp(a.normal, b.normal, t),
            lerp(a.color, b.color, t)};
}

// Screen-space vertex with attributes pre-divided by w for perspective correction.
struct ScreenVert {
    float x, y, z, invW;
    Vec3 posW, normalW, colorW;
};

struct ScreenTri {
    ScreenVert v[3];
    float minY, maxY;
};

ScreenVert toScreen(const ClipVert& c, int w, int h) {
    float iw = 1.0f / c.clip.w;
    return {(c.clip.x * iw * 0.5f + 0.5f) * w, (c.clip.y * iw * 0.5f + 0.5f) * h, c.clip.z * iw, iw,
            c.pos * iw, c.normal * iw, c.color * iw};
}

ClipVert makeClip(const Mat4& vp, const Vertex& v) {
    Vec3 p{v.pos[0], v.pos[1], v.pos[2]};
    return {vp * Vec4{p.x, p.y, p.z, 1.0f}, p, {v.normal[0], v.normal[1], v.normal[2]}, {v.color[0], v.color[1], v.color[2]}};
}

// Sutherland-Hodgman against the near plane (z >= 0 in Vulkan clip space) and w > 0.
int clipNear(const ClipVert in[3], ClipVert out[4]) {
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % 3];
        float da = a.clip.z, db = b.clip.z;
        if (da >= 0) out[n++] = a;
        if ((da >= 0) != (db >= 0)) out[n++] = lerpClip(a, b, da / (da - db));
    }
    return n;
}

class CpuRenderer : public Renderer {
public:
    std::string name() const override { return "CPU software rasterizer (2x SSAA)"; }

    Image render(const Frame& f) override {
        const int ss = 2;
        const int W = f.width * ss, H = f.height * ss;
        std::vector<float> depth(static_cast<size_t>(W) * H, 1.0f);
        std::vector<Vec3> color(static_cast<size_t>(W) * H, f.background);

        std::vector<ScreenTri> tris;
        tris.reserve(f.triangles.size() / 3);
        for (size_t i = 0; i + 2 < f.triangles.size(); i += 3) {
            ClipVert in[3] = {makeClip(f.viewProj, f.triangles[i]), makeClip(f.viewProj, f.triangles[i + 1]),
                              makeClip(f.viewProj, f.triangles[i + 2])};
            ClipVert poly[4];
            int n = clipNear(in, poly);
            for (int k = 1; k + 1 < n; ++k) {
                ScreenTri t{{toScreen(poly[0], W, H), toScreen(poly[k], W, H), toScreen(poly[k + 1], W, H)}, 0, 0};
                t.minY = std::min({t.v[0].y, t.v[1].y, t.v[2].y});
                t.maxY = std::max({t.v[0].y, t.v[1].y, t.v[2].y});
                if (t.maxY < 0 || t.minY >= H) continue;
                tris.push_back(t);
            }
        }

        int threads = static_cast<int>(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
        int band = (H + threads - 1) / threads;
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) {
            int y0 = t * band, y1 = std::min(H, y0 + band);
            if (y0 >= y1) break;
            pool.emplace_back([&, y0, y1] { rasterBand(f, tris, y0, y1, W, depth, color); });
        }
        for (auto& th : pool) th.join();

        drawLines(f, W, H, ss, depth, color);

        Image out(f.width, f.height);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                Vec3 acc{0, 0, 0};
                for (int sy = 0; sy < ss; ++sy)
                    for (int sx = 0; sx < ss; ++sx) acc += color[static_cast<size_t>(y * ss + sy) * W + (x * ss + sx)];
                acc = acc / static_cast<float>(ss * ss);
                out.set(x, y, {static_cast<uint8_t>(std::lround(std::clamp(acc.x, 0.0f, 1.0f) * 255)),
                               static_cast<uint8_t>(std::lround(std::clamp(acc.y, 0.0f, 1.0f) * 255)),
                               static_cast<uint8_t>(std::lround(std::clamp(acc.z, 0.0f, 1.0f) * 255))});
            }
        return out;
    }

private:
    static void rasterBand(const Frame& f, const std::vector<ScreenTri>& tris, int y0, int y1, int W,
                           std::vector<float>& depth, std::vector<Vec3>& color) {
        for (const auto& t : tris) {
            if (t.maxY < y0 || t.minY >= y1) continue;
            const ScreenVert &a = t.v[0], &b = t.v[1], &c = t.v[2];
            float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(area) < 1e-12f) continue;
            int minX = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
            int maxX = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
            int minY = std::max(y0, static_cast<int>(std::floor(t.minY)));
            int maxY = std::min(y1 - 1, static_cast<int>(std::ceil(t.maxY)));
            float invArea = 1.0f / area;
            for (int y = minY; y <= maxY; ++y) {
                float py = y + 0.5f;
                for (int x = minX; x <= maxX; ++x) {
                    float px = x + 0.5f;
                    float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * invArea;
                    float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * invArea;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float z = w0 * a.z + w1 * b.z + w2 * c.z;
                    if (z < 0 || z > 1) continue;
                    size_t idx = static_cast<size_t>(y) * W + x;
                    if (z >= depth[idx]) continue;
                    float iw = w0 * a.invW + w1 * b.invW + w2 * c.invW;
                    float pw = 1.0f / iw;
                    Vec3 pos = (a.posW * w0 + b.posW * w1 + c.posW * w2) * pw;
                    Vec3 nrm = (a.normalW * w0 + b.normalW * w1 + c.normalW * w2) * pw;
                    Vec3 col = (a.colorW * w0 + b.colorW * w1 + c.colorW * w2) * pw;
                    depth[idx] = z;
                    color[idx] = shadeFragment(f.uniforms, pos, nrm, col);
                }
            }
        }
    }

    static void drawLines(const Frame& f, int W, int H, int ss, std::vector<float>& depth, std::vector<Vec3>& color) {
        for (size_t i = 0; i + 1 < f.lines.size(); i += 2) {
            ClipVert a = makeClip(f.viewProj, f.lines[i]);
            ClipVert b = makeClip(f.viewProj, f.lines[i + 1]);
            if (a.clip.z < 0 && b.clip.z < 0) continue;
            if (a.clip.z < 0) a = lerpClip(a, b, a.clip.z / (a.clip.z - b.clip.z));
            else if (b.clip.z < 0) b = lerpClip(b, a, b.clip.z / (b.clip.z - a.clip.z));
            ScreenVert sa = toScreen(a, W, H), sb = toScreen(b, W, H);
            float dx = sb.x - sa.x, dy = sb.y - sa.y;
            int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
            if (steps > 20000) steps = 20000;
            Vec3 col = a.color;
            for (int s = 0; s <= steps; ++s) {
                float t = steps ? static_cast<float>(s) / steps : 0.0f;
                float x = sa.x + dx * t, y = sa.y + dy * t, z = sa.z + (sb.z - sa.z) * t;
                if (z < 0 || z > 1) continue;
                for (int oy = 0; oy < ss; ++oy)
                    for (int ox = 0; ox < ss; ++ox) {
                        int px = static_cast<int>(x) + ox - ss / 2, py = static_cast<int>(y) + oy - ss / 2;
                        if (px < 0 || py < 0 || px >= W || py >= H) continue;
                        size_t idx = static_cast<size_t>(py) * W + px;
                        if (z > depth[idx] + 1e-5f) continue;
                        depth[idx] = z;
                        color[idx] = col;
                    }
            }
        }
    }
};

}  // namespace

std::unique_ptr<Renderer> createCpuRenderer() { return std::make_unique<CpuRenderer>(); }

}  // namespace sf
