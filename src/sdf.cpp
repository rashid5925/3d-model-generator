#include "sdf.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace sf {

namespace {

float length2(float x, float y) { return std::sqrt(x * x + y * y); }
float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

// Exact signed distance to a closed 2D polygon (Inigo Quilez).
float sdPolygon(const std::vector<Vec2>& v, float px, float py) {
    size_t n = v.size();
    float d = (px - v[0].x) * (px - v[0].x) + (py - v[0].y) * (py - v[0].y);
    float s = 1.0f;
    for (size_t i = 0, j = n - 1; i < n; j = i, ++i) {
        float ex = v[j].x - v[i].x, ey = v[j].y - v[i].y;
        float wx = px - v[i].x, wy = py - v[i].y;
        float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
        float bx = wx - ex * t, by = wy - ey * t;
        d = std::min(d, bx * bx + by * by);
        bool c1 = py >= v[i].y, c2 = py < v[j].y, c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * std::sqrt(d);
}

class BoxSdf : public SdfNode {
public:
    BoxSdf(Vec3 size, float r) : half_(size * 0.5f), r_(std::min(r, minComponent(size * 0.5f))) {}
    float eval(const Vec3& p) const override {
        Vec3 q = vabs(p) - (half_ - Vec3{r_, r_, r_});
        return length(vmax(q, Vec3{0, 0, 0})) + std::min(maxComponent(q), 0.0f) - r_;
    }
    AABB bounds() const override { return AABB{-half_, half_}; }

private:
    Vec3 half_;
    float r_;
};

class SphereSdf : public SdfNode {
public:
    explicit SphereSdf(float r) : r_(r) {}
    float eval(const Vec3& p) const override { return length(p) - r_; }
    AABB bounds() const override { return AABB{{-r_, -r_, -r_}, {r_, r_, r_}}; }

private:
    float r_;
};

class CappedConeSdf : public SdfNode {
public:
    CappedConeSdf(float height, float r1, float r2) : h_(height * 0.5f), r1_(r1), r2_(r2) {}
    float eval(const Vec3& p) const override {
        float qx = length2(p.x, p.z), qy = p.y;
        float k1x = r2_, k1y = h_;
        float k2x = r2_ - r1_, k2y = 2.0f * h_;
        float cax = qx - std::min(qx, qy < 0.0f ? r1_ : r2_);
        float cay = std::fabs(qy) - h_;
        float t = clampf(((k1x - qx) * k2x + (k1y - qy) * k2y) / (k2x * k2x + k2y * k2y), 0.0f, 1.0f);
        float cbx = qx - k1x + k2x * t, cby = qy - k1y + k2y * t;
        float s = (cbx < 0.0f && cay < 0.0f) ? -1.0f : 1.0f;
        return s * std::sqrt(std::min(cax * cax + cay * cay, cbx * cbx + cby * cby));
    }
    AABB bounds() const override {
        float r = std::max(r1_, r2_);
        return AABB{{-r, -h_, -r}, {r, h_, r}};
    }

private:
    float h_, r1_, r2_;
};

class CapsuleSdf : public SdfNode {
public:
    CapsuleSdf(float r, float height) : r_(r), a_(std::max(0.0f, height * 0.5f - r)) {}
    float eval(const Vec3& p) const override {
        Vec3 q = p;
        q.y -= clampf(q.y, -a_, a_);
        return length(q) - r_;
    }
    AABB bounds() const override { return AABB{{-r_, -a_ - r_, -r_}, {r_, a_ + r_, r_}}; }

private:
    float r_, a_;
};

class TorusSdf : public SdfNode {
public:
    TorusSdf(float major, float tube) : R_(major), r_(tube) {}
    float eval(const Vec3& p) const override { return length2(length2(p.x, p.z) - R_, p.y) - r_; }
    AABB bounds() const override {
        float e = R_ + r_;
        return AABB{{-e, -r_, -e}, {e, r_, e}};
    }

private:
    float R_, r_;
};

class ExtrusionSdf : public SdfNode {
public:
    ExtrusionSdf(std::vector<Vec2> poly, float depth, char axis) : poly_(std::move(poly)), h_(depth * 0.5f), axis_(axis) {
        for (const auto& p : poly_) {
            b2_.expand(Vec3{p.x, p.y, 0});
        }
    }
    float eval(const Vec3& p) const override {
        float u, v, w;
        if (axis_ == 'y') {
            u = p.x; v = p.z; w = p.y;
        } else if (axis_ == 'x') {
            u = p.z; v = p.y; w = p.x;
        } else {
            u = p.x; v = p.y; w = p.z;
        }
        float d = sdPolygon(poly_, u, v);
        float wx = d, wy = std::fabs(w) - h_;
        return std::min(std::max(wx, wy), 0.0f) + length2(std::max(wx, 0.0f), std::max(wy, 0.0f));
    }
    AABB bounds() const override {
        Vec3 lo = b2_.min, hi = b2_.max;
        if (axis_ == 'y') return AABB{{lo.x, -h_, lo.y}, {hi.x, h_, hi.y}};
        if (axis_ == 'x') return AABB{{-h_, lo.y, lo.x}, {h_, hi.y, hi.x}};
        return AABB{{lo.x, lo.y, -h_}, {hi.x, hi.y, h_}};
    }

private:
    std::vector<Vec2> poly_;
    float h_;
    char axis_;
    AABB b2_;
};

class RevolveSdf : public SdfNode {
public:
    explicit RevolveSdf(std::vector<Vec2> profile) : prof_(std::move(profile)) {
        for (const auto& p : prof_) {
            rmax_ = std::max(rmax_, p.x);
            ymin_ = std::min(ymin_, p.y);
            ymax_ = std::max(ymax_, p.y);
        }
    }
    float eval(const Vec3& p) const override { return sdPolygon(prof_, length2(p.x, p.z), p.y); }
    AABB bounds() const override { return AABB{{-rmax_, ymin_, -rmax_}, {rmax_, ymax_, rmax_}}; }

private:
    std::vector<Vec2> prof_;
    float rmax_ = 0, ymin_ = 1e30f, ymax_ = -1e30f;
};

float smin(float a, float b, float k) {
    if (k <= 0) return std::min(a, b);
    float h = std::max(k - std::fabs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
float smax(float a, float b, float k) { return -smin(-a, -b, k); }

class CombineSdf : public SdfNode {
public:
    CombineSdf(CsgOp op, std::vector<SdfPtr> children, float k) : op_(op), kids_(std::move(children)), k_(k) {}
    float eval(const Vec3& p) const override {
        float d = kids_[0]->eval(p);
        for (size_t i = 1; i < kids_.size(); ++i) {
            float e = kids_[i]->eval(p);
            switch (op_) {
                case CsgOp::Union: d = smin(d, e, k_); break;
                case CsgOp::Subtract: d = smax(d, -e, k_); break;
                case CsgOp::Intersect: d = smax(d, e, k_); break;
            }
        }
        return d;
    }
    AABB bounds() const override {
        AABB b;
        if (op_ == CsgOp::Union) {
            for (const auto& c : kids_) b.expand(c->bounds());
            return b.padded(k_ * 0.25f);
        }
        b = kids_[0]->bounds();
        if (op_ == CsgOp::Intersect) {
            for (size_t i = 1; i < kids_.size(); ++i) {
                AABB c = kids_[i]->bounds();
                b.min = vmax(b.min, c.min);
                b.max = vmin(b.max, c.max);
            }
        }
        return b;
    }
    float minFeature() const override {
        float m = 1e30f;
        for (const auto& c : kids_) m = std::min(m, c->minFeature());
        return m;
    }

private:
    CsgOp op_;
    std::vector<SdfPtr> kids_;
    float k_;
};

class TransformSdf : public SdfNode {
public:
    TransformSdf(SdfPtr child, const Mat4& m) : child_(std::move(child)), m_(m), inv_(inverse(m)) {
        // Distances shrink/grow with scale; use the smallest axis scale so the field
        // stays conservative under non-uniform scaling.
        float sx = length(transformDir(m, {1, 0, 0}));
        float sy = length(transformDir(m, {0, 1, 0}));
        float sz = length(transformDir(m, {0, 0, 1}));
        scale_ = std::min(sx, std::min(sy, sz));
    }
    float eval(const Vec3& p) const override { return child_->eval(transformPoint(inv_, p)) * scale_; }
    AABB bounds() const override { return child_->bounds().transformed(m_); }
    float minFeature() const override { return child_->minFeature() * scale_; }

private:
    SdfPtr child_;
    Mat4 m_, inv_;
    float scale_ = 1;
};

class ShellSdf : public SdfNode {
public:
    ShellSdf(SdfPtr child, float t) : child_(std::move(child)), t_(t) {}
    float eval(const Vec3& p) const override {
        // Hollow the solid, keeping the outer surface in place (wall grows inward).
        float d = child_->eval(p);
        return std::max(d, -(d + t_));
    }
    AABB bounds() const override { return child_->bounds(); }
    float minFeature() const override { return std::min(child_->minFeature(), t_); }

private:
    SdfPtr child_;
    float t_;
};

}  // namespace

SdfPtr sdfBox(Vec3 size, float rounding) { return std::make_unique<BoxSdf>(size, rounding); }
SdfPtr sdfSphere(float radius) { return std::make_unique<SphereSdf>(radius); }
SdfPtr sdfCappedCone(float h, float rb, float rt) { return std::make_unique<CappedConeSdf>(h, rb, rt); }
SdfPtr sdfCapsule(float r, float h) { return std::make_unique<CapsuleSdf>(r, h); }
SdfPtr sdfTorus(float R, float r) { return std::make_unique<TorusSdf>(R, r); }
SdfPtr sdfExtrusion(std::vector<Vec2> poly, float depth, char axis) {
    return std::make_unique<ExtrusionSdf>(std::move(poly), depth, axis);
}
SdfPtr sdfRevolve(std::vector<Vec2> profile) { return std::make_unique<RevolveSdf>(std::move(profile)); }
SdfPtr sdfCombine(CsgOp op, std::vector<SdfPtr> children, float smooth) {
    return std::make_unique<CombineSdf>(op, std::move(children), smooth);
}
SdfPtr sdfTransform(SdfPtr child, const Mat4& m) { return std::make_unique<TransformSdf>(std::move(child), m); }
SdfPtr sdfShell(SdfPtr child, float thickness) { return std::make_unique<ShellSdf>(std::move(child), thickness); }

namespace {

template <typename Fn>
void parallelFor(int count, Fn&& fn) {
    int threads = static_cast<int>(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
    threads = std::min(threads, std::max(1, count));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (int i = t; i < count; i += threads) fn(i);
        });
    }
    for (auto& th : pool) th.join();
}

Vec3 gradient(const SdfNode& sdf, Vec3 p, float e) {
    return Vec3{sdf.eval(p + Vec3{e, 0, 0}) - sdf.eval(p - Vec3{e, 0, 0}),
                sdf.eval(p + Vec3{0, e, 0}) - sdf.eval(p - Vec3{0, e, 0}),
                sdf.eval(p + Vec3{0, 0, e}) - sdf.eval(p - Vec3{0, 0, e})} /
           (2.0f * e);
}

}  // namespace

int autoResolution(const SdfNode& sdf, int minRes, int maxRes, float cellsPerFeature) {
    AABB b = sdf.bounds();
    float feature = sdf.minFeature();
    if (!b.valid() || !(feature > 0)) return minRes;
    float want = maxComponent(b.size()) / (feature / cellsPerFeature);
    return std::clamp(static_cast<int>(std::ceil(want)), minRes, maxRes);
}

Mesh polygonize(const SdfNode& sdf, int resolution, float* cellSize) {
    Mesh mesh;
    AABB b = sdf.bounds();
    if (!b.valid()) return mesh;
    resolution = std::clamp(resolution, 8, 512);
    float extent = maxComponent(b.size());
    if (extent <= 0) return mesh;
    float h = extent / static_cast<float>(resolution);
    if (cellSize) *cellSize = h;
    Vec3 origin = b.min - Vec3{2 * h, 2 * h, 2 * h};
    Vec3 size = b.size();
    int nx = static_cast<int>(std::ceil(size.x / h)) + 5;
    int ny = static_cast<int>(std::ceil(size.y / h)) + 5;
    int nz = static_cast<int>(std::ceil(size.z / h)) + 5;
    auto sidx = [&](int i, int j, int k) { return (static_cast<size_t>(k) * ny + j) * nx + i; };

    std::vector<float> field(static_cast<size_t>(nx) * ny * nz);
    parallelFor(nz, [&](int k) {
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                field[sidx(i, j, k)] = sdf.eval(origin + Vec3{i * h, j * h, k * h});
    });

    // One vertex per cell that straddles the surface.
    int cx = nx - 1, cy = ny - 1, cz = nz - 1;
    auto cidx = [&](int i, int j, int k) { return (static_cast<size_t>(k) * cy + j) * cx + i; };
    std::vector<int32_t> cellVertex(static_cast<size_t>(cx) * cy * cz, -1);
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

    // First pass (parallel per slice): compute vertex positions.
    std::vector<std::vector<std::pair<size_t, Vec3>>> sliceVerts(cz);
    parallelFor(cz, [&](int k) {
        auto& out = sliceVerts[k];
        for (int j = 0; j < cy; ++j) {
            for (int i = 0; i < cx; ++i) {
                float d[8];
                Vec3 c[8];
                int inside = 0;
                for (int n = 0; n < 8; ++n) {
                    int di = n & 1, dj = (n >> 1) & 1, dk = (n >> 2) & 1;
                    d[n] = field[sidx(i + di, j + dj, k + dk)];
                    c[n] = origin + Vec3{(i + di) * h, (j + dj) * h, (k + dk) * h};
                    inside += d[n] < 0 ? 1 : 0;
                }
                if (inside == 0 || inside == 8) continue;
                Vec3 sum{0, 0, 0};
                int cnt = 0;
                for (const auto& e : kEdges) {
                    float a = d[e[0]], bb = d[e[1]];
                    if ((a < 0) == (bb < 0)) continue;
                    float t = a / (a - bb);
                    sum += lerp(c[e[0]], c[e[1]], t);
                    ++cnt;
                }
                Vec3 p = sum / static_cast<float>(cnt);
                // Project onto the true surface for crisper results.
                Vec3 start = p;
                for (int it = 0; it < 4; ++it) {
                    float dist = sdf.eval(p);
                    Vec3 g = gradient(sdf, p, h * 0.1f);
                    float g2 = dot(g, g);
                    if (g2 < 1e-12f) break;
                    p -= g * (dist / g2);
                }
                Vec3 delta = p - start;
                float dl = length(delta);
                if (dl > h) p = start + delta * (h / dl);
                out.emplace_back(cidx(i, j, k), p);
            }
        }
    });
    for (int k = 0; k < cz; ++k) {
        for (const auto& [ci, p] : sliceVerts[k]) {
            cellVertex[ci] = static_cast<int32_t>(mesh.addVertex(p, normalize(gradient(sdf, p, h * 0.1f))));
        }
    }

    // The four cells around a grid edge are listed counter-clockwise about the
    // edge axis for X and Z edges and clockwise for Y edges (see the orders
    // below), so orientation follows from topology alone; geometric tests are
    // unreliable on thin walls where projected quads can twist.
    auto emitQuad = [&](int32_t v0, int32_t v1, int32_t v2, int32_t v3, bool flip) {
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return;
        uint32_t q[4] = {static_cast<uint32_t>(v0), static_cast<uint32_t>(v1), static_cast<uint32_t>(v2),
                         static_cast<uint32_t>(v3)};
        if (flip) std::swap(q[1], q[3]);
        const auto& P = mesh.positions;
        // Split along the shorter diagonal.
        if (length(P[q[0]] - P[q[2]]) <= length(P[q[1]] - P[q[3]])) {
            mesh.addTriangle(q[0], q[1], q[2]);
            mesh.addTriangle(q[0], q[2], q[3]);
        } else {
            mesh.addTriangle(q[0], q[1], q[3]);
            mesh.addTriangle(q[1], q[2], q[3]);
        }
    };

    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                bool in0 = field[sidx(i, j, k)] < 0;  // inside at the lower end: normal points +axis
                if (i + 1 < nx && j >= 1 && k >= 1 && j < cy && k < cz && in0 != (field[sidx(i + 1, j, k)] < 0))
                    emitQuad(cellVertex[cidx(i, j - 1, k - 1)], cellVertex[cidx(i, j, k - 1)],
                             cellVertex[cidx(i, j, k)], cellVertex[cidx(i, j - 1, k)], !in0);
                if (j + 1 < ny && i >= 1 && k >= 1 && i < cx && k < cz && in0 != (field[sidx(i, j + 1, k)] < 0))
                    emitQuad(cellVertex[cidx(i - 1, j, k - 1)], cellVertex[cidx(i, j, k - 1)],
                             cellVertex[cidx(i, j, k)], cellVertex[cidx(i - 1, j, k)], in0);
                if (k + 1 < nz && i >= 1 && j >= 1 && i < cx && j < cy && in0 != (field[sidx(i, j, k + 1)] < 0))
                    emitQuad(cellVertex[cidx(i - 1, j - 1, k)], cellVertex[cidx(i, j - 1, k)],
                             cellVertex[cidx(i, j, k)], cellVertex[cidx(i - 1, j, k)], !in0);
            }
        }
    }
    return mesh;
}

}  // namespace sf
