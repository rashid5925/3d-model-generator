// Wavefront OBJ + MTL export. One OBJ object ("o") and one material per scene
// object, so the parts stay separate and named when imported into Blender etc.
#pragma once

#include <string>
#include <vector>

#include "scene.hpp"

namespace sf {

struct ObjExportOptions {
    std::vector<std::string> only;  // empty = every visible object
    bool includeHidden = false;
    bool writeNormals = true;
    float scale = 1.0f;
};

struct ObjExportResult {
    std::string objPath, mtlPath;
    size_t objects = 0, vertices = 0, triangles = 0;
};

// Throws std::runtime_error on I/O failure or unknown object names.
ObjExportResult exportObj(const BuildResult& scene, const std::string& objPath, const ObjExportOptions& options);

}  // namespace sf
