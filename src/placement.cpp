#include "placement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

namespace sf {

namespace {

const char* kAxisNames[3] = {"x", "y", "z"};

int parseAxis(const Json& v, const std::string& what) {
    if (v.isString()) {
        std::string s = v.asString();
        if (s == "x" || s == "X") return 0;
        if (s == "y" || s == "Y") return 1;
        if (s == "z" || s == "Z") return 2;
    }
    throw SceneError(what + " must be \"x\", \"y\" or \"z\"");
}

std::string fmtNum(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

float tidy(float v, float q = 1e-6f) {
    float r = std::round(v / q) * q;
    return r == 0 ? 0.0f : r;
}

Json vec3Json(Vec3 v, float q = 1e-6f) { return Json::array({tidy(v.x, q), tidy(v.y, q), tidy(v.z, q)}); }

Vec3 readVec3(const Json& spec, const char* key, Vec3 def) {
    const Json* v = spec.find(key);
    if (!v || v->isNull()) return def;
    if (v->isNumber()) {
        float s = static_cast<float>(v->asNumber());
        return {s, s, s};
    }
    if (!v->isArray() || v->size() != 3) throw SceneError(std::string("'") + key + "' must be [x, y, z]");
    return {static_cast<float>(v->at(0).asNumber()), static_cast<float>(v->at(1).asNumber()),
            static_cast<float>(v->at(2).asNumber())};
}

const Json& requireObject(const Scene& scene, const std::string& name) {
    const Json* o = scene.get(name);
    if (!o) throw SceneError("no object named '" + name + "'");
    return *o;
}

std::string requireString(const Json& args, const char* key) {
    const Json* v = args.find(key);
    if (!v || !v->isString() || v->asString().empty()) throw SceneError(std::string("'") + key + "' (object name) is required");
    return v->asString();
}

// World geometry of an object together with everything parented under it.
struct Assembly {
    Mesh mesh;
    AABB bounds;
};

Assembly assembly(const Scene& scene, const BuildResult& built, const std::string& name) {
    Assembly a;
    for (const auto& n : scene.subtree(name)) {
        const BuiltObject* o = built.find(n);
        if (!o || !o->bounds.valid()) continue;
        a.mesh.append(o->worldMesh);
        a.bounds.expand(o->bounds);
    }
    return a;
}

// Moves an object by a world-space offset by editing its local position.
void translateWorld(Scene& scene, const BuildResult& built, const std::string& name, Vec3 delta) {
    const Json& spec = requireObject(scene, name);
    Vec3 local = delta;
    std::string parent = spec.stringOr("parent", "");
    if (!parent.empty()) {
        if (const BuiltObject* p = built.find(parent)) local = transformDir(inverse(p->world), delta);
    }
    Vec3 pos = readVec3(spec, "position", {0, 0, 0}) + local;
    std::vector<std::string> ignored;
    scene.update(name, Json::object({{"position", vec3Json(pos)}}), ignored);
}

// ---- mesh sweep (exact contact) -------------------------------------------

struct TriGrid {
    const Mesh* mesh = nullptr;
    int axis = 1, u = 0, v = 2;
    float minU = 0, minV = 0, cell = 1;
    int nu = 1, nv = 1;
    std::vector<std::vector<uint32_t>> cells;

    void build(const Mesh& m, int ax) {
        mesh = &m;
        axis = ax;
        u = (ax + 1) % 3;
        v = (ax + 2) % 3;
        AABB b = m.bounds();
        minU = b.min[u];
        minV = b.min[v];
        float su = std::max(b.size()[u], 1e-6f), sv = std::max(b.size()[v], 1e-6f);
        size_t tris = m.triangleCount();
        int res = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(tris))), 1, 256);
        cell = std::max(su, sv) / static_cast<float>(res);
        nu = std::max(1, static_cast<int>(std::ceil(su / cell)));
        nv = std::max(1, static_cast<int>(std::ceil(sv / cell)));
        cells.assign(static_cast<size_t>(nu) * nv, {});
        for (uint32_t t = 0; t < tris; ++t) {
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (int k = 0; k < 3; ++k) {
                Vec3 p = m.positions[m.indices[3 * t + k]];
                lo[0] = std::min(lo[0], p[u]);
                hi[0] = std::max(hi[0], p[u]);
                lo[1] = std::min(lo[1], p[v]);
                hi[1] = std::max(hi[1], p[v]);
            }
            int u0 = cellU(lo[0]), u1 = cellU(hi[0]), v0 = cellV(lo[1]), v1 = cellV(hi[1]);
            for (int j = v0; j <= v1; ++j)
                for (int i = u0; i <= u1; ++i) cells[static_cast<size_t>(j) * nu + i].push_back(t);
        }
    }
    int cellU(float x) const { return std::clamp(static_cast<int>((x - minU) / cell), 0, nu - 1); }
    int cellV(float y) const { return std::clamp(static_cast<int>((y - minV) / cell), 0, nv - 1); }

    // Smallest t >= 0 with p + t * dir (dir = sign along axis) hitting a triangle; -1 if none.
    float cast(Vec3 p, int sign) const {
        float best = -1;
        if (p[u] < minU - cell || p[v] < minV - cell || p[u] > minU + (nu + 1) * cell || p[v] > minV + (nv + 1) * cell)
            return best;
        const auto& list = cells[static_cast<size_t>(cellV(p[v])) * nu + cellU(p[u])];
        for (uint32_t t : list) {
            Vec3 a = mesh->positions[mesh->indices[3 * t]];
            Vec3 b = mesh->positions[mesh->indices[3 * t + 1]];
            Vec3 c = mesh->positions[mesh->indices[3 * t + 2]];
            // 2D barycentric test in the (u, v) plane, then interpolate the axis coordinate.
            float d = (b[u] - a[u]) * (c[v] - a[v]) - (c[u] - a[u]) * (b[v] - a[v]);
            if (std::fabs(d) < 1e-20f) continue;
            float w1 = ((p[u] - a[u]) * (c[v] - a[v]) - (c[u] - a[u]) * (p[v] - a[v])) / d;
            float w2 = ((b[u] - a[u]) * (p[v] - a[v]) - (p[u] - a[u]) * (b[v] - a[v])) / d;
            const float e = -1e-5f;
            if (w1 < e || w2 < e || w1 + w2 > 1 - e) continue;
            float h = a[axis] + w1 * (b[axis] - a[axis]) + w2 * (c[axis] - a[axis]);
            float tt = (h - p[axis]) * static_cast<float>(sign);
            if (tt >= -1e-6f && (best < 0 || tt < best)) best = std::max(tt, 0.0f);
        }
        return best;
    }
};

}  // namespace

float sweepDistance(const Mesh& moving, const Mesh& fixed, int axis, int sign) {
    if (moving.empty() || fixed.empty()) return -1;
    TriGrid fixedGrid, movingGrid;
    fixedGrid.build(fixed, axis);
    movingGrid.build(moving, axis);
    float best = -1;
    auto consider = [&](float t) {
        if (t >= 0 && (best < 0 || t < best)) best = t;
    };
    for (const auto& p : moving.positions) consider(fixedGrid.cast(p, sign));   // moving vertices hit fixed faces
    for (const auto& p : fixed.positions) consider(movingGrid.cast(p, -sign));  // fixed vertices hit moving faces
    return best;
}

PlacementResult placeOn(Scene& scene, const Json& args) {
    PlacementResult res;
    std::vector<std::string> objects;
    if (const Json* o = args.find("object"); o && o->isString()) objects.push_back(o->asString());
    if (const Json* os = args.find("objects"); os && os->isArray())
        for (const auto& n : os->asArray())
            if (n.isString()) objects.push_back(n.asString());
    if (objects.empty()) throw SceneError("'object' (or 'objects') is required");
    std::string target = args.stringOr("target", "ground");
    bool ground = target == "ground";
    if (!ground) requireObject(scene, target);

    std::string side = args.stringOr("side", "top");
    int axis = 1, sign = 1;
    if (side == "top" || side == "+y") { axis = 1; sign = 1; }
    else if (side == "bottom" || side == "-y") { axis = 1; sign = -1; }
    else if (side == "right" || side == "+x") { axis = 0; sign = 1; }
    else if (side == "left" || side == "-x") { axis = 0; sign = -1; }
    else if (side == "front" || side == "+z") { axis = 2; sign = 1; }
    else if (side == "back" || side == "-z") { axis = 2; sign = -1; }
    else throw SceneError("'side' must be top, bottom, left, right, front or back");
    if (ground && side != "top" && side != "+y") throw SceneError("only side 'top' makes sense for the ground");

    float gap = static_cast<float>(args.numberOr("gap", 0));
    bool contact = args.boolOr("contact", true);
    const int ua = (axis + 1) % 3, va = (axis + 2) % 3;  // in-plane axes
    bool hasAt = false;
    float at[3] = {0, 0, 0};
    if (const Json* a = args.find("at"); a && !a->isNull()) {
        if (!a->isArray() || a->size() != 2) throw SceneError("'at' must be a 2-element offset on the face's in-plane axes");
        at[ua] = static_cast<float>(a->at(0).asNumber());
        at[va] = static_cast<float>(a->at(1).asNumber());
        hasAt = true;
    }
    if (args.boolOr("center", false)) hasAt = true;

    std::set<std::string> targetTree;
    if (!ground)
        for (const auto& n : scene.subtree(target)) targetTree.insert(n);

    for (const auto& name : objects) {
        requireObject(scene, name);
        if (targetTree.count(name)) throw SceneError("cannot place '" + name + "' on '" + target + "': it is part of the target");
        for (const auto& n : scene.subtree(name))
            if (n == target) throw SceneError("cannot place '" + name + "' on its own child '" + target + "'");

        BuildResult built = scene.build();
        Assembly obj = assembly(scene, built, name);
        if (!obj.bounds.valid()) throw SceneError("'" + name + "' has no geometry to place");
        AABB tb;
        Assembly tgt;
        if (ground) {
            tb = AABB{{-1e6f, -1e6f, -1e6f}, {1e6f, 0, 1e6f}};
        } else {
            tgt = assembly(scene, built, target);
            if (!tgt.bounds.valid()) throw SceneError("target '" + target + "' has no geometry");
            tb = tgt.bounds;
        }

        Vec3 delta{0, 0, 0};
        if (hasAt) {
            for (int ax : {ua, va}) {
                float base = ground ? 0.0f : tb.center()[ax];
                delta[ax] = base + at[ax] - obj.bounds.center()[ax];
            }
        }
        delta[axis] = sign > 0 ? (tb.max[axis] + gap) - obj.bounds.min[axis] : (tb.min[axis] - gap) - obj.bounds.max[axis];
        translateWorld(scene, built, name, delta);

        if (!ground && contact) {
            BuildResult moved = scene.build();
            Assembly o2 = assembly(scene, moved, name);
            float d = sweepDistance(o2.mesh, tgt.mesh, axis, -sign);
            if (d < 0) {
                res.notes.push_back("'" + name + "': no surface of '" + target + "' lies in that direction under it; placed against the bounding box instead");
            } else if (d - gap > 1e-6f) {
                Vec3 step{0, 0, 0};
                step[axis] = -static_cast<float>(sign) * (d - gap);
                translateWorld(scene, moved, name, step);
                res.notes.push_back("'" + name + "': moved " + fmtNum(d - gap) +
                                    " further to touch the actual surface of '" + target + "'");
            }
        }
        res.changed.push_back(name);
    }
    return res;
}

PlacementResult alignObjects(Scene& scene, const Json& args) {
    PlacementResult res;
    const Json* os = args.find("objects");
    if (!os || !os->isArray() || os->size() == 0) throw SceneError("'objects' must list the objects to align");
    std::vector<std::string> names;
    for (const auto& n : os->asArray()) {
        if (!n.isString()) throw SceneError("'objects' must contain names");
        requireObject(scene, n.asString());
        names.push_back(n.asString());
    }
    std::vector<int> axes;
    const Json* ax = args.find("axis");
    if (!ax) throw SceneError("'axis' is required (\"x\", \"y\", \"z\" or a list like [\"x\", \"z\"])");
    if (ax->isArray())
        for (const auto& a : ax->asArray()) axes.push_back(parseAxis(a, "'axis'"));
    else
        axes.push_back(parseAxis(*ax, "'axis'"));
    std::string mode = args.stringOr("mode", "center");
    if (mode != "min" && mode != "center" && mode != "max") throw SceneError("'mode' must be min, center or max");
    auto pick = [&](const AABB& b, int a) {
        return mode == "min" ? b.min[a] : mode == "max" ? b.max[a] : b.center()[a];
    };

    BuildResult built = scene.build();
    float value[3] = {0, 0, 0};
    const Json* to = args.find("to");
    for (int a : axes) {
        if (to && to->isNumber()) {
            value[a] = static_cast<float>(to->asNumber());
        } else {
            std::string ref = (to && to->isString()) ? to->asString() : names.front();
            requireObject(scene, ref);
            AABB rb = assembly(scene, built, ref).bounds;
            if (!rb.valid()) throw SceneError("'" + ref + "' has no geometry to align to");
            value[a] = pick(rb, a);
        }
    }
    for (const auto& n : names) {
        BuildResult cur = scene.build();
        AABB b = assembly(scene, cur, n).bounds;
        if (!b.valid()) continue;
        Vec3 delta{0, 0, 0};
        for (int a : axes) delta[a] = value[a] - pick(b, a);
        if (length(delta) < 1e-9f) continue;
        translateWorld(scene, cur, n, delta);
        res.changed.push_back(n);
    }
    std::string axesText;
    for (int a : axes) axesText += kAxisNames[a];
    res.notes.push_back("aligned " + mode + " on " + axesText);
    return res;
}

namespace {

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// left <-> right style renaming for mirrored copies.
std::string mirroredName(const std::string& n) {
    static const std::pair<const char*, const char*> swaps[] = {
        {"left", "right"}, {"Left", "Right"}, {"LEFT", "RIGHT"}};
    for (const auto& [a, b] : swaps) {
        if (n.find(a) != std::string::npos) return replaceAll(n, a, b);
        if (n.find(b) != std::string::npos) return replaceAll(n, b, a);
    }
    auto endsWith = [&](const char* suf) {
        size_t l = std::char_traits<char>::length(suf);
        return n.size() > l && n.compare(n.size() - l, l, suf) == 0;
    };
    auto startsWith = [&](const char* pre) { return n.rfind(pre, 0) == 0 && n.size() > 2; };
    if (endsWith("_l")) return n.substr(0, n.size() - 2) + "_r";
    if (endsWith("_r")) return n.substr(0, n.size() - 2) + "_l";
    if (endsWith("_L")) return n.substr(0, n.size() - 2) + "_R";
    if (endsWith("_R")) return n.substr(0, n.size() - 2) + "_L";
    if (startsWith("l_")) return "r_" + n.substr(2);
    if (startsWith("r_")) return "l_" + n.substr(2);
    return n + "_mirror";
}

// True when the shape looks identical after reflecting its local `axis`.
bool symmetricUnder(const Json& spec, int axis) {
    std::string shape = spec.stringOr("shape", "");
    if (shape == "box" || shape == "sphere" || shape == "capsule" || shape == "torus") return true;
    if (shape == "cylinder") {
        if (axis != 1) return true;
        double r = spec.numberOr("radius", 0.5);
        return spec.numberOr("radius_top", r) == spec.numberOr("radius_bottom", r);
    }
    if (shape == "cone") return axis != 1;
    return false;
}

// Rewrites a spec's local transform as reflected across `axis` at `plane`.
void mirrorTransform(Json& spec, int axis, float plane, bool keepShapeScale) {
    Vec3 pos = readVec3(spec, "position", {0, 0, 0});
    Vec3 rot = readVec3(spec, "rotation", {0, 0, 0});
    Vec3 scl = readVec3(spec, "scale", {1, 1, 1});
    pos[axis] = 2 * plane - pos[axis];
    for (int a = 0; a < 3; ++a)
        if (a != axis) rot[a] = -rot[a];
    if (!keepShapeScale) scl[axis] = -scl[axis];
    spec["position"] = vec3Json(pos);
    if (spec.find("rotation") || length(rot) > 0) spec["rotation"] = vec3Json(rot, 1e-4f);
    if (scl.x == scl.y && scl.y == scl.z) {
        if (scl.x == 1) spec.erase("scale");
        else spec["scale"] = scl.x;
    } else {
        spec["scale"] = vec3Json(scl);
    }
}

// Copies `root` and its descendants, renaming with `rename`. The copy of the
// root gets `rootSpecEdit` applied. Returns the new names (root first).
template <typename RenameFn, typename EditFn>
std::vector<std::string> copySubtree(Scene& scene, const std::string& root, RenameFn rename, EditFn rootSpecEdit) {
    std::vector<std::string> members = scene.subtree(root);
    std::vector<Json> specs;
    std::vector<std::string> created;
    for (const auto& m : members) {
        Json spec = *scene.get(m);
        std::string newName = rename(m);
        if (newName == m) throw SceneError("copy of '" + m + "' would keep the same name");
        if (scene.get(newName)) throw SceneError("an object named '" + newName + "' already exists; pass a different name");
        spec["name"] = newName;
        if (m == root) rootSpecEdit(spec);
        else spec["parent"] = rename(spec.stringOr("parent", ""));
        specs.push_back(std::move(spec));
        created.push_back(newName);
    }
    std::vector<std::string> ignored;
    for (auto& s : specs) scene.put(std::move(s), ignored);
    return created;
}

}  // namespace

PlacementResult mirrorObject(Scene& scene, const Json& args) {
    PlacementResult res;
    std::string name = requireString(args, "object");
    const Json spec = requireObject(scene, name);  // copy: the scene vector grows below
    int axis = args.find("axis") ? parseAxis(args["axis"], "'axis'") : 0;
    float plane = static_cast<float>(args.numberOr("plane", 0));
    bool inPlace = args.boolOr("in_place", false);
    std::vector<std::string> members = scene.subtree(name);
    bool keepScale = members.size() == 1 && symmetricUnder(spec, axis);

    if (inPlace) {
        Json edited = spec;
        mirrorTransform(edited, axis, plane, keepScale);
        std::vector<std::string> ignored;
        scene.put(std::move(edited), ignored);
        res.changed.push_back(name);
        return res;
    }

    std::string from, to;
    if (const Json* r = args.find("replace"); r && !r->isNull()) {
        if (!r->isArray() || r->size() != 2 || !r->at(0).isString() || !r->at(1).isString())
            throw SceneError("'replace' must be [\"from\", \"to\"], e.g. [\"_left\", \"_right\"]");
        from = r->at(0).asString();
        to = r->at(1).asString();
    }
    std::string rootName = args.stringOr("name", "");
    auto rename = [&](const std::string& n) -> std::string {
        if (n == name && !rootName.empty()) return rootName;
        if (!from.empty()) {
            std::string r = replaceAll(n, from, to);
            return r == n ? n + "_mirror" : r;
        }
        return mirroredName(n);
    };
    res.changed = copySubtree(scene, name, rename, [&](Json& s) { mirrorTransform(s, axis, plane, keepScale); });
    std::string parent = spec.stringOr("parent", "");
    res.notes.push_back(std::string("mirrored across the plane ") + kAxisNames[axis] + " = " + fmtNum(plane) +
                        (parent.empty() ? " (world space)" : " in the local space of parent '" + parent + "'"));
    return res;
}

PlacementResult arrayObject(Scene& scene, const Json& args) {
    PlacementResult res;
    std::string name = requireString(args, "object");
    const Json spec = requireObject(scene, name);
    int count = static_cast<int>(args.numberOr("count", 0));
    if (count < 2 || count > 500) throw SceneError("'count' must be 2..500 (total, including the original)");
    std::string mode = args.stringOr("mode", args.find("offset") ? "linear" : "radial");
    std::string base = args.stringOr("name_prefix", name);

    Vec3 pos0 = readVec3(spec, "position", {0, 0, 0});
    Vec3 rot0 = readVec3(spec, "rotation", {0, 0, 0});
    for (int i = 1; i < count; ++i) {
        std::string suffix = "_" + std::to_string(i + 1);
        auto rename = [&](const std::string& n) { return n == name ? base + suffix : n + suffix; };
        std::function<void(Json&)> edit;
        if (mode == "linear") {
            Vec3 offset = readVec3(args, "offset", {0, 0, 0});
            if (length(offset) == 0) throw SceneError("linear arrays need a non-zero 'offset' [dx, dy, dz] between copies");
            edit = [=](Json& s) { s["position"] = vec3Json(pos0 + offset * static_cast<float>(i)); };
        } else if (mode == "radial") {
            int axis = args.find("axis") ? parseAxis(args["axis"], "'axis'") : 1;
            Vec3 center = readVec3(args, "center", {0, 0, 0});
            float total = static_cast<float>(args.numberOr("angle", 360));
            float step = std::fabs(std::fabs(total) - 360.0f) < 1e-3f ? total / count : total / (count - 1);
            float theta = step * i;
            bool rotateCopies = args.boolOr("rotate_copies", true);
            Vec3 e{0, 0, 0};
            e[axis] = theta;
            Mat4 r = rotationEuler(e);
            edit = [=](Json& s) {
                s["position"] = vec3Json(center + transformDir(r, pos0 - center));
                if (rotateCopies) s["rotation"] = vec3Json(eulerFromRotation(r * rotationEuler(rot0)), 1e-4f);
            };
        } else {
            throw SceneError("'mode' must be \"linear\" or \"radial\"");
        }
        auto created = copySubtree(scene, name, rename, edit);
        res.changed.insert(res.changed.end(), created.begin(), created.end());
    }
    if (!spec.stringOr("parent", "").empty())
        res.notes.push_back("offsets/centers are in the local space of parent '" + spec.stringOr("parent", "") + "'");
    return res;
}

}  // namespace sf
