#include "mcp_server.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <iostream>
#include <sstream>

#include "inspect.hpp"
#include "obj_export.hpp"
#include "placement.hpp"
#include "platform.hpp"

namespace sf {

namespace fs = std::filesystem;

namespace {

constexpr const char* kServerName = "shapeforge";
constexpr const char* kServerVersion = SHAPEFORGE_VERSION;
constexpr size_t kUndoDepth = 64;

const char* kInstructions = R"(shapeforge lets you build 3D models from parametric parts, look at them, measure them and export Wavefront OBJ.

Recommended loop:
1. Plan the model as named parts with real-world dimensions (meters). +Y is up, the model's front faces +Z.
2. add_objects with all parts in one call (use "parent" + a "group" to move assemblies together).
3. render to look at it (default: front/right/top orthographic + a perspective view). Check silhouettes and proportions.
4. inspect for exact numbers: bounds, sizes, which parts touch, floating parts, parts below the ground.
5. Fix with update_objects (merge-patch, "translate" for relative moves) and re-render. undo reverts the last change.
6. export_obj when it looks right.

Let the placement tools do spatial math instead of computing coordinates by hand:
- place_on: rest a part on top of (or against a side of) another part or the ground, touching its real surface.
- align_objects: line parts up (min/center/max) on x, y and/or z.
- mirror_object: build one side, mirror it to get the other (left/right names are swapped automatically).
- array_object: linear rows or radial rings of copies (chair legs, spokes, fence posts, bolts).
For small details, render with "focus" (and optionally "isolate") to zoom in.

Shapes are centered on their origin, so a part resting on the ground needs position.y = half its height.
Use csg (union/subtract/intersect, optional smooth blending) for holes, cut-outs and organic blends.)";

Json textContent(const std::string& text) { return Json::object({{"type", "text"}, {"text", text}}); }

Json toolResult(Json content, bool isError = false) {
    Json r = Json::object({{"content", std::move(content)}});
    if (isError) r["isError"] = true;
    return r;
}

Json errorResult(const std::string& msg) { return toolResult(Json::array({textContent("Error: " + msg)}), true); }

Json warningsJson(const std::vector<std::string>& w) {
    Json a = Json::array();
    for (const auto& s : w) a.push_back(s);
    return a;
}

std::vector<std::string> stringList(const Json& args, const char* key) {
    std::vector<std::string> out;
    const Json* v = args.find(key);
    if (!v || v->isNull()) return out;
    if (v->isString()) {
        out.push_back(v->asString());
        return out;
    }
    if (!v->isArray()) throw SceneError(std::string("'") + key + "' must be an array of names");
    for (const auto& s : v->asArray()) {
        if (!s.isString()) throw SceneError(std::string("'") + key + "' must contain only strings");
        out.push_back(s.asString());
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hierarchyText(const Scene& scene) {
    std::ostringstream os;
    std::map<std::string, std::vector<const Json*>> children;
    std::vector<const Json*> roots;
    for (const auto& o : scene.objects()) {
        std::string p = o.stringOr("parent", "");
        if (!p.empty() && scene.get(p)) children[p].push_back(&o);
        else roots.push_back(&o);
    }
    std::function<void(const Json*, int, int)> walk = [&](const Json* o, int depth, int guard) {
        if (guard > 64) return;
        std::string name = o->stringOr("name", "?");
        os << std::string(static_cast<size_t>(depth) * 2, ' ') << "- " << name << " (" << o->stringOr("shape", "?") << ")";
        if (const Json* d = o->find("description"); d && d->isString()) os << ": " << d->asString();
        os << "\n";
        for (const Json* c : children[name]) walk(c, depth + 1, guard + 1);
    };
    for (const Json* r : roots) walk(r, 0, 0);
    return os.str();
}

const char* kToolSchemas = R"JSON([
{
  "name": "add_objects",
  "inputSchema": {
    "type": "object",
    "properties": {
      "objects": {
        "type": "array",
        "description": "Object specs. Each needs a unique 'name' and a 'shape'. An existing name is replaced. Use \"copy_of\": \"<name>\" to start from another object's spec and override fields.",
        "items": {"type": "object"}
      },
      "clear_scene": {"type": "boolean", "description": "Remove all existing objects first (default false)."}
    },
    "required": ["objects"]
  }
},
{
  "name": "update_objects",
  "description": "Modify existing objects with a merge-patch: given keys replace old values, a null value removes the key. Extra keys: \"rename\" (new name; children follow), \"translate\" [dx,dy,dz] (relative move). Example: {\"updates\": [{\"name\": \"leg_1\", \"translate\": [0, 0.05, 0]}, {\"name\": \"seat\", \"color\": \"#8b5a2b\", \"size\": [0.5, 0.05, 0.5]}]}. The batch is atomic.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "updates": {
        "type": "array",
        "items": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}
      }
    },
    "required": ["updates"]
  }
},
{
  "name": "delete_objects",
  "description": "Delete objects by name. With recursive (default true) their children are deleted too; otherwise children are detached to the root.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "names": {"type": "array", "items": {"type": "string"}},
      "recursive": {"type": "boolean"}
    },
    "required": ["names"]
  }
},
{
  "name": "get_scene",
  "description": "Return the full scene: the hierarchy outline and every object's JSON spec exactly as stored. Use it to recall what exists before editing.",
  "inputSchema": {"type": "object", "properties": {"names": {"type": "array", "items": {"type": "string"}, "description": "Only these objects (default all)."}}}
},
{
  "name": "clear_scene",
  "description": "Remove every object (undoable).",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "render",
  "inputSchema": {
    "type": "object",
    "properties": {
      "views": {
        "type": "array",
        "description": "Up to 9 views. Default [\"front\", \"right\", \"top\", \"iso\"].",
        "items": {"anyOf": [{"type": "string"}, {"type": "object"}]}
      },
      "size": {"type": "integer", "description": "Pixels per view, 128-1024 (default 512)."},
      "highlight": {"type": "array", "items": {"type": "string"}, "description": "Show these objects in color and grey out the rest."},
      "labels": {"type": "boolean", "description": "Draw object names at their centers (default false)."},
      "focus": {"type": "array", "items": {"type": "string"}, "description": "Zoom the cameras onto these objects (and their children) to see small details."},
      "isolate": {"type": "boolean", "description": "With focus: hide everything else."},
      "grid": {"type": "boolean", "description": "Draw the ground grid on y = 0 (default true). Red line = X axis, blue line = Z axis."},
      "save_path": {"type": "string", "description": "Optional .png path to also save the image to."}
    }
  }
},
{
  "name": "place_on",
  "description": "Move objects so they rest on a target without gaps or overlaps. The object is first put against the target's bounding box, then slid until it touches the target's actual surface (e.g. a ball settles into a bowl, a lamp sits on a curved base). Objects with children move together. Examples: {\"object\": \"mug\", \"target\": \"tabletop\"}; {\"objects\": [\"leg_1\", \"leg_2\"], \"target\": \"ground\"}; {\"object\": \"handle\", \"target\": \"door\", \"side\": \"front\", \"at\": [0.35, 0]}.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "object": {"type": "string"},
      "objects": {"type": "array", "items": {"type": "string"}, "description": "Several objects, each placed independently."},
      "target": {"type": "string", "description": "Object name, or \"ground\" for the y = 0 plane (default)."},
      "side": {"type": "string", "enum": ["top", "bottom", "left", "right", "front", "back"], "description": "Face of the target to place against (default top). right = +X, front = +Z."},
      "at": {"type": "array", "items": {"type": "number"}, "description": "Optional [a, b] offset from the center of that face along its two in-plane world axes (top/bottom: [x, z]; left/right: [y, z]; front/back: [x, y]). Without it the object keeps its current position on those axes."},
      "center": {"type": "boolean", "description": "Center the object on the face (same as at [0, 0])."},
      "gap": {"type": "number", "description": "Distance to leave between the surfaces (default 0)."},
      "contact": {"type": "boolean", "description": "Settle onto the real surface (default true); false stops at the bounding box."}
    }
  }
},
{
  "name": "align_objects",
  "description": "Align objects' bounding boxes on one or more axes. mode min/center/max; 'to' is a reference object name or a world coordinate (default: the first listed object). Example: {\"objects\": [\"lamp\", \"vase\", \"clock\"], \"axis\": \"z\", \"mode\": \"center\"}.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "objects": {"type": "array", "items": {"type": "string"}},
      "axis": {"anyOf": [{"type": "string", "enum": ["x", "y", "z"]}, {"type": "array", "items": {"type": "string", "enum": ["x", "y", "z"]}}]},
      "mode": {"type": "string", "enum": ["min", "center", "max"]},
      "to": {"anyOf": [{"type": "string"}, {"type": "number"}]}
    },
    "required": ["objects", "axis"]
  }
},
{
  "name": "mirror_object",
  "description": "Create a mirrored copy of an object (with all its children) across the plane axis = plane (default x = 0, i.e. left/right symmetry). Names are swapped automatically (left<->right, _l<->_r, l_<->r_; otherwise '_mirror' is appended), or pass 'name' / 'replace'. For parented objects the plane is in the parent's local space. in_place: true mirrors the object itself instead.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "object": {"type": "string"},
      "axis": {"type": "string", "enum": ["x", "y", "z"]},
      "plane": {"type": "number"},
      "name": {"type": "string", "description": "Name for the mirrored root object."},
      "replace": {"type": "array", "items": {"type": "string"}, "description": "[from, to] substring replacement for all copied names."},
      "in_place": {"type": "boolean"}
    },
    "required": ["object"]
  }
},
{
  "name": "array_object",
  "description": "Make copies of an object (with its children). count = total including the original; copies are named <name>_2, <name>_3, ... Linear: {\"object\": \"post\", \"count\": 6, \"offset\": [0.5, 0, 0]}. Radial around an axis through 'center' (copies rotated to face outward like the original): {\"object\": \"spoke\", \"count\": 8, \"mode\": \"radial\", \"axis\": \"y\", \"center\": [0, 0, 0]}; 'angle' (default 360) spreads them over an arc. Positions are in the parent's local space.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "object": {"type": "string"},
      "count": {"type": "integer"},
      "mode": {"type": "string", "enum": ["linear", "radial"]},
      "offset": {"type": "array", "items": {"type": "number"}},
      "axis": {"type": "string", "enum": ["x", "y", "z"]},
      "center": {"type": "array", "items": {"type": "number"}},
      "angle": {"type": "number"},
      "rotate_copies": {"type": "boolean"},
      "name_prefix": {"type": "string"}
    },
    "required": ["object", "count"]
  }
},
{
  "name": "inspect",
  "description": "Exact numbers about the model: world-space bounding boxes (min/max/size/center), triangle counts, volume, surface area, which parts touch or overlap, and warnings for floating parts or parts below the ground. Pass 2+ names to get per-axis gaps between those parts - use this to line parts up precisely.",
  "inputSchema": {"type": "object", "properties": {"names": {"type": "array", "items": {"type": "string"}}}}
},
{
  "name": "export_obj",
  "description": "Write the model as Wavefront OBJ plus an MTL material file (one named object + material per part, Y up). Relative paths are resolved against the server's working directory.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "path": {"type": "string", "description": "Output .obj path (default model.obj)."},
      "objects": {"type": "array", "items": {"type": "string"}, "description": "Only these objects (default all visible)."},
      "include_hidden": {"type": "boolean"},
      "scale": {"type": "number", "description": "Uniform scale applied on export (default 1)."}
    }
  }
},
{
  "name": "save_scene",
  "description": "Save the scene's object specs as JSON so the model can be reloaded and edited later.",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string", "description": "Output .json path (default scene.json)."}}}
},
{
  "name": "load_scene",
  "description": "Load a scene JSON file saved by save_scene (or any {\"objects\": [...]} document). Replaces the current scene unless merge is true.",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "merge": {"type": "boolean"}}, "required": ["path"]}
},
{
  "name": "undo",
  "description": "Revert the most recent scene change (add/update/delete/clear/load).",
  "inputSchema": {"type": "object", "properties": {"steps": {"type": "integer", "description": "How many changes to revert (default 1)."}}}
}
])JSON";

}  // namespace

Json McpServer::toolDefinitions() {
    Json tools = Json::parse(kToolSchemas);
    for (auto& t : tools.asArray()) {
        std::string name = t["name"].asString();
        if (name == "add_objects") {
            t["description"] = std::string(
                                   "Add (or replace) parts in the scene. Send many parts in one call. Returns each "
                                   "part's world bounding box so you can verify placement. The batch is atomic: on "
                                   "any error nothing changes.\n\n") +
                               shapeReference() +
                               "\n\nExample: {\"objects\": [{\"name\": \"table\", \"shape\": \"group\"}, {\"name\": "
                               "\"top\", \"parent\": \"table\", \"shape\": \"box\", \"size\": [1.2, 0.04, 0.7], "
                               "\"position\": [0, 0.73, 0], \"color\": \"wood\"}, {\"name\": \"leg_fl\", \"parent\": "
                               "\"table\", \"shape\": \"cylinder\", \"radius\": 0.03, \"height\": 0.71, \"position\": "
                               "[-0.55, 0.355, 0.3]}, {\"name\": \"leg_fr\", \"copy_of\": \"leg_fl\", \"position\": "
                               "[0.55, 0.355, 0.3]}]}";
        } else if (name == "render") {
            t["description"] = std::string(
                                   "Render the scene and return an image so you can see it. Several views are tiled "
                                   "into one picture, each labeled with its name, which world axes point right/up "
                                   "on screen, and the ground-grid spacing; an XYZ gizmo sits in each corner "
                                   "(X red, Y green, Z blue).\n") +
                               viewReference();
        }
    }
    return tools;
}

McpServer::McpServer(ServerConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.workdir.empty()) cfg_.workdir = defaultWorkdir();
}

McpServer::~McpServer() = default;

Renderer& McpServer::renderer() {
    if (!renderer_) {
        std::string log;
        renderer_ = createRenderer(cfg_.renderer, log);
        std::cerr << "[shapeforge] " << log << std::endl;
    }
    return *renderer_;
}

std::string McpServer::resolvePath(const std::string& path, const std::string& ext) const {
    if (path.empty()) throw SceneError("path must not be empty");
    fs::path p(path);
    if (p.is_relative()) p = fs::path(cfg_.workdir) / p;
    if (lower(p.extension().string()) != ext) throw SceneError("path must end with " + ext + ": " + path);
    p = p.lexically_normal();
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    return p.string();
}

void McpServer::snapshot() {
    undo_.push_back(scene_.objects());
    if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
}

int McpServer::run(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        Json response;
        try {
            Json msg = Json::parse(line);
            if (msg.isArray()) {
                Json batch = Json::array();
                for (const auto& m : msg.asArray()) {
                    Json r = handle(m);
                    if (!r.isNull()) batch.push_back(std::move(r));
                }
                if (batch.size()) response = std::move(batch);
            } else {
                response = handle(msg);
            }
        } catch (const JsonError& e) {
            response = Json::object({{"jsonrpc", "2.0"},
                                     {"id", Json()},
                                     {"error", Json::object({{"code", -32700}, {"message", e.what()}})}});
        }
        if (!response.isNull()) {
            out << response.dump() << "\n";
            out.flush();
        }
    }
    return 0;
}

Json McpServer::handle(const Json& msg) {
    const Json* id = msg.find("id");
    std::string method = msg.stringOr("method", "");
    bool isRequest = id != nullptr;
    auto reply = [&](Json result) {
        return Json::object({{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(result)}});
    };
    auto error = [&](int code, const std::string& message) {
        return Json::object({{"jsonrpc", "2.0"},
                             {"id", id ? *id : Json()},
                             {"error", Json::object({{"code", code}, {"message", message}})}});
    };

    if (!isRequest) return Json();  // notifications (initialized, cancelled, ...) and stray responses

    try {
        if (method == "initialize") {
            std::string version = msg["params"].stringOr("protocolVersion", "2025-06-18");
            return reply(Json::object(
                {{"protocolVersion", version},
                 {"capabilities", Json::object({{"tools", Json::object({{"listChanged", false}})}})},
                 {"serverInfo", Json::object({{"name", kServerName}, {"version", kServerVersion}})},
                 {"instructions", kInstructions}}));
        }
        if (method == "ping") return reply(Json::object());
        if (method == "tools/list") return reply(Json::object({{"tools", toolDefinitions()}}));
        if (method == "tools/call") {
            const Json& params = msg["params"];
            std::string name = params.stringOr("name", "");
            const Json* args = params.find("arguments");
            return reply(callTool(name, args && args->isObject() ? *args : Json::object()));
        }
        if (method == "resources/list") return reply(Json::object({{"resources", Json::array()}}));
        if (method == "prompts/list") return reply(Json::object({{"prompts", Json::array()}}));
        return error(-32601, "method not found: " + method);
    } catch (const std::exception& e) {
        return error(-32603, e.what());
    }
}

Json McpServer::callTool(const std::string& name, const Json& args) {
    try {
        if (name == "add_objects") return toolAddObjects(args);
        if (name == "update_objects") return toolUpdateObjects(args);
        if (name == "delete_objects") return toolDeleteObjects(args);
        if (name == "get_scene") return toolGetScene(args);
        if (name == "clear_scene") return toolClearScene(args);
        if (name == "render") return toolRender(args);
        if (name == "place_on") return toolPlacement(args, placeOn);
        if (name == "align_objects") return toolPlacement(args, alignObjects);
        if (name == "mirror_object") return toolPlacement(args, mirrorObject);
        if (name == "array_object") return toolPlacement(args, arrayObject);
        if (name == "inspect") return toolInspect(args);
        if (name == "export_obj") return toolExportObj(args);
        if (name == "save_scene") return toolSaveScene(args);
        if (name == "load_scene") return toolLoadScene(args);
        if (name == "undo") return toolUndo(args);
        return errorResult("unknown tool '" + name + "'");
    } catch (const std::exception& e) {
        return errorResult(e.what());
    }
}

Json McpServer::toolAddObjects(const Json& args) {
    const Json* objs = args.find("objects");
    if (!objs || !objs->isArray()) {
        // Be forgiving: a single object spec passed directly.
        if (args.find("shape") && args.find("name")) return toolAddObjects(Json::object({{"objects", Json::array({args})}}));
        throw SceneError("'objects' must be an array of object specs");
    }
    Scene trial = scene_;
    if (args.boolOr("clear_scene", false)) trial.clear();
    std::vector<std::string> warnings;
    std::vector<std::string> added;
    for (size_t i = 0; i < objs->size(); ++i) {
        Json spec = objs->at(i);
        if (!spec.isObject()) throw SceneError("objects[" + std::to_string(i) + "] must be an object");
        if (const Json* src = spec.find("copy_of")) {
            if (!src->isString()) throw SceneError("'copy_of' must be an object name");
            const Json* base = trial.get(src->asString());
            if (!base) throw SceneError("objects[" + std::to_string(i) + "]: copy_of '" + src->asString() + "' does not exist");
            Json merged = *base;
            for (const auto& [k, v] : spec.asObject()) {
                if (k == "copy_of") continue;
                if (v.isNull()) merged.erase(k);
                else merged[k] = v;
            }
            spec = std::move(merged);
        }
        std::string nm = spec.stringOr("name", "");
        if (std::find(added.begin(), added.end(), nm) != added.end())
            warnings.push_back("name '" + nm + "' appears twice in this batch; the later spec replaced the earlier one");
        trial.put(std::move(spec), warnings);
        added.push_back(nm);
    }
    BuildResult built = trial.build();
    snapshot();
    scene_ = std::move(trial);

    Json summary = Json::array();
    for (const auto& n : added)
        if (const BuiltObject* o = built.find(n)) summary.push_back(summarizeObject(*o));
    Json report = Json::object({{"added_or_replaced", std::move(summary)},
                                {"scene_object_count", scene_.size()}});
    if (built.bounds.valid())
        report["scene_bounds"] = Json::object({{"min", vecJson(built.bounds.min)}, {"max", vecJson(built.bounds.max)}});
    std::vector<std::string> allWarnings = warnings;
    allWarnings.insert(allWarnings.end(), built.warnings.begin(), built.warnings.end());
    if (!allWarnings.empty()) report["warnings"] = warningsJson(allWarnings);
    return toolResult(Json::array({textContent(report.dump(1) + "\n\nNext: call render to look at the result.")}));
}

Json McpServer::toolUpdateObjects(const Json& args) {
    const Json* ups = args.find("updates");
    Json single;
    if (!ups || !ups->isArray()) {
        if (!args.find("name")) throw SceneError("'updates' must be an array of {\"name\": ..., <fields>} patches");
        single = Json::array({args});
        ups = &single;
    }
    Scene trial = scene_;
    std::vector<std::string> warnings, touched;
    for (size_t i = 0; i < ups->size(); ++i) {
        const Json& u = ups->at(i);
        if (!u.isObject() || !u.find("name") || !u["name"].isString())
            throw SceneError("updates[" + std::to_string(i) + "] needs a string 'name'");
        std::string name = u["name"].asString();
        trial.update(name, u, warnings);
        const Json* rn = u.find("rename");
        touched.push_back(rn && rn->isString() ? rn->asString() : name);
    }
    BuildResult built = trial.build();
    snapshot();
    scene_ = std::move(trial);
    Json summary = Json::array();
    for (const auto& n : touched)
        if (const BuiltObject* o = built.find(n)) summary.push_back(summarizeObject(*o));
    Json report = Json::object({{"updated", std::move(summary)}});
    warnings.insert(warnings.end(), built.warnings.begin(), built.warnings.end());
    if (!warnings.empty()) report["warnings"] = warningsJson(warnings);
    return toolResult(Json::array({textContent(report.dump(1))}));
}

Json McpServer::toolDeleteObjects(const Json& args) {
    std::vector<std::string> names = stringList(args, "names");
    if (names.empty()) throw SceneError("'names' must list at least one object");
    Scene trial = scene_;
    auto removed = trial.remove(names, args.boolOr("recursive", true));
    snapshot();
    scene_ = std::move(trial);
    Json report = Json::object({{"deleted", warningsJson(removed)}, {"scene_object_count", scene_.size()}});
    return toolResult(Json::array({textContent(report.dump(1))}));
}

Json McpServer::toolGetScene(const Json& args) {
    std::vector<std::string> names = stringList(args, "names");
    Json objs = Json::array();
    for (const auto& o : scene_.objects()) {
        if (!names.empty() && std::find(names.begin(), names.end(), o.stringOr("name", "")) == names.end()) continue;
        objs.push_back(o);
    }
    for (const auto& n : names)
        if (!scene_.get(n)) throw SceneError("no object named '" + n + "'");
    std::string text = scene_.size() == 0 ? std::string("The scene is empty.\n")
                                          : "Hierarchy (" + std::to_string(scene_.size()) + " objects):\n" +
                                                hierarchyText(scene_);
    text += "\nObject specs:\n" + objs.dump(1);
    return toolResult(Json::array({textContent(text)}));
}

Json McpServer::toolClearScene(const Json&) {
    snapshot();
    size_t n = scene_.size();
    scene_.clear();
    return toolResult(Json::array({textContent("Removed " + std::to_string(n) + " objects. (undo restores them)")}));
}

Json McpServer::toolRender(const Json& args) {
    RenderOptions opts;
    if (const Json* v = args.find("views"); v && !v->isNull()) {
        if (v->isString()) opts.views.push_back(*v);
        else if (v->isArray()) opts.views = v->asArray();
        else throw SceneError("'views' must be an array");
    }
    opts.size = static_cast<int>(args.numberOr("size", 512));
    opts.highlight = stringList(args, "highlight");
    opts.labels = args.boolOr("labels", false);
    opts.focus = stringList(args, "focus");
    opts.isolate = args.boolOr("isolate", false);
    opts.grid = args.boolOr("grid", true);

    BuildResult built = scene_.build();
    RenderOutput out = renderScene(renderer(), built, opts);
    std::vector<uint8_t> png = encodePng(out.image);

    Json info = out.info;
    if (built.bounds.valid())
        info["scene_bounds"] = Json::object({{"min", vecJson(built.bounds.min)},
                                             {"max", vecJson(built.bounds.max)},
                                             {"size", vecJson(built.bounds.size())}});
    else
        info["note"] = "the scene has no visible geometry";
    if (const Json* sp = args.find("save_path"); sp && sp->isString()) {
        std::string path = resolvePath(sp->asString(), ".png");
        if (!writeFile(path, png)) throw SceneError("could not write " + path);
        info["saved_to"] = path;
    }
    if (!built.warnings.empty()) info["warnings"] = warningsJson(built.warnings);
    return toolResult(Json::array({textContent(info.dump(1)),
                                   Json::object({{"type", "image"}, {"data", base64Encode(png)}, {"mimeType", "image/png"}})}));
}

Json McpServer::toolPlacement(const Json& args, PlacementResult (*op)(Scene&, const Json&)) {
    Scene trial = scene_;
    PlacementResult r = op(trial, args);
    BuildResult built = trial.build();
    snapshot();
    scene_ = std::move(trial);
    Json summary = Json::array();
    for (const auto& n : r.changed)
        if (const BuiltObject* o = built.find(n)) summary.push_back(summarizeObject(*o));
    Json report = Json::object({{"changed", std::move(summary)}});
    if (!r.notes.empty()) report["notes"] = warningsJson(r.notes);
    if (!built.warnings.empty()) report["warnings"] = warningsJson(built.warnings);
    return toolResult(Json::array({textContent(report.dump(1))}));
}

Json McpServer::toolInspect(const Json& args) {
    BuildResult built = scene_.build();
    Json report = inspectScene(built, stringList(args, "names"));
    return toolResult(Json::array({textContent(report.dump(1))}));
}

Json McpServer::toolExportObj(const Json& args) {
    std::string path = resolvePath(args.stringOr("path", "model.obj"), ".obj");
    ObjExportOptions opts;
    opts.only = stringList(args, "objects");
    opts.includeHidden = args.boolOr("include_hidden", false);
    opts.scale = static_cast<float>(args.numberOr("scale", 1.0));
    BuildResult built = scene_.build();
    if (!built.bounds.valid() && opts.only.empty()) throw SceneError("nothing to export: the scene has no visible geometry");
    ObjExportResult r = exportObj(built, path, opts);
    Json report = Json::object({{"obj", r.objPath},
                                {"mtl", r.mtlPath},
                                {"objects", r.objects},
                                {"vertices", r.vertices},
                                {"triangles", r.triangles}});
    return toolResult(Json::array({textContent(report.dump(1))}));
}

Json McpServer::toolSaveScene(const Json& args) {
    std::string path = resolvePath(args.stringOr("path", "scene.json"), ".json");
    std::ofstream f(path);
    if (!f) throw SceneError("could not write " + path);
    f << scene_.toJson().dump(2) << "\n";
    return toolResult(Json::array({textContent("Saved " + std::to_string(scene_.size()) + " objects to " + path)}));
}

Json McpServer::toolLoadScene(const Json& args) {
    std::string path = resolvePath(args.stringOr("path", ""), ".json");
    std::ifstream f(path);
    if (!f) throw SceneError("could not read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    Json doc = Json::parse(ss.str());
    std::vector<std::string> warnings;
    Scene trial;
    if (args.boolOr("merge", false)) {
        trial = scene_;
        const Json* arr = doc.isArray() ? &doc : doc.find("objects");
        if (!arr || !arr->isArray()) throw SceneError("scene file must contain an 'objects' array");
        for (const auto& o : arr->asArray()) trial.put(o, warnings);
    } else {
        trial.loadJson(doc, warnings);
    }
    BuildResult built = trial.build();
    snapshot();
    scene_ = std::move(trial);
    Json report = Json::object({{"loaded_from", path}, {"scene_object_count", scene_.size()}});
    if (built.bounds.valid())
        report["scene_bounds"] = Json::object({{"min", vecJson(built.bounds.min)}, {"max", vecJson(built.bounds.max)}});
    warnings.insert(warnings.end(), built.warnings.begin(), built.warnings.end());
    if (!warnings.empty()) report["warnings"] = warningsJson(warnings);
    return toolResult(Json::array({textContent(report.dump(1) + "\n\n" + hierarchyText(scene_))}));
}

Json McpServer::toolUndo(const Json& args) {
    int steps = std::max(1, static_cast<int>(args.numberOr("steps", 1)));
    int done = 0;
    while (done < steps && !undo_.empty()) {
        scene_.restore(std::move(undo_.back()));
        undo_.pop_back();
        ++done;
    }
    if (done == 0) return toolResult(Json::array({textContent("Nothing to undo.")}));
    return toolResult(Json::array({textContent("Reverted " + std::to_string(done) + " change(s). Scene now has " +
                                               std::to_string(scene_.size()) + " objects.\n" + hierarchyText(scene_))}));
}

}  // namespace sf
