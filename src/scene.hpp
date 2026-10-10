// The scene: an ordered list of named objects, each described by a flat JSON spec
// (the same JSON an LLM sends through the tool API). Specs are the source of
// truth; meshes are derived from them on demand and cached.
#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
#include "math.hpp"
#include "mesh.hpp"

namespace sf {

class SceneError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct BuiltObject {
    std::string name;
    std::string shape;
    std::string parent;
    Mat4 world;
    Vec3 color;  // sRGB, 0..1
    bool visible = true;
    std::shared_ptr<const Mesh> localMesh;
    Mesh worldMesh;
    AABB bounds;  // world space; invalid for empty meshes (groups)
};

struct BuildResult {
    std::vector<BuiltObject> objects;
    std::vector<std::string> warnings;
    AABB bounds;  // of visible geometry

    const BuiltObject* find(const std::string& name) const;
};

class Scene {
public:
    const std::vector<Json>& objects() const { return objects_; }
    size_t size() const { return objects_.size(); }
    int indexOf(const std::string& name) const;
    const Json* get(const std::string& name) const;
    // The object and all of its descendants, in scene order.
    std::vector<std::string> subtree(const std::string& name) const;

    // Adds or replaces (by name). Validates first; throws SceneError on bad specs.
    void put(Json spec, std::vector<std::string>& warnings);
    // Applies a merge-patch (null removes a key). Handles "rename" and "translate".
    void update(const std::string& name, const Json& patch, std::vector<std::string>& warnings);
    // Removes objects; with recursive, also their descendants. Returns removed names.
    std::vector<std::string> remove(const std::vector<std::string>& names, bool recursive);
    void clear() { objects_.clear(); }
    // Restores a previously captured object list verbatim (used by undo).
    void restore(std::vector<Json> objects) { objects_ = std::move(objects); }

    BuildResult build() const;

    Json toJson() const;
    // Accepts {"objects": [...]} or a bare array. Throws SceneError.
    void loadJson(const Json& doc, std::vector<std::string>& warnings);

    // Throws SceneError for invalid specs; appends non-fatal issues to warnings.
    static void validate(const Json& spec, std::vector<std::string>& warnings);

private:
    std::shared_ptr<const Mesh> localMesh(const Json& spec, const std::string& ctx) const;
    void buildChecked(const Json& spec, std::vector<std::string>& warnings) const;

    std::vector<Json> objects_;
    mutable std::map<std::string, std::shared_ptr<const Mesh>> cache_;
};

// Parses "#rrggbb", "#rgb", [r,g,b] (0..1 or 0..255) or a CSS-like color name.
Vec3 parseColor(const Json& value, const std::string& ctx);
std::string colorToHex(Vec3 c);

// Human/LLM-readable catalog of shapes and their parameters.
const char* shapeReference();

}  // namespace sf
