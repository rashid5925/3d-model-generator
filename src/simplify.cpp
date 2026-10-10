#include "simplify.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace sf {

namespace {

// Symmetric 4x4 quadric stored as its 10 unique coefficients (Garland & Heckbert).
struct Quadric {
    double a[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // xx xy xz xw yy yz yw zz zw ww

    void addPlane(double nx, double ny, double nz, double d) {
        a[0] += nx * nx; a[1] += nx * ny; a[2] += nx * nz; a[3] += nx * d;
        a[4] += ny * ny; a[5] += ny * nz; a[6] += ny * d;
        a[7] += nz * nz; a[8] += nz * d;
        a[9] += d * d;
    }
    Quadric operator+(const Quadric& o) const {
        Quadric r;
        for (int i = 0; i < 10; ++i) r.a[i] = a[i] + o.a[i];
        return r;
    }
    double eval(Vec3 p) const {
        double x = p.x, y = p.y, z = p.z;
        return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z +
               2 * a[6] * y + a[7] * z * z + 2 * a[8] * z + a[9];
    }
    // Position minimizing the error, if the 3x3 system is well conditioned.
    bool optimum(Vec3& out) const {
        double m00 = a[0], m01 = a[1], m02 = a[2], m11 = a[4], m12 = a[5], m22 = a[7];
        double det = m00 * (m11 * m22 - m12 * m12) - m01 * (m01 * m22 - m12 * m02) + m02 * (m01 * m12 - m11 * m02);
        double scale = std::max({std::fabs(m00), std::fabs(m11), std::fabs(m22), 1e-30});
        if (std::fabs(det) < 1e-9 * scale * scale * scale) return false;
        double bx = -a[3], by = -a[6], bz = -a[8];
        double inv = 1.0 / det;
        out.x = static_cast<float>(inv * (bx * (m11 * m22 - m12 * m12) - m01 * (by * m22 - m12 * bz) + m02 * (by * m12 - m11 * bz)));
        out.y = static_cast<float>(inv * (m00 * (by * m22 - m12 * bz) - bx * (m01 * m22 - m12 * m02) + m02 * (m01 * bz - by * m02)));
        out.z = static_cast<float>(inv * (m00 * (m11 * bz - by * m12) - m01 * (m01 * bz - by * m02) + bx * (m01 * m12 - m11 * m02)));
        return true;
    }
};

struct Candidate {
    double cost;
    uint32_t a, b;
    uint32_t va, vb;  // vertex versions at push time
    Vec3 pos;
    bool operator>(const Candidate& o) const { return cost > o.cost; }
};

}  // namespace

void simplifyMesh(Mesh& mesh, float maxError) {
    const size_t nv = mesh.positions.size(), nf = mesh.indices.size() / 3;
    if (nf < 8 || maxError <= 0) return;
    std::vector<Vec3>& P = mesh.positions;
    std::vector<uint32_t> F = mesh.indices;  // working copy, 3 per face
    std::vector<bool> faceAlive(nf, true), vertAlive(nv, true);
    std::vector<uint32_t> version(nv, 0);
    std::vector<std::vector<uint32_t>> vf(nv);
    std::vector<Quadric> Q(nv);

    for (uint32_t f = 0; f < nf; ++f) {
        Vec3 p0 = P[F[3 * f]], p1 = P[F[3 * f + 1]], p2 = P[F[3 * f + 2]];
        Vec3 n = cross(p1 - p0, p2 - p0);
        float len = length(n);
        if (len < 1e-20f) continue;
        n = n / len;
        double d = -static_cast<double>(dot(n, p0));
        for (int k = 0; k < 3; ++k) {
            Q[F[3 * f + k]].addPlane(n.x, n.y, n.z, d);
            vf[F[3 * f + k]].push_back(f);
        }
    }

    const double limit = static_cast<double>(maxError) * maxError;
    auto evaluate = [&](uint32_t a, uint32_t b, Candidate& c) {
        Quadric q = Q[a] + Q[b];
        Vec3 best = (P[a] + P[b]) * 0.5f;
        double bestCost = q.eval(best);
        Vec3 opt;
        if (q.optimum(opt)) {
            // Only trust the optimum near the edge; far-away solutions mean the
            // system is nearly degenerate.
            AABB box;
            box.expand(P[a]);
            box.expand(P[b]);
            box = box.padded(length(P[a] - P[b]) * 0.5f);
            if (opt.x >= box.min.x && opt.y >= box.min.y && opt.z >= box.min.z && opt.x <= box.max.x &&
                opt.y <= box.max.y && opt.z <= box.max.z) {
                double c0 = q.eval(opt);
                if (c0 < bestCost) {
                    bestCost = c0;
                    best = opt;
                }
            }
        }
        for (Vec3 p : {P[a], P[b]}) {
            double c1 = q.eval(p);
            if (c1 < bestCost) {
                bestCost = c1;
                best = p;
            }
        }
        c = Candidate{std::max(0.0, bestCost), a, b, version[a], version[b], best};
    };

    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> heap;
    for (uint32_t f = 0; f < nf; ++f) {
        for (int k = 0; k < 3; ++k) {
            uint32_t a = F[3 * f + k], b = F[3 * f + (k + 1) % 3];
            if (a < b) {  // each interior edge is seen twice; keep one
                Candidate c;
                evaluate(a, b, c);
                if (c.cost <= limit) heap.push(c);
            }
        }
    }

    std::vector<uint32_t> markA(nv, 0);
    uint32_t stamp = 0;
    size_t faceCount = nf;

    auto faceHas = [&](uint32_t f, uint32_t v) { return F[3 * f] == v || F[3 * f + 1] == v || F[3 * f + 2] == v; };

    while (!heap.empty()) {
        Candidate c = heap.top();
        heap.pop();
        if (c.cost > limit) break;
        uint32_t a = c.a, b = c.b;
        if (!vertAlive[a] || !vertAlive[b] || version[a] != c.va || version[b] != c.vb) continue;
        if (faceCount <= 4) break;

        // Link condition: a and b may share exactly the two vertices opposite the edge.
        ++stamp;
        for (uint32_t f : vf[a])
            if (faceAlive[f])
                for (int k = 0; k < 3; ++k) markA[F[3 * f + k]] = stamp;
        int shared = 0;
        bool edgeExists = false;
        std::vector<uint32_t> seen;
        for (uint32_t f : vf[b]) {
            if (!faceAlive[f]) continue;
            if (faceHas(f, a)) edgeExists = true;
            for (int k = 0; k < 3; ++k) {
                uint32_t v = F[3 * f + k];
                if (v == a || v == b || markA[v] != stamp) continue;
                if (std::find(seen.begin(), seen.end(), v) == seen.end()) {
                    seen.push_back(v);
                    ++shared;
                }
            }
        }
        if (!edgeExists || shared != 2) continue;

        // Reject collapses that flip or squash surrounding triangles.
        bool ok = true;
        for (uint32_t v : {a, b}) {
            for (uint32_t f : vf[v]) {
                if (!faceAlive[f] || (faceHas(f, a) && faceHas(f, b))) continue;
                Vec3 p[3], q[3];
                for (int k = 0; k < 3; ++k) {
                    uint32_t vi = F[3 * f + k];
                    p[k] = P[vi];
                    q[k] = (vi == a || vi == b) ? c.pos : P[vi];
                }
                Vec3 n0 = cross(p[1] - p[0], p[2] - p[0]);
                Vec3 n1 = cross(q[1] - q[0], q[2] - q[0]);
                float l0 = length(n0), l1 = length(n1);
                if (l1 < 1e-12f || l1 < l0 * 1e-3f || dot(n0, n1) < 0.25f * l0 * l1) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }
        if (!ok) continue;

        // Apply: keep a, remove b.
        P[a] = c.pos;
        Q[a] = Q[a] + Q[b];
        for (uint32_t f : vf[b]) {
            if (!faceAlive[f]) continue;
            if (faceHas(f, a)) {
                faceAlive[f] = false;
                --faceCount;
                continue;
            }
            for (int k = 0; k < 3; ++k)
                if (F[3 * f + k] == b) F[3 * f + k] = a;
            vf[a].push_back(f);
        }
        vertAlive[b] = false;
        vf[b].clear();
        ++version[a];
        std::vector<uint32_t> live;
        for (uint32_t f : vf[a])
            if (faceAlive[f]) live.push_back(f);
        vf[a] = std::move(live);

        // Re-queue edges around a.
        for (uint32_t f : vf[a]) {
            for (int k = 0; k < 3; ++k) {
                uint32_t v = F[3 * f + k];
                if (v == a) continue;
                Candidate nc;
                evaluate(std::min(a, v), std::max(a, v), nc);
                if (nc.cost <= limit) heap.push(nc);
            }
        }
    }

    // Compact.
    std::vector<int64_t> remap(nv, -1);
    Mesh out;
    for (uint32_t f = 0; f < nf; ++f) {
        if (!faceAlive[f]) continue;
        uint32_t tri[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t v = F[3 * f + k];
            if (remap[v] < 0) remap[v] = out.addVertex(P[v], mesh.normals.empty() ? Vec3{} : mesh.normals[v]);
            tri[k] = static_cast<uint32_t>(remap[v]);
        }
        out.addTriangle(tri[0], tri[1], tri[2]);
    }
    mesh = std::move(out);
}

void computeCreaseNormals(Mesh& mesh, float creaseDeg) {
    const size_t nv = mesh.positions.size(), nf = mesh.indices.size() / 3;
    std::vector<Vec3> fn(nf);
    std::vector<std::vector<uint32_t>> vf(nv);
    for (uint32_t f = 0; f < nf; ++f) {
        Vec3 p0 = mesh.positions[mesh.indices[3 * f]], p1 = mesh.positions[mesh.indices[3 * f + 1]],
             p2 = mesh.positions[mesh.indices[3 * f + 2]];
        fn[f] = cross(p1 - p0, p2 - p0);  // area weighted
        for (int k = 0; k < 3; ++k) vf[mesh.indices[3 * f + k]].push_back(f);
    }
    const float cosCrease = std::cos(radians(creaseDeg));
    Mesh out;
    out.positions.reserve(nv);
    // Per original vertex, the (normal -> new index) variants created so far.
    std::vector<std::vector<std::pair<Vec3, uint32_t>>> variants(nv);
    for (uint32_t f = 0; f < nf; ++f) {
        Vec3 nf0 = normalize(fn[f]);
        uint32_t tri[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t v = mesh.indices[3 * f + k];
            Vec3 sum{0, 0, 0};
            for (uint32_t g : vf[v])
                if (dot(nf0, normalize(fn[g])) >= cosCrease) sum += fn[g];
            Vec3 n = normalize(sum);
            if (dot(n, n) < 0.5f) n = nf0;
            int64_t found = -1;
            for (const auto& [vn, idx] : variants[v])
                if (dot(vn, n) > 0.9999f) {
                    found = idx;
                    break;
                }
            if (found < 0) {
                found = out.addVertex(mesh.positions[v], n);
                variants[v].push_back({n, static_cast<uint32_t>(found)});
            }
            tri[k] = static_cast<uint32_t>(found);
        }
        out.addTriangle(tri[0], tri[1], tri[2]);
    }
    mesh = std::move(out);
}

}  // namespace sf
