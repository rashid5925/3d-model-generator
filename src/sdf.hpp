// Signed distance fields for constructive solid geometry (union / subtract /
// intersect, optionally smooth) and a surface-nets polygonizer that turns an SDF
// into a triangle mesh. Negative distance = inside.
#pragma once

#include <memory>
#include <vector>

#include "math.hpp"
#include "mesh.hpp"

namespace sf {

class SdfNode {
public:
    virtual ~SdfNode() = default;
    virtual float eval(const Vec3& p) const = 0;
    virtual AABB bounds() const = 0;
    // Smallest dimension of any primitive in the tree; used to pick a grid fine
    // enough that thin parts (plates, walls) are not lost.
    virtual float minFeature() const { return minComponent(bounds().size()); }
};

using SdfPtr = std::unique_ptr<SdfNode>;

SdfPtr sdfBox(Vec3 size, float rounding);
SdfPtr sdfSphere(float radius);
SdfPtr sdfCappedCone(float height, float radiusBottom, float radiusTop);  // Y axis, centered
SdfPtr sdfCapsule(float radius, float height);                             // total height, Y axis
SdfPtr sdfTorus(float majorRadius, float tubeRadius);                      // ring in XZ plane
SdfPtr sdfExtrusion(std::vector<Vec2> polygon, float depth, char axis);
SdfPtr sdfRevolve(std::vector<Vec2> profile);  // closed (radius, y) polygon around Y

enum class CsgOp { Union, Subtract, Intersect };
SdfPtr sdfCombine(CsgOp op, std::vector<SdfPtr> children, float smooth);
SdfPtr sdfTransform(SdfPtr child, const Mat4& transform);
SdfPtr sdfShell(SdfPtr child, float thickness);

// Grid resolution (cells along the longest side) that gives the thinnest
// feature at least `cellsPerFeature` cells, clamped to [minRes, maxRes].
int autoResolution(const SdfNode& sdf, int minRes, int maxRes, float cellsPerFeature = 6.0f);

// Polygonizes the zero level set. `resolution` = cells along the longest axis.
// The grid cell size is written to *cellSize when given.
Mesh polygonize(const SdfNode& sdf, int resolution, float* cellSize = nullptr);

}  // namespace sf
