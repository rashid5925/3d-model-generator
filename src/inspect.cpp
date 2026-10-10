#include "inspect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace sf {

double roundTo(double v, int decimals) {
    double p = std::pow(10.0, decimals);
    double r = std::round(v * p) / p;
    return r == 0 ? 0 : r;  // avoid -0
}

namespace {
std::string fmtG(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", roundTo(v));
    return buf;
}
}  // namespace

Json vecJson(Vec3 v) { return Json::array({roundTo(v.x), roundTo(v.y), roundTo(v.z)}); }

Json summarizeObject(const BuiltObject& o) {
    Json j = Json::object({{"name", o.name}, {"shape", o.shape}});
    if (!o.parent.empty()) j["parent"] = o.parent;
    if (o.bounds.valid()) {
        j["min"] = vecJson(o.bounds.min);
        j["max"] = vecJson(o.bounds.max);
        j["size"] = vecJson(o.bounds.size());
        j["triangles"] = o.worldMesh.triangleCount();
    }
    if (!o.visible) j["visible"] = false;
    return j;
}

namespace {

struct Relation {
    std::string status;  // overlapping | touching | separated
    float distance = 0;  // for separated: AABB gap distance
    Vec3 gaps;           // per axis; negative = overlap depth on that axis
};

Relation relate(const AABB& a, const AABB& b, float eps) {
    Relation r;
    for (int i = 0; i < 3; ++i) r.gaps[i] = std::max(a.min[i] - b.max[i], b.min[i] - a.max[i]);
    float maxGap = maxComponent(r.gaps);
    if (maxGap > eps) {
        r.status = "separated";
        Vec3 pos = vmax(r.gaps, Vec3{0, 0, 0});
        r.distance = length(pos);
    } else if (maxGap >= -eps) {
        r.status = "touching";
    } else {
        r.status = "overlapping";
    }
    return r;
}

}  // namespace

Json inspectScene(const BuildResult& scene, const std::vector<std::string>& names) {
    Json out = Json::object();
    if (scene.bounds.valid()) {
        out["scene_bounds"] = Json::object({{"min", vecJson(scene.bounds.min)},
                                            {"max", vecJson(scene.bounds.max)},
                                            {"size", vecJson(scene.bounds.size())},
                                            {"center", vecJson(scene.bounds.center())}});
    } else {
        out["scene_bounds"] = Json();
    }

    std::vector<const BuiltObject*> selected;
    if (names.empty()) {
        for (const auto& o : scene.objects) selected.push_back(&o);
    } else {
        for (const auto& n : names) {
            const BuiltObject* o = scene.find(n);
            if (!o) throw SceneError("no object named '" + n + "'");
            selected.push_back(o);
        }
    }

    float sceneSize = scene.bounds.valid() ? maxComponent(scene.bounds.size()) : 1.0f;
    float eps = std::max(1e-5f, sceneSize * 1e-4f);

    // Contacts between all solid, visible objects (AABB based).
    std::vector<const BuiltObject*> solids;
    for (const auto& o : scene.objects)
        if (o.visible && o.bounds.valid()) solids.push_back(&o);
    std::map<std::string, std::vector<std::string>> contacts;
    for (size_t i = 0; i < solids.size(); ++i)
        for (size_t j = i + 1; j < solids.size(); ++j) {
            Relation r = relate(solids[i]->bounds, solids[j]->bounds, eps);
            if (r.status == "separated") continue;
            contacts[solids[i]->name].push_back(solids[j]->name);
            contacts[solids[j]->name].push_back(solids[i]->name);
        }

    Json objs = Json::array();
    for (const BuiltObject* o : selected) {
        Json j = Json::object({{"name", o->name}, {"shape", o->shape}});
        if (!o->parent.empty()) j["parent"] = o->parent;
        if (!o->visible) j["visible"] = false;
        if (o->bounds.valid()) {
            j["bounds"] = Json::object({{"min", vecJson(o->bounds.min)},
                                        {"max", vecJson(o->bounds.max)},
                                        {"size", vecJson(o->bounds.size())},
                                        {"center", vecJson(o->bounds.center())}});
            j["triangles"] = o->worldMesh.triangleCount();
            j["volume"] = roundTo(std::fabs(o->worldMesh.volume()), 5);
            j["surface_area"] = roundTo(o->worldMesh.surfaceArea(), 5);
            Json c = Json::array();
            for (const auto& n : contacts[o->name]) c.push_back(n);
            j["touching_or_overlapping"] = std::move(c);
        } else {
            j["note"] = "no geometry";
        }
        objs.push_back(std::move(j));
    }
    out["objects"] = std::move(objs);

    if (selected.size() >= 2 && !names.empty()) {
        Json rels = Json::array();
        for (size_t i = 0; i < selected.size(); ++i)
            for (size_t j = i + 1; j < selected.size(); ++j) {
                const BuiltObject *a = selected[i], *b = selected[j];
                if (!a->bounds.valid() || !b->bounds.valid()) continue;
                Relation r = relate(a->bounds, b->bounds, eps);
                Json rj = Json::object({{"a", a->name}, {"b", b->name}, {"status", r.status}});
                if (r.status == "separated") rj["gap_distance"] = roundTo(r.distance);
                rj["axis_gaps"] = vecJson(r.gaps);
                rj["center_offset_b_minus_a"] = vecJson(b->bounds.center() - a->bounds.center());
                rels.push_back(std::move(rj));
            }
        out["relations"] = std::move(rels);
        out["relations_note"] = "axis_gaps: positive = empty space between the boxes on that axis, negative = overlap depth";
    }

    // Common mistakes.
    Json warnings = Json::array();
    if (scene.bounds.valid()) {
        for (const BuiltObject* o : solids) {
            if (o->bounds.min.y < -eps * 10)
                warnings.push_back("'" + o->name + "' extends below the ground plane (min y = " +
                                   fmtG(o->bounds.min.y) + ")");
            if (o->bounds.min.y > eps * 10 && contacts[o->name].empty())
                warnings.push_back("'" + o->name + "' appears to float: bottom at y = " +
                                   fmtG(o->bounds.min.y) + " and it touches no other part");
        }
    }
    for (const auto& w : scene.warnings) warnings.push_back(w);
    out["warnings"] = std::move(warnings);
    out["note"] = "bounds and contacts are axis-aligned bounding boxes in world space (Y up)";
    return out;
}

}  // namespace sf
