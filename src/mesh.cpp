#include "mesh.hpp"

#include <cmath>
#include <stdexcept>

namespace sf {

void Mesh::append(const Mesh& other) {
    uint32_t base = static_cast<uint32_t>(positions.size());
    positions.insert(positions.end(), other.positions.begin(), other.positions.end());
    normals.insert(normals.end(), other.normals.begin(), other.normals.end());
    indices.reserve(indices.size() + other.indices.size());
    for (uint32_t i : other.indices) indices.push_back(base + i);
}

void Mesh::transform(const Mat4& m) {
    Mat4 normalMat = transpose(inverse(m));
    for (auto& p : positions) p = transformPoint(m, p);
    for (auto& n : normals) n = normalize(transformDir(normalMat, n));
    if (determinant3(m) < 0) {
        for (size_t i = 0; i + 2 < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
    }
}

AABB Mesh::bounds() const {
    AABB b;
    for (const auto& p : positions) b.expand(p);
    return b;
}

double Mesh::volume() const {
    double v = 0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vec3 a = positions[indices[i]], b = positions[indices[i + 1]], c = positions[indices[i + 2]];
        v += static_cast<double>(dot(a, cross(b, c)));
    }
    return v / 6.0;
}

double Mesh::surfaceArea() const {
    double s = 0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vec3 a = positions[indices[i]], b = positions[indices[i + 1]], c = positions[indices[i + 2]];
        s += 0.5 * static_cast<double>(length(cross(b - a, c - a)));
    }
    return s;
}

namespace {

void addQuadFace(Mesh& m, Vec3 center, Vec3 u, Vec3 v) {
    Vec3 n = normalize(cross(u, v));
    uint32_t a = m.addVertex(center - u - v, n);
    uint32_t b = m.addVertex(center + u - v, n);
    uint32_t c = m.addVertex(center + u + v, n);
    uint32_t d = m.addVertex(center - u + v, n);
    m.addTriangle(a, b, c);
    m.addTriangle(a, c, d);
}

bool isDegenerate(Vec3 a, Vec3 b, Vec3 c) { return length(cross(b - a, c - a)) < 1e-12f; }

}  // namespace

Mesh makeBox(Vec3 size) {
    Vec3 h = size * 0.5f;
    Mesh m;
    addQuadFace(m, {h.x, 0, 0}, {0, 0, -h.z}, {0, h.y, 0});
    addQuadFace(m, {-h.x, 0, 0}, {0, 0, h.z}, {0, h.y, 0});
    addQuadFace(m, {0, h.y, 0}, {h.x, 0, 0}, {0, 0, -h.z});
    addQuadFace(m, {0, -h.y, 0}, {h.x, 0, 0}, {0, 0, h.z});
    addQuadFace(m, {0, 0, h.z}, {h.x, 0, 0}, {0, h.y, 0});
    addQuadFace(m, {0, 0, -h.z}, {-h.x, 0, 0}, {0, h.y, 0});
    return m;
}

Mesh makePlane(float sizeX, float sizeZ) {
    Mesh m;
    addQuadFace(m, {0, 0, 0}, {sizeX * 0.5f, 0, 0}, {0, 0, -sizeZ * 0.5f});
    return m;
}

Mesh makeLathe(const std::vector<Vec2>& rawProfile, int segments, float creaseDeg) {
    Mesh m;
    // Drop consecutive duplicates.
    std::vector<Vec2> profile;
    for (const auto& p : rawProfile) {
        if (!profile.empty() && std::fabs(profile.back().x - p.x) < 1e-7f && std::fabs(profile.back().y - p.y) < 1e-7f)
            continue;
        profile.push_back({std::max(0.0f, p.x), p.y});
    }
    if (profile.size() < 2 || segments < 3) return m;

    size_t nseg = profile.size() - 1;
    // Outward 2D normal (in r, y space) of each profile segment.
    std::vector<Vec2> segN(nseg);
    for (size_t s = 0; s < nseg; ++s) {
        float dr = profile[s + 1].x - profile[s].x, dy = profile[s + 1].y - profile[s].y;
        float l = std::sqrt(dr * dr + dy * dy);
        segN[s] = {dy / l, -dr / l};
    }
    float cosCrease = std::cos(radians(creaseDeg));
    auto vertexNormal = [&](size_t s, bool atStart) -> Vec2 {
        Vec2 n = segN[s];
        size_t other;
        if (atStart) {
            if (s == 0) return n;
            other = s - 1;
        } else {
            if (s + 1 >= nseg) return n;
            other = s + 1;
        }
        Vec2 o = segN[other];
        if (n.x * o.x + n.y * o.y < cosCrease) return n;
        Vec2 avg{n.x + o.x, n.y + o.y};
        float l = std::sqrt(avg.x * avg.x + avg.y * avg.y);
        return l > 1e-8f ? Vec2{avg.x / l, avg.y / l} : n;
    };

    for (size_t s = 0; s < nseg; ++s) {
        Vec2 p0 = profile[s], p1 = profile[s + 1];
        Vec2 n0 = vertexNormal(s, true), n1 = vertexNormal(s, false);
        uint32_t base = static_cast<uint32_t>(m.positions.size());
        for (int k = 0; k <= segments; ++k) {
            float t = 2.0f * kPi * static_cast<float>(k % segments) / static_cast<float>(segments);
            float c = std::cos(t), sn = std::sin(t);
            m.addVertex({p0.x * c, p0.y, -p0.x * sn}, normalize(Vec3{n0.x * c, n0.y, -n0.x * sn}));
            m.addVertex({p1.x * c, p1.y, -p1.x * sn}, normalize(Vec3{n1.x * c, n1.y, -n1.x * sn}));
        }
        for (int k = 0; k < segments; ++k) {
            uint32_t a = base + 2 * k, d = base + 2 * k + 1;
            uint32_t b = base + 2 * (k + 1), c = base + 2 * (k + 1) + 1;
            if (!isDegenerate(m.positions[a], m.positions[b], m.positions[c])) m.addTriangle(a, b, c);
            if (!isDegenerate(m.positions[a], m.positions[c], m.positions[d])) m.addTriangle(a, c, d);
        }
    }
    return m;
}

Mesh makeSphere(float radius, int segments, int rings) {
    rings = std::max(rings, 2);
    std::vector<Vec2> profile;
    for (int i = 0; i <= rings; ++i) {
        float phi = -kPi * 0.5f + kPi * static_cast<float>(i) / static_cast<float>(rings);
        profile.push_back({radius * std::cos(phi), radius * std::sin(phi)});
    }
    profile.front().x = 0;
    profile.back().x = 0;
    return makeLathe(profile, segments, 89.0f);
}

Mesh makeCylinder(float radiusBottom, float radiusTop, float height, int segments) {
    float h = height * 0.5f;
    std::vector<Vec2> profile{{0, -h}, {radiusBottom, -h}, {radiusTop, h}, {0, h}};
    return makeLathe(profile, segments, 30.0f);
}

Mesh makeCapsule(float radius, float height, int segments) {
    float half = std::max(0.0f, height * 0.5f - radius);
    int capRings = std::max(4, segments / 4);
    std::vector<Vec2> profile;
    for (int i = 0; i <= capRings; ++i) {
        float phi = -kPi * 0.5f + (kPi * 0.5f) * static_cast<float>(i) / static_cast<float>(capRings);
        profile.push_back({radius * std::cos(phi), -half + radius * std::sin(phi)});
    }
    for (int i = 0; i <= capRings; ++i) {
        float phi = (kPi * 0.5f) * static_cast<float>(i) / static_cast<float>(capRings);
        profile.push_back({radius * std::cos(phi), half + radius * std::sin(phi)});
    }
    profile.front().x = 0;
    profile.back().x = 0;
    return makeLathe(profile, segments, 89.0f);
}

Mesh makeTorus(float majorRadius, float tubeRadius, int segments, int sides) {
    Mesh m;
    for (int i = 0; i <= segments; ++i) {
        float u = 2.0f * kPi * static_cast<float>(i % segments) / static_cast<float>(segments);
        Vec3 d{std::cos(u), 0, -std::sin(u)};
        for (int j = 0; j <= sides; ++j) {
            float v = 2.0f * kPi * static_cast<float>(j % sides) / static_cast<float>(sides);
            Vec3 n = d * std::cos(v) + Vec3{0, 1, 0} * std::sin(v);
            m.addVertex(d * majorRadius + n * tubeRadius, n);
        }
    }
    uint32_t row = static_cast<uint32_t>(sides + 1);
    for (int i = 0; i < segments; ++i) {
        for (int j = 0; j < sides; ++j) {
            uint32_t a = i * row + j, b = (i + 1) * row + j, c = (i + 1) * row + j + 1, d = i * row + j + 1;
            m.addTriangle(a, b, c);
            m.addTriangle(a, c, d);
        }
    }
    return m;
}

float signedArea(const std::vector<Vec2>& poly) {
    double a = 0;
    for (size_t i = 0, n = poly.size(); i < n; ++i) {
        const Vec2& p = poly[i];
        const Vec2& q = poly[(i + 1) % n];
        a += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    return static_cast<float>(a * 0.5);
}

namespace {
float cross2(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
bool pointInTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    float d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
    return d1 >= 0 && d2 >= 0 && d3 >= 0;
}
}  // namespace

bool triangulatePolygon(const std::vector<Vec2>& polygon, std::vector<uint32_t>& out) {
    size_t n = polygon.size();
    if (n < 3) return false;
    float area = signedArea(polygon);
    if (std::fabs(area) < 1e-12f) return false;
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = static_cast<uint32_t>(area > 0 ? i : n - 1 - i);

    size_t guard = 0;
    while (idx.size() > 3) {
        bool clipped = false;
        size_t m = idx.size();
        for (size_t i = 0; i < m; ++i) {
            uint32_t ia = idx[(i + m - 1) % m], ib = idx[i], ic = idx[(i + 1) % m];
            Vec2 a = polygon[ia], b = polygon[ib], c = polygon[ic];
            if (cross2(a, b, c) <= 1e-12f) continue;  // reflex or collinear
            bool contains = false;
            for (size_t k = 0; k < m; ++k) {
                uint32_t ik = idx[k];
                if (ik == ia || ik == ib || ik == ic) continue;
                if (pointInTriangle(polygon[ik], a, b, c)) {
                    contains = true;
                    break;
                }
            }
            if (contains) continue;
            out.push_back(ia);
            out.push_back(ib);
            out.push_back(ic);
            idx.erase(idx.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Remove a collinear vertex if one exists, otherwise give up.
            bool removed = false;
            for (size_t i = 0; i < m && !removed; ++i) {
                Vec2 a = polygon[idx[(i + m - 1) % m]], b = polygon[idx[i]], c = polygon[idx[(i + 1) % m]];
                if (std::fabs(cross2(a, b, c)) <= 1e-12f) {
                    idx.erase(idx.begin() + static_cast<long>(i));
                    removed = true;
                }
            }
            if (!removed) return false;
        }
        if (++guard > n * n + 16) return false;
    }
    out.push_back(idx[0]);
    out.push_back(idx[1]);
    out.push_back(idx[2]);
    return true;
}

Mesh makeExtrusion(const std::vector<Vec2>& rawPolygon, float depth, char axis) {
    std::vector<Vec2> poly;
    for (const auto& p : rawPolygon) {
        if (!poly.empty() && std::fabs(poly.back().x - p.x) < 1e-7f && std::fabs(poly.back().y - p.y) < 1e-7f) continue;
        poly.push_back(p);
    }
    if (poly.size() > 1 && std::fabs(poly.front().x - poly.back().x) < 1e-7f &&
        std::fabs(poly.front().y - poly.back().y) < 1e-7f)
        poly.pop_back();
    if (poly.size() < 3) throw std::runtime_error("extrude needs at least 3 distinct points");
    if (signedArea(poly) < 0) std::reverse(poly.begin(), poly.end());

    std::vector<uint32_t> tri;
    if (!triangulatePolygon(poly, tri))
        throw std::runtime_error("extrude polygon could not be triangulated (self-intersecting or degenerate?)");

    float h = depth * 0.5f;
    Mesh m;
    // Caps.
    uint32_t topBase = static_cast<uint32_t>(m.positions.size());
    for (const auto& p : poly) m.addVertex({p.x, p.y, h}, {0, 0, 1});
    for (size_t i = 0; i < tri.size(); i += 3) m.addTriangle(topBase + tri[i], topBase + tri[i + 1], topBase + tri[i + 2]);
    uint32_t botBase = static_cast<uint32_t>(m.positions.size());
    for (const auto& p : poly) m.addVertex({p.x, p.y, -h}, {0, 0, -1});
    for (size_t i = 0; i < tri.size(); i += 3) m.addTriangle(botBase + tri[i], botBase + tri[i + 2], botBase + tri[i + 1]);

    // Sides, smoothing across shallow corners so curved outlines look round.
    size_t n = poly.size();
    std::vector<Vec3> edgeN(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2 a = poly[i], b = poly[(i + 1) % n];
        edgeN[i] = normalize(Vec3{b.y - a.y, -(b.x - a.x), 0});
    }
    const float cosCrease = std::cos(radians(40.0f));
    auto cornerNormal = [&](size_t edge, size_t neighbor) {
        if (dot(edgeN[edge], edgeN[neighbor]) < cosCrease) return edgeN[edge];
        return normalize(edgeN[edge] + edgeN[neighbor]);
    };
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        Vec3 n0 = cornerNormal(i, (i + n - 1) % n), n1 = cornerNormal(i, j);
        Vec2 a = poly[i], b = poly[j];
        uint32_t v0 = m.addVertex({a.x, a.y, -h}, n0);
        uint32_t v1 = m.addVertex({b.x, b.y, -h}, n1);
        uint32_t v2 = m.addVertex({b.x, b.y, h}, n1);
        uint32_t v3 = m.addVertex({a.x, a.y, h}, n0);
        m.addTriangle(v0, v1, v2);
        m.addTriangle(v0, v2, v3);
    }

    Mat4 remap;  // maps generated (u, v, w) onto the requested axes
    if (axis == 'y') {
        // (u, v, w) -> (x = u, y = w, z = v)
        remap.at(0, 0) = 1; remap.at(1, 1) = 0; remap.at(2, 2) = 0;
        remap.at(1, 2) = 1;
        remap.at(2, 1) = 1;
        m.transform(remap);
    } else if (axis == 'x') {
        // (u, v, w) -> (x = w, y = v, z = u)
        remap.at(0, 0) = 0; remap.at(2, 2) = 0;
        remap.at(0, 2) = 1;
        remap.at(2, 0) = 1;
        m.transform(remap);
    }
    return m;
}

Mesh makeRawMesh(const std::vector<Vec3>& vertices, const std::vector<std::vector<uint32_t>>& faces, bool smooth) {
    Mesh m;
    if (smooth) {
        std::vector<Vec3> acc(vertices.size());
        for (const auto& f : faces) {
            for (size_t k = 1; k + 1 < f.size(); ++k) {
                Vec3 a = vertices[f[0]], b = vertices[f[k]], c = vertices[f[k + 1]];
                Vec3 fn = cross(b - a, c - a);  // area-weighted
                acc[f[0]] += fn;
                acc[f[k]] += fn;
                acc[f[k + 1]] += fn;
            }
        }
        for (size_t i = 0; i < vertices.size(); ++i) m.addVertex(vertices[i], normalize(acc[i]));
        for (const auto& f : faces)
            for (size_t k = 1; k + 1 < f.size(); ++k) m.addTriangle(f[0], f[k], f[k + 1]);
        return m;
    }
    for (const auto& f : faces) {
        Vec3 fn{0, 0, 0};
        for (size_t k = 1; k + 1 < f.size(); ++k)
            fn += cross(vertices[f[k]] - vertices[f[0]], vertices[f[k + 1]] - vertices[f[0]]);
        fn = normalize(fn);
        uint32_t base = static_cast<uint32_t>(m.positions.size());
        for (uint32_t vi : f) m.addVertex(vertices[vi], fn);
        for (size_t k = 1; k + 1 < f.size(); ++k)
            m.addTriangle(base, base + static_cast<uint32_t>(k), base + static_cast<uint32_t>(k + 1));
    }
    return m;
}

}  // namespace sf
