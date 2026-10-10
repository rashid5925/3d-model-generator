#include "obj_export.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include "render/shading.hpp"

namespace sf {

namespace {

std::string sanitize(const std::string& name) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
    return out.empty() ? "object" : out;
}

std::string fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", std::fabs(v) < 1e-9f ? 0.0f : v);
    return buf;
}

}  // namespace

ObjExportResult exportObj(const BuildResult& scene, const std::string& objPath, const ObjExportOptions& opts) {
    namespace fs = std::filesystem;
    for (const auto& n : opts.only)
        if (!scene.find(n)) throw std::runtime_error("no object named '" + n + "' to export");

    fs::path obj(objPath);
    fs::path mtl = obj;
    mtl.replace_extension(".mtl");
    if (obj.has_parent_path()) fs::create_directories(obj.parent_path());

    std::ofstream o(obj), m(mtl);
    if (!o) throw std::runtime_error("cannot write " + obj.string());
    if (!m) throw std::runtime_error("cannot write " + mtl.string());

    ObjExportResult res;
    res.objPath = fs::absolute(obj).string();
    res.mtlPath = fs::absolute(mtl).string();
    o << "# Exported by shapeforge (Y up, front faces +Z)\n";
    o << "mtllib " << mtl.filename().string() << "\n";
    m << "# shapeforge materials\n";

    std::set<std::string> usedNames;
    size_t vbase = 1;
    for (const auto& ob : scene.objects) {
        if (ob.worldMesh.empty()) continue;
        if (!opts.only.empty()) {
            if (std::find(opts.only.begin(), opts.only.end(), ob.name) == opts.only.end()) continue;
        } else if (!ob.visible && !opts.includeHidden) {
            continue;
        }
        std::string name = sanitize(ob.name);
        std::string unique = name;
        for (int k = 2; usedNames.count(unique); ++k) unique = name + "_" + std::to_string(k);
        usedNames.insert(unique);

        // Kd is linear reflectance; convert from the sRGB display color.
        Vec3 c = ob.color;
        m << "\nnewmtl mat_" << unique << "\n";
        m << "Kd " << fmt(srgbToLinear(c.x)) << " " << fmt(srgbToLinear(c.y)) << " " << fmt(srgbToLinear(c.z)) << "\n";
        m << "Ka 0 0 0\nKs 0.1 0.1 0.1\nNs 50\nd 1\nillum 2\n";

        const Mesh& mesh = ob.worldMesh;
        o << "\no " << unique << "\n";
        for (const auto& p : mesh.positions)
            o << "v " << fmt(p.x * opts.scale) << " " << fmt(p.y * opts.scale) << " " << fmt(p.z * opts.scale) << "\n";
        if (opts.writeNormals)
            for (const auto& n : mesh.normals) o << "vn " << fmt(n.x) << " " << fmt(n.y) << " " << fmt(n.z) << "\n";
        o << "usemtl mat_" << unique << "\ns " << (opts.writeNormals ? "1" : "off") << "\n";
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            size_t a = vbase + mesh.indices[i], b = vbase + mesh.indices[i + 1], cc = vbase + mesh.indices[i + 2];
            if (opts.writeNormals)
                o << "f " << a << "//" << a << " " << b << "//" << b << " " << cc << "//" << cc << "\n";
            else
                o << "f " << a << " " << b << " " << cc << "\n";
        }
        vbase += mesh.positions.size();
        res.objects++;
        res.vertices += mesh.positions.size();
        res.triangles += mesh.triangleCount();
    }
    if (!o || !m) throw std::runtime_error("error while writing " + obj.string());
    return res;
}

}  // namespace sf
