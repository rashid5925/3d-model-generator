// Self-contained unit tests (no framework): run `ctest` or ./shapeforge_tests.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "inspect.hpp"
#include "json.hpp"
#include "mesh.hpp"
#include "placement.hpp"
#include "simplify.hpp"
#include "render/renderer.hpp"
#include "scene.hpp"
#include "sdf.hpp"

using namespace sf;

constexpr double kPiD = 3.14159265358979323846;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        ++checks;                                                                                      \
        double va = (a), vb = (b);                                                                     \
        if (std::fabs(va - vb) > (tol)) {                                                              \
            ++failures;                                                                                \
            std::printf("  FAIL %s:%d: %s = %g, expected %g (tol %g)\n", __FILE__, __LINE__, #a, va, vb, \
                        static_cast<double>(tol));                                                     \
        }                                                                                              \
    } while (0)

template <typename E, typename Fn>
bool throws(Fn&& fn, std::string* msg = nullptr) {
    try {
        fn();
    } catch (const E& e) {
        if (msg) *msg = e.what();
        return true;
    }
    return false;
}

void testJson() {
    Json j = Json::parse(R"({"b": 1, "a": [1, 2.5, "xé", true, null], "c": {"d": -0.125},})");
    CHECK(j["b"].asNumber() == 1);
    CHECK(j["a"].size() == 5);
    CHECK(j["a"].at(2).asString() == "x\xc3\xa9");
    CHECK(j.asObject()[0].first == "b");  // insertion order preserved
    Json round = Json::parse(j.dump());
    CHECK(round == j);
    CHECK(Json(0.1).dump() == "0.1");
    CHECK(Json(3.0).dump() == "3");
    CHECK(throws<JsonError>([] { Json::parse("{\"a\": }"); }));
    CHECK(throws<JsonError>([] { Json::parse("[1, 2"); }));
}

void testPrimitives() {
    CHECK_NEAR(makeBox({1, 2, 3}).volume(), 6.0, 1e-5);
    CHECK_NEAR(makeBox({1, 1, 1}).surfaceArea(), 6.0, 1e-5);
    double sphereV = 4.0 / 3.0 * kPiD * 0.125;
    CHECK_NEAR(makeSphere(0.5f, 64, 32).volume(), sphereV, sphereV * 0.01);
    CHECK_NEAR(makeCylinder(0.5f, 0.5f, 2, 128).volume(), kPiD * 0.25 * 2, 0.01);
    CHECK_NEAR(makeCylinder(0.5f, 0.0f, 1, 128).volume(), kPiD * 0.25 / 3, 0.005);  // cone
    double torusV = 2 * kPiD * kPiD * 0.5 * 0.1 * 0.1;
    CHECK_NEAR(makeTorus(0.5f, 0.1f, 96, 48).volume(), torusV, torusV * 0.01);
    double capV = kPiD * 0.04 * 0.6 + 4.0 / 3.0 * kPiD * 0.008;
    CHECK_NEAR(makeCapsule(0.2f, 1.0f, 96).volume(), capV, capV * 0.01);

    // Concave "L" polygon extruded along each axis keeps the same volume.
    std::vector<Vec2> L{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    for (char ax : {'x', 'y', 'z'}) CHECK_NEAR(makeExtrusion(L, 0.5f, ax).volume(), 3 * 0.5, 1e-4);
    std::vector<Vec2> Lcw(L.rbegin(), L.rend());
    CHECK_NEAR(makeExtrusion(Lcw, 1.0f, 'z').volume(), 3.0, 1e-4);

    // Mirroring transform must keep outward winding (positive volume).
    Mesh m = makeBox({1, 1, 1});
    m.transform(scaling({-1, 1, 1}));
    CHECK(m.volume() > 0.99);
}

void testCsg() {
    auto cube = sdfBox({1, 1, 1}, 0);
    Mesh mc = polygonize(*cube, 64);
    CHECK_NEAR(mc.volume(), 1.0, 0.03);
    std::vector<SdfPtr> kids;
    kids.push_back(sdfBox({1, 1, 1}, 0));
    kids.push_back(sdfSphere(0.4f));
    auto diff = sdfCombine(CsgOp::Subtract, std::move(kids), 0);
    double expected = 1.0 - 4.0 / 3.0 * kPiD * 0.064;
    CHECK_NEAR(polygonize(*diff, 96).volume(), expected, 0.03);
}

// Volume via the divergence theorem along each axis separately: they agree only
// when the surface is closed and consistently oriented.
Vec3 axisVolumes(const Mesh& m) {
    Vec3 v{0, 0, 0};
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        Vec3 a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]], c = m.positions[m.indices[i + 2]];
        Vec3 n = cross(b - a, c - a) * 0.5f;
        Vec3 cen = (a + b + c) / 3.0f;
        v += Vec3{cen.x * n.x, cen.y * n.y, cen.z * n.z};
    }
    return v;
}

void testThinSdfOrientation() {
    // Thin rounded plate: twisted quads used to get inverted faces.
    for (int res : {64, 150, 180}) {
        Mesh m = polygonize(*sdfBox({1.2f, 0.04f, 0.7f}, 0.01f), res);
        Vec3 v = axisVolumes(m);
        CHECK_NEAR(v.x, v.y, 1e-4);
        CHECK_NEAR(v.z, v.y, 1e-4);
        CHECK_NEAR(m.volume(), 0.0334, 0.0006);
        float h = 0;
        Mesh s = polygonize(*sdfBox({1.2f, 0.04f, 0.7f}, 0.01f), res, &h);
        simplifyMesh(s, h * 0.3f);
        CHECK(s.triangleCount() < m.triangleCount() / 10);
        CHECK_NEAR(s.volume(), m.volume(), 0.0005);
    }
}

void testPlacement() {
    Scene s;
    std::vector<std::string> w;
    s.put(Json::parse(R"({"name": "ball", "shape": "sphere", "radius": 1, "segments": 96, "rings": 48})"), w);
    s.put(Json::parse(R"({"name": "cube", "shape": "box", "size": [0.1, 0.1, 0.1], "position": [0.6, 3, 0]})"), w);
    placeOn(s, Json::parse(R"({"object": "cube", "target": "ball"})"));
    // The cube's inner bottom corner (x = 0.55) rests on the sphere: y = sqrt(1 - 0.55^2).
    CHECK_NEAR(s.build().find("cube")->bounds.min.y, std::sqrt(1 - 0.55 * 0.55), 0.01);
    CHECK_NEAR(s.build().find("cube")->bounds.center().x, 0.6, 1e-5);

    placeOn(s, Json::parse(R"({"object": "cube", "target": "ball", "side": "right", "center": true})"));
    AABB cb = s.build().find("cube")->bounds;
    CHECK_NEAR(cb.min.x, 1.0, 0.01);
    CHECK_NEAR(cb.center().y, 0.0, 1e-4);

    s.put(Json::parse(R"({"name": "tilted", "shape": "box", "size": [1, 0.2, 0.2], "rotation": [0, 0, 30], "position": [3, -2, 0]})"), w);
    placeOn(s, Json::parse(R"({"object": "tilted"})"));
    CHECK_NEAR(s.build().find("tilted")->bounds.min.y, 0.0, 1e-4);

    // Align.
    s.put(Json::parse(R"({"name": "a", "shape": "box", "position": [0, 5, 0]})"), w);
    s.put(Json::parse(R"({"name": "b", "shape": "box", "size": [2, 2, 2], "position": [4, 1, 3]})"), w);
    alignObjects(s, Json::parse(R"({"objects": ["a", "b"], "axis": ["y", "z"], "mode": "min"})"));
    CHECK_NEAR(s.build().find("b")->bounds.min.y, 4.5, 1e-4);
    CHECK_NEAR(s.build().find("b")->bounds.min.z, -0.5, 1e-4);
}

void testMirrorAndArray() {
    Scene s;
    std::vector<std::string> w;
    s.put(Json::parse(R"({"name": "body", "shape": "group", "position": [5, 0, 0]})"), w);
    s.put(Json::parse(R"({"name": "arm_left", "parent": "body", "shape": "group", "position": [-0.4, 1, 0], "rotation": [0, 0, 20]})"), w);
    s.put(Json::parse(R"({"name": "hand_left", "parent": "arm_left", "shape": "cone", "radius": 0.1, "height": 0.3, "position": [-0.2, 0, 0], "rotation": [0, 0, 90]})"), w);
    PlacementResult r = mirrorObject(s, Json::parse(R"({"object": "arm_left"})"));
    CHECK(r.changed.size() == 2 && s.get("arm_right") && s.get("hand_right"));
    CHECK(s.get("hand_right")->stringOr("parent", "") == "arm_right");
    BuildResult b = s.build();
    AABB L = b.find("hand_left")->bounds, R = b.find("hand_right")->bounds;
    // Mirror plane is x = 0 in the parent's space, i.e. world x = 5.
    CHECK_NEAR(L.min.x - 5, -(R.max.x - 5), 1e-4);
    CHECK_NEAR(L.min.y, R.min.y, 1e-4);
    CHECK(b.find("hand_right")->worldMesh.volume() > 0);  // still outward-facing

    s.put(Json::parse(R"({"name": "spoke", "shape": "box", "size": [0.4, 0.05, 0.05], "position": [1, 0, 0]})"), w);
    r = arrayObject(s, Json::parse(R"({"object": "spoke", "count": 4, "mode": "radial", "axis": "y"})"));
    CHECK(r.changed.size() == 3);
    b = s.build();
    CHECK_NEAR(b.find("spoke_3")->bounds.center().x, -1.0, 1e-4);
    CHECK_NEAR(b.find("spoke_2")->bounds.center().z, -1.0, 1e-4);  // +90 deg about Y takes +X to -Z
    CHECK_NEAR(b.find("spoke_2")->bounds.size().z, 0.4, 1e-4);     // rotated with the ring
    r = arrayObject(s, Json::parse(R"({"object": "spoke", "count": 3, "offset": [0, 0.5, 0], "name_prefix": "rung"})"));
    CHECK(s.get("rung_3") != nullptr);
    CHECK_NEAR(s.build().find("rung_3")->bounds.center().y, 1.0, 1e-4);
}

void testSceneValidation() {
    Scene s;
    std::vector<std::string> w;
    std::string msg;
    CHECK(throws<SceneError>([&] { s.put(Json::parse(R"({"name": "a", "shape": "cube"})"), w); }, &msg));
    CHECK(msg.find("did you mean 'csg'") != std::string::npos || msg.find("valid shapes") != std::string::npos);
    CHECK(throws<SceneError>([&] { s.put(Json::parse(R"({"shape": "box"})"), w); }));
    CHECK(throws<SceneError>([&] { s.put(Json::parse(R"({"name": "a", "shape": "box", "size": [1, 2]})"), w); }));
    CHECK(throws<SceneError>([&] { s.put(Json::parse(R"({"name": "a", "shape": "sphere", "radius": -1})"), w); }));
    CHECK(s.size() == 0);

    w.clear();
    s.put(Json::parse(R"({"name": "a", "shape": "box", "sise": [1, 1, 1]})"), w);
    CHECK(w.size() == 1 && w[0].find("did you mean 'size'") != std::string::npos);
    CHECK(throws<SceneError>([&] { s.put(Json::parse(R"({"name": "c", "shape": "box", "color": "blurple"})"), w); }));
}

void testHierarchy() {
    Scene s;
    std::vector<std::string> w;
    s.put(Json::parse(R"({"name": "g", "shape": "group", "position": [10, 0, 0], "rotation": [0, 90, 0]})"), w);
    s.put(Json::parse(R"({"name": "b", "parent": "g", "shape": "box", "size": [2, 1, 1], "position": [0, 0.5, 0]})"), w);
    BuildResult r = s.build();
    const BuiltObject* b = r.find("b");
    CHECK(b != nullptr);
    // A 2-wide box rotated 90 degrees about Y becomes 2 deep, centered at x = 10.
    CHECK_NEAR(b->bounds.size().x, 1.0, 1e-4);
    CHECK_NEAR(b->bounds.size().z, 2.0, 1e-4);
    CHECK_NEAR(b->bounds.center().x, 10.0, 1e-4);
    CHECK_NEAR(b->bounds.min.y, 0.0, 1e-5);

    s.update("g", Json::parse(R"({"rename": "base", "translate": [0, 1, 0]})"), w);
    CHECK(s.get("b")->stringOr("parent", "") == "base");
    r = s.build();
    CHECK_NEAR(r.find("b")->bounds.min.y, 1.0, 1e-5);

    s.put(Json::parse(R"({"name": "c", "parent": "b", "shape": "sphere"})"), w);
    auto removed = s.remove({"base"}, true);
    CHECK(removed.size() == 3 && s.size() == 0);
}

void testInspect() {
    Scene s;
    std::vector<std::string> w;
    s.put(Json::parse(R"({"name": "floor_box", "shape": "box", "position": [0, 0.5, 0]})"), w);
    s.put(Json::parse(R"({"name": "stacked", "shape": "box", "position": [0, 1.5, 0]})"), w);
    s.put(Json::parse(R"({"name": "floater", "shape": "sphere", "radius": 0.2, "position": [3, 2, 0]})"), w);
    Json rep = inspectScene(s.build(), {"floor_box", "stacked"});
    CHECK(rep["relations"].at(0)["status"].asString() == "touching");
    bool sawFloater = false;
    for (const auto& x : rep["warnings"].asArray())
        if (x.asString().find("'floater' appears to float") != std::string::npos) sawFloater = true;
    CHECK(sawFloater);
}

void testCpuRender() {
    Scene s;
    std::vector<std::string> w;
    s.put(Json::parse(R"({"name": "a", "shape": "box", "color": "#ff0000", "position": [0, 0.5, 0]})"), w);
    auto r = createCpuRenderer();
    RenderOptions opts;
    opts.views = {Json("front")};
    opts.size = 128;
    RenderOutput out = renderScene(*r, s.build(), opts);
    CHECK(out.image.width == 128 && out.image.height == 128);
    Rgb c = out.image.get(64, 64);  // center of the panel should be the red box
    CHECK(c.r > 120 && c.g < 80 && c.b < 80);
    std::vector<uint8_t> png = encodePng(out.image);
    CHECK(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
}

}  // namespace

int main() {
    std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"json", testJson},           {"primitives", testPrimitives}, {"csg", testCsg},
        {"validation", testSceneValidation}, {"hierarchy", testHierarchy}, {"inspect", testInspect},
        {"cpu_render", testCpuRender},  {"sdf_orientation", testThinSdfOrientation},
        {"placement", testPlacement},   {"mirror_array", testMirrorAndArray},
    };
    for (auto& [name, fn] : tests) {
        std::printf("[%s]\n", name);
        try {
            fn();
        } catch (const std::exception& e) {
            ++failures;
            std::printf("  FAIL: unexpected exception: %s\n", e.what());
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
