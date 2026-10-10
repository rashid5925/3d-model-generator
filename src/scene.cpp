#include "scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

#include "sdf.hpp"
#include "simplify.hpp"

namespace sf {

namespace {

const std::vector<std::string> kObjectKeys = {"name",   "shape", "position", "rotation", "scale",
                                              "color",  "parent", "visible", "description"};
const std::vector<std::string> kChildKeys = {"shape", "position", "rotation", "scale", "description"};

const std::map<std::string, std::vector<std::string>>& shapeKeys() {
    static const std::map<std::string, std::vector<std::string>> keys = {
        {"box", {"size", "rounding", "resolution", "simplify"}},
        {"sphere", {"radius", "segments", "rings"}},
        {"cylinder", {"radius", "radius_top", "radius_bottom", "height", "segments"}},
        {"cone", {"radius", "height", "segments"}},
        {"capsule", {"radius", "height", "segments"}},
        {"torus", {"radius", "tube", "segments", "sides"}},
        {"plane", {"size"}},
        {"extrude", {"points", "depth", "axis"}},
        {"lathe", {"points", "segments", "cap"}},
        {"mesh", {"vertices", "faces", "smooth"}},
        {"csg", {"op", "children", "smooth", "resolution", "shell", "simplify"}},
        {"group", {}},
    };
    return keys;
}

std::string joinNames(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s;
}

// Levenshtein distance, used for "did you mean" hints on typos.
size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string suggest(const std::string& key, const std::vector<std::string>& options) {
    std::string best;
    size_t bestD = 3;
    for (const auto& o : options) {
        size_t d = editDistance(key, o);
        if (d < bestD) {
            bestD = d;
            best = o;
        }
    }
    return best.empty() ? "" : " (did you mean '" + best + "'?)";
}

[[noreturn]] void fail(const std::string& ctx, const std::string& msg) { throw SceneError(ctx + ": " + msg); }

float num(const Json& spec, const char* key, float def, const std::string& ctx) {
    const Json* v = spec.find(key);
    if (!v || v->isNull()) return def;
    if (!v->isNumber()) fail(ctx, std::string("'") + key + "' must be a number, got " + jsonTypeName(v->type()));
    double d = v->asNumber();
    if (!std::isfinite(d)) fail(ctx, std::string("'") + key + "' must be finite");
    return static_cast<float>(d);
}

float positive(const Json& spec, const char* key, float def, const std::string& ctx) {
    float v = num(spec, key, def, ctx);
    if (!(v > 0)) fail(ctx, std::string("'") + key + "' must be > 0");
    return v;
}

int integer(const Json& spec, const char* key, int def, int lo, int hi, const std::string& ctx) {
    float v = num(spec, key, static_cast<float>(def), ctx);
    return std::clamp(static_cast<int>(std::lround(v)), lo, hi);
}

Vec3 vec3From(const Json& v, const std::string& what, const std::string& ctx, bool allowScalar) {
    if (allowScalar && v.isNumber()) {
        float s = static_cast<float>(v.asNumber());
        return {s, s, s};
    }
    if (!v.isArray() || v.size() != 3) fail(ctx, what + " must be an array of 3 numbers [x, y, z]");
    Vec3 r;
    for (int i = 0; i < 3; ++i) {
        if (!v.at(i).isNumber()) fail(ctx, what + " must contain only numbers");
        r[i] = static_cast<float>(v.at(i).asNumber());
    }
    return r;
}

Vec3 vec3(const Json& spec, const char* key, Vec3 def, const std::string& ctx, bool allowScalar = false) {
    const Json* v = spec.find(key);
    if (!v || v->isNull()) return def;
    return vec3From(*v, std::string("'") + key + "'", ctx, allowScalar);
}

std::vector<Vec2> points2(const Json& spec, const char* key, const std::string& ctx, size_t minCount) {
    const Json* v = spec.find(key);
    if (!v || !v->isArray()) fail(ctx, std::string("'") + key + "' must be an array of [a, b] points");
    std::vector<Vec2> pts;
    for (const auto& p : v->asArray()) {
        if (!p.isArray() || p.size() != 2 || !p.at(0).isNumber() || !p.at(1).isNumber())
            fail(ctx, std::string("every entry of '") + key + "' must be a pair of numbers [a, b]");
        pts.push_back({static_cast<float>(p.at(0).asNumber()), static_cast<float>(p.at(1).asNumber())});
    }
    if (pts.size() < minCount) fail(ctx, std::string("'") + key + "' needs at least " + std::to_string(minCount) + " points");
    return pts;
}

char axisOf(const Json& spec, const std::string& ctx) {
    std::string a = spec.stringOr("axis", "z");
    if (a == "x" || a == "X") return 'x';
    if (a == "y" || a == "Y") return 'y';
    if (a == "z" || a == "Z") return 'z';
    fail(ctx, "'axis' must be \"x\", \"y\" or \"z\"");
}

std::string shapeOf(const Json& spec, const std::string& ctx) {
    const Json* s = spec.find("shape");
    if (!s || !s->isString()) fail(ctx, "missing 'shape' (one of: box, sphere, cylinder, cone, capsule, torus, plane, extrude, lathe, mesh, csg, group)");
    const auto& keys = shapeKeys();
    if (!keys.count(s->asString())) {
        std::vector<std::string> names;
        for (const auto& [k, _] : keys) names.push_back(k);
        fail(ctx, "unknown shape '" + s->asString() + "'" + suggest(s->asString(), names) + "; valid shapes: " + joinNames(names));
    }
    return s->asString();
}

Mat4 localTransform(const Json& spec, const std::string& ctx) {
    Vec3 pos = vec3(spec, "position", {0, 0, 0}, ctx);
    Vec3 rot = vec3(spec, "rotation", {0, 0, 0}, ctx);
    Vec3 scl = vec3(spec, "scale", {1, 1, 1}, ctx, true);
    if (std::fabs(scl.x * scl.y * scl.z) < 1e-12f) fail(ctx, "'scale' components must be non-zero");
    return translation(pos) * rotationEuler(rot) * scaling(scl);
}

std::vector<Vec2> latheProfile(const Json& spec, const std::string& ctx, bool cap) {
    std::vector<Vec2> pts = points2(spec, "points", ctx, 2);
    for (const auto& p : pts)
        if (p.x < 0) fail(ctx, "lathe 'points' are [radius, y] pairs; radius must be >= 0");
    if (cap) {
        if (pts.front().x > 1e-6f) pts.insert(pts.begin(), Vec2{0, pts.front().y});
        if (pts.back().x > 1e-6f) pts.push_back(Vec2{0, pts.back().y});
    }
    return pts;
}

void checkKeys(const Json& spec, const std::string& shape, bool isChild, const std::string& ctx,
               std::vector<std::string>& warnings) {
    std::vector<std::string> allowed = isChild ? kChildKeys : kObjectKeys;
    const auto& extra = shapeKeys().at(shape);
    allowed.insert(allowed.end(), extra.begin(), extra.end());
    for (const auto& [k, v] : spec.asObject()) {
        if (std::find(allowed.begin(), allowed.end(), k) != allowed.end()) continue;
        if (isChild && (k == "color" || k == "name" || k == "parent")) {
            warnings.push_back(ctx + ": '" + k + "' is ignored on csg children (the csg object's own value applies)");
            continue;
        }
        warnings.push_back(ctx + ": unknown key '" + k + "' for shape '" + shape + "'" + suggest(k, allowed) +
                           " - ignored. Allowed: " + joinNames(allowed));
    }
}

SdfPtr buildSdf(const Json& spec, const std::string& ctx, bool applyTransform, std::vector<std::string>& warnings);

SdfPtr buildSdfShape(const Json& spec, const std::string& shape, const std::string& ctx,
                     std::vector<std::string>& warnings) {
    if (shape == "box") {
        Vec3 size = vec3(spec, "size", {1, 1, 1}, ctx);
        if (minComponent(size) <= 0) fail(ctx, "box 'size' components must be > 0");
        return sdfBox(size, std::max(0.0f, num(spec, "rounding", 0, ctx)));
    }
    if (shape == "sphere") return sdfSphere(positive(spec, "radius", 0.5f, ctx));
    if (shape == "cylinder") {
        float r = positive(spec, "radius", 0.5f, ctx);
        float rb = num(spec, "radius_bottom", r, ctx), rt = num(spec, "radius_top", r, ctx);
        if (rb < 0 || rt < 0 || (rb == 0 && rt == 0)) fail(ctx, "cylinder radii must be >= 0 and not both zero");
        return sdfCappedCone(positive(spec, "height", 1, ctx), rb, rt);
    }
    if (shape == "cone") return sdfCappedCone(positive(spec, "height", 1, ctx), positive(spec, "radius", 0.5f, ctx), 0);
    if (shape == "capsule") return sdfCapsule(positive(spec, "radius", 0.25f, ctx), positive(spec, "height", 1, ctx));
    if (shape == "torus") return sdfTorus(positive(spec, "radius", 0.5f, ctx), positive(spec, "tube", 0.15f, ctx));
    if (shape == "extrude") {
        auto pts = points2(spec, "points", ctx, 3);
        return sdfExtrusion(pts, positive(spec, "depth", 1, ctx), axisOf(spec, ctx));
    }
    if (shape == "lathe") return sdfRevolve(latheProfile(spec, ctx, true));
    if (shape == "csg") {
        std::string op = spec.stringOr("op", "union");
        CsgOp cop;
        if (op == "union") cop = CsgOp::Union;
        else if (op == "subtract" || op == "difference") cop = CsgOp::Subtract;
        else if (op == "intersect" || op == "intersection") cop = CsgOp::Intersect;
        else fail(ctx, "csg 'op' must be \"union\", \"subtract\" or \"intersect\"");
        const Json* kids = spec.find("children");
        if (!kids || !kids->isArray() || kids->size() == 0) fail(ctx, "csg needs a non-empty 'children' array");
        if (cop != CsgOp::Union && kids->size() < 2)
            warnings.push_back(ctx + ": csg '" + op + "' with a single child has no effect");
        std::vector<SdfPtr> nodes;
        for (size_t i = 0; i < kids->size(); ++i) {
            const Json& c = kids->at(i);
            std::string cctx = ctx + " > children[" + std::to_string(i) + "]";
            if (!c.isObject()) fail(cctx, "each csg child must be an object");
            nodes.push_back(buildSdf(c, cctx, true, warnings));
        }
        SdfPtr out = sdfCombine(cop, std::move(nodes), std::max(0.0f, num(spec, "smooth", 0, ctx)));
        float shell = num(spec, "shell", 0, ctx);
        if (shell > 0) out = sdfShell(std::move(out), shell);
        return out;
    }
    fail(ctx, "shape '" + shape + "' cannot be used inside csg (allowed: box, sphere, cylinder, cone, capsule, torus, extrude, lathe, csg)");
}

SdfPtr buildSdf(const Json& spec, const std::string& ctx, bool applyTransform, std::vector<std::string>& warnings) {
    std::string shape = shapeOf(spec, ctx);
    if (applyTransform) checkKeys(spec, shape, true, ctx, warnings);
    SdfPtr node = buildSdfShape(spec, shape, ctx, warnings);
    if (applyTransform) node = sdfTransform(std::move(node), localTransform(spec, ctx));
    return node;
}

// Polygonizes an SDF and, unless disabled with "simplify": false, removes
// triangles that add no shape (error budget: a fraction of one grid cell, i.e.
// below the polygonizer's own sampling error).
Mesh meshFromSdf(const SdfNode& sdf, int defaultRes, const Json& spec, const std::string& ctx) {
    // An explicit "resolution" wins; otherwise refine until thin parts get enough cells.
    int resolution = spec.find("resolution") ? integer(spec, "resolution", defaultRes, 16, 384, ctx)
                                             : autoResolution(sdf, defaultRes, 256);
    float h = 0;
    Mesh m = polygonize(sdf, resolution, &h);
    if (spec.boolOr("simplify", true)) {
        simplifyMesh(m, h * 0.3f);
        computeCreaseNormals(m, 35.0f);
    }
    return m;
}

Mesh buildMesh(const Json& spec, const std::string& shape, const std::string& ctx, std::vector<std::string>& warnings) {
    if (shape == "group") return Mesh{};
    if (shape == "box") {
        Vec3 size = vec3(spec, "size", {1, 1, 1}, ctx);
        if (minComponent(size) <= 0) fail(ctx, "box 'size' components must be > 0");
        float rounding = num(spec, "rounding", 0, ctx);
        if (rounding > 0) return meshFromSdf(*sdfBox(size, rounding), 64, spec, ctx);
        return makeBox(size);
    }
    if (shape == "sphere")
        return makeSphere(positive(spec, "radius", 0.5f, ctx), integer(spec, "segments", 32, 3, 256, ctx),
                          integer(spec, "rings", 16, 2, 128, ctx));
    if (shape == "cylinder") {
        float r = positive(spec, "radius", 0.5f, ctx);
        float rb = num(spec, "radius_bottom", r, ctx), rt = num(spec, "radius_top", r, ctx);
        if (rb < 0 || rt < 0 || (rb == 0 && rt == 0)) fail(ctx, "cylinder radii must be >= 0 and not both zero");
        return makeCylinder(rb, rt, positive(spec, "height", 1, ctx), integer(spec, "segments", 32, 3, 256, ctx));
    }
    if (shape == "cone")
        return makeCylinder(positive(spec, "radius", 0.5f, ctx), 0, positive(spec, "height", 1, ctx),
                            integer(spec, "segments", 32, 3, 256, ctx));
    if (shape == "capsule") {
        float r = positive(spec, "radius", 0.25f, ctx), h = positive(spec, "height", 1, ctx);
        if (h < 2 * r) warnings.push_back(ctx + ": capsule 'height' (total) is less than 2*radius; it becomes a sphere");
        return makeCapsule(r, h, integer(spec, "segments", 32, 4, 256, ctx));
    }
    if (shape == "torus")
        return makeTorus(positive(spec, "radius", 0.5f, ctx), positive(spec, "tube", 0.15f, ctx),
                         integer(spec, "segments", 48, 3, 256, ctx), integer(spec, "sides", 24, 3, 128, ctx));
    if (shape == "plane") {
        const Json* s = spec.find("size");
        float sx = 1, sz = 1;
        if (s) {
            if (!s->isArray() || s->size() != 2 || !s->at(0).isNumber() || !s->at(1).isNumber())
                fail(ctx, "plane 'size' must be [x, z]");
            sx = static_cast<float>(s->at(0).asNumber());
            sz = static_cast<float>(s->at(1).asNumber());
        }
        return makePlane(sx, sz);
    }
    if (shape == "extrude") {
        try {
            return makeExtrusion(points2(spec, "points", ctx, 3), positive(spec, "depth", 1, ctx), axisOf(spec, ctx));
        } catch (const SceneError&) {
            throw;
        } catch (const std::exception& e) {
            fail(ctx, e.what());
        }
    }
    if (shape == "lathe") {
        bool cap = spec.boolOr("cap", true);
        return makeLathe(latheProfile(spec, ctx, cap), integer(spec, "segments", 48, 3, 256, ctx));
    }
    if (shape == "mesh") {
        const Json* verts = spec.find("vertices");
        const Json* faces = spec.find("faces");
        if (!verts || !verts->isArray() || !faces || !faces->isArray())
            fail(ctx, "mesh needs 'vertices' ([[x,y,z],...]) and 'faces' ([[i,j,k],...], 0-based)");
        std::vector<Vec3> vs;
        for (const auto& v : verts->asArray()) vs.push_back(vec3From(v, "each vertex", ctx, false));
        std::vector<std::vector<uint32_t>> fs;
        for (const auto& f : faces->asArray()) {
            if (!f.isArray() || f.size() < 3) fail(ctx, "each face must list at least 3 vertex indices");
            std::vector<uint32_t> face;
            for (const auto& idx : f.asArray()) {
                if (!idx.isNumber()) fail(ctx, "face indices must be numbers");
                double d = idx.asNumber();
                if (d < 0 || d >= static_cast<double>(vs.size()))
                    fail(ctx, "face index " + std::to_string(static_cast<long long>(d)) + " out of range (have " +
                                  std::to_string(vs.size()) + " vertices, indices are 0-based)");
                face.push_back(static_cast<uint32_t>(d));
            }
            fs.push_back(std::move(face));
        }
        return makeRawMesh(vs, fs, spec.boolOr("smooth", false));
    }
    if (shape == "csg") {
        SdfPtr sdf = buildSdf(spec, ctx, false, warnings);
        if (!sdf->bounds().valid()) {
            warnings.push_back(ctx + ": csg result is empty");
            return Mesh{};
        }
        Mesh m = meshFromSdf(*sdf, 96, spec, ctx);
        if (m.empty()) warnings.push_back(ctx + ": csg result is empty (did the subtraction remove everything?)");
        return m;
    }
    fail(ctx, "unsupported shape '" + shape + "'");
}

std::string ctxOf(const Json& spec) {
    const Json* n = spec.find("name");
    return "object '" + ((n && n->isString()) ? n->asString() : std::string("?")) + "'";
}

// Geometry cache key: the spec minus fields that only affect placement/appearance.
std::string geometryKey(const Json& spec) {
    Json g = spec;
    for (const char* k : {"name", "position", "rotation", "scale", "color", "parent", "visible", "description"})
        g.erase(k);
    return g.dump();
}

struct NamedColor {
    const char* name;
    float r, g, b;
};
const NamedColor kColors[] = {
    {"white", 1, 1, 1},          {"black", 0.05f, 0.05f, 0.05f}, {"gray", 0.5f, 0.5f, 0.5f},
    {"grey", 0.5f, 0.5f, 0.5f},  {"lightgray", 0.8f, 0.8f, 0.8f}, {"darkgray", 0.3f, 0.3f, 0.3f},
    {"red", 0.85f, 0.15f, 0.12f}, {"green", 0.2f, 0.65f, 0.25f},  {"blue", 0.15f, 0.35f, 0.85f},
    {"yellow", 0.95f, 0.85f, 0.2f}, {"orange", 0.95f, 0.55f, 0.15f}, {"purple", 0.55f, 0.3f, 0.75f},
    {"pink", 0.95f, 0.6f, 0.7f},  {"brown", 0.5f, 0.32f, 0.18f},   {"cyan", 0.2f, 0.8f, 0.85f},
    {"magenta", 0.85f, 0.25f, 0.75f}, {"wood", 0.65f, 0.45f, 0.28f}, {"darkwood", 0.4f, 0.26f, 0.15f},
    {"silver", 0.75f, 0.76f, 0.78f}, {"gold", 0.85f, 0.68f, 0.25f}, {"steel", 0.55f, 0.58f, 0.62f},
    {"skin", 0.92f, 0.75f, 0.62f}, {"glass", 0.7f, 0.85f, 0.9f},   {"navy", 0.1f, 0.15f, 0.4f},
    {"olive", 0.45f, 0.5f, 0.2f},  {"beige", 0.9f, 0.85f, 0.7f},   {"teal", 0.1f, 0.5f, 0.5f},
};

}  // namespace

const BuiltObject* BuildResult::find(const std::string& name) const {
    for (const auto& o : objects)
        if (o.name == name) return &o;
    return nullptr;
}

Vec3 parseColor(const Json& v, const std::string& ctx) {
    if (v.isString()) {
        std::string s = v.asString();
        if (!s.empty() && s[0] == '#') {
            auto hex = [&](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                fail(ctx, "invalid hex color '" + s + "'");
            };
            if (s.size() == 7)
                return {(hex(s[1]) * 16 + hex(s[2])) / 255.0f, (hex(s[3]) * 16 + hex(s[4])) / 255.0f,
                        (hex(s[5]) * 16 + hex(s[6])) / 255.0f};
            if (s.size() == 4) return {hex(s[1]) / 15.0f, hex(s[2]) / 15.0f, hex(s[3]) / 15.0f};
            fail(ctx, "hex colors must look like #rrggbb or #rgb");
        }
        std::string lower;
        for (char c : s)
            if (c != ' ' && c != '_' && c != '-') lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& c : kColors)
            if (lower == c.name) return {c.r, c.g, c.b};
        std::vector<std::string> names;
        for (const auto& c : kColors) names.push_back(c.name);
        fail(ctx, "unknown color '" + s + "'" + suggest(lower, names) + "; use #rrggbb or one of: " + joinNames(names));
    }
    Vec3 c = vec3From(v, "'color'", ctx, false);
    if (maxComponent(c) > 1.0f) c = c / 255.0f;
    return vmin(vmax(c, Vec3{0, 0, 0}), Vec3{1, 1, 1});
}

std::string colorToHex(Vec3 c) {
    char buf[8];
    auto b = [](float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.x), b(c.y), b(c.z));
    return buf;
}

int Scene::indexOf(const std::string& name) const {
    for (size_t i = 0; i < objects_.size(); ++i)
        if (objects_[i].stringOr("name", "") == name) return static_cast<int>(i);
    return -1;
}

const Json* Scene::get(const std::string& name) const {
    int i = indexOf(name);
    return i >= 0 ? &objects_[static_cast<size_t>(i)] : nullptr;
}

std::vector<std::string> Scene::subtree(const std::string& name) const {
    std::set<std::string> members{name};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& o : objects_) {
            std::string n = o.stringOr("name", ""), p = o.stringOr("parent", "");
            if (!p.empty() && members.count(p) && !members.count(n)) {
                members.insert(n);
                grew = true;
            }
        }
    }
    std::vector<std::string> out;
    for (const auto& o : objects_)
        if (members.count(o.stringOr("name", ""))) out.push_back(o.stringOr("name", ""));
    return out;
}

void Scene::validate(const Json& spec, std::vector<std::string>& warnings) {
    if (!spec.isObject()) throw SceneError("each object must be a JSON object");
    const Json* name = spec.find("name");
    if (!name || !name->isString() || name->asString().empty())
        throw SceneError("every object needs a non-empty string 'name'");
    std::string ctx = ctxOf(spec);
    std::string shape = shapeOf(spec, ctx);
    checkKeys(spec, shape, false, ctx, warnings);
    localTransform(spec, ctx);
    if (const Json* c = spec.find("color"); c && !c->isNull()) parseColor(*c, ctx);
    if (const Json* p = spec.find("parent"); p && !p->isNull()) {
        if (!p->isString()) fail(ctx, "'parent' must be the name of another object");
        if (p->asString() == name->asString()) fail(ctx, "an object cannot be its own parent");
    }
    if (const Json* v = spec.find("visible"); v && !v->isBool()) fail(ctx, "'visible' must be true or false");
}

std::shared_ptr<const Mesh> Scene::localMesh(const Json& spec, const std::string& ctx) const {
    std::string key = geometryKey(spec);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    std::vector<std::string> ignored;
    auto mesh = std::make_shared<const Mesh>(buildMesh(spec, shapeOf(spec, ctx), ctx, ignored));
    if (cache_.size() > 512) cache_.clear();
    cache_[key] = mesh;
    return mesh;
}

void Scene::buildChecked(const Json& spec, std::vector<std::string>& warnings) const {
    std::string key = geometryKey(spec);
    if (cache_.count(key)) return;
    std::string ctx = ctxOf(spec);
    auto mesh = std::make_shared<const Mesh>(buildMesh(spec, shapeOf(spec, ctx), ctx, warnings));
    if (cache_.size() > 512) cache_.clear();
    cache_[key] = mesh;
}

void Scene::put(Json spec, std::vector<std::string>& warnings) {
    validate(spec, warnings);
    // Build now so geometry errors surface immediately (and warm the cache).
    buildChecked(spec, warnings);

    std::string name = spec["name"].asString();
    int idx = indexOf(name);
    if (idx >= 0) objects_[static_cast<size_t>(idx)] = std::move(spec);
    else objects_.push_back(std::move(spec));
}

void Scene::update(const std::string& name, const Json& patch, std::vector<std::string>& warnings) {
    int idx = indexOf(name);
    if (idx < 0) throw SceneError("no object named '" + name + "'");
    if (!patch.isObject()) throw SceneError("update for '" + name + "' must be an object");
    Json spec = objects_[static_cast<size_t>(idx)];
    std::string newName = name;
    for (const auto& [k, v] : patch.asObject()) {
        if (k == "name") continue;
        if (k == "rename") {
            if (!v.isString() || v.asString().empty()) throw SceneError("'rename' must be a non-empty string");
            newName = v.asString();
            continue;
        }
        if (k == "translate") {
            Vec3 d = vec3From(v, "'translate'", "object '" + name + "'", false);
            Vec3 p = vec3(spec, "position", {0, 0, 0}, "object '" + name + "'");
            p += d;
            spec["position"] = Json::array({p.x, p.y, p.z});
            continue;
        }
        if (v.isNull()) spec.erase(k);
        else spec[k] = v;
    }
    if (newName != name) {
        if (indexOf(newName) >= 0) throw SceneError("cannot rename '" + name + "': '" + newName + "' already exists");
        spec["name"] = newName;
    }
    std::vector<std::string> w;
    validate(spec, w);
    buildChecked(spec, w);
    warnings.insert(warnings.end(), w.begin(), w.end());
    objects_[static_cast<size_t>(idx)] = std::move(spec);
    if (newName != name) {
        for (auto& o : objects_)
            if (o.stringOr("parent", "") == name) o["parent"] = newName;
    }
}

std::vector<std::string> Scene::remove(const std::vector<std::string>& names, bool recursive) {
    std::set<std::string> doomed(names.begin(), names.end());
    for (const auto& n : names)
        if (indexOf(n) < 0) throw SceneError("no object named '" + n + "'");
    if (recursive) {
        bool grew = true;
        while (grew) {
            grew = false;
            for (const auto& o : objects_) {
                std::string p = o.stringOr("parent", "");
                std::string n = o.stringOr("name", "");
                if (!p.empty() && doomed.count(p) && !doomed.count(n)) {
                    doomed.insert(n);
                    grew = true;
                }
            }
        }
    }
    std::vector<std::string> removed;
    std::vector<Json> kept;
    for (auto& o : objects_) {
        std::string n = o.stringOr("name", "");
        if (doomed.count(n)) {
            removed.push_back(n);
            continue;
        }
        if (doomed.count(o.stringOr("parent", ""))) o.erase("parent");
        kept.push_back(std::move(o));
    }
    objects_ = std::move(kept);
    return removed;
}

BuildResult Scene::build() const {
    BuildResult res;
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < objects_.size(); ++i) byName[objects_[i].stringOr("name", "")] = i;

    std::vector<int> state(objects_.size(), 0);  // 0 = todo, 1 = in progress, 2 = done
    std::vector<Mat4> world(objects_.size());
    std::vector<bool> visible(objects_.size(), true);
    std::function<void(size_t)> resolve = [&](size_t i) {
        if (state[i] == 2) return;
        const Json& spec = objects_[i];
        std::string ctx = ctxOf(spec);
        if (state[i] == 1) {
            res.warnings.push_back(ctx + ": parent cycle detected; treating as a root object");
            world[i] = localTransform(spec, ctx);
            state[i] = 2;
            return;
        }
        state[i] = 1;
        Mat4 parentWorld;
        bool parentVisible = true;
        std::string parent = spec.stringOr("parent", "");
        if (!parent.empty()) {
            auto it = byName.find(parent);
            if (it == byName.end()) {
                res.warnings.push_back(ctx + ": parent '" + parent + "' does not exist; treating as a root object");
            } else {
                resolve(it->second);
                if (state[i] == 2) return;  // resolved through a cycle
                parentWorld = world[it->second];
                parentVisible = visible[it->second];
            }
        }
        world[i] = parentWorld * localTransform(spec, ctx);
        visible[i] = parentVisible && spec.boolOr("visible", true);
        state[i] = 2;
    };
    for (size_t i = 0; i < objects_.size(); ++i) resolve(i);

    for (size_t i = 0; i < objects_.size(); ++i) {
        const Json& spec = objects_[i];
        std::string ctx = ctxOf(spec);
        BuiltObject o;
        o.name = spec.stringOr("name", "");
        o.shape = spec.stringOr("shape", "");
        o.parent = spec.stringOr("parent", "");
        o.world = world[i];
        o.visible = visible[i];
        const Json* c = spec.find("color");
        o.color = (c && !c->isNull()) ? parseColor(*c, ctx) : Vec3{0.72f, 0.72f, 0.74f};
        o.localMesh = localMesh(spec, ctx);
        o.worldMesh = *o.localMesh;
        o.worldMesh.transform(o.world);
        o.bounds = o.worldMesh.bounds();
        if (o.visible) res.bounds.expand(o.bounds);
        res.objects.push_back(std::move(o));
    }
    return res;
}

Json Scene::toJson() const {
    Json doc = Json::object({{"format", "shapeforge-scene"}, {"version", 1}, {"up_axis", "+Y"}, {"front_axis", "+Z"}});
    Json arr = Json::array();
    for (const auto& o : objects_) arr.push_back(o);
    doc["objects"] = std::move(arr);
    return doc;
}

void Scene::loadJson(const Json& doc, std::vector<std::string>& warnings) {
    const Json* arr = doc.isArray() ? &doc : doc.find("objects");
    if (!arr || !arr->isArray()) throw SceneError("scene document must be an array of objects or {\"objects\": [...]}");
    Scene fresh;
    for (const auto& o : arr->asArray()) {
        if (o.isObject() && o.find("name") && fresh.indexOf(o["name"].isString() ? o["name"].asString() : "") >= 0)
            warnings.push_back("duplicate object name '" + o["name"].asString() + "'; later definition wins");
        fresh.put(o, warnings);
    }
    objects_ = std::move(fresh.objects_);
    cache_.insert(fresh.cache_.begin(), fresh.cache_.end());
}

const char* shapeReference() {
    return R"(Coordinate system: right-handed, +Y is up, the model's front faces +Z, +X is to the model's right.
Units are arbitrary but meters are recommended. Rest objects on the ground plane y = 0.
Every shape is centered on its own origin (a box of height 1 spans y = -0.5..0.5), so to put
something on the ground set position.y = half its height.

Common keys (all objects):
  name (required, unique)   shape (required)   description (free-text note)
  position [x,y,z] (default [0,0,0])
  rotation [rx,ry,rz] Euler degrees, applied X then Y then Z
  scale  number or [sx,sy,sz]; negative values mirror
  color  "#rrggbb" | [r,g,b] 0..1 | name (red, wood, steel, gold, skin, ...)
  parent name of another object; position/rotation/scale become relative to it
  visible true/false

Shapes and their keys (defaults in parentheses):
  box      size [x,y,z] ([1,1,1]); rounding (0) = corner radius (built as an SDF; optional
           resolution like csg)
  sphere   radius (0.5); segments (32); rings (16)
  cylinder radius (0.5), or radius_bottom/radius_top for a frustum; height (1); segments (32); axis = Y
  cone     radius (0.5) at the bottom, tip at the top; height (1); segments (32)
  capsule  radius (0.25); height (1) = total tip-to-tip height; segments (32)
  torus    radius (0.5) = ring radius; tube (0.15) = tube radius; lies flat in the XZ plane
  plane    size [x,z] ([1,1]); a flat one-sided-looking quad at y = 0, facing +Y
  extrude  points [[a,b],...] polygon outline (>= 3, no self-intersections); depth (1);
           axis "z" (points are [x,y]) | "y" (points are [x,z]) | "x" (points are [z,y])
  lathe    points [[radius,y],...] profile listed bottom to top, revolved around Y;
           cap (true) closes the ends at the axis; segments (48)
  mesh     vertices [[x,y,z],...]; faces [[i,j,k(,l...)],...] 0-based, counter-clockwise
           seen from outside; smooth (false)
  csg      op "union" | "subtract" | "intersect"; children [shape specs...];
           subtract removes children[1..] from children[0]; smooth (0) blends seams;
           shell (0) hollows the result with that wall thickness; resolution = cells
           along the longest side (default: automatic, 96-256 based on the thinnest part); simplify (true) reduces
           triangles on flat areas. Children take shape,
           position, rotation, scale and shape keys (not name/color/parent);
           children may be csg nodes themselves. Allowed child shapes: box, sphere,
           cylinder, cone, capsule, torus, extrude, lathe, csg.
  group    no geometry; an empty transform used as a parent for other objects)";
}

}  // namespace sf
