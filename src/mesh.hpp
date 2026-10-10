// Triangle meshes and the procedural primitives the scene language exposes.
// All primitives are centered on the origin unless noted, with Y as the "up" axis,
// and wound counter-clockwise when seen from outside (outward-facing normals).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math.hpp"

namespace sf {

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;  // one per position
    std::vector<uint32_t> indices;

    size_t vertexCount() const { return positions.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
    bool empty() const { return indices.empty(); }

    uint32_t addVertex(Vec3 p, Vec3 n) {
        positions.push_back(p);
        normals.push_back(n);
        return static_cast<uint32_t>(positions.size() - 1);
    }
    void addTriangle(uint32_t a, uint32_t b, uint32_t c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }

    void append(const Mesh& other);
    // Transforms positions and normals; flips winding for mirroring transforms.
    void transform(const Mat4& m);
    AABB bounds() const;
    // Signed volume (meaningful for closed meshes) and total surface area.
    double volume() const;
    double surfaceArea() const;
};

Mesh makeBox(Vec3 size);
Mesh makePlane(float sizeX, float sizeZ);
Mesh makeSphere(float radius, int segments, int rings);
Mesh makeCylinder(float radiusBottom, float radiusTop, float height, int segments);
Mesh makeCapsule(float radius, float height, int segments);
Mesh makeTorus(float majorRadius, float tubeRadius, int segments, int sides);

// Revolves a profile of (radius, y) points around the Y axis. Points run bottom to
// top. Adjacent segments meeting at less than `creaseDeg` are smoothed.
Mesh makeLathe(const std::vector<Vec2>& profile, int segments, float creaseDeg = 40.0f);

// Extrudes a simple polygon. axis 'z': points are (x, y), depth along Z.
// axis 'y': points are (x, z), depth along Y. axis 'x': points are (z, y), depth along X.
// Throws std::runtime_error if the polygon cannot be triangulated.
Mesh makeExtrusion(const std::vector<Vec2>& polygon, float depth, char axis);

// Polygon soup from explicit vertices and faces (each face has >= 3 indices).
Mesh makeRawMesh(const std::vector<Vec3>& vertices, const std::vector<std::vector<uint32_t>>& faces, bool smooth);

// Ear-clipping triangulation of a simple polygon (either winding). Output indices
// are wound counter-clockwise. Returns false for degenerate input.
bool triangulatePolygon(const std::vector<Vec2>& polygon, std::vector<uint32_t>& outIndices);
float signedArea(const std::vector<Vec2>& polygon);

}  // namespace sf
