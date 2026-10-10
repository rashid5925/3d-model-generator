#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

#include "render/renderer.hpp"

namespace sf {

namespace {

struct ViewDef {
    std::string name;
    Vec3 dir;  // from target toward the eye
    Vec3 up;
    bool ortho = true;
    float fov = 35.0f;
    float azimuth = 0, elevation = 0;
};

Vec3 dirFromAngles(float azDeg, float elDeg) {
    float az = radians(azDeg), el = radians(elDeg);
    return normalize(Vec3{std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)});
}

ViewDef angleView(const std::string& name, float az, float el, bool ortho) {
    ViewDef v;
    v.name = name;
    v.azimuth = az;
    v.elevation = el;
    v.dir = dirFromAngles(az, el);
    v.ortho = ortho;
    if (el > 89.0f) v.up = dirFromAngles(az + 180.0f, 0);       // looking down: screen-up = away from front
    else if (el < -89.0f) v.up = dirFromAngles(az, 0);
    else v.up = {0, 1, 0};
    return v;
}

ViewDef parseView(const Json& j) {
    if (j.isString()) {
        std::string s = j.asString();
        if (s == "front") return angleView("front", 0, 0, true);
        if (s == "back") return angleView("back", 180, 0, true);
        if (s == "right") return angleView("right", 90, 0, true);
        if (s == "left") return angleView("left", -90, 0, true);
        if (s == "top") return angleView("top", 0, 90, true);
        if (s == "bottom") return angleView("bottom", 0, -90, true);
        if (s == "iso" || s == "perspective") return angleView("iso", 35, 28, false);
        if (s == "iso_back") return angleView("iso_back", 215, 28, false);
        if (s == "iso_left") return angleView("iso_left", -35, 28, false);
        if (s == "iso_below") return angleView("iso_below", 35, -28, false);
        throw SceneError("unknown view '" + s +
                         "'; use front, back, left, right, top, bottom, iso, iso_back, iso_left, iso_below or "
                         "{\"azimuth\": deg, \"elevation\": deg}");
    }
    if (j.isObject()) {
        float az = static_cast<float>(j.numberOr("azimuth", 35));
        float el = std::clamp(static_cast<float>(j.numberOr("elevation", 25)), -90.0f, 90.0f);
        bool ortho = j.boolOr("ortho", false);
        ViewDef v = angleView(j.stringOr("label", "custom"), az, el, ortho);
        v.fov = std::clamp(static_cast<float>(j.numberOr("fov", 35)), 10.0f, 100.0f);
        return v;
    }
    throw SceneError("each view must be a name string or an object with azimuth/elevation");
}

float niceStep(float raw) {
    if (!(raw > 0)) return 1;
    float p = std::pow(10.0f, std::floor(std::log10(raw)));
    float m = raw / p;
    float nice = m < 1.5f ? 1 : m < 3.5f ? 2 : m < 7.5f ? 5 : 10;
    return nice * p;
}

std::string fmtNum(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::fabs(v) < 1e-9f ? 0.0f : v);
    return buf;
}

// Names the world axis that a screen direction corresponds to, e.g. "+X".
std::string axisName(Vec3 d) {
    const char* names[3] = {"X", "Y", "Z"};
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(d[i]) > std::fabs(d[best])) best = i;
    if (std::fabs(d[best]) < 0.98f) return "";
    return std::string(d[best] > 0 ? "+" : "-") + names[best];
}

struct Camera {
    Mat4 view, proj, viewProj;
    Vec3 eye, forward, right, up;
    bool ortho;
};

Camera makeCamera(const ViewDef& vd, const AABB& bounds) {
    Camera cam;
    Vec3 target = bounds.center();
    float radius = std::max(length(bounds.size()) * 0.5f, 1e-3f);
    cam.ortho = vd.ortho;
    cam.forward = -vd.dir;
    if (vd.ortho) {
        float dist = radius * 4.0f;
        cam.eye = target + vd.dir * dist;
        cam.view = lookAt(cam.eye, target, vd.up);
        AABB vb = bounds.transformed(cam.view);
        float half = std::max(vb.size().x, vb.size().y) * 0.5f * 1.15f;
        half = std::max(half, radius * 0.05f);
        float cx = vb.center().x, cy = vb.center().y;
        cam.proj = orthographic(cx - half, cx + half, cy - half, cy + half, 0.0f, dist + radius * 24.0f);
    } else {
        float fov = radians(vd.fov);
        float dist = radius / std::sin(fov * 0.5f) * 1.05f;
        // Tighten framing: shrink distance until the projected box fills ~85% of the view.
        for (int it = 0; it < 4; ++it) {
            Vec3 eye = target + vd.dir * dist;
            Mat4 view = lookAt(eye, target, vd.up);
            Mat4 proj = perspective(fov, 1.0f, dist * 0.01f, dist + radius * 30.0f);
            Mat4 vp = proj * view;
            float m = 0;
            for (int i = 0; i < 8; ++i) {
                Vec3 c = bounds.corner(i);
                Vec4 h = vp * Vec4{c.x, c.y, c.z, 1};
                if (h.w <= 1e-6f) {
                    m = 10;
                    break;
                }
                m = std::max(m, std::max(std::fabs(h.x / h.w), std::fabs(h.y / h.w)));
            }
            float next = dist * (0.25f + 0.75f * m / 0.85f);
            dist = std::max(next, radius * 1.15f);
        }
        cam.eye = target + vd.dir * dist;
        cam.view = lookAt(cam.eye, target, vd.up);
        cam.proj = perspective(fov, 1.0f, dist * 0.01f, dist + radius * 30.0f);
    }
    cam.viewProj = cam.proj * cam.view;
    Mat4 inv = inverse(cam.view);
    cam.right = normalize(transformDir(inv, {1, 0, 0}));
    cam.up = normalize(transformDir(inv, {0, 1, 0}));
    return cam;
}

void pushLine(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c) {
    out.push_back({{a.x, a.y, a.z}, {0, 0, 0}, {c.x, c.y, c.z}});
    out.push_back({{b.x, b.y, b.z}, {0, 0, 0}, {c.x, c.y, c.z}});
}

float buildGrid(const AABB& bounds, std::vector<Vertex>& lines) {
    Vec3 size = bounds.size();
    float step = niceStep(std::max({size.x, size.y, size.z, 1e-3f}) / 8.0f);
    int x0 = static_cast<int>(std::floor(bounds.min.x / step)) - 2, x1 = static_cast<int>(std::ceil(bounds.max.x / step)) + 2;
    int z0 = static_cast<int>(std::floor(bounds.min.z / step)) - 2, z1 = static_cast<int>(std::ceil(bounds.max.z / step)) + 2;
    if ((x1 - x0) > 400 || (z1 - z0) > 400) return step;
    float y = -step * 0.002f;
    Vec3 minor{0.36f, 0.37f, 0.40f}, major{0.47f, 0.48f, 0.52f};
    for (int i = x0; i <= x1; ++i) {
        float x = i * step;
        Vec3 c = (i == 0) ? Vec3{0.35f, 0.45f, 0.85f} : (i % 5 == 0 ? major : minor);  // x = 0 line runs along Z
        pushLine(lines, {x, y, z0 * step}, {x, y, z1 * step}, c);
    }
    for (int k = z0; k <= z1; ++k) {
        float z = k * step;
        Vec3 c = (k == 0) ? Vec3{0.85f, 0.35f, 0.35f} : (k % 5 == 0 ? major : minor);  // z = 0 line runs along X
        pushLine(lines, {x0 * step, y, z}, {x1 * step, y, z}, c);
    }
    return step;
}

Vec3 desaturate(Vec3 c) {
    float l = 0.3f * c.x + 0.59f * c.y + 0.11f * c.z;
    Vec3 g{l, l, l};
    return lerp(g, Vec3{0.62f, 0.63f, 0.66f}, 0.6f);
}

bool projectToPixel(const Camera& cam, Vec3 p, int size, float& px, float& py) {
    Vec4 h = cam.viewProj * Vec4{p.x, p.y, p.z, 1};
    if (h.w <= 1e-6f) return false;
    px = (h.x / h.w * 0.5f + 0.5f) * size;
    py = (h.y / h.w * 0.5f + 0.5f) * size;
    return px >= 0 && py >= 0 && px < size && py < size;
}

void drawGizmo(Image& img, const Camera& cam, float k) {
    struct Ax {
        Vec3 d;
        const char* name;
        Rgb color;
    } axes[3] = {{{1, 0, 0}, "X", {235, 85, 85}}, {{0, 1, 0}, "Y", {120, 205, 95}}, {{0, 0, 1}, "Z", {95, 150, 245}}};
    // Draw axes pointing away from the viewer first.
    std::sort(std::begin(axes), std::end(axes),
              [&](const Ax& a, const Ax& b) { return dot(a.d, cam.forward) > dot(b.d, cam.forward); });
    // k scales the gizmo with the panel size (1 at 512 px).
    int box = static_cast<int>(68 * k);
    float cx = 6 + box * 0.5f, cy = static_cast<float>(img.height) - 6 - box * 0.5f, len = 26 * k;
    fillRect(img, 6, img.height - 6 - box, box, box, {20, 21, 24}, 0.55f);
    for (const auto& a : axes) {
        float sx = dot(a.d, cam.right), sy = -dot(a.d, cam.up);
        float l = std::sqrt(sx * sx + sy * sy);
        if (l < 0.2f) {
            // Axis points (almost) straight at or away from the camera.
            bool toward = dot(a.d, cam.forward) < 0;
            fillRect(img, static_cast<int>(cx) - 3, static_cast<int>(cy) - 3, 7, 7, a.color);
            drawText(img, static_cast<int>(cx) + 6, static_cast<int>(cy) + 4, std::string(toward ? "+" : "-") + a.name, a.color, 1);
            continue;
        }
        float ex = cx + sx * len, ey = cy + sy * len;
        drawLine(img, cx, cy, ex, ey, a.color, 2.2f);
        drawText(img, static_cast<int>(ex + sx / l * 7) - 2, static_cast<int>(ey + sy / l * 7) - 3, a.name, a.color, 1);
    }
}

}  // namespace

const char* viewReference() {
    return "Views: front (camera on +Z looking at the model's front), back, left, right, top (looking down, "
           "front of model at the bottom of the picture), bottom - all orthographic, good for checking alignment "
           "and proportions; iso, iso_back, iso_left, iso_below - perspective 3/4 views. Custom: {\"azimuth\": deg "
           "(0 = front, 90 = right side), \"elevation\": deg (90 = top), \"ortho\": bool, \"fov\": deg, \"label\": str}.";
}

RenderOutput renderScene(Renderer& renderer, const BuildResult& scene, const RenderOptions& opts) {
    std::vector<Json> viewSpecs = opts.views;
    if (viewSpecs.empty()) viewSpecs = {Json("front"), Json("right"), Json("top"), Json("iso")};
    if (viewSpecs.size() > 9) throw SceneError("at most 9 views per render");
    std::vector<ViewDef> views;
    for (const auto& v : viewSpecs) views.push_back(parseView(v));
    int size = std::clamp(opts.size, 128, 1024);

    // Focus: expand the requested names with their descendants.
    std::set<std::string> focusSet;
    for (const auto& f : opts.focus) {
        if (!scene.find(f)) throw SceneError("focus: no object named '" + f + "'");
        focusSet.insert(f);
    }
    for (bool grew = !focusSet.empty(); grew;) {
        grew = false;
        for (const auto& o : scene.objects)
            if (!o.parent.empty() && focusSet.count(o.parent) && !focusSet.count(o.name)) {
                focusSet.insert(o.name);
                grew = true;
            }
    }
    auto drawn = [&](const BuiltObject& o) {
        return o.visible && (!opts.isolate || focusSet.empty() || focusSet.count(o.name));
    };

    AABB bounds;
    if (!focusSet.empty()) {
        for (const auto& o : scene.objects)
            if (focusSet.count(o.name) && o.visible) bounds.expand(o.bounds);
    } else {
        bounds = scene.bounds;
    }
    if (!bounds.valid()) bounds = AABB{{-0.5f, 0, -0.5f}, {0.5f, 1, 0.5f}};

    std::vector<Vertex> tris;
    std::vector<std::string> unknownHighlights;
    for (const auto& h : opts.highlight)
        if (!scene.find(h)) unknownHighlights.push_back(h);
    for (const auto& o : scene.objects) {
        if (!drawn(o) || o.worldMesh.empty()) continue;
        Vec3 color = o.color;
        if (!opts.highlight.empty()) {
            bool hl = std::find(opts.highlight.begin(), opts.highlight.end(), o.name) != opts.highlight.end();
            if (!hl) color = desaturate(color);
        }
        const Mesh& m = o.worldMesh;
        tris.reserve(tris.size() + m.indices.size());
        for (uint32_t idx : m.indices) {
            Vec3 p = m.positions[idx], n = m.normals[idx];
            tris.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {color.x, color.y, color.z}});
        }
    }
    std::vector<Vertex> lines;
    float gridStep = buildGrid(bounds, lines);
    if (!opts.grid) lines.clear();

    int n = static_cast<int>(views.size());
    int cols = n == 1 ? 1 : (n <= 4 ? 2 : 3);
    int rows = (n + cols - 1) / cols;
    const int gap = 4;
    Image out(cols * size + (cols - 1) * gap, rows * size + (rows - 1) * gap, {12, 12, 14});

    Json viewInfo = Json::array();
    for (int vi = 0; vi < n; ++vi) {
        const ViewDef& vd = views[static_cast<size_t>(vi)];
        Camera cam = makeCamera(vd, bounds);
        Frame frame;
        frame.width = frame.height = size;
        frame.viewProj = cam.viewProj;
        std::copy(cam.viewProj.m, cam.viewProj.m + 16, frame.uniforms.viewProj);
        frame.uniforms.eye[0] = cam.eye.x;
        frame.uniforms.eye[1] = cam.eye.y;
        frame.uniforms.eye[2] = cam.eye.z;
        frame.uniforms.eye[3] = cam.ortho ? 1.0f : 0.0f;
        frame.uniforms.forward[0] = cam.forward.x;
        frame.uniforms.forward[1] = cam.forward.y;
        frame.uniforms.forward[2] = cam.forward.z;
        // Lights are attached to the camera so every view is lit from the upper left.
        Vec3 key = normalize(cam.up * 0.75f - cam.right * 0.45f - cam.forward * 0.55f);
        Vec3 fill = normalize(cam.right * 0.8f - cam.forward * 0.4f - cam.up * 0.1f);
        frame.uniforms.keyDir[0] = key.x;
        frame.uniforms.keyDir[1] = key.y;
        frame.uniforms.keyDir[2] = key.z;
        frame.uniforms.fillDir[0] = fill.x;
        frame.uniforms.fillDir[1] = fill.y;
        frame.uniforms.fillDir[2] = fill.z;
        frame.triangles = tris;
        frame.lines = lines;

        Image panel = renderer.render(frame);

        if (opts.labels) {
            // Greedy placement: nudge a label down/up until it stops overlapping earlier ones.
            struct Rect {
                int x, y, w, h;
            };
            std::vector<Rect> placed;
            auto overlaps = [&](const Rect& r) {
                for (const auto& p : placed)
                    if (r.x < p.x + p.w && p.x < r.x + r.w && r.y < p.y + p.h && p.y < r.y + r.h) return true;
                return false;
            };
            for (const auto& o : scene.objects) {
                if (!drawn(o) || !o.bounds.valid()) continue;
                float px, py;
                if (!projectToPixel(cam, o.bounds.center(), size, px, py)) continue;
                int w = textWidth(o.name, 1);
                Rect r{static_cast<int>(px) - w / 2 - 2, static_cast<int>(py) - 5, w + 4, 11};
                const int offsets[] = {0, 12, -12, 24, -24, 36, -36};
                for (int off : offsets) {
                    Rect cand{r.x, r.y + off, r.w, r.h};
                    if (!overlaps(cand)) {
                        r = cand;
                        break;
                    }
                }
                placed.push_back(r);
                drawLabel(panel, r.x + 2, r.y + 2, o.name, {255, 255, 255}, {10, 10, 12}, 1, 0.7f);
            }
        }
        if (!opts.overlays) {
            int col = vi % cols, row = vi / cols;
            blit(out, panel, col * (size + gap), row * (size + gap));
            viewInfo.push_back(Json::object({{"name", vd.name}}));
            continue;
        }
        std::string title = vd.name;
        for (auto& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        title += vd.ortho ? "  ORTHO" : "  PERSP";
        int titleScale = size >= 384 ? 2 : 1;
        drawLabel(panel, 4 + 3 * titleScale, 4 + 3 * titleScale, title, {255, 255, 255}, {15, 15, 18}, titleScale, 0.6f);
        std::string sub;
        if (vd.ortho) {
            std::string r = axisName(cam.right), u = axisName(cam.up);
            if (!r.empty() && !u.empty()) sub = "right " + r + "  up " + u + "  ";
        }
        if (!vd.ortho || sub.empty()) sub = "az " + fmtNum(vd.azimuth) + " el " + fmtNum(vd.elevation) + "  " + sub;
        if (opts.grid) sub += "grid " + fmtNum(gridStep);
        if (!sub.empty())
            drawLabel(panel, 4 + 3 * titleScale, 12 + 11 * titleScale, sub, {215, 218, 225}, {15, 15, 18}, 1, 0.6f);
        drawGizmo(panel, cam, std::clamp(size / 512.0f, 0.6f, 1.5f));

        int col = vi % cols, row = vi / cols;
        blit(out, panel, col * (size + gap), row * (size + gap));

        Json vj = Json::object({{"name", vd.name},
                                {"projection", vd.ortho ? "orthographic" : "perspective"},
                                {"camera_position", Json::array({cam.eye.x, cam.eye.y, cam.eye.z})},
                                {"looking_toward", Json::array({cam.forward.x, cam.forward.y, cam.forward.z})}});
        if (vd.ortho) {
            std::string r = axisName(cam.right), u = axisName(cam.up);
            if (!r.empty()) vj["screen_right"] = r;
            if (!u.empty()) vj["screen_up"] = u;
        }
        viewInfo.push_back(std::move(vj));
    }

    RenderOutput res;
    res.image = std::move(out);
    res.info = Json::object({{"renderer", renderer.name()},
                             {"image_size", Json::array({res.image.width, res.image.height})},
                             {"layout", std::to_string(cols) + " columns x " + std::to_string(rows) + " rows, row-major"},
                             {"grid_spacing", opts.grid ? Json(gridStep) : Json()},
                             {"views", std::move(viewInfo)}});
    if (!unknownHighlights.empty()) {
        Json arr = Json::array();
        for (const auto& u : unknownHighlights) arr.push_back(u);
        res.info["unknown_highlight_names"] = std::move(arr);
    }
    return res;
}

std::unique_ptr<Renderer> createRenderer(const std::string& preference, std::string& log) {
    if (preference == "cpu") {
        log = "using CPU renderer (requested)";
        return createCpuRenderer();
    }
#ifdef SF_HAVE_VULKAN
    try {
        auto r = createVulkanRenderer();
        log = "using " + r->name();
        return r;
    } catch (const std::exception& e) {
        if (preference == "vulkan") throw;
        log = std::string("Vulkan unavailable (") + e.what() + "); falling back to CPU renderer";
        return createCpuRenderer();
    }
#else
    if (preference == "vulkan") throw std::runtime_error("this build has no Vulkan support");
    log = "built without Vulkan; using CPU renderer";
    return createCpuRenderer();
#endif
}

}  // namespace sf
